#pragma once

#include <cstdint>

// GPIO map of the LilyGo T-Encoder Pro (ESP32-S3R8).
// Facts taken from the vendor documentation; this file is our own.
// Pin numbers are plain ints so they can be used with both Arduino and ESP-IDF APIs.

namespace pins {

// AMOLED panel, QSPI (390x390, CO5300 or SH8601 controller)
constexpr int kLcdCs = 10;
constexpr int kLcdSclk = 12;
constexpr int kLcdSdio0 = 11;
constexpr int kLcdSdio1 = 13;
constexpr int kLcdSdio2 = 7;
constexpr int kLcdSdio3 = 14;
constexpr int kLcdRst = 4;
constexpr int kLcdVciEn = 3;  // Panel power enable, must be driven HIGH before display init

constexpr int kLcdWidth = 390;
constexpr int kLcdHeight = 390;

// Capacitive touch, I2C (CST816 at 0x15 or CHSC5816 at 0x2E)
constexpr int kTouchSda = 5;
constexpr int kTouchScl = 6;
constexpr int kTouchRst = 8;
constexpr int kTouchInt = 9;
constexpr uint8_t kTouchAddrCst816 = 0x15;
constexpr uint8_t kTouchAddrChsc5816 = 0x2E;

// Rotary encoder with push button
constexpr int kEncA = 1;
constexpr int kEncB = 2;
constexpr int kEncKey = 0;  // Active low (INPUT_PULLUP). IO0 is the BOOT strapping pin.

// Buzzer (LEDC PWM, about 2 kHz)
constexpr int kBuzzer = 17;

}  // namespace pins
