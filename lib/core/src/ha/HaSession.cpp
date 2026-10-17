#include "ha/HaSession.h"

#include <algorithm>
#include <cstring>
#include <string_view>

#include "ha/HaProtocol.h"
#include "util/Text.h"

namespace core {
namespace ha {

namespace {

constexpr uint32_t kBackoffMs[] = {1000, 2000, 4000, 8000, 16000, 30000};
constexpr size_t kBackoffSteps = sizeof(kBackoffMs) / sizeof(kBackoffMs[0]);

// Same ids regardless of order (both lists are de-duplicated).
bool sameIdSet(std::vector<std::string> a, std::vector<std::string> b) {
    if (a.size() != b.size()) return false;
    std::sort(a.begin(), a.end());
    std::sort(b.begin(), b.end());
    return a == b;
}

// Milliseconds from sinceMs to nowMs (modulo 2^32); 0 if nowMs is before sinceMs (a
// caller clock that went backwards must not expire a request at once).
uint32_t elapsedMs(uint32_t sinceMs, uint32_t nowMs) {
    const uint32_t d = nowMs - sinceMs;
    return static_cast<int32_t>(d) < 0 ? 0 : d;
}

}  // namespace

RetainedText &RetainedText::operator=(RetainedText &&other) noexcept {
    if (this != &other) {
        reset();
        data_ = other.data_;
        length_ = other.length_;
        release_ = other.release_;
        context_ = other.context_;
        other.data_ = nullptr;
        other.length_ = 0;
        other.release_ = nullptr;
        other.context_ = nullptr;
    }
    return *this;
}

void RetainedText::reset() {
    if (data_ != nullptr && release_ != nullptr) {
        release_(context_, data_);
    }
    data_ = nullptr;
    length_ = 0;
    release_ = nullptr;
    context_ = nullptr;
}

const char *stateName(State state) {
    switch (state) {
        case State::NoConfig: return "no_config";
        case State::Connecting: return "connecting";
        case State::Authenticating: return "authenticating";
        case State::Subscribing: return "subscribing";
        case State::Ready: return "ready";
        case State::AuthFailed: return "auth_failed";
        case State::Backoff: return "backoff";
        default: return "unknown";
    }
}

TlsMode tlsModeFor(const HaUrl &endpoint, const TlsSettings &tls) {
    if (!endpoint.ok || !endpoint.tls) return TlsMode::None;
    if (!tls.caPem.empty()) return TlsMode::CustomCa;
    return tls.insecure ? TlsMode::Insecure : TlsMode::Bundle;
}

const char *statesStateName(StatesState state) {
    switch (state) {
        case StatesState::None: return "none";
        case StatesState::Fetching: return "fetching";
        case StatesState::Building: return "building";
        case StatesState::Ready: return "ready";
        case StatesState::Failed: return "failed";
        default: return "unknown";
    }
}

const char *tlsModeName(TlsMode mode) {
    switch (mode) {
        case TlsMode::None: return "none";
        case TlsMode::Bundle: return "bundle";
        case TlsMode::CustomCa: return "custom_ca";
        case TlsMode::Insecure: return "insecure";
        default: return "unknown";
    }
}

uint32_t HaSession::backoffDelayMs(uint32_t failures) {
    if (failures == 0) return 0;
    const size_t index = std::min<size_t>(failures - 1, kBackoffSteps - 1);
    return kBackoffMs[index];
}

HaSession::HaSession(Transport &transport, ArduinoJson::Allocator *allocator, const Options &options)
    : transport_(transport), allocator_(allocator), options_(options), store_(allocator) {}

// ---- Inputs ----------------------------------------------------------------------------

void HaSession::configure(const std::string &url, const std::string &token, const TlsSettings &tls,
                          const std::vector<std::string> &entityIds, uint32_t nowMs) {
    std::vector<std::string> ids;
    for (const std::string &id : entityIds) {
        if (std::find(ids.begin(), ids.end(), id) == ids.end()) {
            ids.push_back(id);
        }
    }
    const bool idsChanged = !sameIdSet(ids, entityIds_);
    entityIds_ = std::move(ids);
    if (!firstConfigure_ && url == url_ && token == token_ && tls == tls_) {
        if (idsChanged) {
            store_.clear();
            if (transportOpen_ && (state_ == State::Subscribing || state_ == State::Ready)) {
                resubscribe(nowMs);
            }
        }
        return;
    }
    firstConfigure_ = false;
    url_ = url;
    token_ = token;
    tls_ = tls;
    endpoint_ = url.empty() ? HaUrl() : parseHaUrl(url);

    closeTransport();
    clearPending(nowMs);
    subscriptionId_ = 0;
    pendingSubscribeId_ = 0;
    store_.clear();
    abortStates();  // possibly another HA instance
    failures_ = 0;
    setVersion("");
    setError("");
    if (url.empty() || token.empty()) {
        setState(State::NoConfig);
        return;
    }
    if (!endpoint_.ok || endpoint_.hasQueryOrFragment) {
        setError("invalid_url");
        setState(State::NoConfig);
        return;
    }
    if (networkUp_) {
        connect(nowMs);
    } else {
        nextAttemptMs_ = nowMs;
        setError("network_down");
        setState(State::Backoff);
    }
}

void HaSession::setNetworkUp(bool up, uint32_t nowMs) {
    if (up == networkUp_) {
        return;
    }
    networkUp_ = up;
    if (!up && active()) {
        fail("network_lost", nowMs);
    } else if (up && state_ == State::Backoff && static_cast<int32_t>(nowMs - nextAttemptMs_) >= 0) {
        connect(nowMs);
    }
}

bool HaSession::retry(uint32_t nowMs) {
    if (state_ != State::AuthFailed && state_ != State::Backoff) {
        return false;
    }
    failures_ = 0;
    setError("");
    if (networkUp_) {
        connect(nowMs);
    } else {
        nextAttemptMs_ = nowMs;
        setError("network_down");
        setState(State::Backoff);
    }
    return true;
}

void HaSession::tick(uint32_t nowMs) {
    if (statesState_ == StatesState::Building) {
        stepStates(nowMs);  // also after the connection ended
    }
    switch (state_) {
        case State::Backoff:
            if (networkUp_ && static_cast<int32_t>(nowMs - nextAttemptMs_) >= 0) {
                connect(nowMs);
            }
            return;
        case State::Connecting:
            if (nowMs - phaseStartMs_ >= kConnectTimeoutMs) {
                fail("connect_timeout", nowMs);
            }
            return;
        case State::Authenticating:
            if (nowMs - phaseStartMs_ >= kAuthTimeoutMs) {
                fail("auth_timeout", nowMs);
            }
            return;
        case State::Subscribing:
        case State::Ready:
            break;
        default:
            return;
    }

    for (auto it = pending_.begin(); it != pending_.end();) {
        const uint32_t timeoutMs = it->purpose == Purpose::CallService ? kCallTimeoutMs : kRequestTimeoutMs;
        const uint32_t elapsed = elapsedMs(it->sentAtMs, nowMs);
        if (elapsed < timeoutMs) {
            ++it;
            continue;
        }
        const Purpose purpose = it->purpose;
        const uint32_t id = it->id;
        const uint32_t tag = it->tag;
        it = pending_.erase(it);
        if (purpose == Purpose::CallService) {
            callResults_.push_back(CallResult{tag, CallOutcome::Timeout, "timeout", "", elapsed});
            log("call timeout id=" + std::to_string(id));
            continue;
        }
        if (purpose == Purpose::Subscribe && id == pendingSubscribeId_) {
            fail("subscribe_timeout", nowMs);
            return;
        }
        if (purpose == Purpose::Unsubscribe) {
            log("unsubscribe failed: timeout");
        }
    }

    // Received bytes count as liveness (a large message delays the pong / the result).
    const uint32_t sinceRx = nowMs - lastRxMs_;
    if (statesState_ == StatesState::Fetching) {
        const uint32_t sinceSent = nowMs - statesSentMs_;
        if (std::min(sinceSent, sinceRx) >= kRequestTimeoutMs || sinceSent >= kStatesHardTimeoutMs) {
            failStates("states_timeout");
        }
    }

    if (pingOutstanding_) {
        const uint32_t sincePing = nowMs - lastPingMs_;
        if (std::min(sincePing, sinceRx) >= kPongTimeoutMs || sincePing >= kPongHardTimeoutMs) {
            fail("pong_timeout", nowMs);
        }
    } else if (nowMs - lastPingMs_ >= kPingIntervalMs) {
        const uint32_t id = nextId_++;
        if (send(buildPing(id, allocator_), nowMs)) {
            pingId_ = id;
            pingOutstanding_ = true;
            lastPingMs_ = nowMs;
        }
    }
}

void HaSession::onClosed(const std::string &reason, uint32_t nowMs) {
    if (!transportOpen_ || !active()) {
        return;
    }
    transportOpen_ = false;  // already closed: nothing to close
    fail(reason.empty() ? std::string("connection_lost") : reason, nowMs);
}

void HaSession::onText(const char *data, size_t length, uint32_t nowMs) {
    if (!transportOpen_ || !active()) {
        return;
    }
    const bool fetching = statesState_ == StatesState::Fetching;
    const bool checkStale = staleStatesId_ != 0 && length > kStatesRetainMinBytes;
    bool envelopeFirst = false;
    if (fetching || checkStale) {
        const EnvelopePeek peek = peekEnvelope(data, length);
        if (checkStale && peek.hasId && peek.id == staleStatesId_) {
            // The late result of an abandoned fetch: not worth an envelope parse of MBs.
            staleStatesId_ = 0;
            log("states late result dropped len=" + std::to_string(length));
            return;
        }
        // The get_states result (possibly several MB) is taken over unparsed. HA sends "id"
        // first; without a conclusive peek only a large message is taken to be it.
        if (fetching && (peek.hasId ? peek.id == statesId_ : length > kStatesRetainMinBytes)) {
            retainStates(data, length, nowMs);
            return;
        }
        // A small message whose "id" is not first may still be the result: it is classified
        // envelope-only, so a states result is never parsed with its whole "result" kept
        // (that is one long main-loop pass, ~0.4 ms per entity on the device).
        envelopeFirst = fetching && !peek.hasId;
    }
    Message msg = parseMessage(data, length, allocator_, envelopeFirst);
    if (envelopeFirst) {
        if (msg.type == MessageType::Result && msg.id == statesId_ &&
            (state_ == State::Subscribing || state_ == State::Ready)) {
            retainStates(data, length, nowMs);  // small result, "id" not first
            return;
        }
        if (msg.type == MessageType::Event) {
            msg = parseMessage(data, length, allocator_);  // events need their payload
        }
    }
    switch (state_) {
        case State::Connecting:
            if (msg.type == MessageType::AuthRequired && send(buildAuth(token_, allocator_), nowMs)) {
                phaseStartMs_ = nowMs;
                setState(State::Authenticating);
            }
            break;
        case State::Authenticating:
            if (msg.type == MessageType::AuthOk) {
                setVersion(msg.haVersion);
                onAuthenticated(nowMs);
            } else if (msg.type == MessageType::AuthInvalid) {
                closeTransport();
                clearPending(nowMs);
                setError(msg.message.empty() ? std::string("auth_invalid") : "auth_invalid: " + msg.message);
                setState(State::AuthFailed);
            }
            break;
        case State::Subscribing:
        case State::Ready:
            if (msg.type == MessageType::Result) {
                if (statesState_ == StatesState::Fetching && msg.id == statesId_) {
                    retainStates(data, length, nowMs);  // fallback (normally recognised above)
                } else {
                    handleResult(msg.id, msg.success, msg.errorCode, msg.message, nowMs);
                }
            } else if (msg.type == MessageType::Pong) {
                if (pingOutstanding_ && msg.id == pingId_) {
                    pingOutstanding_ = false;
                }
            } else if (msg.type == MessageType::Event) {
                handleEvent(msg);
            } else if (msg.type == MessageType::Error) {
                handleUnparsed(msg, data, length);
            }
            break;
        default:
            break;
    }
}

void HaSession::onMessageDropped(size_t length, const char *reason, uint32_t nowMs) {
    (void)length;
    (void)nowMs;
    if (!transportOpen_ || !active()) {
        return;
    }
    ++droppedMessages_;
    if (statesState_ == StatesState::Fetching && reason != nullptr) {
        if (std::strcmp(reason, "too_large") == 0) {
            failStates("states_too_large");
        } else if (std::strcmp(reason, "no_memory") == 0) {
            failStates("states_no_memory");
        }
    }
}

void HaSession::onRxProgress(size_t bytes, uint32_t nowMs) {
    if (bytes == 0 || !transportOpen_ || !active()) {
        return;
    }
    lastRxMs_ = nowMs;
}

bool HaSession::requestStates(uint32_t nowMs) {
    if (state_ != State::Ready || !transportOpen_) {
        return false;
    }
    if (statesState_ == StatesState::Fetching || statesState_ == StatesState::Building) {
        return true;  // coalesced
    }
    catalog_.reset();
    setStatesError("");
    const uint32_t id = nextId_++;
    statesId_ = id;
    statesAttempt_ = attempts_;
    statesSentMs_ = nowMs;
    setStatesState(StatesState::Fetching);  // a failed send below ends it (connection_lost)
    if (!send(buildGetStates(id, allocator_), nowMs)) {
        if (statesState_ == StatesState::Fetching) {
            failStates("connection_lost");
        }
        return false;
    }
    log("states request id=" + std::to_string(id));
    return true;
}

uint32_t HaSession::callService(const domain_actions::ServiceCall &call, uint32_t tag, uint32_t nowMs) {
    if (state_ != State::Ready || !transportOpen_ || callsPending() >= kMaxCallsPending ||
        pending_.size() >= kMaxPending) {
        return 0;
    }
    const uint32_t id = nextId_++;
    if (!send(buildCallService(id, call.domain, call.service, call.data.as<JsonVariantConst>(), call.entityId,
                               allocator_),
              nowMs)) {
        return 0;  // send() failed the connection (Backoff); nothing tracked, no result
    }
    if (!transportOpen_ || state_ != State::Ready) {
        return 0;  // closed during the send (reported through onClosed): no result follows
    }
    track(id, Purpose::CallService, tag, nowMs);  // room checked above
    std::string data;
    serializeJson(call.data, data);  // "null" without service_data
    log("call id=" + std::to_string(id) + " " + util::logSafe(call.domain, 32) + "." +
        util::logSafe(call.service, 48) + " entity=" + util::logSafe(call.entityId, 96) +
        " data=" + util::logSafe(data, 96));
    return id;
}

size_t HaSession::callsPending() const {
    return static_cast<size_t>(std::count_if(pending_.begin(), pending_.end(),
                                             [](const Pending &p) { return p.purpose == Purpose::CallService; }));
}

std::vector<CallResult> HaSession::takeCallResults() {
    std::vector<CallResult> out;
    out.swap(callResults_);
    return out;
}

std::optional<EntityCatalog> HaSession::takeCatalog() {
    std::optional<EntityCatalog> out = std::move(catalog_);
    catalog_.reset();
    return out;
}

// ---- Internals -------------------------------------------------------------------------

bool HaSession::active() const {
    return state_ == State::Connecting || state_ == State::Authenticating ||
           state_ == State::Subscribing || state_ == State::Ready;
}

void HaSession::setState(State state) {
    if (state != state_) {
        state_ = state;
        ++changeCount_;
    }
}

void HaSession::setError(const std::string &error) {
    std::string cut = util::truncateUtf8(error, kMaxErrorLen);
    if (cut != lastError_) {
        lastError_ = std::move(cut);
        ++changeCount_;
    }
}

void HaSession::setVersion(const std::string &version) {
    if (version != haVersion_) {
        haVersion_ = version;
        ++changeCount_;
    }
}

void HaSession::connect(uint32_t nowMs) {
    closeTransport();
    ++attempts_;
    nextId_ = 1;
    clearPending(nowMs);
    pingOutstanding_ = false;
    subscriptionId_ = 0;
    pendingSubscribeId_ = 0;
    store_.clear();
    if (statesState_ == StatesState::Fetching) {
        failStates("connection_lost");
    }
    staleStatesId_ = 0;  // ids restart with this connection
    lastRxMs_ = nowMs;
    phaseStartMs_ = nowMs;
    transportOpen_ = true;
    setVersion("");  // learned again from this connection's auth_ok
    setState(State::Connecting);
    transport_.open(endpoint_, tls_);
}

void HaSession::fail(const std::string &reason, uint32_t nowMs) {
    closeTransport();
    clearPending(nowMs);
    pingOutstanding_ = false;
    subscriptionId_ = 0;
    pendingSubscribeId_ = 0;
    // The store keeps the last values (shown dimmed while offline); a running build goes on.
    if (statesState_ == StatesState::Fetching) {
        failStates("connection_lost");
    }
    ++failures_;
    nextAttemptMs_ = nowMs + backoffDelayMs(failures_);
    setError(reason);
    setState(State::Backoff);
}

void HaSession::closeTransport() {
    if (transportOpen_) {
        transportOpen_ = false;
        transport_.close();
    }
}

bool HaSession::send(const std::string &text, uint32_t nowMs) {
    if (transport_.sendText(text)) {
        return true;
    }
    // sendText() may already have reported the close (onClosed -> Backoff): count the
    // failure only once and keep that reason.
    if (active() && transportOpen_) {
        fail("send_failed", nowMs);
    }
    return false;
}

bool HaSession::track(uint32_t id, Purpose purpose, uint32_t tag, uint32_t nowMs) {
    if (pending_.size() >= kMaxPending) {
        return false;
    }
    pending_.push_back(Pending{id, purpose, tag, nowMs});
    return true;
}

void HaSession::clearPending(uint32_t nowMs) {
    // Every unanswered call ends exactly once: as ConnectionLost here (the map is cleared
    // right after, so a later path finds nothing to report).
    size_t dropped = 0;
    for (const Pending &p : pending_) {
        if (p.purpose == Purpose::CallService) {
            callResults_.push_back(CallResult{p.tag, CallOutcome::ConnectionLost, "connection_lost", "",
                                              elapsedMs(p.sentAtMs, nowMs)});
            ++dropped;
        }
    }
    pending_.clear();
    if (dropped > 0) {
        log("calls dropped n=" + std::to_string(dropped) + " reason=connection_lost");
    }
}

void HaSession::becomeReady() {
    failures_ = 0;
    setError("");
    setState(State::Ready);
}

void HaSession::onAuthenticated(uint32_t nowMs) {
    lastPingMs_ = nowMs;  // first ping 30 s after auth_ok
    pingOutstanding_ = false;
    if (entityIds_.empty()) {
        becomeReady();  // nothing to subscribe to
        return;
    }
    if (!subscribe(nowMs)) {
        return;
    }
    setState(State::Subscribing);
}

std::optional<uint32_t> HaSession::msSinceSubscribe(uint32_t nowMs) const {
    if (subscriptionId_ == 0 && pendingSubscribeId_ == 0) {
        return std::nullopt;
    }
    return nowMs - subscribeSentMs_;
}

bool HaSession::subscribe(uint32_t nowMs) {
    const uint32_t id = nextId_++;
    if (!send(buildSubscribeEntities(id, entityIds_, allocator_), nowMs)) {
        return false;
    }
    pendingSubscribeId_ = id;
    subscribeSentMs_ = nowMs;
    if (pending_.size() >= kMaxPending) {
        // The subscribe needs its expiry more than the oldest unsubscribe its failure log.
        auto oldest = std::find_if(pending_.begin(), pending_.end(),
                                   [](const Pending &p) { return p.purpose == Purpose::Unsubscribe; });
        if (oldest != pending_.end()) {
            pending_.erase(oldest);
        }
    }
    track(id, Purpose::Subscribe, 0, nowMs);  // still full (no unsubscribe): only the expiry is lost
    log("subscribe id=" + std::to_string(id) + " ids=" + std::to_string(entityIds_.size()));
    return true;
}

void HaSession::resubscribe(uint32_t nowMs) {
    const uint32_t old = (subscriptionId_ != 0) ? subscriptionId_ : pendingSubscribeId_;
    if (pendingSubscribeId_ != 0) {
        // Its result no longer matters (an unknown id afterwards), nor do its events.
        const uint32_t pendingId = pendingSubscribeId_;
        pending_.erase(std::remove_if(pending_.begin(), pending_.end(),
                                      [pendingId](const Pending &p) { return p.id == pendingId; }),
                       pending_.end());
    }
    subscriptionId_ = 0;
    pendingSubscribeId_ = 0;
    if (old != 0) {
        // HA handles messages in order, so a still pending subscribe exists by the time this
        // arrives. A failure is only logged.
        const uint32_t id = nextId_++;
        if (!send(buildUnsubscribeEvents(id, old, allocator_), nowMs)) {
            return;
        }
        track(id, Purpose::Unsubscribe, 0, nowMs);
        log("unsubscribe id=" + std::to_string(id) + " subscription=" + std::to_string(old));
    }
    if (entityIds_.empty()) {
        becomeReady();  // Subscribing -> Ready; also clears an old subscribe_failed error
        return;
    }
    subscribe(nowMs);  // Ready stays Ready, Subscribing stays Subscribing
}

bool HaSession::ownSubscription(uint32_t id) const {
    return id != 0 && (id == subscriptionId_ || id == pendingSubscribeId_);
}

void HaSession::handleEvent(const Message &msg) {
    if (!ownSubscription(msg.id)) {
        return;  // stale: an event of an old subscription
    }
    if (msg.envelopeOnly) {
        log("event dropped error=too_large");  // a lost snapshot or update
        return;
    }
    const bool seen = store_.snapshotSeen();
    store_.apply(msg.event());
    if (!seen && store_.snapshotSeen()) {
        log("snapshot entities=" + std::to_string(store_.size()));
    }
}

void HaSession::handleUnparsed(const Message &msg, const char *data, size_t length) {
    // Only an event of our subscription is reported (other unparsable messages are
    // ignored as before).
    const EnvelopePeek peek = peekEnvelope(data, length);
    if (peek.hasId && ownSubscription(peek.id) && peek.hasType && peek.type == std::string_view("event")) {
        log(std::string("event dropped error=") + msg.parseError);
    }
}

void HaSession::handleResult(uint32_t id, bool success, const std::string &errorCode, const std::string &message,
                             uint32_t nowMs) {
    auto it = std::find_if(pending_.begin(), pending_.end(), [id](const Pending &p) { return p.id == id; });
    Purpose purpose = Purpose::Subscribe;
    if (it != pending_.end()) {
        purpose = it->purpose;
        const uint32_t tag = it->tag;
        const uint32_t rtt = elapsedMs(it->sentAtMs, nowMs);
        pending_.erase(it);
        if (purpose == Purpose::CallService) {
            if (success) {
                callResults_.push_back(CallResult{tag, CallOutcome::Ok, "", "", rtt});
                log("call ok id=" + std::to_string(id) + " ms=" + std::to_string(rtt));
            } else {
                const std::string code = errorCode.empty() ? std::string("unknown") : errorCode;
                callResults_.push_back(CallResult{tag, CallOutcome::Failed, code, message, rtt});
                std::string line = "call failed id=" + std::to_string(id) + " error=" + util::logSafe(code, 48);
                if (!message.empty()) {
                    line += ": " + util::logSafe(message, 60);
                }
                log(line);
            }
            return;
        }
    } else if (id == 0 || id != pendingSubscribeId_) {
        return;  // unknown or expired id (a subscribe not tracked because the map was full
                 // is still matched by pendingSubscribeId_)
    }
    if (purpose == Purpose::Subscribe) {
        if (id != pendingSubscribeId_) {
            return;  // superseded (not expected: resubscribe() drops it from the map)
        }
        pendingSubscribeId_ = 0;
        subscriptionId_ = success ? id : 0;
        if (!success) {
            store_.clear();
        }
        becomeReady();
        if (!success) {
            setError(message.empty() ? std::string("subscribe_failed") : "subscribe_failed: " + message);
        }
    } else if (purpose == Purpose::Unsubscribe && !success) {
        log(message.empty() ? std::string("unsubscribe failed") : "unsubscribe failed: " + message);
    }
}

// ---- Entity list -----------------------------------------------------------------------

void HaSession::retainStates(const char *data, size_t length, uint32_t nowMs) {
    RetainedText text = transport_.retainText(data, length);
    if (text.empty()) {
        failStates("states_no_memory");
        return;
    }
    retained_ = std::move(text);
    builder_.reset(new EntityCatalogBuilder(retained_.data(), retained_.length(), statesId_, allocator_));
    statesRetainedMs_ = nowMs;
    setStatesState(StatesState::Building);
    log("states retained len=" + std::to_string(retained_.length()));
}

void HaSession::stepStates(uint32_t nowMs) {
    const EntityCatalogBuilder::Status status =
        builder_->step(options_.statesBytesPerTick, options_.statesElementsPerTick);
    if (status == EntityCatalogBuilder::Status::Running) {
        return;
    }
    catalogBuildMs_ = nowMs - statesRetainedMs_;
    if (status == EntityCatalogBuilder::Status::Done) {
        catalog_ = builder_->take();
        builder_.reset();  // before the text it points into
        retained_.reset();
        ++statesChangeCount_;  // a new catalog to take
        setStatesState(StatesState::Ready);
    } else {
        const std::string error = builder_->error();
        failStates(error.empty() ? std::string("states_unexpected") : error);
    }
}

void HaSession::forgetStatesRequest() {
    // An abandoned fetch of the current connection: its result may still arrive (after a
    // timeout, a drop taken to be it, or another message retained in its place). A build
    // left over from an earlier connection has an id of that connection: not remembered.
    const bool running = statesState_ == StatesState::Fetching || statesState_ == StatesState::Building;
    if (running && statesId_ != 0 && statesAttempt_ == attempts_ && transportOpen_) {
        staleStatesId_ = statesId_;
    }
    statesId_ = 0;
}

void HaSession::failStates(const std::string &error) {
    forgetStatesRequest();
    builder_.reset();
    retained_.reset();
    catalog_.reset();
    setStatesError(error);
    setStatesState(StatesState::Failed);
    log("catalog failed error=" + error);
}

void HaSession::abortStates() {
    forgetStatesRequest();
    builder_.reset();
    retained_.reset();
    catalog_.reset();
    setStatesError("");
    setStatesState(StatesState::None);
}

void HaSession::setStatesState(StatesState state) {
    if (state != statesState_) {
        statesState_ = state;
        ++statesChangeCount_;
    }
}

void HaSession::setStatesError(const std::string &error) {
    std::string cut = util::truncateUtf8(error, kMaxErrorLen);
    if (cut != statesError_) {
        statesError_ = std::move(cut);
        ++statesChangeCount_;
    }
}

void HaSession::log(const std::string &line) {
    if (logger_) {
        logger_(line);
    }
}

}  // namespace ha
}  // namespace core
