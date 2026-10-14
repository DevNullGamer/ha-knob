#pragma once

#include <cstdint>
#include <lvgl.h>

// Status screen: Wi-Fi state (not configured until Step 4), plus input feedback while
// there is nothing else to control (encoder count, last button/touch event), and a
// Danish test string to verify the fonts.
namespace ui {
namespace statusScreen {

lv_obj_t *create();

void setEncoderCount(int32_t count);

// Short description of the last input, e.g. "short press" or "swipe left".
void setLastInput(const char *text);

}  // namespace statusScreen
}  // namespace ui
