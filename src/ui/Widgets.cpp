#include "ui/Widgets.h"

namespace ui {

lv_obj_t *createScreen() {
    lv_obj_t *scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    return scr;
}

lv_obj_t *addCenteredLabel(lv_obj_t *parent, const lv_font_t *font, int32_t yOffset,
                           const char *text) {
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(label, text);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, yOffset);
    return label;
}

lv_obj_t *addClippedLabel(lv_obj_t *parent, const lv_font_t *font, int32_t yOffset,
                          int32_t maxWidth, const char *text) {
    lv_obj_t *label = addCenteredLabel(parent, font, yOffset, text);
    lv_obj_set_width(label, maxWidth);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, yOffset);
    return label;
}

}  // namespace ui
