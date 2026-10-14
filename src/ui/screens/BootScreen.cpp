#include "ui/screens/BootScreen.h"

#include "ui/Widgets.h"
#include "ui/fonts/Fonts.h"
#include "version.h"

namespace ui {
namespace bootScreen {

lv_obj_t *create() {
    lv_obj_t *scr = createScreen();
    addCenteredLabel(scr, &ui_font_56, -30, "HA Knob");
    addCenteredLabel(scr, &ui_font_20, 30, "v" FW_VERSION);
    addCenteredLabel(scr, &ui_font_20, 70, "Starting...");
    return scr;
}

}  // namespace bootScreen
}  // namespace ui
