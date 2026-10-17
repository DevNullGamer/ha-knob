#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "config/ConfigModel.h"
#include "control/DomainActions.h"
#include "control/LocalValue.h"
#include "control/Throttle.h"
#include "ha/EntityStore.h"
#include "ha/HaSession.h"

// Turns encoder detents and button presses on the configured controls into Home Assistant
// service calls (plan-step7.md §3.4, Q7-1, Q7-3, Q7-5..Q7-7, Q7-11). Pure and clock-injected
// (all times are millis(), wrap-safe; callers pass one `now` per loop pass, so timestamps
// never decrease between calls); the caller passes HA's entity state and whether the
// session is Ready on every call, sends through a CallSender, feeds every CallResult back
// via onResult() and calls tick() every loop pass. Every CallResult of a sent call must
// reach onResult() (on the device: drain ha::takeCallResults() every pass before tick());
// a lost result keeps its in-flight slot occupied until the next setControls() change.
//
//   - Encoder: each control has a Throttle (leading edge at once, then the latest value at
//     most once per sendIntervalMs, final value always sent) and the latest built call. At
//     most maxInFlight calls (encoder and press, all controls) are unanswered; more are
//     deferred, never dropped. While a control holds (see Throttle), local() returns its
//     local value and the next turn starts from it; afterwards from HA's value.
//   - Press: not optimistic; drops the control's hold and sends at once (Busy at the cap).
//   - HA not Ready: turns and presses change nothing (notice HaOffline); tick() drops every
//     unsent value. Unanswered calls end as ConnectionLost from the session (no notice).
//   - Sender refusal while HA is Ready (session cap, pending map full, send failure; the
//     session does not say which): notice Busy and the value is dropped.
//   - Notices: only the newest is kept (takeNotice()); the same kind for the same control
//     is suppressed for noticeRepeatMs, except Failed and Timeout.
namespace core {
namespace control {

struct EngineOptions {
    uint32_t sendIntervalMs = 250;  // per control, device.send_interval_ms
    uint32_t settleMs = 1500;       // hold after the last turn (kSettleMs)
    size_t maxInFlight = 8;         // unanswered calls overall (kMaxInFlight)
    uint32_t noticeRepeatMs = 2000; // same notice for the same control (kNoticeRepeatMs)

    bool operator==(const EngineOptions &o) const {
        return sendIntervalMs == o.sendIntervalMs && settleMs == o.settleMs &&
               maxInFlight == o.maxInFlight && noticeRepeatMs == o.noticeRepeatMs;
    }
    bool operator!=(const EngineOptions &o) const { return !(*this == o); }
};

// Sends one service call (on the device: ha::callService). tag identifies the call's
// CallResult. false = not sent (no result will follow).
class CallSender {
public:
    virtual ~CallSender() = default;
    virtual bool send(const domain_actions::ServiceCall &call, uint32_t tag) = 0;
};

enum class NoticeKind : uint8_t { HaOffline, NotFound, Unavailable, NoValue, Busy, Failed, Timeout };

// "ha_offline", "not_found", "unavailable", "no_value", "busy", "failed", "timeout" (logs).
const char *noticeKindName(NoticeKind kind);

struct Notice {
    NoticeKind kind;
    std::string text;  // for the toast; at most kMaxNoticeBytes bytes, UTF-8 intact
    size_t control;    // index into the controls
};

class ControlEngine {
public:
    static constexpr size_t kMaxNoticeBytes = 60;

    // maxInFlight is clamped to at least 1 (0 would block every call forever).
    explicit ControlEngine(CallSender &sender, const EngineOptions &options = {});

    // Keeps all state. maxInFlight (clamped to >= 1) and noticeRepeatMs apply at once; a changed send interval
    // or settle time applies to a control once it is idle (not holding, nothing in flight,
    // the interval since its last send over), i.e. never in the middle of a hold.
    void setOptions(const EngineOptions &options);

    // Controls that differ from the current ones reset all state (holds, unsent values,
    // notice suppression, the pending notice); calls still in flight become orphans whose
    // results only free their in-flight slot. Equal controls change nothing.
    void setControls(const std::vector<config::Control> &controls);

