#pragma once

#include <cstdint>

// Buzzer on IO17 (LEDC PWM, core-2.x API). Beeps are non-blocking.
namespace hal {
namespace buzzer {

void begin();

// Starts a beep of durationMs; loop() ends it.
void beep(uint32_t nowMs, uint32_t durationMs = 30);

// Call every loop.
void loop(uint32_t nowMs);

}  // namespace buzzer
}  // namespace hal
