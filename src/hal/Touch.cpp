#include "hal/Touch.h"

#include <Arduino.h>
#include <TouchDrvCHSC5816.hpp>
#include <TouchDrvCSTXXX.hpp>
#include <Wire.h>

#include "board_pins.h"
#include "util/MathUtil.h"

namespace hal {
namespace touch {

namespace {

constexpr uint32_t kPollMs = 20;

Variant currentVariant = Variant::None;
TouchDrvCSTXXX cst816;
TouchDrvCHSC5816 chsc5816;
core::SwipeClassifier classifier;
uint32_t lastPollMs = 0;

bool i2cProbe(uint8_t addr) {
    Wire.beginTransmission(addr);
    return Wire.endTransmission() == 0;
}

// Returns true and the clamped point while the panel is touched.
bool readPoint(int16_t &x, int16_t &y) {
    uint8_t n = 0;
    if (currentVariant == Variant::Cst816) {
        n = cst816.getPoint(&x, &y, 1);
    } else if (currentVariant == Variant::Chsc5816) {
        n = chsc5816.getPoint(&x, &y, 1);
    }
    if (n == 0) {
        return false;
    }
    x = static_cast<int16_t>(core::clampInt(x, 0, pins::kLcdWidth - 1));
    y = static_cast<int16_t>(core::clampInt(y, 0, pins::kLcdHeight - 1));
    return true;
}

}  // namespace

void begin() {
    // Reset the controller so it is awake (CST816 does not ACK while in auto-sleep).
    pinMode(pins::kTouchRst, OUTPUT);
    digitalWrite(pins::kTouchRst, LOW);
    delay(10);
    digitalWrite(pins::kTouchRst, HIGH);
    delay(50);

    // The bus is started here; the drivers get sda/scl = -1 so they do not re-pin it.
    Wire.begin(pins::kTouchSda, pins::kTouchScl);
    if (i2cProbe(pins::kTouchAddrCst816)) {
        cst816.setTouchDrvModel(TouchDrv_CST8XX);  // skip auto-detect (avoids a CST226 probe error)
        cst816.setPins(pins::kTouchRst, pins::kTouchInt);
        if (cst816.begin(Wire, pins::kTouchAddrCst816, -1, -1)) {
            cst816.setMaxCoordinates(pins::kLcdWidth, pins::kLcdHeight);
            currentVariant = Variant::Cst816;
        }
    } else if (i2cProbe(pins::kTouchAddrChsc5816)) {
        chsc5816.setPins(pins::kTouchRst, pins::kTouchInt);
        if (chsc5816.begin(Wire, pins::kTouchAddrChsc5816, -1, -1)) {
            currentVariant = Variant::Chsc5816;
        }
    }
}

Variant variant() {
    return currentVariant;
}

const char *variantName() {
    switch (currentVariant) {
        case Variant::Cst816: return "CST816";
        case Variant::Chsc5816: return "CHSC5816";
        default: return "none";
    }
}

core::Swipe poll(uint32_t nowMs) {
    if (currentVariant == Variant::None || nowMs - lastPollMs < kPollMs) {
        return core::Swipe::None;
    }
    lastPollMs = nowMs;
    int16_t x = 0;
    int16_t y = 0;
    const bool touched = readPoint(x, y);
    return classifier.update(touched, x, y, nowMs);
}

}  // namespace touch
}  // namespace hal
