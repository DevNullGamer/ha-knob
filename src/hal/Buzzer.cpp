#include "hal/Buzzer.h"

#include <Arduino.h>

#include "board_pins.h"
#if defined(HAKNOB_TEST_INPUT)
#include "util/Log.h"
#endif

namespace hal {
namespace buzzer {

namespace {

constexpr uint32_t kFreqHz = 2000;
constexpr uint8_t kLedcChannel = 0;
constexpr uint8_t kResolutionBits = 8;
constexpr uint32_t kDutyHalf = 128;  // 50 % at 8 bits
constexpr uint32_t kClickMs = 5;          // plan-step8.md Q8-11
constexpr uint32_t kClickMinGapMs = 40;

bool beeping = false;
uint32_t beepEndMs = 0;
bool clicked = false;  // a click was requested (at lastClickMs)
uint32_t lastClickMs = 0;

}  // namespace

void begin() {
    ledcSetup(kLedcChannel, kFreqHz, kResolutionBits);
    ledcAttachPin(pins::kBuzzer, kLedcChannel);
    ledcWrite(kLedcChannel, 0);
}

void beep(uint32_t nowMs, uint32_t durationMs) {
    const uint32_t endMs = nowMs + durationMs;
    // A running beep that ends later wins (e.g. a click during the 150 ms error beep).
    const bool started = !beeping || static_cast<int32_t>(beepEndMs - endMs) <= 0;
#if defined(HAKNOB_TEST_INPUT)
    util::logPrintf("BUZ ms=%u started=%d\n", static_cast<unsigned>(durationMs), started ? 1 : 0);
#endif
    if (!started) {
        return;
    }
    if (!beeping) {
        ledcWrite(kLedcChannel, kDutyHalf);
        beeping = true;
    }
    beepEndMs = endMs;
}

void click(uint32_t nowMs) {
    if (clicked && nowMs - lastClickMs < kClickMinGapMs) {
        return;
    }
    clicked = true;
    lastClickMs = nowMs;
    beep(nowMs, kClickMs);
}

void loop(uint32_t nowMs) {
    if (beeping && static_cast<int32_t>(nowMs - beepEndMs) >= 0) {
        ledcWrite(kLedcChannel, 0);
        beeping = false;
    }
}

}  // namespace buzzer
}  // namespace hal
