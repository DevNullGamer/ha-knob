#pragma once

#include <IPAddress.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "ha/HaSession.h"
#include "ws/WsReceiver.h"

// core::ha::Transport on Arduino WiFiClient / WiFiClientSecure: the project's own WebSocket
// client (RFC 6455, client side; codec in lib/core/src/ws, plan-step5b.md §3.3). Main loop
// only. http endpoints use ws, https endpoints wss (TLS 1.2, mbedTLS 2.28).
//
// Phases of one attempt:
//   Resolving   asynchronous lwIP DNS in the tcpip task (incl. mDNS for ".local"), so a
//               name that does not resolve never blocks the main loop
//   Connecting  ws:  WiFiClient::connect(ip, port, 2000 ms): blocks the main loop for up to
//               2 s when the host does not answer (the timeout argument is in ms on core
//               2.0.17)
//               wss: WiFiClientSecure::connect(ip, port, host, ...): TCP connect (2 s) plus
//               TLS handshake (4 s), so up to about 6 s; the host name goes into SNI and
//               the certificate name check. Then the upgrade request is written.
//   Upgrading   non-blocking: the response is read as it arrives, at most 10 s
//               (handshake_timeout); bytes after the header block go to the receiver
//   Open        frames are read only when available() > 0, at most 32 KB or 8 ms per
//               poll(), and fed to core::ws::WsReceiver
//
// TLS (wss): exactly one mode per attempt (core::ha::TlsMode, chosen from the TlsSettings),
// never a fallback to another mode or to plaintext. The one long-lived WiFiClientSecure is
// reset before every attempt (CA, bundle and insecure flags cleared, then the mode set):
//   bundle     verify against the IDF certificate bundle linked into the firmware
//              (_binary_x509_crt_bundle_start, about 64 KB of flash)
//   custom_ca  verify against the stored CA PEM only (parsed with mbedTLS first: a CA it
//              cannot parse fails with tls_ca_invalid before any connection is made)
//   insecure   no verification (logs "WARN HA tls verify=off" on every connect)
// mbedTLS 2.28 limits: TLS 1.2 only; certificate dates are never checked
// (MBEDTLS_HAVE_TIME_DATE is off); the host name is matched against dNSName SANs only
// (no IP SANs), or the CN when the certificate has no SAN. The token is only sent after
// the TLS handshake (verified per mode) and the 101 upgrade.
// Core log_e lines from ssl_client ("[E][ssl_client.cpp...]") accompany TLS failures (one
// to three per failed attempt, rate-limited by the session's backoff).
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
// closed_by_server, protocol_error, connection_lost, send_failed, and for wss:
//   tls_cert_rejected     certificate verification failed (-0x2700, or not issued by a
//                         CA in the bundle)
//   tls_ca_invalid        the stored CA PEM cannot be parsed by mbedTLS
//   tls_handshake_failed  any other TLS error (code and message on the serial line)
//   tls_timeout           TLS handshake not finished within 4 s
//   connect_failed        TCP connect refused or timed out (as for ws)
//
// Serial lines:
//   HA connect attempt=<n> url=<ws|wss>://<host>:<port><path>
//   HA dns host=<host> ok=<0|1>[ ip=<ip>]
//   HA tcp ok=<0|1> ms=<n>                         ws: TCP connect (blocking) result
//   HA tls mode=<bundle|custom_ca|insecure> host=<host>   wss: before the TLS connect
//   WARN HA tls verify=off                         wss insecure, every connect
//   HA tls ok ms=<n> heap=<internal free bytes>    wss: TCP + TLS handshake done
//   HA tls failed error=<e> code=-0x<hex> ms=<n> msg=<mbedTLS text, max 60 chars>
//   HA socket open ms=<n>                          101 received; ms since the TCP connect
//   HA handshake failed error=<e>  |  HA handshake timeout
//   HA rx len=<n> ms=<n> handle_ms=<n>             messages >= 64 KB (receive / onText time)
//   HA frame dropped len=<n> ms=<n> reason=<too_large|no_memory|binary>
//   HA socket closed by=server code=<n> reason=<text, max 40 chars>
//   HA socket closed by=peer                       TCP closed or reset while open
//   HA protocol error reason=<r>
namespace ha {

// core::ws::BufferProvider in PSRAM with a fragmentation guard (see above).
class PsramBufferProvider : public core::ws::BufferProvider {
public:
    static constexpr size_t kGuardFromBytes = 64u * 1024u;

    uint8_t *grow(uint8_t *current, size_t total) override;
    void release(uint8_t *buffer) override;
};

// WiFiClientSecure with a fixed stop(). In core 2.0.17 stop() closes the socket, sets it to
// -1 and then stop_ssl_socket() zeroes the whole context, so the socket number is left at
// 0: fd() then reports 0, setTimeout() tries socket options on fd 0, and the next stop()
// calls close(0), which closes whatever VFS file descriptor 0 is at that moment (stdin,
// or later e.g. a LittleFS file of the web server). The core calls stop() itself after a
// failed connect, read, write or available(); all of these dispatch here (Client::stop()
// is virtual).
class SecureClient : public WiFiClientSecure {
public:
    void stop() override;
};

class WsTransport : public core::ha::Transport {
public:
    static constexpr int32_t kTcpConnectTimeoutMs = 2000;
    static constexpr uint32_t kTlsSocketTimeoutS = 2;     // TCP connect and socket I/O (wss)
    static constexpr uint32_t kTlsHandshakeTimeoutS = 4;  // TLS handshake (wss)
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
    // connect and the TLS handshake (see above).
    void poll();

private:
    enum class Phase : uint8_t { Idle, Resolving, Connecting, Upgrading, Open };

    void start(const core::ha::HaUrl &endpoint, const core::ha::TlsSettings &tls);
    void pollDns();
    void connectSocket();
    // Blocking connects; false (and fail() called) if the connection was not made.
    bool connectTcp();
    bool connectTls();
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
    WiFiClient plain_;          // ws
    SecureClient secure_;       // wss; one instance, reset before every attempt
    WiFiClient *client_ = &plain_;  // the one in use for the current attempt

    core::ha::HaUrl endpoint_;
    core::ha::TlsSettings tls_;
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
