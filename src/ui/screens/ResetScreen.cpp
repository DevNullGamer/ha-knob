#include "ui/screens/ResetScreen.h"

#include <cstdio>

#include "ui/Widgets.h"

namespace ui {
namespace resetScreen {

namespace {
constexpr uint32_t kTitleColor = 0xE53935;  // red accent
lv_obj_t *titleLabel = nullptr;
lv_obj_t *pressLabel = nullptr;
lv_obj_t *turnLabel = nullptr;
lv_obj_t *countLabel = nullptr;
}  // namespace

lv_obj_t *create() {
    lv_obj_t *scr = createScreen();
    titleLabel = addCenteredLabel(scr, &ui_font_28, -90, "Factory reset?");
    lv_obj_set_style_text_color(titleLabel, lv_color_hex(kTitleColor), 0);
    pressLabel = addCenteredLabel(scr, &ui_font_20, -40, "Press to erase all settings");
    turnLabel = addCenteredLabel(scr, &ui_font_20, -10, "Turn to cancel");
    countLabel = addCenteredLabel(scr, &ui_font_56, 60, "10");
    return scr;
}

void showQuestion() {
    if (titleLabel == nullptr) {
        return;
    }
    lv_label_set_text(titleLabel, "Factory reset?");
    lv_obj_align(titleLabel, LV_ALIGN_CENTER, 0, -90);
    lv_obj_remove_flag(pressLabel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(turnLabel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(countLabel, LV_OBJ_FLAG_HIDDEN);
}

void setSecondsLeft(uint32_t seconds) {
    if (countLabel == nullptr) {
        return;
    }
    char text[12];
    snprintf(text, sizeof(text), "%u", static_cast<unsigned>(seconds));
    lv_label_set_text(countLabel, text);
}

void showResetting() {
    if (titleLabel == nullptr) {
        return;
    }
    lv_label_set_text(titleLabel, "Resetting...");
    lv_obj_align(titleLabel, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(pressLabel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(turnLabel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(countLabel, LV_OBJ_FLAG_HIDDEN);
}

}  // namespace resetScreen
}  // namespace ui
