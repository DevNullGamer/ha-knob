#pragma once

#include <lvgl.h>

// Small helpers shared by the screens.
namespace ui {

// A black, non-scrollable screen object.
lv_obj_t *createScreen();

// A white label centred horizontally, yOffset px from the screen centre.
lv_obj_t *addCenteredLabel(lv_obj_t *parent, const lv_font_t *font, int32_t yOffset,
                           const char *text);

// Like addCenteredLabel, but a single line maxWidth px wide (text centred in it) and one
// line of `font` high; text that does not fit shows its first wrapped line ending in "..."
// (LVGL wraps at spaces). For single-line texts; a later font change must set the height
// again.
lv_obj_t *addClippedLabel(lv_obj_t *parent, const lv_font_t *font, int32_t yOffset,
                          int32_t maxWidth, const char *text);

}  // namespace ui
