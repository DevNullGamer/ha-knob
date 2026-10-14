#pragma once

#include <lvgl.h>

// Small helpers shared by the screens.
namespace ui {

// A black, non-scrollable screen object.
lv_obj_t *createScreen();

// A white label centred horizontally, yOffset px from the screen centre.
lv_obj_t *addCenteredLabel(lv_obj_t *parent, const lv_font_t *font, int32_t yOffset,
                           const char *text);

}  // namespace ui
