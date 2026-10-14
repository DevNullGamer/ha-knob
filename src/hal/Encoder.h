#pragma once

#include <cstdint>

// Rotary encoder: a GPIO ISR on both inputs runs core::QuadratureDecoder and adds each
// detent to an atomic counter. The main loop reads the change with takeDelta().
namespace hal {
namespace encoder {

// reverse = true flips the direction (clockwise is positive by default).
void begin(bool reverse = false);

// Detents since the previous call (positive = clockwise). Main loop only.
int32_t takeDelta();

// Total detents since boot.
int32_t count();

}  // namespace encoder
}  // namespace hal
