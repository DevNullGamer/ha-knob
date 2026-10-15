#include "ha/HaClient.h"

#include <Arduino.h>
#include <WebSocketsClient.h>
#include <lwip/dns.h>
#include <lwip/ip_addr.h>
#include <lwip/tcpip.h>

#include <atomic>
#include <climits>
#include <cstring>
#include <vector>

#include "hal/PsramAllocator.h"
#include "util/Log.h"

namespace ha {

namespace {

namespace hc = core::ha;

// The library may only connect when asked: interval 0 for the first loop() after begin(),
// then "never" (millis() - lastFail < ULONG_MAX always holds).
constexpr unsigned long kConnectNow = 0;
constexpr unsigned long kNeverReconnect = ULONG_MAX;
constexpr size_t kMaxLoggedReason = 40;

// ---- Asynchronous name resolution ------------------------------------------------------
// One lookup at a time, run by lwIP in the tcpip task (dns_gethostbyname must not be
// called from another task). The result lands in lwIP's DNS cache, so the WebSocket
// library's own (blocking) lookup right afterwards returns at once.

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

// ---- Transport -------------------------------------------------------------------------

// core::ha::Transport on links2004 WebSocketsClient. The session may call close() from
// inside a library event (e.g. on auth_invalid); the library must not be torn down while
// its loop() is still running, so close() is deferred until loop() has returned.
class WsTransport : public hc::Transport {
public:
    void begin(hc::HaSession &session) {
        session_ = &session;
        ws_.setExtraHeaders(nullptr);  // no "Origin: file://" (the library default)
        ws_.onEvent([this](WStype_t type, uint8_t *payload, size_t length) { onEvent(type, payload, length); });
    }

    void open(const hc::HaUrl &endpoint) override {
        shutdown();
        if (endpoint.tls) {
            // HaSession never opens an https endpoint (tls_not_supported); refuse it here
            // too rather than connect in plaintext to a TLS port.
            util::logPrintf("ERR HA tls endpoint refused\n");
            return;
        }
        endpoint_ = endpoint;
        ++attempt_;
        util::logPrintf("HA connect attempt=%u url=%s\n", static_cast<unsigned>(attempt_),
                        hc::webSocketUrl(endpoint_).c_str());
        socketOpen_ = false;
        dnsStarted_ = false;
        phase_ = Phase::Resolving;
    }

    void close() override {
        if (inLoop_) {
            closeRequested_ = true;
            return;
        }
        shutdown();
    }

    bool sendText(const std::string &text) override {
        if (phase_ != Phase::Socket || closeRequested_ || !ws_.isConnected()) {
            return false;
        }
        return ws_.sendTXT(text.c_str(), text.size());
    }

    void poll() {
        if (phase_ == Phase::Resolving) {
            pollDns();
            return;
        }
        if (phase_ != Phase::Socket) {
            return;
        }
        inLoop_ = true;
        ws_.loop();  // connects on the first call after begin(); may block (see HaClient.h)
        inLoop_ = false;
        if (connectRequested_) {
            connectRequested_ = false;
            ws_.setReconnectInterval(kNeverReconnect);
        }
        if (closeRequested_) {
            shutdown();
        }
    }

private:
    enum class Phase : uint8_t { Idle, Resolving, Socket };

    void pollDns() {
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
        if (status != kDnsOk) {
            util::logPrintf("HA dns host=%s ok=0\n", endpoint_.host.c_str());
            phase_ = Phase::Idle;
            session_->onClosed("dns_failed", millis());
            return;
        }
        char ip[IPADDR_STRLEN_MAX];
        ipaddr_ntoa_r(&dnsJob.addr, ip, sizeof(ip));
        util::logPrintf("HA dns host=%s ok=1 ip=%s\n", endpoint_.host.c_str(), ip);

        // "" protocol: no Sec-WebSocket-Protocol header (HA does not use one).
        ws_.begin(endpoint_.host.c_str(), endpoint_.port, endpoint_.path.c_str(), "");
        ws_.setReconnectInterval(kConnectNow);
        connectRequested_ = true;
        phase_ = Phase::Socket;
    }

    // Closes the library socket (if any) without reporting anything to the session.
    void shutdown() {
        closeRequested_ = false;
        if (phase_ == Phase::Socket) {
            suppressEvents_ = true;
            ws_.disconnect();  // also frees a socket left by a failed connect
            ws_.setReconnectInterval(kNeverReconnect);
            suppressEvents_ = false;
        }
        connectRequested_ = false;
        phase_ = Phase::Idle;
    }