    // steps: signed detents after acceleration. base = the held target while holding, else
    // baseValue() of HA's state; the stepped value's call is built at once and sent if due.
    // Action "none" (or one the domain lacks): nothing. HA not Ready, entity not found /
    // unavailable, no value: a notice and no change. Out-of-range index or 0 steps: nothing.
    void rotate(size_t index, int steps, const ha::EntityState *entity, bool haReady,
                uint32_t nowMs);

    // Press "none" (or one the domain lacks): nothing. HA not Ready: notice HaOffline.
    // Otherwise drops the control's hold (Q7-11), then sends the press call at once
    // (NotFound / Unavailable notice, Busy at the in-flight cap or when the sender refuses).
    void press(size_t index, const ha::EntityState *entity, bool haReady, uint32_t nowMs);

    // A CallResult from the session. Unknown tags are ignored; orphans only free their slot.
    // Encoder calls: Ok -> answered; Failed -> hold ends + notice Failed (HA's message cut to
    // kMaxNoticeBytes, or "Failed: <code>"); Timeout -> hold ends + notice Timeout ("No
    // answer from HA"); ConnectionLost -> unsent value dropped, no notice. Press calls:
    // Failed / Timeout notices only.
    void onResult(const ha::CallResult &result, uint32_t nowMs);

    // Every loop pass. haReady false: drops every unsent value and hold. Otherwise sends what
    // is due (controls in index order, up to the cap: no round robin, accepted because only
    // the shown page's control is turned, so rarely more than one control has a due value).
    // Then notices hold expiry (version()), resets idle controls and expires notice
    // suppression (see DESIGN-NOTES Step 7.4, wrap).
    void tick(bool haReady, uint32_t nowMs);

    // The control's local value while it holds; nullopt otherwise (show HA's value).
    std::optional<LocalValue> local(size_t index, uint32_t nowMs) const;

    // The newest notice since the last take (older ones are discarded).
    std::optional<Notice> takeNotice();

    // +1 whenever local() of any control can change (turn, press, result, drop, hold
    // expiry seen in tick(), setControls). Compare for inequality only.
    uint32_t version() const { return version_; }

    // Unanswered calls (incl. presses and orphans).
    size_t inFlight() const { return flights_.size(); }

private:
    static constexpr size_t kNoticeKinds = 7;
    static constexpr size_t kOrphan = static_cast<size_t>(-1);

    struct Slot {
        explicit Slot(const EngineOptions &o) : throttle(o.sendIntervalMs, o.settleMs) {}
        Throttle throttle;
        std::optional<domain_actions::ServiceCall> call;  // built for throttle.target()
        bool idle = true;          // throttle is fresh (no stale timestamps)
        bool wasHolding = false;   // holding as last seen by tick(); rotate() sets it
        bool hasSend = false;
        uint32_t lastSendMs = 0;
        std::array<bool, kNoticeKinds> noticeActive{};
        std::array<uint32_t, kNoticeKinds> noticeAtMs{};
    };

    struct Flight {
        uint32_t tag;
        size_t control;  // kOrphan after a setControls() reset
        bool press;
    };

    void applyOptions(const EngineOptions &options);  // clamps, reserves flights_
    void sendDue(size_t index, uint32_t nowMs);
    bool sendCall(size_t index, const domain_actions::ServiceCall &call, bool press,
                  uint32_t &tag);
    void dropSlot(Slot &slot);
    void notice(NoticeKind kind, size_t index, uint32_t nowMs, std::string text = {});
    void noticeFor(domain_actions::NoCall reason, size_t index, uint32_t nowMs);
    uint32_t nextTag();

    CallSender &sender_;
    EngineOptions options_;
    std::vector<config::Control> controls_;
    std::vector<Slot> slots_;
    std::vector<Flight> flights_;
    std::optional<Notice> notice_;
    uint32_t tagCounter_ = 0;
    uint32_t version_ = 0;
};

}  // namespace control
}  // namespace core
