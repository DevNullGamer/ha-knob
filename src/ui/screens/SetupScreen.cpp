#include "ui/screens/SetupScreen.h"

#include "ui/Widgets.h"

namespace ui {
namespace setupScreen {

namespace {
constexpr int32_t kTextWidth = 280;  // fits the round panel at these heights
lv_obj_t *ssidLabel = nullptr;
lv_obj_t *passwordLabel = nullptr;
lv_obj_t *statusLabel = nullptr;
}  // namespace

lv_obj_t *create() {
    lv_obj_t *scr = createScreen();
    addCenteredLabel(scr, &ui_font_28, -125, "Setup");
    addCenteredLabel(scr, &ui_font_20, -85, "Join Wi-Fi");
    ssidLabel = addClippedLabel(scr, &ui_font_28, -52, kTextWidth + 40, "");
    addCenteredLabel(scr, &ui_font_20, -15, "Password");
    passwordLabel = addCenteredLabel(scr, &ui_font_28, 18, "");
    addCenteredLabel(scr, &ui_font_20, 58, "then open");
    addCenteredLabel(scr, &ui_font_20, 88, "http://192.168.4.1");
    statusLabel = addClippedLabel(scr, &ui_font_20, 128, kTextWidth - 30, "");
    return scr;
}

void setAccessPoint(const char *ssid, const char *password) {
    if (ssidLabel != nullptr) {
        lv_label_set_text(ssidLabel, ssid);
    }
    if (passwordLabel != nullptr) {
        lv_label_set_text(passwordLabel, password);
    }
}

void setStatusLine(const char *text) {
    if (statusLabel != nullptr) {
        lv_label_set_text(statusLabel, text);
    }
}

}  // namespace setupScreen
}  // namespace ui