    void onEvent(WStype_t type, uint8_t *payload, size_t length) {
        if (suppressEvents_ || closeRequested_ || phase_ != Phase::Socket) {
            return;
        }
        switch (type) {
            case WStype_CONNECTED:
                socketOpen_ = true;
                util::logPrintf("HA socket open\n");
                break;
            case WStype_TEXT:
                session_->onText(reinterpret_cast<const char *>(payload), length, millis());
                break;
            case WStype_DISCONNECTED: {
                const int shown = static_cast<int>(length < kMaxLoggedReason ? length : kMaxLoggedReason);
                util::logPrintf("HA socket closed open=%d reason=%.*s\n", socketOpen_ ? 1 : 0, shown,
                                payload != nullptr ? reinterpret_cast<const char *>(payload) : "");
                phase_ = Phase::Idle;  // the library has already closed and freed the socket
                session_->onClosed(socketOpen_ ? "connection_lost" : "connect_failed", millis());
                break;
            }
            case WStype_ERROR:
                util::logPrintf("HA socket error len=%u\n", static_cast<unsigned>(length));
                break;
            case WStype_BIN:
            case WStype_FRAGMENT_TEXT_START:
            case WStype_FRAGMENT_BIN_START:
            case WStype_FRAGMENT:
            case WStype_FRAGMENT_FIN:
                // HA sends every message as one text frame; nothing else is expected.
                util::logPrintf("HA frame ignored type=%d len=%u\n", static_cast<int>(type),
                                static_cast<unsigned>(length));
                break;
            default:
                break;  // WebSocket-level ping / pong
        }
    }

    WebSocketsClient ws_;
    hc::HaSession *session_ = nullptr;
    hc::HaUrl endpoint_;
    Phase phase_ = Phase::Idle;
    uint32_t attempt_ = 0;
    bool dnsStarted_ = false;
    bool connectRequested_ = false;
    bool socketOpen_ = false;  // handshake done in the current attempt
    bool inLoop_ = false;
    bool closeRequested_ = false;
    bool suppressEvents_ = false;
};

WsTransport transport;
hc::HaSession session(transport, PsramAllocator::instance());
Status status_;
uint32_t shownChange = UINT32_MAX;
std::string configuredUrl;
std::string configuredToken;
bool configuredOnce = false;

// Mirrors the session into status_ and logs every change.
void refresh() {
    if (session.changeCount() == shownChange) {
        return;
    }
    shownChange = session.changeCount();
    status_.state = session.state();
    status_.haVersion = session.haVersion();
    status_.lastError = session.lastError();

    char extra[48] = "";
    if (status_.state == hc::State::Backoff && session.networkUp()) {
        const int32_t waitMs = static_cast<int32_t>(session.nextAttemptMs() - millis());
        snprintf(extra, sizeof(extra), " retry_in_ms=%d", static_cast<int>(waitMs > 0 ? waitMs : 0));
    }
    util::logPrintf("HA state=%s%s%s%s%s%s\n", hc::stateName(status_.state),
                    status_.haVersion.empty() ? "" : " ha_version=", status_.haVersion.c_str(),
                    status_.lastError.empty() ? "" : " error=", status_.lastError.c_str(), extra);
}

}  // namespace

void begin() {
    transport.begin(session);
}

void configure(const std::string &url, const std::string &token, uint32_t nowMs) {
    if (!configuredOnce || url != configuredUrl || token != configuredToken) {
        configuredOnce = true;
        configuredUrl = url;
        configuredToken = token;
        if (url.empty() || token.empty()) {
            util::logPrintf("HA config none url_set=%d token_set=%d\n", url.empty() ? 0 : 1,
                            token.empty() ? 0 : 1);
        } else {
            util::logPrintf("HA config url=%s token_set len=%u\n", url.c_str(),
                            static_cast<unsigned>(token.size()));
        }
    }
    // Step 5 subscribes to nothing; Step 6 passes the configured controls' entity ids.
    session.configure(url, token, std::vector<std::string>(), nowMs);
    refresh();
}

void setNetworkUp(bool up, uint32_t nowMs) {
    session.setNetworkUp(up, nowMs);
    refresh();
}

void retry(uint32_t nowMs) {
    const hc::State state = session.state();
    if (state == hc::State::AuthFailed || state == hc::State::Backoff) {
        util::logPrintf("HA retry\n");  // HaSession::retry() acts in exactly these states
    }
    session.retry(nowMs);
    refresh();
}

void loop() {
    transport.poll();
    session.tick(millis());  // taken after poll(), which can block during a TCP connect
    refresh();
}

const Status &status() { return status_; }

}  // namespace ha
