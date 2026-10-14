#pragma once

#include <cstdint>

#include "input/SwipeClassifier.h"

// Capacitive touch (SensorLib), with runtime detection of the panel variant.
// Touch is used for horizontal swipes only; taps are deliberately not reported
// (the screen is the knob cap, so pressing it also clicks the button).
namespace hal {
namespace touch {

enum class Variant : uint8_t { None, Cst816, Chsc5816 };

// Resets the controller, probes I2C 0x15 (CST816) then 0x2E (CHSC5816) and
// initialises the matching driver.
void begin();

Variant variant();

// "CST816", "CHSC5816" or "none".
const char *variantName();

// Polls the controller (rate-limited internally) and returns a swipe when a
// stroke ends, Swipe::None otherwise. Call every loop.
core::Swipe poll(uint32_t nowMs);

}  // namespace touch
}  // namespace hal
