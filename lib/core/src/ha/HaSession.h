#pragma once

#include <ArduinoJson.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "ha/EntityCatalog.h"
#include "ha/EntityStore.h"
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
//    Received bytes (onRxProgress) prove the link is alive: the pong timeout counts from
//    the later of the ping and the last received byte, so a large message that is still
//    arriving causes no false pong_timeout (plan-step6.md §3.2); 90 s after the ping
//    without a pong it fails regardless.
//  - Message ids are monotonic per connection and start at 1 on every connection.
//    Requests waiting for a result are kept in a pending map (at most kMaxPending,
//    expired after 15 s; an expired subscribe means Backoff).
//  - The token is kept only to build the auth message; it is never part of any status.
//
// Subscription (plan-step6.md §3.3): the de-duplicated entity ids are subscribed with
// subscribe_entities after auth_ok. A changed id set while Subscribing / Ready unsubscribes
// the current subscription (active or still pending), clears the store and subscribes the
// new ids (if any) on the same connection: Ready stays Ready, Subscribing with no ids left
// becomes Ready. Only events carrying the active or the pending subscribe id are applied
// to entities(); others are stale and ignored. The store is cleared on every new
// connection, on an id change and on a URL / token / TLS change; it keeps the last values
// while the session waits in Backoff.
//
// Entity list (plan-step6.md §3.1): requestStates() (Ready only) sends get_states. Its
// result is recognised with peekEnvelope() and taken over from the transport without copy
// or parse (Transport::retainText); tick() then builds the catalog in slices of
// Options::statesBytesPerTick bytes. States: None -> Fetching -> Building -> Ready (the
// catalog is handed over once by takeCatalog()) or Failed (statesError()). A build in
// progress finishes even if the connection drops; a URL / token / TLS change aborts it and
// drops the catalog (possibly another HA instance).
namespace core {
namespace ha {

struct Message;

enum class State : uint8_t { NoConfig, Connecting, Authenticating, Subscribing, Ready, AuthFailed, Backoff };

// "no_config", "connecting", "authenticating", "subscribing", "ready", "auth_failed",
// "backoff" (as in /api/status and the serial log).
const char *stateName(State state);

// How an https (wss) endpoint is to be verified; passed to Transport::open(). Not used for
// http endpoints. Defaults: verify against the built-in public CA bundle.
//  - insecure: no certificate verification at all
//  - caPem:    verify against this CA certificate (PEM) instead of the bundle
struct TlsSettings {
    bool insecure = false;
    std::string caPem;

    bool operator==(const TlsSettings &o) const { return insecure == o.insecure && caPem == o.caPem; }
    bool operator!=(const TlsSettings &o) const { return !(*this == o); }
};

// The one TLS mode a connection uses (never a fallback to another one):
//  None      http endpoint (plain ws)
//  Bundle    https, verified against the built-in public CA bundle (no caPem, not insecure)
//  CustomCa  https, verified against caPem only
//  Insecure  https, no certificate verification
// Config validation never stores insecure together with a CA; if both are set anyway the
// verifying mode (CustomCa) wins.
enum class TlsMode : uint8_t { None, Bundle, CustomCa, Insecure };

TlsMode tlsModeFor(const HaUrl &endpoint, const TlsSettings &tls);

// "none", "bundle", "custom_ca", "insecure" (as in /api/status ha.tls and the serial log).
const char *tlsModeName(TlsMode mode);

// A received text message whose memory the session took over (Transport::retainText).
// Move-only; the text stays valid and unchanged until the object is reset or destroyed,
// which frees it through release(context, data). Empty = nothing retained.
class RetainedText {
public:
    using ReleaseFn = void (*)(void *context, char *data);

    RetainedText() = default;
    // data is NUL-terminated at length (as delivered to onText); release may be nullptr
    // (nothing to free).
    RetainedText(char *data, size_t length, ReleaseFn release, void *context)
        : data_(data), length_(length), release_(release), context_(context) {}
    ~RetainedText() { reset(); }
    RetainedText(RetainedText &&other) noexcept { *this = std::move(other); }
    RetainedText &operator=(RetainedText &&other) noexcept;
    RetainedText(const RetainedText &) = delete;
    RetainedText &operator=(const RetainedText &) = delete;

