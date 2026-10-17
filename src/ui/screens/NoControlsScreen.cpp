#include "ui/screens/NoControlsScreen.h"

#include "ui/Widgets.h"

namespace ui {
namespace noControlsScreen {

namespace {
lv_obj_t *addressLabel = nullptr;
lv_obj_t *haLabel = nullptr;
}  // namespace

lv_obj_t *create() {
    lv_obj_t *scr = createScreen();
    addCenteredLabel(scr, &ui_font_28, -60, "No controls yet");
    addressLabel = addClippedLabel(scr, &ui_font_20, -10, 320, "Open the web UI");
    addCenteredLabel(scr, &ui_font_20, 20, "to add controls");
    haLabel = addClippedLabel(scr, &ui_font_20, 70, 320, "HA: not configured");
    return scr;
}

void setAddress(const char *line) {
    if (addressLabel != nullptr) {
        lv_label_set_text(addressLabel, line);
    }
}

void setHa(const char *haLine) {
    if (haLabel != nullptr) {
        lv_label_set_text(haLabel, haLine);
    }
}

}  // namespace noControlsScreen
}  // namespace ui
