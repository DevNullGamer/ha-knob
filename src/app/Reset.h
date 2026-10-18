#pragma once

#include <cstdint>

#include "input/ButtonFsm.h"

// Factory-reset gesture glue (plan-step8.md §3.5, Step 8.7, Q8-12..Q8-14): owns the
// core::ui::ResetConfirm and shows / hides the confirmation through ui::. A very long press
// (10 s) opens it, a short press confirms, rotation / swipe / 10 s cancel. Armed only after
// hal::button::releaseSeen() (IO0 is the BOOT strap, DESIGN-NOTES "Step 8.7"). The reset
// itself is main's: takeConfirmed(), then the delayed ConfigStore::factoryReset() + restart.
// Main loop only.
//
// Serial lines:
//   RST open                                     the confirmation opened
//   RST cancel reason=<rotate|swipe|timeout>     it closed without a reset
//   RST ignored reason=not_armed                 a very long press before the first release
//   RST confirm                                  the confirming short press
namespace app {
namespace reset {

// device.buzzer: the 300 ms beep when the confirmation opens. After boot and every config
// re-read.
void setBeep(bool enabled);

// A button event that passed the idle gate. True if the reset took it (the confirmation is
// open or confirmed, or this event opened it): route it nowhere else. Opening cancels an
// open Select list (`UI select cancel reason=reset`). button / rotate / swipe first apply
// an expired timeout (RST cancel reason=timeout), so a late short press never confirms.
// A button gesture the reset took (or whose event found the confirmation expired) stays
// with the reset up to its release (ShortPress / Released), even after a cancel.
bool button(core::ButtonFsm::Event event, uint32_t nowMs);
// While captures(): a detent batch (cancels) / a swipe (cancels). The caller routes the
// batch / swipe nowhere else, also when it only found the confirmation expired.
void rotate(int detents, uint32_t nowMs);
void swipe(uint32_t nowMs);
// Every pass: arming, timeout and the countdown on the screen.
void loop(uint32_t nowMs);
// The confirmation is open or confirmed: input goes only to the reset.
bool captures();
// True once, after the confirming press.
bool takeConfirmed();

}  // namespace reset
}  // namespace app
