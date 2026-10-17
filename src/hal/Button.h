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

}  // namespace button
}  // namespace hal
