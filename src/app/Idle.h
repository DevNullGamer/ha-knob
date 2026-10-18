#pragma once

#include <cstdint>

#include "config/ConfigModel.h"
#include "input/ButtonFsm.h"
#include "ui/IdleTimer.h"

// Screen idle glue (plan-step8.md §3.5, Step 8.4): owns the core::ui::IdleTimer, gates the
// inputs (the first input after Dim / Off only wakes the screen) and drives the panel
// (brightness, DISPON / DISPOFF). Main loop only.
//
// Serial lines:
//   IDLE wake by=<encoder|swipe|button> consumed=1   an input woke the screen and was dropped
//   IDLE wake by=<config|toast|reset> consumed=0     a config save / toast / the reset
//                                                    confirmation woke the screen
//   DISP level=<on|dim|off> pct=<n>                  the panel setting applied (on a change)
namespace app {
namespace idle {

enum class Source : uint8_t { Encoder, Swipe, Button, Config, Toast, Reset };

// device.dim_after_s / off_after_s / brightness / dim_level; counts as activity. After
// boot and every config re-read (loop() applies a brightness change).
void applyConfig(const core::config::DeviceConfig &device, uint32_t nowMs);

// An encoder detent batch or a completed swipe. true = act on it; false = it only woke
// the screen.
bool input(Source source, uint32_t nowMs);

// A button event (None -> true). false = swallowed (it woke the screen, or belongs to the
// gesture that did).
bool button(core::ButtonFsm::Event event, uint32_t nowMs);

// Non-input activity (Config, Toast, Reset): wakes the screen, nothing is swallowed. Logs
// only when it woke the screen, so it may be called every pass (the reset confirmation).
void keepAwake(Source source, uint32_t nowMs);

// Every loop pass after the inputs, the config re-read and the toasts (so a wake applies
// in the same pass): advances the level and applies the panel setting on a change. Only
// with a working display.
void loop(uint32_t nowMs);

core::ui::ScreenLevel level();

}  // namespace idle
}  // namespace app
