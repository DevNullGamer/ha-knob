#include "app/Reset.h"

#include "hal/Button.h"
#include "hal/Buzzer.h"
#include "ui/ResetConfirm.h"
#include "ui/Ui.h"
#include "util/Log.h"

namespace app {
namespace reset {

namespace {

constexpr uint32_t kOpenBeepMs = 300;

core::ui::ResetConfirm confirm;
bool beepEnabled = true;  // device.buzzer
bool confirmedPending = false;  // takeConfirmed() not yet called
// A button gesture the reset took (it began or went on while the confirmation was shown, or
// its event found the confirmation expired): its remaining events up to the release are
// swallowed too, also after a cancel, so it never reaches a control (Parent decision 8.7).
bool gestureTaken = false;

// Logs and shows the outcome of a ResetConfirm call.
void handle(core::ui::ResetConfirm::Result result, uint32_t nowMs) {
    using Result = core::ui::ResetConfirm::Result;
    switch (result) {
        case Result::Opened:
            ui::selectCancel("reset");  // it opened at 600 ms of the same hold (Q8-14)
            if (beepEnabled) {
                hal::buzzer::beep(nowMs, kOpenBeepMs);
            }
            util::logPrintf("RST open\n");
            ui::showResetConfirm(confirm.secondsLeft(nowMs));
            break;
        case Result::Confirmed:
            util::logPrintf("RST confirm\n");
            confirmedPending = true;
            break;
        case Result::Cancelled:
            util::logPrintf("RST cancel reason=%s\n", core::ui::cancelReasonName(confirm.lastCancel()));
            ui::hideResetConfirm();
            break;
        case Result::IgnoredNotArmed:
            util::logPrintf("RST ignored reason=not_armed\n");
            break;
        case Result::None:
            break;
    }
}

// The timeout first: an input at or after the deadline must find the confirmation already
// cancelled (reason=timeout), so a late short press never confirms. True if it expired now.
bool expire(uint32_t nowMs) {
    const core::ui::ResetConfirm::Result result = confirm.tick(nowMs);
    handle(result, nowMs);
    return result == core::ui::ResetConfirm::Result::Cancelled;
}

}  // namespace

void setBeep(bool enabled) { beepEnabled = enabled; }

bool button(core::ButtonFsm::Event event, uint32_t nowMs) {
    using Event = core::ButtonFsm::Event;
    if (event == Event::None) {
        return false;
    }
    const bool expired = expire(nowMs);
    const bool captured = confirm.captures();
    const core::ui::ResetConfirm::Result result = confirm.onButton(event, nowMs);
    handle(result, nowMs);
    const bool take =
        captured || expired || gestureTaken || result == core::ui::ResetConfirm::Result::Opened;
    if (take) {
        gestureTaken = event != Event::ShortPress && event != Event::Released;
    }
    return take;
}

void rotate(int detents, uint32_t nowMs) {
    expire(nowMs);
    handle(confirm.onRotate(detents, nowMs), nowMs);
}

void swipe(uint32_t nowMs) {
    expire(nowMs);
    handle(confirm.onSwipe(nowMs), nowMs);
}

void loop(uint32_t nowMs) {
    // Arms once a genuine debounced release was seen (Q8-13); hal::button::isDown() would
    // arm on the first pass even while the knob is held from boot.
    confirm.observeButton(!hal::button::releaseSeen());
    expire(nowMs);  // tick() before secondsLeft() (DESIGN-NOTES 8.1 / 8.2)
    if (confirm.confirming()) {
        ui::showResetConfirm(confirm.secondsLeft(nowMs));
    }
}

bool captures() { return confirm.captures(); }

bool takeConfirmed() {
    const bool pending = confirmedPending;
    confirmedPending = false;
    return pending;
}

}  // namespace reset
}  // namespace app
