#pragma once

#include <cstdint>

#include "input/ButtonFsm.h"

// Encoder push button on IO0 (active low), polled through core::ButtonFsm.
namespace hal {
namespace button {

void begin();

// Call every loop; returns at most one event per call.
core::ButtonFsm::Event poll(uint32_t nowMs);

// The debounced state as of the last poll() (true = held).
bool isDown();

// True once poll() has seen the raw level up (released) for at least the debounce time
// since boot; sticky. Arms the factory reset (plan-step8.md Q8-13). isDown() is no
// substitute: the FSM starts "up", so it reads false for the first debounce time even
// while the knob is held from boot.
bool releaseSeen();

}  // namespace button
}  // namespace hal
