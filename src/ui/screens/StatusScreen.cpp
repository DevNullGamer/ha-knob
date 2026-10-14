#include "ui/screens/StatusScreen.h"

#include "ui/Widgets.h"
#include "ui/fonts/Fonts.h"

namespace ui {
namespace statusScreen {

namespace {
lv_obj_t *countLabel = nullptr;
lv_obj_t *lastInputLabel = nullptr;
}  // namespace

lv_obj_t *create() {
    lv_obj_t *scr = createScreen();
    addCenteredLabel(scr, &ui_font_28, -120, "HA Knob");
    addCenteredLabel(scr, &ui_font_20, -80, "Wi-Fi: not configured");
    addCenteredLabel(scr, &ui_font_28, -35, "Køkken Æble Ørn");  // font check (Danish)
    countLabel = addCenteredLabel(scr, &ui_font_56, 30, "0");
    lastInputLabel = addCenteredLabel(scr, &ui_font_20, 95, "Turn, press or swipe");
    return scr;
}

void setEncoderCount(int32_t count) {
    if (countLabel != nullptr) {
        lv_label_set_text_fmt(countLabel, "%d", static_cast<int>(count));
    }
}

void setLastInput(const char *text) {
    if (lastInputLabel != nullptr) {
        lv_label_set_text(lastInputLabel, text);
    }
}

}  // namespace statusScreen
}  // namespace ui
