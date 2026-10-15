#pragma once

#include <ArduinoJson.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "ha/HaUrl.h"

// Connection state machine for the Home Assistant WebSocket API (plan.md §3.7). Pure and
// clock-injected: it talks to the socket through the Transport interface and is driven by
// the configure / network / transport calls below plus tick(). src/ha/WsTransport (the
// project's own WebSocket client) implements Transport on the device; native tests use a
// fake transport.
//
//  NoConfig ---- URL and token configured ----------------------------> Connecting
//  Connecting -- auth_required received, auth sent --------------------> Authenticating
//  Authenticating -- auth_ok (ha_version kept) ------------------------> Subscribing,
//                    or Ready at once when there are no entity ids to subscribe
//  Authenticating -- auth_invalid -------------------------------------> AuthFailed
//  Subscribing -- result of subscribe_entities ------------------------> Ready
//                 (a failed result is kept in lastError; still Ready)
//  Connecting..Ready -- socket closed, connect failed, network lost, timeout, no pong,
//                       send failed ---------------------------------> Backoff
//  Backoff -- wait over and the network up ----------------------------> Connecting
//
//  - Backoff waits 1, 2, 4, 8, 16, 30, 30 ... s (consecutive failures); reaching Ready, a
//    new URL/token or retry() starts the sequence again. With the network down the
//    session waits in Backoff and connects as soon as the network is up and the wait over.
//  - AuthFailed is terminal: no automatic reconnect (avoids HA's ip_ban), not even after a
//    network change. Only configure() with a different URL or token, or retry(), leave it.
//  - Timeouts: no auth_required within 20 s of open(), or no auth answer within 10 s,
//    means Backoff. Authenticated: a ping every 30 s; no pong within 10 s means Backoff.
//  - Message ids are monotonic per connection and start at 1 on every connection.
//    Requests waiting for a result are kept in a pending map (at most kMaxPending,
//    expired after 15 s; an expired subscribe means Backoff).
//  - The token is kept only to build the auth message; it is never part of any status.
namespace core {
namespace ha {

enum class State : uint8_t { NoConfig, Connecting, Authenticating, Subscribing, Ready, AuthFailed, Backoff };

// "no_config", "connecting", "authenticating", "subscribing", "ready", "auth_failed",
// "backoff" (as in /api/status and the serial log).
const char *stateName(State state);

// How an https (wss) endpoint is to be verified; passed to Transport::open(). Not used for
// http endpoints. Defaults: verify against the built-in public CA bundle.
//  - insecure: no certificate verification at all
//  - caPem:    verify against this CA certificate (PEM) instead of the bundle
// TLS is not connected yet (HaSession refuses https with "tls_not_supported"); the settings
// are only plumbed through.
struct TlsSettings {
    bool insecure = false;
    std::string caPem;

    bool operator==(const TlsSettings &o) const { return insecure == o.insecure && caPem == o.caPem; }
    bool operator!=(const TlsSettings &o) const { return !(*this == o); }
};

// The WebSocket as the session sees it. All calls are made from within the session's
// methods.
class Transport {
public:
    virtual ~Transport() = default;
    // Starts connecting (name resolution, TCP, WebSocket handshake). Once the socket is up
    // HA sends auth_required (HaSession::onText); a failure is reported through
    // HaSession::onClosed(). tls applies to https endpoints only.
    virtual void open(const HaUrl &endpoint, const TlsSettings &tls) = 0;
    // Closes the connection or abandons the attempt. Nothing is reported for it afterwards.
    virtual void close() = 0;
    // Sends one text frame. False if it could not be sent. May report the close through
    // HaSession::onClosed() before returning.
    virtual bool sendText(const std::string &text) = 0;
};

// What a pending request was for (results are matched by id).
enum class Purpose : uint8_t { Subscribe, Unsubscribe, GetStates, CallService };

class HaSession {
public:
    static constexpr uint32_t kConnectTimeoutMs = 20000;  // open() -> auth_required
    static constexpr uint32_t kAuthTimeoutMs = 10000;     // auth sent -> auth_ok / auth_invalid
    static constexpr uint32_t kPingIntervalMs = 30000;
    static constexpr uint32_t kPongTimeoutMs = 10000;
    static constexpr uint32_t kRequestTimeoutMs = 15000;  // pending map expiry
    static constexpr size_t kMaxPending = 16;
    static constexpr size_t kMaxErrorLen = 120;           // lastError is cut to this length

    // Backoff delay before the attempt after `failures` consecutive failures (>= 1):
    // 1000, 2000, 4000, 8000, 16000, then 30000 ms.
    static uint32_t backoffDelayMs(uint32_t failures);

    explicit HaSession(Transport &transport, ArduinoJson::Allocator *allocator = nullptr);

