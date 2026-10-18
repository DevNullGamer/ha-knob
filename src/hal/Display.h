#pragma once

#include <cstdint>

// AMOLED panel (Arduino_GFX CO5300 over QSPI) and the LVGL 9 display driver.
namespace hal {
namespace display {

// Powers the panel, initialises the controller and LVGL (partial render, one 390x80 px
// buffer in internal RAM, else two 390x40 px with "ERR draw buffer fallback"; even-area
// rounding). Returns false on failure.
bool begin();

// Panel brightness, 0 to 100 % (mapped to the controller's 0 to 255).
void setBrightnessPercent(int32_t percent);

// Panel output on / off: DISPON (0x29) / DISPOFF (0x28) without SLPIN / SLPOUT, so no
// delay (plan-step8.md Q8-5). The panel keeps its RAM; LVGL keeps rendering while off.
// Call only on a change; set the brightness to 0 before turning it off.
void setPanelOn(bool on);

// Time spent in the LVGL flush callback (panel transfer, microseconds) and the pixels it
// flushed since the previous call; resets both. Main loop only (plan-step8.md §3.6).
void takeFlushStats(uint32_t *us, uint32_t *px);

}  // namespace display
}  // namespace hal
