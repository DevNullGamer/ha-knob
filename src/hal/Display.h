#pragma once

#include <cstdint>

// AMOLED panel (Arduino_GFX CO5300 over QSPI) and the LVGL 9 display driver.
namespace hal {
namespace display {

// Powers the panel, initialises the controller and LVGL (partial render, two
// 390x40 px buffers in internal RAM, even-area rounding). Returns false on failure.
bool begin();

// Panel brightness, 0 to 100 % (mapped to the controller's 0 to 255).
void setBrightnessPercent(int32_t percent);

}  // namespace display
}  // namespace hal