    // Applies the configuration. A different URL, token or TLS setting (or the first call)
    // restarts:
    // the connection is closed and, with both set and a usable URL, a new one is started
    // (Connecting, or Backoff while the network is down). Empty URL or token: NoConfig.
    // An invalid URL (or one with a query/fragment): NoConfig with lastError
    // "invalid_url". An https URL: NoConfig with lastError "tls_not_supported" (TLS is not
    // implemented; the token is never sent unencrypted to it). Transport::open() is never
    // called in NoConfig. Unchanged URL, token and TLS settings keep the connection (also
    // AuthFailed); entity ids are de-duplicated and used from the next subscribe on.
    void configure(const std::string &url, const std::string &token, const TlsSettings &tls,
                   const std::vector<std::string> &entityIds, uint32_t nowMs);

    // Station link state. Losing it closes an active connection (Backoff).
    void setNetworkUp(bool up, uint32_t nowMs);

    // "Retry" from the web UI: leaves AuthFailed, or ends a Backoff wait early, and starts
    // a new connection series (backoff from 1 s). Returns false (no-op) in other states.
    bool retry(uint32_t nowMs);

    // Timers: backoff, connect / auth timeouts, ping and pong, pending expiry. Call often.
    void tick(uint32_t nowMs);

    // Transport events (ignored unless the session has a connection open).
    void onClosed(const std::string &reason, uint32_t nowMs);
    void onText(const char *data, size_t length, uint32_t nowMs);
    // The transport discarded a received message (too large, no memory, binary): length =
    // payload bytes seen, reason e.g. "too_large". Counted only while a connection is open;
    // the connection stays up. A request whose result was dropped expires from the pending
    // map as usual.
    void onMessageDropped(size_t length, const char *reason, uint32_t nowMs);

    State state() const { return state_; }
    // From auth_ok of the current connection ("" until then; cleared by every new attempt).
    const std::string &haVersion() const { return haVersion_; }
    // Why the session is not Ready (e.g. "connect_failed", "pong_timeout",
    // "auth_invalid: <HA message>"); cleared on Ready, retry() and a config change.
    const std::string &lastError() const { return lastError_; }
    const HaUrl &endpoint() const { return endpoint_; }
    const TlsSettings &tlsSettings() const { return tls_; }
    bool networkUp() const { return networkUp_; }
    uint32_t attempts() const { return attempts_; }      // connections opened so far
    uint32_t failures() const { return failures_; }      // consecutive failed connections
    uint32_t nextAttemptMs() const { return nextAttemptMs_; }  // Backoff: when the wait ends
    size_t pendingCount() const { return pending_.size(); }
    uint32_t droppedMessages() const { return droppedMessages_; }  // onMessageDropped() counted
    // Incremented on every change of state(), haVersion() or lastError().
    uint32_t changeCount() const { return changeCount_; }

private:
    struct Pending {
        uint32_t id;
        Purpose purpose;
        int32_t tag;
        uint32_t sentAtMs;
    };

    bool configured() const { return endpoint_.ok && !token_.empty(); }
    void setState(State state);
    void setError(const std::string &error);
    void setVersion(const std::string &version);
    void connect(uint32_t nowMs);
    void fail(const std::string &reason, uint32_t nowMs);
    void closeTransport();
    bool send(const std::string &text, uint32_t nowMs);
    bool track(uint32_t id, Purpose purpose, int32_t tag, uint32_t nowMs);
    void becomeReady();
    void onAuthenticated(uint32_t nowMs);
    void handleResult(uint32_t id, bool success, const std::string &message);
    bool active() const;

    Transport &transport_;
    ArduinoJson::Allocator *allocator_;

    std::string url_;
    std::string token_;
    TlsSettings tls_;
    HaUrl endpoint_;
    std::vector<std::string> entityIds_;

    State state_ = State::NoConfig;
    std::string haVersion_;
    std::string lastError_;
    bool networkUp_ = false;
    bool transportOpen_ = false;  // open() called and not closed since
    bool firstConfigure_ = true;

    uint32_t attempts_ = 0;
    uint32_t failures_ = 0;
    uint32_t nextAttemptMs_ = 0;
    uint32_t phaseStartMs_ = 0;  // open() (Connecting) / auth sent (Authenticating)
    uint32_t nextId_ = 1;
    uint32_t lastPingMs_ = 0;
    bool pingOutstanding_ = false;
    uint32_t pingId_ = 0;
    uint32_t subscriptionId_ = 0;  // id of the active subscribe_entities (0 = none)
    std::vector<Pending> pending_;
    uint32_t changeCount_ = 0;
    uint32_t droppedMessages_ = 0;
};

}  // namespace ha
}  // namespace core
