#include "ui/screens/StatusScreen.h"

#include "ui/Widgets.h"

namespace ui {
namespace statusScreen {

namespace {
lv_obj_t *wifiLabel = nullptr;
lv_obj_t *ipLabel = nullptr;
lv_obj_t *hostLabel = nullptr;
lv_obj_t *haLabel = nullptr;
lv_obj_t *countLabel = nullptr;
lv_obj_t *lastInputLabel = nullptr;
}  // namespace

lv_obj_t *create() {
    lv_obj_t *scr = createScreen();
    addCenteredLabel(scr, &ui_font_28, -140, "HA Knob");
    wifiLabel = addClippedLabel(scr, &ui_font_20, -102, 300, "Wi-Fi: not configured");
    ipLabel = addClippedLabel(scr, &ui_font_20, -72, 320, "");
    hostLabel = addClippedLabel(scr, &ui_font_20, -42, 330, "");
    haLabel = addClippedLabel(scr, &ui_font_20, -12, 340, "HA: not configured");
    countLabel = addCenteredLabel(scr, &ui_font_56, 35, "0");
    lastInputLabel = addCenteredLabel(scr, &ui_font_20, 88, "Turn, press or swipe");
    addCenteredLabel(scr, &ui_font_28, 126, "Køkken Æble Ørn");  // font check (Danish)
    return scr;
}

void setNetwork(const char *wifiLine, const char *ipLine, const char *hostLine) {
    if (wifiLabel != nullptr) {
        lv_label_set_text(wifiLabel, wifiLine);
        lv_label_set_text(ipLabel, ipLine);
        lv_label_set_text(hostLabel, hostLine);
    }
}

void setHa(const char *haLine) {
    if (haLabel != nullptr) {
        lv_label_set_text(haLabel, haLine);
    }
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
