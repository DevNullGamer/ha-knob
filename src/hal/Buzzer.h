#pragma once

#include <cstdint>

// Buzzer on IO17 (LEDC PWM, core-2.x API, fixed 2 kHz). Beeps are non-blocking.
//
// Serial line (test builds only, -DHAKNOB_TEST_INPUT): every beep request logs
//   BUZ ms=<duration> started=<0|1>   (0 = a running beep already lasts longer)
namespace hal {
namespace buzzer {

void begin();

// Requests a beep of durationMs; loop() ends it. Never shortens a running beep: if one
// runs and ends after nowMs + durationMs nothing changes, otherwise it starts or is
// extended to that end (plan-step8.md §3.5).
void beep(uint32_t nowMs, uint32_t durationMs = 30);

// The detent click: a 5 ms beep, at most one per 40 ms (calls inside that window are
// dropped without a beep request).
void click(uint32_t nowMs);

// Call every loop.
void loop(uint32_t nowMs);

}  // namespace buzzer
}  // namespace hal
