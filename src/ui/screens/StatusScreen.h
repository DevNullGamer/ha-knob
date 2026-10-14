#pragma once

#include <cstdint>
#include <lvgl.h>

// Status screen: Wi-Fi state with the addresses of the web UI, plus input feedback while
// there is nothing else to control (encoder count, last button/touch event), and a
// Danish test string to verify the fonts.
namespace ui {
namespace statusScreen {

lv_obj_t *create();

// Wi-Fi state line (e.g. "Wi-Fi: MyNetwork") and the two web UI addresses ("" = empty).
void setNetwork(const char *wifiLine, const char *ipLine, const char *hostLine);

void setEncoderCount(int32_t count);

// Short description of the last input, e.g. "short press" or "swipe left".
void setLastInput(const char *text);

}  // namespace statusScreen
}  // namespace ui
