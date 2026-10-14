#include "hal/Button.h"

#include <Arduino.h>

#include "board_pins.h"

namespace hal {
namespace button {

namespace {
core::ButtonFsm fsm;
}  // namespace

void begin() {
    pinMode(pins::kEncKey, INPUT_PULLUP);
}

core::ButtonFsm::Event poll(uint32_t nowMs) {
    return fsm.update(digitalRead(pins::kEncKey) == LOW, nowMs);
}

}  // namespace button
}  // namespace hal
