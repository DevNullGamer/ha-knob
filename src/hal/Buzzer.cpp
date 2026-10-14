#include "hal/Buzzer.h"

#include <Arduino.h>

#include "board_pins.h"

namespace hal {
namespace buzzer {

namespace {

constexpr uint32_t kFreqHz = 2000;
constexpr uint8_t kLedcChannel = 0;
constexpr uint8_t kResolutionBits = 8;
constexpr uint32_t kDutyHalf = 128;  // 50 % at 8 bits

bool beeping = false;
uint32_t beepEndMs = 0;

}  // namespace

void begin() {
    ledcSetup(kLedcChannel, kFreqHz, kResolutionBits);
    ledcAttachPin(pins::kBuzzer, kLedcChannel);
    ledcWrite(kLedcChannel, 0);
}

void beep(uint32_t nowMs, uint32_t durationMs) {
    ledcWrite(kLedcChannel, kDutyHalf);
    beeping = true;
    beepEndMs = nowMs + durationMs;
}

void loop(uint32_t nowMs) {
    if (beeping && static_cast<int32_t>(nowMs - beepEndMs) >= 0) {
        ledcWrite(kLedcChannel, 0);
        beeping = false;
    }
}

}  // namespace buzzer
}  // namespace hal