    const char *data() const { return data_; }
    size_t length() const { return length_; }
    bool empty() const { return data_ == nullptr; }
    void reset();  // frees the text (no-op when empty)

private:
    char *data_ = nullptr;
    size_t length_ = 0;
    ReleaseFn release_ = nullptr;
    void *context_ = nullptr;
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
    // Called only from within HaSession::onText(data, length): takes over the text being
    // delivered so it stays valid after onText returns, until the RetainedText is reset
    // (the device transport detaches its receive buffer without copying; a test fake may
    // copy). Returns an empty RetainedText if it cannot (the default); the session then
    // reports states_no_memory.
    virtual RetainedText retainText(const char *data, size_t length) {
        (void)data;
        (void)length;
        return RetainedText();
    }
};

// What a pending request was for (results are matched by id). get_states is not in the
// pending map (it has its own state and expiry, see requestStates()).
enum class Purpose : uint8_t { Subscribe, Unsubscribe, CallService };

// The entity list fetch (requestStates):
//  None      nothing fetched yet (or dropped by a URL / token / TLS change)
//  Fetching  get_states sent, waiting for its result
//  Building  result retained, the catalog is being built in tick()
//  Ready     catalog built (handed over once by takeCatalog(); stays Ready afterwards)
//  Failed    see statesError()
enum class StatesState : uint8_t { None, Fetching, Building, Ready, Failed };

// "none", "fetching", "building", "ready", "failed" (as in /api/status ha.catalog.state).
const char *statesStateName(StatesState state);

struct SessionOptions {
    // Bytes of the retained get_states text the catalog builder works through per tick()
    // (whole elements, at least one per tick). Device: 32 KB, about 5 ms.
    size_t statesBytesPerTick = 32u * 1024u;
};

class HaSession {
public:
    static constexpr uint32_t kConnectTimeoutMs = 20000;  // open() -> auth_required
    static constexpr uint32_t kAuthTimeoutMs = 10000;     // auth sent -> auth_ok / auth_invalid
    static constexpr uint32_t kPingIntervalMs = 30000;
    static constexpr uint32_t kPongTimeoutMs = 10000;     // from max(ping, last received byte)
    // No pong this long after the ping fails the connection even while bytes keep
    // arriving (a peer that streams but never answers pings).
    static constexpr uint32_t kPongHardTimeoutMs = 90000;
    static constexpr uint32_t kRequestTimeoutMs = 15000;  // pending map expiry
    // get_states fails (states_timeout) after kRequestTimeoutMs without a received byte
    // since it was sent, and at the latest kStatesHardTimeoutMs after sending.
    static constexpr uint32_t kStatesHardTimeoutMs = 90000;
    // While get_states is pending, a message whose id peekEnvelope() cannot find is taken
    // to be its result if it is longer than this.
    static constexpr size_t kStatesRetainMinBytes = 64u * 1024u;
    static constexpr size_t kMaxPending = 16;
    static constexpr size_t kMaxErrorLen = 120;           // lastError is cut to this length

    using Options = SessionOptions;

    // Receives one line per notable session event, without the "HA " prefix:
    //   subscribe id=<n> ids=<k>
    //   unsubscribe id=<n> subscription=<old id>
    //   unsubscribe failed: <HA message>      (also "unsubscribe failed: timeout")
    //   snapshot entities=<n>                 (first "a" of a subscription)
    //   event dropped error=<e>               (an event of the active / pending
    //                                          subscription that could not be applied:
    //                                          too_large or ArduinoJson's error)
    //   states request id=<n>
    //   states retained len=<bytes>
    //   catalog failed error=<statesError>
    //   states late result dropped len=<bytes>  (a large result of an abandoned fetch)
    // The logger is called from inside session methods (also while the pending map is
    // being iterated): it must only log and never call back into the session.
    using Logger = std::function<void(const std::string &line)>;

    // Backoff delay before the attempt after `failures` consecutive failures (>= 1):
    // 1000, 2000, 4000, 8000, 16000, then 30000 ms.
    static uint32_t backoffDelayMs(uint32_t failures);

    explicit HaSession(Transport &transport, ArduinoJson::Allocator *allocator = nullptr,
                       const Options &options = Options());

    // Applies the configuration. A different URL, token or TLS setting (or the first call)
    // restarts:
    // the connection is closed and, with both set and a usable URL, a new one is started
    // (Connecting, or Backoff while the network is down). Empty URL or token: NoConfig.
    // An invalid URL (or one with a query/fragment): NoConfig with lastError
    // "invalid_url". An https URL is opened with tls = true and these settings (wss, see
    // TlsMode). Transport::open() is never called in NoConfig. Unchanged URL, token and TLS settings keep the connection (also
    // AuthFailed). A restart also clears entities() and aborts / drops the entity list
    // (states None). Entity ids are de-duplicated; a changed set (the order does not
    // matter) clears entities() and, while Subscribing / Ready, resubscribes at once (see
    // above); in other states it is used from the next subscribe on.
    void configure(const std::string &url, const std::string &token, const TlsSettings &tls,
                   const std::vector<std::string> &entityIds, uint32_t nowMs);

    // Station link state. Losing it closes an active connection (Backoff).
    void setNetworkUp(bool up, uint32_t nowMs);

    // "Retry" from the web UI: leaves AuthFailed, or ends a Backoff wait early, and starts
    // a new connection series (backoff from 1 s). Returns false (no-op) in other states.
    bool retry(uint32_t nowMs);

    // Timers: backoff, connect / auth timeouts, ping and pong, pending and get_states
    // expiry; plus one catalog build slice while Building (in every state). Call often.
    void tick(uint32_t nowMs);

