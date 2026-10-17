#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "config/ConfigModel.h"
#include "control/LocalValue.h"

// Control glue (plan-step7.md §3.6): turns encoder detents and button presses on the
// control pages into Home Assistant service calls through core::control::ControlEngine,
// sent with ha::callService. Main loop only. `page` is the control index (ui::controlPage());
// `nowMs` is the loop pass's single millis() value (never later than the next call's).
//
// Serial lines (page is 1-based, as in "UI page"):
//   CTL press page=<i> press=<press>
//   CTL notice=<ha_offline|not_found|unavailable|no_value|busy|failed|timeout> page=<i> text=<t>
namespace app {
namespace control {

// device.accel (batched acceleration, plan-step7.md Q7-2), device.send_interval_ms,
// device.buzzer (the error beep, Q7-8) and the controls (a change resets every hold and
// unsent value). After boot and every config re-read.
void applyConfig(const core::config::DeviceConfig &device,
                 const std::vector<core::config::Control> &controls);

// Signed detents of one loop pass (already flipped for device.encoder_reverse).
void rotate(size_t page, int detents, uint32_t nowMs);

// A short press on a control page.
void press(size_t page, uint32_t nowMs);

// Every loop pass after ha::loop(): drains ha::takeCallResults() into the engine, sends
// due values, and shows the newest notice (log + ui::showToast + a 150 ms error beep if
// device.buzzer, at most one per 2 s).
void loop(uint32_t nowMs);

// The control's local value while it holds (presenter::PresentContext::local).
std::optional<core::control::LocalValue> local(size_t page, uint32_t nowMs);

// Changes whenever local() of any control can change (compare for inequality).
uint32_t version();

}  // namespace control
}  // namespace app
