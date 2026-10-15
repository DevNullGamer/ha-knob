#include "ha/HaSession.h"

#include <algorithm>

#include "ha/HaProtocol.h"

namespace core {
namespace ha {

namespace {

constexpr uint32_t kBackoffMs[] = {1000, 2000, 4000, 8000, 16000, 30000};
constexpr size_t kBackoffSteps = sizeof(kBackoffMs) / sizeof(kBackoffMs[0]);

// Cuts s to at most maxLen bytes without splitting a UTF-8 sequence.
std::string truncateUtf8(const std::string &s, size_t maxLen) {
    if (s.size() <= maxLen) return s;
    size_t len = maxLen;
    while (len > 0 && (static_cast<unsigned char>(s[len]) & 0xC0) == 0x80) --len;
    return s.substr(0, len);
}

}  // namespace

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

uint32_t HaSession::backoffDelayMs(uint32_t failures) {
    if (failures == 0) return 0;
    const size_t index = std::min<size_t>(failures - 1, kBackoffSteps - 1);
    return kBackoffMs[index];
}

HaSession::HaSession(Transport &transport, ArduinoJson::Allocator *allocator)
    : transport_(transport), allocator_(allocator) {}

// ---- Inputs ----------------------------------------------------------------------------

void HaSession::configure(const std::string &url, const std::string &token, const TlsSettings &tls,
                          const std::vector<std::string> &entityIds, uint32_t nowMs) {
    entityIds_.clear();
    for (const std::string &id : entityIds) {
        if (std::find(entityIds_.begin(), entityIds_.end(), id) == entityIds_.end()) {
            entityIds_.push_back(id);
        }
    }
    if (!firstConfigure_ && url == url_ && token == token_ && tls == tls_) {
        return;
    }
    firstConfigure_ = false;
    url_ = url;
    token_ = token;
    tls_ = tls;
    endpoint_ = url.empty() ? HaUrl() : parseHaUrl(url);

    closeTransport();
    pending_.clear();
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
    if (endpoint_.tls) {
        // TLS (wss) is not implemented: never send the token in cleartext to an https URL.
        setError("tls_not_supported");
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
        if (nowMs - it->sentAtMs < kRequestTimeoutMs) {
            ++it;
            continue;
        }
        const Purpose purpose = it->purpose;
        it = pending_.erase(it);
        if (purpose == Purpose::Subscribe) {
            fail("subscribe_timeout", nowMs);
            return;
        }
    }

    if (pingOutstanding_) {
        if (nowMs - lastPingMs_ >= kPongTimeoutMs) {
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
    const Message msg = parseMessage(data, length, allocator_);
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
                pending_.clear();
                setError(msg.message.empty() ? std::string("auth_invalid") : "auth_invalid: " + msg.message);
                setState(State::AuthFailed);
            }
            break;
        case State::Subscribing:
        case State::Ready:
            if (msg.type == MessageType::Result) {
                handleResult(msg.id, msg.success, msg.message);
            } else if (msg.type == MessageType::Pong) {
                if (pingOutstanding_ && msg.id == pingId_) {
                    pingOutstanding_ = false;
                }
            }
            // Events are not used yet (Step 6 applies subscribe_entities updates).
            break;
        default:
            break;
    }
}

void HaSession::onMessageDropped(size_t length, const char *reason, uint32_t nowMs) {
    (void)length;
    (void)reason;
    (void)nowMs;
    if (!transportOpen_ || !active()) {
        return;
    }
    ++droppedMessages_;
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
    std::string cut = truncateUtf8(error, kMaxErrorLen);
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
    pending_.clear();
    pingOutstanding_ = false;
    subscriptionId_ = 0;
    phaseStartMs_ = nowMs;
    transportOpen_ = true;
    setVersion("");  // learned again from this connection's auth_ok
    setState(State::Connecting);
    transport_.open(endpoint_, tls_);
}

void HaSession::fail(const std::string &reason, uint32_t nowMs) {
    closeTransport();
    pending_.clear();
    pingOutstanding_ = false;
    subscriptionId_ = 0;
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

bool HaSession::track(uint32_t id, Purpose purpose, int32_t tag, uint32_t nowMs) {
    if (pending_.size() >= kMaxPending) {
        return false;
    }
    pending_.push_back(Pending{id, purpose, tag, nowMs});
    return true;
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
    const uint32_t id = nextId_++;
    if (!send(buildSubscribeEntities(id, entityIds_, allocator_), nowMs)) {
        return;
    }
    track(id, Purpose::Subscribe, 0, nowMs);  // the map is empty right after connecting
    setState(State::Subscribing);
}

void HaSession::handleResult(uint32_t id, bool success, const std::string &message) {
    auto it = std::find_if(pending_.begin(), pending_.end(), [id](const Pending &p) { return p.id == id; });
    if (it == pending_.end()) {
        return;  // unknown or expired id
    }
    const Purpose purpose = it->purpose;
    pending_.erase(it);
    if (purpose == Purpose::Subscribe) {
        subscriptionId_ = success ? id : 0;
        becomeReady();
        if (!success) {
            setError(message.empty() ? std::string("subscribe_failed") : "subscribe_failed: " + message);
        }
    }
}

}  // namespace ha
}  // namespace core