    // Starts fetching the entity list (get_states). Only in Ready. A fetch already running
    // (Fetching / Building) is kept (coalesced, returns true); otherwise an untaken
    // catalog and the previous error are dropped. False when not Ready or the send failed.
    bool requestStates(uint32_t nowMs);

    StatesState statesState() const { return statesState_; }
    // Failed only: "states_too_large", "states_no_memory", "states_timeout",
    // "states_failed: <HA message>" / "states_failed", "states_unexpected",
    // "connection_lost" (the connection ended before the result arrived).
    const std::string &statesError() const { return statesError_; }
    // The built catalog, once per build (nullopt before, and after it was taken).
    std::optional<EntityCatalog> takeCatalog();
    // Milliseconds from retaining the result to the end of the last build.
    uint32_t catalogBuildMs() const { return catalogBuildMs_; }
    // Incremented on every change of statesState() / statesError() and when a new catalog
    // becomes available.
    uint32_t statesChangeCount() const { return statesChangeCount_; }

    // Live state of the subscribed entities (see above).
    const EntityStore &entities() const { return store_; }
    // The configured entity ids, de-duplicated, in configuration order.
    const std::vector<std::string> &entityIds() const { return entityIds_; }

    void setLogger(Logger logger) { logger_ = std::move(logger); }

    // Transport events (ignored unless the session has a connection open).
    void onClosed(const std::string &reason, uint32_t nowMs);
    void onText(const char *data, size_t length, uint32_t nowMs);
    // The transport discarded a received message (too large, no memory, binary): length =
    // payload bytes seen, reason e.g. "too_large". Counted only while a connection is open;
    // the connection stays up. A request whose result was dropped expires from the pending
    // map as usual. While get_states is pending the dropped message is taken to be its
    // result: "too_large" -> states_too_large, "no_memory" -> states_no_memory.
    void onMessageDropped(size_t length, const char *reason, uint32_t nowMs);
    // The transport received bytes (> 0) on the open connection; called for every read,
    // also in the middle of a message. Keeps the pong and get_states timeouts from
    // expiring while a large message is arriving.
    void onRxProgress(size_t bytes, uint32_t nowMs);

    State state() const { return state_; }
    // From auth_ok of the current connection ("" until then; cleared by every new attempt).
    const std::string &haVersion() const { return haVersion_; }
    // Why the session is not Ready (e.g. "connect_failed", "pong_timeout",
    // "auth_invalid: <HA message>"); cleared on Ready, retry() and a config change.
    // While Ready it may hold "subscribe_failed: <HA message>".
    const std::string &lastError() const { return lastError_; }
    const HaUrl &endpoint() const { return endpoint_; }
    const TlsSettings &tlsSettings() const { return tls_; }
    // The mode the configured endpoint uses (None for http or no valid URL).
    TlsMode tlsMode() const { return tlsModeFor(endpoint_, tls_); }
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
    void handleEvent(const Message &msg);
    void handleUnparsed(const Message &msg, const char *data, size_t length);
    bool active() const;
    bool ownSubscription(uint32_t id) const;
    bool subscribe(uint32_t nowMs);
    void resubscribe(uint32_t nowMs);
    void retainStates(const char *data, size_t length, uint32_t nowMs);
    void stepStates(uint32_t nowMs);
    void forgetStatesRequest();
    void failStates(const std::string &error);
    void abortStates();
    void setStatesState(StatesState state);
    void setStatesError(const std::string &error);
    void log(const std::string &line);

    Transport &transport_;
    ArduinoJson::Allocator *allocator_;
    Options options_;
    Logger logger_;

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
    uint32_t lastRxMs_ = 0;            // last onRxProgress (connect() for a new connection)
    uint32_t subscriptionId_ = 0;      // id of the active subscribe_entities (0 = none)
    uint32_t pendingSubscribeId_ = 0;  // subscribe_entities sent, no result yet (0 = none)
    std::vector<Pending> pending_;
    uint32_t changeCount_ = 0;
    uint32_t droppedMessages_ = 0;
    EntityStore store_;

    StatesState statesState_ = StatesState::None;
    std::string statesError_;
    uint32_t statesId_ = 0;  // Fetching: id of the get_states request
    uint32_t statesAttempt_ = 0;  // attempts_ when it was sent (its connection)
    // id of an abandoned get_states on this connection (0 = none): its late result is
    // dropped unparsed when large (cleared by connect() and once dropped).
    uint32_t staleStatesId_ = 0;
    uint32_t statesSentMs_ = 0;
    uint32_t statesRetainedMs_ = 0;
    uint32_t catalogBuildMs_ = 0;
    uint32_t statesChangeCount_ = 0;
    RetainedText retained_;                          // Building: the result text
    std::unique_ptr<EntityCatalogBuilder> builder_;  // Building: works through retained_
    std::optional<EntityCatalog> catalog_;           // Ready and not taken yet
};

}  // namespace ha
}  // namespace core
