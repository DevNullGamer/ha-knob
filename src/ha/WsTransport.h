#pragma once

#include <IPAddress.h>
#include <WiFiClient.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "ha/HaSession.h"
#include "ws/WsReceiver.h"

// core::ha::Transport on Arduino WiFiClient: the project's own WebSocket client (RFC 6455,
// client side; codec in lib/core/src/ws, plan-step5b.md §3.3). Main loop only. ws:// only
// for now; an https endpoint is refused ("tls_not_supported", HaSession never opens one).
//
// Phases of one attempt:
//   Resolving   asynchronous lwIP DNS in the tcpip task (incl. mDNS for ".local"), so a
//               name that does not resolve never blocks the main loop
//   Connecting  WiFiClient::connect(ip, port, 2000 ms): blocks the main loop for up to
//               2 s when the host does not answer (the timeout argument is in ms on core
//               2.0.17); then the upgrade request is written
//   Upgrading   non-blocking: the response is read as it arrives, at most 10 s
//               (handshake_timeout); bytes after the header block go to the receiver
//   Open        frames are read only when available() > 0, at most 32 KB or 8 ms per
//               poll(), and fed to core::ws::WsReceiver
//
// Open connection:
//  - Text messages go to HaSession::onText. Message memory comes from PSRAM (realloc as the
//    message grows, freed as soon as onText returns); above 64 KB the largest free PSRAM block must be >= 1.5 x the size.
//    Messages up to 4 MB are delivered; larger ones, binary ones, or ones PSRAM cannot hold
//    are skipped without buffering and reported (HaSession::onMessageDropped); the
//    connection stays up. A stalled transfer is ended by the session's timers (connect /
//    auth timeouts, HA ping every 30 s with a 10 s pong timeout).
//  - Server PING -> PONG with the same payload (HA's heartbeat needs it). Server Close ->
//    Close reply (the server's code, 1000 instead of 1005/1006/1015 or an invalid code,
//    empty if the server sent none), then closed_by_server. Protocol error -> Close 1002,
//    protocol_error. TCP closed / reset -> connection_lost.
//  - sendText(): one masked frame (mask from esp_random); a short write returns false
//    (the session then fails with send_failed). WiFiClient::write() can block up to ~10 s
//    on a full send buffer; only small frames are sent, and the pong timeout closes a dead
//    link before that matters.
//  - close() from the session sends Close 1000 (best effort) when the connection is open.
//    Calls made by the session from inside poll() (close(), open()) are carried out when
//    poll() returns, so a received message's buffer is never freed while onText runs.
//
// Errors reported through HaSession::onClosed: dns_failed, connect_failed,
// handshake_failed: <http NNN|bad_accept|bad_response|closed>, handshake_timeout,
// closed_by_server, protocol_error, connection_lost, send_failed, tls_not_supported.
//
// Serial lines:
//   HA connect attempt=<n> url=ws://<host>:<port><path>
//   HA dns host=<host> ok=<0|1>[ ip=<ip>]
//   HA tcp ok=<0|1> ms=<n>                         TCP connect (blocking) result
//   HA socket open ms=<n>                          101 received; ms since the TCP connect
//   HA handshake failed error=<e>  |  HA handshake timeout
//   HA rx len=<n> ms=<n> handle_ms=<n>             messages >= 64 KB (receive / onText time)
//   HA frame dropped len=<n> ms=<n> reason=<too_large|no_memory|binary>
//   HA socket closed by=server code=<n> reason=<text, max 40 chars>
//   HA socket closed by=peer                       TCP closed or reset while open
//   HA protocol error reason=<r>
//   ERR HA tls endpoint refused
namespace ha {

// core::ws::BufferProvider in PSRAM with a fragmentation guard (see above).
class PsramBufferProvider : public core::ws::BufferProvider {
public:
    static constexpr size_t kGuardFromBytes = 64u * 1024u;

    uint8_t *grow(uint8_t *current, size_t total) override;
    void release(uint8_t *buffer) override;
};

class WsTransport : public core::ha::Transport {
public:
    static constexpr int32_t kTcpConnectTimeoutMs = 2000;
    static constexpr uint32_t kHandshakeTimeoutMs = 10000;
    static constexpr size_t kPollMaxBytes = 32u * 1024u;
    static constexpr uint32_t kPollMaxUs = 8000;
    static constexpr size_t kLogRxBytes = 64u * 1024u;  // log "HA rx" from this size

    WsTransport();

    void begin(core::ha::HaSession &session);

    void open(const core::ha::HaUrl &endpoint, const core::ha::TlsSettings &tls) override;
    void close() override;
    bool sendText(const std::string &text) override;

    // Drives the attempt / connection. Call from the main loop; may block during the TCP
    // connect (see above).
    void poll();

private:
    enum class Phase : uint8_t { Idle, Refused, Resolving, Connecting, Upgrading, Open };

    void start(const core::ha::HaUrl &endpoint, const core::ha::TlsSettings &tls);
    void pollDns();
    void connectTcp();
    void pollUpgrade();
    void pollOpen();
    // Feeds received bytes to the receiver and handles its events. False once the
    // connection ended or a close was requested.
    bool handleBytes(const uint8_t *data, size_t len);
    bool handleEvent(const core::ws::Event &event);
    bool peerClosed();
    bool writeFrame(const std::vector<uint8_t> &frame);
    // Closes the socket and reports reason to the session.
    void fail(const char *reason);
    void fail(const std::string &reason);
    // Closes the socket without reporting anything; goodbye sends Close 1000 first if open.
    void shutdown(bool goodbye);

    core::ha::HaSession *session_ = nullptr;
    PsramBufferProvider provider_;
    core::ws::WsReceiver rx_;
    WiFiClient client_;

    core::ha::HaUrl endpoint_;
    core::ha::TlsSettings tls_;  // kept for wss (not connected yet)
    Phase phase_ = Phase::Idle;
    uint32_t attempt_ = 0;
    bool dnsStarted_ = false;
    IPAddress ip_;
    std::string key_;          // Sec-WebSocket-Key of the current attempt
    std::string upgradeBuf_;   // response bytes while Upgrading
    uint32_t tcpStartMs_ = 0;
    uint32_t upgradeStartMs_ = 0;
    uint32_t rxStartMs_ = 0;   // when the message in progress started arriving

    // Calls from the session inside poll() are deferred until poll() returns.
    bool inPoll_ = false;
    bool closeRequested_ = false;
    bool reopenRequested_ = false;
    core::ha::HaUrl reopenEndpoint_;
    core::ha::TlsSettings reopenTls_;

    uint8_t chunk_[2048];      // socket read buffer
};

}  // namespace ha
