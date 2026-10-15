#include "ha/WsTransport.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_random.h>
#include <lwip/dns.h>
#include <lwip/ip_addr.h>
#include <lwip/sockets.h>
#include <lwip/tcpip.h>

#include <atomic>
#include <cerrno>
#include <cstring>

#include "util/Log.h"
#include "ws/WsFrame.h"
#include "ws/WsHandshake.h"

namespace ha {

namespace {

namespace hc = core::ha;
namespace ws = core::ws;

constexpr size_t kMaxLoggedReason = 40;
constexpr uint16_t kCloseNormal = 1000;
constexpr uint16_t kCloseProtocolError = 1002;

// ---- Asynchronous name resolution ------------------------------------------------------
// One lookup at a time, run by lwIP in the tcpip task (dns_gethostbyname must not be
// called from another task). An abandoned lookup finishes in the background; the next
// attempt waits for it before starting its own.

enum : uint8_t { kDnsIdle, kDnsPending, kDnsOk, kDnsFailed };

struct DnsJob {
    std::atomic<uint8_t> status{kDnsIdle};
    char host[hc::kMaxHostLen + 1] = {0};
    ip_addr_t addr = {};
};

DnsJob dnsJob;

void dnsFound(const char *, const ip_addr_t *addr, void *arg) {  // tcpip task
    DnsJob *job = static_cast<DnsJob *>(arg);
    if (addr != nullptr) {
        job->addr = *addr;
        job->status = kDnsOk;
    } else {
        job->status = kDnsFailed;
    }
}

void dnsStart(void *arg) {  // tcpip task
    DnsJob *job = static_cast<DnsJob *>(arg);
    ip_addr_t addr;
    const err_t err = dns_gethostbyname(job->host, &addr, dnsFound, job);
    if (err == ERR_OK) {
        job->addr = addr;
        job->status = kDnsOk;
    } else if (err != ERR_INPROGRESS) {
        job->status = kDnsFailed;
    }
}

uint32_t randomMask() { return esp_random(); }

// The code sent back in reply to a server Close (RFC 6455 §5.5.1, §7.4): the server's code
// if it is one an endpoint may send, else 1000. 1005, 1006 and 1015 must never be sent.
uint16_t closeReplyCode(uint16_t code) {
    const bool sendable = (code >= 1000 && code <= 1003) || (code >= 1007 && code <= 1014) ||
                          (code >= 3000 && code <= 4999);
    return sendable ? code : kCloseNormal;
}

// Copies at most maxLen bytes of untrusted text for a log line; control characters
// become '?' (keeps one event on one line).
void printable(const char *text, size_t maxLen, char *out, size_t outSize) {
    size_t n = 0;
    for (; text[n] != '\0' && n < maxLen && n + 1 < outSize; ++n) {
        const unsigned char c = static_cast<unsigned char>(text[n]);
        out[n] = (c < 0x20 || c == 0x7F) ? '?' : static_cast<char>(c);
    }
    out[n] = '\0';
}

}  // namespace

// ---- PSRAM message buffers -------------------------------------------------------------

uint8_t *PsramBufferProvider::grow(uint8_t *current, size_t total) {
    if (total > kGuardFromBytes) {
        // Keep PSRAM usable for everything else: only take a large block if a much larger
        // one is free (1.5 x).
        const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
        if (largest < total + total / 2) {
            return nullptr;
        }
    }
    return static_cast<uint8_t *>(heap_caps_realloc(current, total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}

void PsramBufferProvider::release(uint8_t *buffer) { heap_caps_free(buffer); }

// ---- Transport -------------------------------------------------------------------------

WsTransport::WsTransport() : rx_(&provider_) {}

void WsTransport::begin(hc::HaSession &session) { session_ = &session; }

void WsTransport::open(const hc::HaUrl &endpoint, const hc::TlsSettings &tls) {
    if (inPoll_) {
        closeRequested_ = true;
        reopenRequested_ = true;
        reopenEndpoint_ = endpoint;
        reopenTls_ = tls;
        return;
    }
    shutdown(true);
    start(endpoint, tls);
}

void WsTransport::close() {
    if (inPoll_) {
        closeRequested_ = true;
        reopenRequested_ = false;
        return;
    }
    shutdown(true);
}

bool WsTransport::sendText(const std::string &text) {
    if (phase_ != Phase::Open || closeRequested_) {
        return false;
    }
    return writeFrame(ws::encodeClientFrame(ws::Opcode::Text, reinterpret_cast<const uint8_t *>(text.data()),
                                            text.size(), randomMask()));
}

void WsTransport::poll() {
    inPoll_ = true;
    switch (phase_) {
        case Phase::Refused:
            phase_ = Phase::Idle;
            session_->onClosed("tls_not_supported", millis());
            break;
        case Phase::Resolving: pollDns(); break;
        case Phase::Connecting: connectTcp(); break;
        case Phase::Upgrading: pollUpgrade(); break;
        case Phase::Open: pollOpen(); break;
        default: break;
    }
    inPoll_ = false;

    if (closeRequested_) {
        shutdown(true);
    }
    if (reopenRequested_) {
        reopenRequested_ = false;
        start(reopenEndpoint_, reopenTls_);
    }
}

void WsTransport::start(const hc::HaUrl &endpoint, const hc::TlsSettings &tls) {
    endpoint_ = endpoint;
    tls_ = tls;
    if (endpoint_.tls) {
        // HaSession never opens an https endpoint yet (tls_not_supported); refuse it here
        // too rather than connect in plaintext to a TLS port.
        util::logPrintf("ERR HA tls endpoint refused\n");
        phase_ = Phase::Refused;  // reported on the next poll()
        return;
    }
    ++attempt_;
    util::logPrintf("HA connect attempt=%u url=%s\n", static_cast<unsigned>(attempt_),
                    hc::webSocketUrl(endpoint_).c_str());
    dnsStarted_ = false;
    phase_ = Phase::Resolving;
}

void WsTransport::pollDns() {
    if (!dnsStarted_) {
        if (dnsJob.status == kDnsPending) {
            return;  // an abandoned lookup is still running; start ours after it
        }
        strncpy(dnsJob.host, endpoint_.host.c_str(), sizeof(dnsJob.host) - 1);
        dnsJob.host[sizeof(dnsJob.host) - 1] = '\0';
        dnsJob.status = kDnsPending;
        dnsStarted_ = true;
        if (tcpip_callback(dnsStart, &dnsJob) != ERR_OK) {
            dnsJob.status = kDnsFailed;
        }
        return;
    }
    const uint8_t status = dnsJob.status;
    if (status == kDnsPending) {
        return;
    }
    dnsJob.status = kDnsIdle;
    if (status != kDnsOk || !IP_IS_V4(&dnsJob.addr)) {
        util::logPrintf("HA dns host=%s ok=0\n", endpoint_.host.c_str());
        fail("dns_failed");
        return;
    }
    char ip[IPADDR_STRLEN_MAX];
    ipaddr_ntoa_r(&dnsJob.addr, ip, sizeof(ip));
    util::logPrintf("HA dns host=%s ok=1 ip=%s\n", endpoint_.host.c_str(), ip);
    ip_ = IPAddress(ip_2_ip4(&dnsJob.addr)->addr);
    phase_ = Phase::Connecting;  // connect on the next poll(): one blocking step per loop
}

void WsTransport::connectTcp() {
    tcpStartMs_ = millis();
    const bool ok = client_.connect(ip_, endpoint_.port, kTcpConnectTimeoutMs) == 1;
    const uint32_t ms = millis() - tcpStartMs_;
    util::logPrintf("HA tcp ok=%d ms=%u\n", ok ? 1 : 0, static_cast<unsigned>(ms));
    if (!ok) {
        fail("connect_failed");
        return;
    }
    client_.setNoDelay(true);
    key_ = ws::makeWebSocketKey(randomMask);
    const std::string request = ws::buildUpgradeRequest(endpoint_.host, endpoint_.port, false, endpoint_.path, key_);
    if (client_.write(reinterpret_cast<const uint8_t *>(request.data()), request.size()) != request.size()) {
        fail("connect_failed");
        return;
    }
    upgradeBuf_.clear();
    upgradeBuf_.reserve(512);
    upgradeStartMs_ = millis();
    phase_ = Phase::Upgrading;
}

void WsTransport::pollUpgrade() {
    const int avail = client_.available();
    if (avail > 0) {
        // The header block is at most kMaxUpgradeResponseBytes; anything read after it (the
        // first frames) is passed on to the receiver.
        const size_t room = ws::kMaxUpgradeResponseBytes + sizeof(chunk_) - upgradeBuf_.size();
        size_t want = static_cast<size_t>(avail);
        if (want > sizeof(chunk_)) want = sizeof(chunk_);
        if (want > room) want = room;
        const int n = want > 0 ? client_.read(chunk_, want) : 0;
        if (n > 0) {
            upgradeBuf_.append(reinterpret_cast<const char *>(chunk_), static_cast<size_t>(n));
        }
        const ws::UpgradeResult result = ws::parseUpgradeResponse(upgradeBuf_.data(), upgradeBuf_.size(), key_);
        if (result.status == ws::UpgradeResult::Status::Failed) {
            util::logPrintf("HA handshake failed error=%s\n", result.error.c_str());
            fail("handshake_failed: " + result.error);
            return;
        }
        if (result.status == ws::UpgradeResult::Status::Ok) {
            util::logPrintf("HA socket open ms=%u\n", static_cast<unsigned>(millis() - tcpStartMs_));
            const std::string rest = upgradeBuf_.substr(result.headerBytes);
            std::string().swap(upgradeBuf_);
            rx_.reset();
            phase_ = Phase::Open;
            if (!rest.empty()) {
                handleBytes(reinterpret_cast<const uint8_t *>(rest.data()), rest.size());
            }
            return;
        }
    }
    if (avail <= 0 && peerClosed()) {
        util::logPrintf("HA handshake failed error=closed\n");
        fail("handshake_failed: closed");
        return;
    }
    if (millis() - upgradeStartMs_ >= kHandshakeTimeoutMs) {
        util::logPrintf("HA handshake timeout\n");
        fail("handshake_timeout");
    }
}

void WsTransport::pollOpen() {
    const uint32_t startUs = micros();
    size_t total = 0;
    while (phase_ == Phase::Open && !closeRequested_) {
        // Read only what is there (WiFiClientSecure::read() would drop the connection on
        // WANT_READ; plain WiFiClient is read the same way).
        const int avail = client_.available();
        if (avail <= 0) {
            if (total == 0 && peerClosed()) {
                util::logPrintf("HA socket closed by=peer\n");
                fail("connection_lost");
            }
            return;
        }
        size_t want = static_cast<size_t>(avail);
        if (want > sizeof(chunk_)) want = sizeof(chunk_);
        if (want > kPollMaxBytes - total) want = kPollMaxBytes - total;
        const int n = client_.read(chunk_, want);
        if (n <= 0) {
            return;  // a failed read stops the client; seen as closed on the next poll
        }
        total += static_cast<size_t>(n);
        if (!handleBytes(chunk_, static_cast<size_t>(n))) {
            return;
        }
        if (total >= kPollMaxBytes || micros() - startUs >= kPollMaxUs) {
            return;
        }
    }
}

bool WsTransport::handleBytes(const uint8_t *data, size_t len) {
    size_t used = 0;
    while (used < len) {
        if (rx_.messageBytesSoFar() == 0) {
            rxStartMs_ = millis();  // the next message's header is in this chunk
        }
        ws::Event event;
        used += rx_.feed(data + used, len - used, event);
        if (event.type == ws::EventType::None) {
            continue;
        }
        if (!handleEvent(event) || closeRequested_ || phase_ != Phase::Open) {
            return false;
        }
    }
    return true;
}

bool WsTransport::handleEvent(const ws::Event &event) {
    switch (event.type) {
        case ws::EventType::Text: {
            const uint32_t now = millis();
            session_->onText(reinterpret_cast<const char *>(event.data), event.length, now);
            // The session keeps no pointer into the message (parseMessage copies what it
            // needs); free the PSRAM now rather than on the next received byte.
            rx_.releaseDelivered();
            if (event.length >= kLogRxBytes) {
                util::logPrintf("HA rx len=%u ms=%u handle_ms=%u\n", static_cast<unsigned>(event.length),
                                static_cast<unsigned>(now - rxStartMs_), static_cast<unsigned>(millis() - now));
            }
            return true;
        }
        case ws::EventType::Ping:
            if (!writeFrame(ws::encodeClientFrame(ws::Opcode::Pong, event.data, event.length, randomMask()))) {
                fail("send_failed");
                return false;
            }
            return true;
        case ws::EventType::Pong:
            return true;  // the device sends no WebSocket pings (liveness = HA ping/pong)
        case ws::EventType::Dropped: {
            const uint32_t now = millis();
            util::logPrintf("HA frame dropped len=%u ms=%u reason=%s\n", static_cast<unsigned>(event.length),
                            static_cast<unsigned>(now - rxStartMs_), event.reason);
            session_->onMessageDropped(event.length, event.reason, now);
            return true;
        }
        case ws::EventType::Close: {
            char reason[kMaxLoggedReason + 1];
            printable(event.reason, kMaxLoggedReason, reason, sizeof(reason));
            util::logPrintf("HA socket closed by=server code=%u reason=%s\n", static_cast<unsigned>(event.closeCode),
                            reason);
            if (event.length == 0) {
                writeFrame(ws::encodeClientFrame(ws::Opcode::Close, nullptr, 0, randomMask()));
            } else {
                writeFrame(ws::encodeClose(closeReplyCode(event.closeCode), randomMask()));
            }
            fail("closed_by_server");
            return false;
        }
        case ws::EventType::ProtocolError:
            util::logPrintf("HA protocol error reason=%s\n", event.reason);
            writeFrame(ws::encodeClose(kCloseProtocolError, randomMask()));
            fail("protocol_error");
            return false;
        default:
            return true;
    }
}

// True if the TCP connection is gone. Only called when nothing is waiting to be read.
// WiFiClient::connected() reads a stale errno after a zero-length recv(), so the socket is
// asked directly: a peeked recv() of 0 bytes means the peer closed (FIN).
bool WsTransport::peerClosed() {
    const int fd = client_.fd();
    if (fd < 0) {
        return true;
    }
    uint8_t byte;
    const int res = recv(fd, &byte, 1, MSG_PEEK | MSG_DONTWAIT);
    if (res > 0) {
        return false;
    }
    if (res == 0) {
        return true;
    }
    return errno != EWOULDBLOCK && errno != EAGAIN;
}

bool WsTransport::writeFrame(const std::vector<uint8_t> &frame) {
    return client_.write(frame.data(), frame.size()) == frame.size();
}

void WsTransport::fail(const char *reason) { fail(std::string(reason)); }

void WsTransport::fail(const std::string &reason) {
    shutdown(false);
    session_->onClosed(reason, millis());
}

void WsTransport::shutdown(bool goodbye) {
    if (goodbye && phase_ == Phase::Open && client_.fd() >= 0) {
        writeFrame(ws::encodeClose(kCloseNormal, randomMask()));  // best effort
    }
    closeRequested_ = false;
    client_.stop();
    rx_.reset();
    std::string().swap(upgradeBuf_);
    phase_ = Phase::Idle;
}

}  // namespace ha
