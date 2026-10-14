#include "ui/Ui.h"

#include <lvgl.h>

#include "ui/screens/BootScreen.h"
#include "ui/screens/StatusScreen.h"

namespace ui {

namespace {

constexpr uint32_t kBootScreenMs = 1500;

lv_obj_t *bootScr = nullptr;
lv_obj_t *statusScr = nullptr;
bool booting = false;
uint32_t bootStartMs = 0;

}  // namespace

void begin(uint32_t nowMs) {
    bootScr = bootScreen::create();
    statusScr = statusScreen::create();
    lv_screen_load(bootScr);
    booting = true;
    bootStartMs = nowMs;
}

void tick(uint32_t nowMs) {
    if (booting && nowMs - bootStartMs >= kBootScreenMs) {
        booting = false;
        lv_screen_load_anim(statusScr, LV_SCR_LOAD_ANIM_FADE_IN, 300, 0, true /* delete boot */);
        bootScr = nullptr;
    }
}

void showEncoderCount(int32_t count) {
    statusScreen::setEncoderCount(count);
}

void showLastInput(const char *text) {
    statusScreen::setLastInput(text);
}

}  // namespace ui
