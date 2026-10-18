#pragma once

#include <lvgl.h>

#include <cstdint>

// Factory-reset confirmation (plan-step8.md Q8-12): "Factory reset?" (red), "Press to erase
// all settings", "Turn to cancel" and the seconds left; after the confirming press only
// "Resetting...".
namespace ui {
namespace resetScreen {

lv_obj_t *create();

// Back to the question (each time the confirmation opens).
void showQuestion();
// The countdown number.
void setSecondsLeft(uint32_t seconds);
// The "Resetting..." state (until the reboot).
void showResetting();

}  // namespace resetScreen
}  // namespace ui
