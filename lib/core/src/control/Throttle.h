#pragma once

#include <cstdint>
#include <optional>

// Rate limit and hold state of one control's encoder value (plan-step7.md §3.2, Q7-1,
// Q7-3). Pure and clock-injected (all times are millis(), wrap-safe). The caller asks due()
// each pass, sends the value it returns and reports sent() / answered():
//   - leading edge: the first unsent target goes out at once (nothing sent for intervalMs);
//   - then at most one send per intervalMs, always the latest target (intermediate values
//     are skipped), and the final target is always sent eventually (it stays due until
//     sent, also while canSend is false);
//   - within one hold, a target equal to the last sent value (|d| <= 1e-9) is not sent
//     again; a change that starts a new hold forgets the last sent value (HA's value may
//     have moved since, plan-step7.md Amendment A1).
// The control "holds" (the screen and the next turn use the local target, not HA's value)
// while a target is unsent, a call is unanswered, or for settleMs after the last change.
namespace core {
namespace control {

class Throttle {
public:
    Throttle(uint32_t intervalMs, uint32_t settleMs);

    // A new local target; starts the settle time and starts holding again (also after a
    // failure or drop()). If the control was not holding, the last sent value is forgotten
    // first, so the target is sent even if it equals that value.
    void change(double value, uint32_t nowMs);

    // The value to send now: an unsent target that differs from the last sent one, the
    // interval since the last send is over (or nothing was sent yet) and canSend (the
    // caller's global in-flight cap). nullopt otherwise.
    std::optional<double> due(uint32_t nowMs, bool canSend) const;

    // value went out at nowMs: in flight +1, starts the interval.
    void sent(double value, uint32_t nowMs);

    // A call of this control was answered: in flight -1 (never below 0); !ok -> failed().
    void answered(bool ok);

    // Ends the hold now (the screen shows HA's value again): drops the unsent target and
    // forgets the last sent value (the same value may be sent again by a later turn). The
    // send interval keeps running; calls still in flight stay counted.
    void failed();

    // HA went down or a press ran: drops the unsent target and ends the hold (the next
    // change() starts a new hold and so forgets the last sent value).
    void drop();

    // !(failed or dropped since the last change) && (unsent || inFlight > 0 ||
    // nowMs - lastChange < settleMs). The settle test is modulo 2^32 ms: a control idle for
    // ~49.7 days holds again for settleMs (harmless; see DESIGN-NOTES Step 7.1 / 7.2).
    bool holding(uint32_t nowMs) const;
    bool unconfirmed(uint32_t nowMs) const { return holding(nowMs); }

    // The latest local target (0 before the first change).
    double target() const { return target_; }

    // Unanswered calls of this control.
    uint32_t inFlight() const { return inFlight_; }

private:
    uint32_t intervalMs_;
    uint32_t settleMs_;
    double target_ = 0;
    bool unsent_ = false;
    bool hasLastSent_ = false;   // lastSent_ is valid (forgotten by failed())
    double lastSent_ = 0;
    bool hasSendTime_ = false;   // lastSendMs_ is valid
    uint32_t lastSendMs_ = 0;
    bool hasChange_ = false;
    uint32_t lastChangeMs_ = 0;
    uint32_t inFlight_ = 0;
    bool released_ = false;      // failed() / drop() since the last change
};

}  // namespace control
}  // namespace core
