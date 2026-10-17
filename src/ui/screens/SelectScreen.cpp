#include "ui/screens/SelectScreen.h"

#include "ui/Widgets.h"
#include "util/Text.h"

namespace ui {
namespace selectScreen {

namespace {

constexpr int32_t kWidth = 300;
constexpr int32_t kPadHor = 12;
constexpr int32_t kLineSpace = 12;
constexpr uint32_t kVisibleRows = 5;
constexpr uint32_t kTextColor = 0x909090;
constexpr uint32_t kSelectedColor = 0xFFA000;
constexpr uint32_t kSelectedBgColor = 0x202020;
constexpr size_t kMaxItemBytes = 64;  // far more than one 276 px line of font 28 holds
const char *const kEllipsis = "\xE2\x80\xA6";  // U+2026, in the project fonts

lv_obj_t *roller = nullptr;

int32_t textWidth(const std::string &text) {
    lv_point_t size;
    lv_text_get_size(&size, text.c_str(), &ui_font_28, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return size.x;
}

// One roller line: control characters as spaces, cut (UTF-8-safe) with "…" to the width.
std::string itemText(const std::string &title) {
    // Pre-cut: more never fits the width, and the measure loop below is quadratic.
    std::string text = core::util::truncateUtf8(title, kMaxItemBytes);
    for (char &c : text) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x20 || u == 0x7F) {
            c = ' ';
        }
    }
    constexpr int32_t maxWidth = kWidth - 2 * kPadHor;
    if (textWidth(text) <= maxWidth) {
        return text;
    }
    while (!text.empty()) {
        // Drop the last UTF-8 character (continuation bytes 10xxxxxx, then its lead byte).
        while (!text.empty() && (static_cast<unsigned char>(text.back()) & 0xC0) == 0x80) {
            text.pop_back();
        }
        if (!text.empty()) {
            text.pop_back();
        }
        if (textWidth(text + kEllipsis) <= maxWidth) {
            break;
        }
    }
    return text + kEllipsis;
}

}  // namespace

lv_obj_t *create() {
    lv_obj_t *scr = createScreen();

    roller = lv_roller_create(scr);
    lv_obj_remove_flag(roller, LV_OBJ_FLAG_CLICKABLE);  // the knob drives it; taps never select
    lv_obj_set_width(roller, kWidth);
    lv_obj_set_style_text_font(roller, &ui_font_28, 0);
    lv_obj_set_style_text_line_space(roller, kLineSpace, 0);
    lv_obj_set_style_text_align(roller, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(roller, lv_color_hex(kTextColor), 0);
    lv_obj_set_style_bg_color(roller, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(roller, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(roller, 0, 0);
    lv_obj_set_style_pad_hor(roller, kPadHor, 0);
    lv_obj_set_style_text_color(roller, lv_color_hex(kSelectedColor), LV_PART_SELECTED);
    lv_obj_set_style_bg_color(roller, lv_color_hex(kSelectedBgColor), LV_PART_SELECTED);
    lv_obj_set_style_bg_opa(roller, LV_OPA_COVER, LV_PART_SELECTED);
    lv_roller_set_options(roller, "Status", LV_ROLLER_MODE_NORMAL);
    lv_roller_set_visible_row_count(roller, kVisibleRows);
    lv_obj_center(roller);
    return scr;
}

void setItems(const std::vector<std::string> &titles) {
    if (roller == nullptr) {
        return;
    }
    std::string options;
    for (const std::string &title : titles) {
        options += itemText(title);
        options += '\n';
    }
    options += "Status";
    lv_roller_set_options(roller, options.c_str(), LV_ROLLER_MODE_NORMAL);
}

void setSelected(size_t index) {
    if (roller != nullptr) {
        lv_roller_set_selected(roller, static_cast<uint32_t>(index), LV_ANIM_OFF);
    }
}

}  // namespace selectScreen
}  // namespace ui
