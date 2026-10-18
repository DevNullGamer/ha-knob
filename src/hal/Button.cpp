#include "hal/Button.h"

#include <Arduino.h>

#include "board_pins.h"

namespace hal {
namespace button {

namespace {
core::ButtonFsm fsm;
bool upTracked = false;  // the raw level was up at the previous poll
uint32_t upSinceMs = 0;  // since when the raw level is up
bool released = false;   // releaseSeen()
}  // namespace

void begin() {
    pinMode(pins::kEncKey, INPUT_PULLUP);
}

core::ButtonFsm::Event poll(uint32_t nowMs) {
    const bool rawDown = digitalRead(pins::kEncKey) == LOW;
    if (rawDown) {
        upTracked = false;
    } else if (!upTracked) {
        upTracked = true;
        upSinceMs = nowMs;
    } else if (!released && nowMs - upSinceMs >= core::ButtonFsm::kDebounceMs) {
        released = true;
    }
    return fsm.update(rawDown, nowMs);
}

bool isDown() { return fsm.isDown(); }

bool releaseSeen() { return released; }

}  // namespace button
}  // namespace hal
