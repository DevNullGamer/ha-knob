#include "control/ControlEngine.h"

#include <cmath>
#include <limits>
#include <string_view>
#include <utility>

#include "util/Text.h"

namespace core {
namespace control {

namespace da = domain_actions;

namespace {

size_t kindIndex(NoticeKind kind) { return static_cast<size_t>(kind); }

const char *defaultText(NoticeKind kind) {
    switch (kind) {
        case NoticeKind::HaOffline: return "HA offline";
        case NoticeKind::NotFound: return "Entity not found";
        case NoticeKind::Unavailable: return "Unavailable";
        case NoticeKind::NoValue: return "No value to change";
        case NoticeKind::Busy: return "Busy, try again";
        case NoticeKind::Failed: return "Failed";
        case NoticeKind::Timeout: return "No answer from HA";
    }
    return "";
}

// The toast text of a failed call: HA's message, else "Failed: <code>", cut on a UTF-8
// boundary.
std::string failedText(const ha::CallResult &result) {
    if (!result.message.empty()) {
        return util::truncateUtf8(result.message, ControlEngine::kMaxNoticeBytes);
    }
    return util::truncateUtf8("Failed: " + result.code, ControlEngine::kMaxNoticeBytes);
}

}  // namespace

const char *noticeKindName(NoticeKind kind) {
    switch (kind) {
        case NoticeKind::HaOffline: return "ha_offline";
        case NoticeKind::NotFound: return "not_found";
        case NoticeKind::Unavailable: return "unavailable";
        case NoticeKind::NoValue: return "no_value";
        case NoticeKind::Busy: return "busy";
        case NoticeKind::Failed: return "failed";
        case NoticeKind::Timeout: return "timeout";
    }
    return "unknown";
}

ControlEngine::ControlEngine(CallSender &sender, const EngineOptions &options)
    : sender_(sender) {
    applyOptions(options);
}

void ControlEngine::applyOptions(const EngineOptions &options) {
    options_ = options;
    if (options_.maxInFlight < 1) {
        options_.maxInFlight = 1;
    }
    flights_.reserve(options_.maxInFlight);
}

void ControlEngine::setOptions(const EngineOptions &options) {
    applyOptions(options);
    // Idle throttles carry no state: rebuild them with the new interval / settle time now.
    // The others pick it up in tick() once idle (see the header).
    for (Slot &slot : slots_) {
        if (slot.idle) {
            slot.throttle = Throttle(options_.sendIntervalMs, options_.settleMs);
        }
    }
}

void ControlEngine::setControls(const std::vector<config::Control> &controls) {
    if (controls == controls_) {
        return;
    }
    controls_ = controls;
    slots_.clear();
    slots_.reserve(controls_.size());
    for (size_t i = 0; i < controls_.size(); ++i) {
        slots_.emplace_back(options_);
    }
    for (Flight &flight : flights_) {
        flight.control = kOrphan;  // still unanswered: keeps its in-flight slot
    }
    notice_.reset();
    ++version_;
}

void ControlEngine::rotate(size_t index, int steps, const ha::EntityState *entity, bool haReady,
                           uint32_t nowMs) {
    if (index >= controls_.size() || steps == 0) {
        return;
    }
    const config::Control &control = controls_[index];
    const da::DomainSpec *domain = da::findDomain(da::domainOf(control.entityId));
    if (domain == nullptr) {
        return;
    }
    const da::ActionSpec *action = da::findAction(*domain, control.action);
    if (action == nullptr || std::string_view(action->name) == "none") {
        return;  // a press-only control: turning it does nothing, not even offline
    }
    if (!haReady) {
        notice(NoticeKind::HaOffline, index, nowMs);
        return;
    }

    Slot &slot = slots_[index];
    double value = std::numeric_limits<double>::quiet_NaN();
    if (entity != nullptr) {
        double step = control.step;
        if (!(step > 0)) {
            step = da::stepDefaultFor(*action, entity->attrs);  // config validates; defensive
        }
        const std::optional<double> base = slot.throttle.holding(nowMs)
                                               ? std::optional<double>(slot.throttle.target())
                                               : da::baseValue(*action, *entity, step);
        if (base) {
            value = da::stepValue(*base, steps, step, da::valueRange(*action, entity->attrs));
        }
    }
    // A NaN value (no entity or no base) makes encoderCall report NotFound / Unavailable /
    // NoValue in its own order.
    da::CallBuild build = da::encoderCall(*domain, *action, control.entityId, value, entity);
    if (!build.call) {
        noticeFor(build.reason, index, nowMs);
        return;
    }

    slot.throttle.change(value, nowMs);
    slot.call = std::move(*build.call);
    slot.idle = false;
    slot.wasHolding = true;
    ++version_;
    sendDue(index, nowMs);
}

void ControlEngine::press(size_t index, const ha::EntityState *entity, bool haReady,
                          uint32_t nowMs) {
    if (index >= controls_.size()) {
        return;
    }
    const config::Control &control = controls_[index];
    const da::DomainSpec *domain = da::findDomain(da::domainOf(control.entityId));
    if (domain == nullptr || control.press == "none" || !da::hasPress(*domain, control.press)) {
        return;
    }
    if (!haReady) {
        notice(NoticeKind::HaOffline, index, nowMs);
        return;
    }

    // Q7-11: a press ends the hold, so HA's state after the press is not hidden by a held
    // encoder value (and an unsent value does not go out after the press).
    dropSlot(slots_[index]);
    ++version_;

    const da::CallBuild build = da::pressCall(*domain, control.press, control.entityId, entity);
    if (!build.call) {
        noticeFor(build.reason, index, nowMs);
        return;
    }
    uint32_t tag = 0;
    if (flights_.size() >= options_.maxInFlight || !sendCall(index, *build.call, true, tag)) {
        notice(NoticeKind::Busy, index, nowMs);
    }
}

void ControlEngine::onResult(const ha::CallResult &result, uint32_t nowMs) {
    size_t at = 0;
    while (at < flights_.size() && flights_[at].tag != result.tag) {
        ++at;
    }
    if (at == flights_.size()) {
        return;  // not ours, or already ended
    }
    const Flight flight = flights_[at];
    flights_.erase(flights_.begin() + static_cast<std::ptrdiff_t>(at));
    if (flight.control == kOrphan || flight.control >= slots_.size()) {
        return;  // sent before a setControls() reset: only frees the in-flight slot
    }

    if (flight.press) {
        if (result.outcome == ha::CallOutcome::Failed) {
            notice(NoticeKind::Failed, flight.control, nowMs, failedText(result));
        } else if (result.outcome == ha::CallOutcome::Timeout) {
            notice(NoticeKind::Timeout, flight.control, nowMs);
        }
        return;
    }

    Slot &slot = slots_[flight.control];
    switch (result.outcome) {
        case ha::CallOutcome::Ok:
            slot.throttle.answered(true);
            break;
        case ha::CallOutcome::Failed:
            slot.throttle.answered(false);  // ends the hold, drops the unsent value
            slot.call.reset();
            notice(NoticeKind::Failed, flight.control, nowMs, failedText(result));
            break;
        case ha::CallOutcome::Timeout:
            slot.throttle.answered(false);
            slot.call.reset();
            notice(NoticeKind::Timeout, flight.control, nowMs);
            break;
        case ha::CallOutcome::ConnectionLost:
            slot.throttle.answered(true);
            dropSlot(slot);  // never sent after a reconnect (Q7-7); the page shows HA offline
            break;
    }
    ++version_;
}

void ControlEngine::tick(bool haReady, uint32_t nowMs) {
    if (!haReady) {
        for (Slot &slot : slots_) {
            if (slot.call || slot.throttle.holding(nowMs)) {
                dropSlot(slot);
                ++version_;
            }
        }
    } else {
        for (size_t i = 0; i < slots_.size(); ++i) {
            sendDue(i, nowMs);
        }
    }

    for (Slot &slot : slots_) {
        const bool holding = slot.throttle.holding(nowMs);
        if (holding != slot.wasHolding) {
            slot.wasHolding = holding;
            ++version_;  // e.g. the settle time ran out: local() is empty now
        }
        // Wrap guard (DESIGN-NOTES Step 7.4): once a control is idle, its throttle is
        // replaced by a fresh one, so no settle / send timestamp can grow old enough
        // (~49.7 days) to look recent again after millis() wraps. A fresh throttle behaves
        // like an expired one: the next change() starts a new hold and sends at once, which
        // the interval already allows here.
        if (!holding && !slot.idle && slot.throttle.inFlight() == 0 &&
            (!slot.hasSend || nowMs - slot.lastSendMs >= options_.sendIntervalMs)) {
            slot.throttle = Throttle(options_.sendIntervalMs, options_.settleMs);
            slot.call.reset();
            slot.idle = true;
            slot.hasSend = false;
        }
        // Same for notice suppression: forget a stamp once its repeat time is over.
        for (size_t k = 0; k < kNoticeKinds; ++k) {
            if (slot.noticeActive[k] && nowMs - slot.noticeAtMs[k] >= options_.noticeRepeatMs) {
                slot.noticeActive[k] = false;
            }
        }
    }
}

std::optional<LocalValue> ControlEngine::local(size_t index, uint32_t nowMs) const {
    if (index >= slots_.size() || !slots_[index].throttle.holding(nowMs)) {
        return std::nullopt;
    }
    return LocalValue{slots_[index].throttle.target(), true};
}

std::optional<Notice> ControlEngine::takeNotice() {
    std::optional<Notice> taken = std::move(notice_);
    notice_.reset();
    return taken;
}

void ControlEngine::sendDue(size_t index, uint32_t nowMs) {
    Slot &slot = slots_[index];
    if (!slot.call) {
        return;
    }
    const std::optional<double> due =
        slot.throttle.due(nowMs, flights_.size() < options_.maxInFlight);
    if (!due) {
        return;
    }
    uint32_t tag = 0;
    if (sendCall(index, *slot.call, false, tag)) {
        slot.throttle.sent(*due, nowMs);
        slot.call.reset();  // built for the target just sent; the next change builds anew
        slot.hasSend = true;
        slot.lastSendMs = nowMs;
        return;
    }
    // Refused although HA is Ready (callers only send then): the session's call cap or
    // pending map is full, or the send failed (the session then leaves Ready and the next
    // tick reports nothing new). Busy describes the likely cause better than HaOffline.
    dropSlot(slot);
    ++version_;
    notice(NoticeKind::Busy, index, nowMs);
}

bool ControlEngine::sendCall(size_t index, const da::ServiceCall &call, bool press,
                             uint32_t &tag) {
    tag = nextTag();
    if (!sender_.send(call, tag)) {
        return false;
    }
    flights_.push_back(Flight{tag, index, press});
    return true;
}

void ControlEngine::dropSlot(Slot &slot) {
    slot.throttle.drop();
    slot.call.reset();
}

void ControlEngine::notice(NoticeKind kind, size_t index, uint32_t nowMs, std::string text) {
    if (kind != NoticeKind::Failed && kind != NoticeKind::Timeout && index < slots_.size()) {
        Slot &slot = slots_[index];
        const size_t k = kindIndex(kind);
        if (slot.noticeActive[k] && nowMs - slot.noticeAtMs[k] < options_.noticeRepeatMs) {
            return;
        }
        slot.noticeActive[k] = true;
        slot.noticeAtMs[k] = nowMs;
    }
    if (text.empty()) {
        text = defaultText(kind);
    }
    notice_ = Notice{kind, std::move(text), index};
}

void ControlEngine::noticeFor(da::NoCall reason, size_t index, uint32_t nowMs) {
    switch (reason) {
        case da::NoCall::NotFound: notice(NoticeKind::NotFound, index, nowMs); break;
        case da::NoCall::Unavailable: notice(NoticeKind::Unavailable, index, nowMs); break;
        case da::NoCall::NoValue: notice(NoticeKind::NoValue, index, nowMs); break;
        case da::NoCall::None:
        case da::NoCall::NoAction: break;
    }
}

uint32_t ControlEngine::nextTag() {
    for (;;) {
        ++tagCounter_;
        if (tagCounter_ == 0) {
            continue;  // 0 is never a tag
        }
        bool used = false;
        for (const Flight &flight : flights_) {
            used = used || flight.tag == tagCounter_;
        }
        if (!used) {
            return tagCounter_;
        }
    }
}

}  // namespace control
}  // namespace core
