#pragma once

#include <cstdint>

#include "input/ButtonFsm.h"

// Factory-reset confirmation (plan-step8.md §3.2, Q8-12, Q8-13): a very long press opens a
// 10 s confirmation, a short press confirms, rotation / swipe / timeout cancel. The hold
// only counts after the button was seen released once since boot (IO0 is the BOOT strap).
// Pure; deadlines are compared signed, so they are safe across the millis() wrap.
namespace core {
namespace ui {

class ResetConfirm {
public:
    static constexpr uint32_t kTimeoutMs = 10000;
    enum class Result : uint8_t { None, Opened, Confirmed, Cancelled, IgnoredNotArmed };
    enum class CancelReason : uint8_t { None, Rotate, Swipe, Timeout };

    // Call every pass with the debounced level; a false arms (Q8-13).
    void observeButton(bool debouncedDown);
    Result onButton(ButtonFsm::Event event, uint32_t nowMs);
    Result onRotate(int detents, uint32_t nowMs);
    Result onSwipe(uint32_t nowMs);
    Result tick(uint32_t nowMs);

    // Confirming or Confirmed: the caller routes no input elsewhere.
    bool captures() const { return phase_ != Phase::Idle; }
    bool confirming() const { return phase_ == Phase::Confirming; }
    // Terminal until reboot.
    bool confirmed() const { return phase_ == Phase::Confirmed; }
    bool armed() const { return armed_; }
    // Ceil, 10..1 while confirming, else 0.
    uint32_t secondsLeft(uint32_t nowMs) const;
    // Reason of the latest cancel (None before the first one; cleared when a new one opens).
    CancelReason lastCancel() const { return lastCancel_; }

private:
    enum class Phase : uint8_t { Idle, Confirming, Confirmed };
    Result cancel(CancelReason reason);

    Phase phase_ = Phase::Idle;
    bool armed_ = false;
    uint32_t deadlineMs_ = 0;
    CancelReason lastCancel_ = CancelReason::None;
};

// "none" | "rotate" | "swipe" | "timeout"
const char *cancelReasonName(ResetConfirm::CancelReason reason);

}  // namespace ui
}  // namespace core
