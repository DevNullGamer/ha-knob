#pragma once

#include <cstdint>

// Screen manager. All calls must come from the main loop (LVGL is not thread-safe).
namespace ui {

// Creates the screens and shows the boot screen. Requires hal::display::begin().
void begin(uint32_t nowMs);

// Switches from the boot screen to the status screen after a short delay.
void tick(uint32_t nowMs);

void showEncoderCount(int32_t count);
void showLastInput(const char *text);

}  // namespace ui
