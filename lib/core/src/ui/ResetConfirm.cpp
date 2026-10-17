#include "ui/ResetConfirm.h"

namespace core {
namespace ui {

const char *cancelReasonName(ResetConfirm::CancelReason reason) {
    switch (reason) {
        case ResetConfirm::CancelReason::None: return "none";
        case ResetConfirm::CancelReason::Rotate: return "rotate";
        case ResetConfirm::CancelReason::Swipe: return "swipe";
        case ResetConfirm::CancelReason::Timeout: return "timeout";
    }
    return "none";
}

void ResetConfirm::observeButton(bool debouncedDown) {
    if (!debouncedDown) armed_ = true;
}

ResetConfirm::Result ResetConfirm::cancel(CancelReason reason) {
    phase_ = Phase::Idle;
    lastCancel_ = reason;
    return Result::Cancelled;
}

ResetConfirm::Result ResetConfirm::onButton(ButtonFsm::Event event, uint32_t nowMs) {
    using Event = ButtonFsm::Event;
    switch (phase_) {
        case Phase::Idle:
            if (event != Event::VeryLongPress) return Result::None;
            if (!armed_) return Result::IgnoredNotArmed;
            phase_ = Phase::Confirming;
            deadlineMs_ = nowMs + kTimeoutMs;
            lastCancel_ = CancelReason::None;
            return Result::Opened;
        case Phase::Confirming:
            if (event == Event::ShortPress) {
                phase_ = Phase::Confirmed;
                return Result::Confirmed;
            }
            return Result::None;
        case Phase::Confirmed:
            break;
    }
    return Result::None;
}

ResetConfirm::Result ResetConfirm::onRotate(int detents, uint32_t) {
    if (phase_ != Phase::Confirming || detents == 0) return Result::None;
    return cancel(CancelReason::Rotate);
}

ResetConfirm::Result ResetConfirm::onSwipe(uint32_t) {
    if (phase_ != Phase::Confirming) return Result::None;
    return cancel(CancelReason::Swipe);
}

ResetConfirm::Result ResetConfirm::tick(uint32_t nowMs) {
    if (phase_ != Phase::Confirming) return Result::None;
    if (static_cast<int32_t>(nowMs - deadlineMs_) >= 0) return cancel(CancelReason::Timeout);
    return Result::None;
}

uint32_t ResetConfirm::secondsLeft(uint32_t nowMs) const {
    if (phase_ != Phase::Confirming) return 0;
    const int32_t left = static_cast<int32_t>(deadlineMs_ - nowMs);
    if (left <= 0) return 0;
    if (left >= static_cast<int32_t>(kTimeoutMs)) return kTimeoutMs / 1000;
    return (static_cast<uint32_t>(left) + 999) / 1000;
}

}  // namespace ui
}  // namespace core
