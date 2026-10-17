#include "ui/screens/ControlScreen.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include "ui/Widgets.h"

namespace ui {
namespace controlScreen {

namespace {

constexpr int32_t kArcSize = 360;
constexpr int32_t kArcWidth = 14;
constexpr int32_t kArcSteps = 1000;  // arc resolution (the view's range is mapped onto it)
constexpr int32_t kValueWidth = 300;
constexpr int32_t kValueY = -10;
constexpr uint32_t kTextColor = 0xFFFFFF;
constexpr uint32_t kDimColor = 0x707070;
constexpr uint32_t kArcColor = 0xFFA000;
constexpr uint32_t kArcUnconfirmedColor = 0xFFFFFF;  // a local value HA has not confirmed (Q7-6)
constexpr uint32_t kArcDimColor = 0x505050;
constexpr uint32_t kArcTrackColor = 0x282828;
constexpr int32_t kStateY = 50;
constexpr int32_t kStateWidth = 270;
// The state line as the "HA not Ready" banner (plan-step8.md Q8-15): a pill in the same slot.
constexpr uint32_t kBannerColor = 0x7A1F1F;
constexpr int32_t kBannerRadius = 12;
constexpr int32_t kBannerPadHor = 12;
constexpr int32_t kBannerPadVer = 4;

lv_obj_t *arc = nullptr;
lv_obj_t *titleLabel = nullptr;
lv_obj_t *valueLabel = nullptr;
lv_obj_t *stateLabel = nullptr;
lv_obj_t *pageLabel = nullptr;

// What the widgets show now (so unchanged texts are not set again).
std::string shownTitle;
std::string shownValue;
std::string shownState;
std::string shownPage;
int32_t shownArcValue = -1;
bool shownArcVisible = true;
bool shownDimmed = false;
bool shownBanner = false;
uint32_t shownArcColor = kArcColor;
const lv_font_t *shownValueFont = &ui_font_56;

void setText(lv_obj_t *label, std::string &shown, const std::string &text) {
    if (text != shown) {
        shown = text;
        lv_label_set_text(label, text.c_str());
    }
}

// The value in font 56, or in font 28 when it is wider than the label (long states of the
// action "none", e.g. "heat_cool"); still too wide ends in "...".
void setValueText(const std::string &text) {
    if (text == shownValue) {
        return;
    }
    shownValue = text;
    lv_point_t size;
    lv_text_get_size(&size, text.c_str(), &ui_font_56, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    const lv_font_t *font = size.x > kValueWidth ? &ui_font_28 : &ui_font_56;
    if (font != shownValueFont) {
        shownValueFont = font;
        lv_obj_set_style_text_font(valueLabel, font, 0);
        lv_obj_set_height(valueLabel, lv_font_get_line_height(font));
        lv_obj_align(valueLabel, LV_ALIGN_CENTER, 0, kValueY);
    }
    lv_label_set_text(valueLabel, text.c_str());
}

int32_t arcPosition(const core::presenter::ControlView &view) {
    const double span = view.arcMax - view.arcMin;
    if (!(span > 0) || !std::isfinite(view.arcValue)) {
        return 0;
    }
    const double fraction = std::min(std::max((view.arcValue - view.arcMin) / span, 0.0), 1.0);
    return static_cast<int32_t>(std::lround(fraction * kArcSteps));
}

// Title, value and (unless it is the banner, which is always white) the state line.
void setDimmed(bool dimmed, bool banner) {
    const lv_color_t text = lv_color_hex(dimmed ? kDimColor : kTextColor);
    lv_obj_set_style_text_color(titleLabel, text, 0);
    lv_obj_set_style_text_color(valueLabel, text, 0);
    lv_obj_set_style_text_color(stateLabel, banner ? lv_color_hex(kTextColor) : text, 0);
}

// Shows or hides the pill behind the state line. Background colour and radius are set once
// in create(); the height grows by the padding so the text keeps its one-line height and
// the line stays centred at kStateY.
void setBanner(bool banner) {
    const int32_t padHor = banner ? kBannerPadHor : 0;
    const int32_t padVer = banner ? kBannerPadVer : 0;
    lv_obj_set_style_bg_opa(stateLabel, banner ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_left(stateLabel, padHor, 0);
    lv_obj_set_style_pad_right(stateLabel, padHor, 0);
    lv_obj_set_style_pad_top(stateLabel, padVer, 0);
    lv_obj_set_style_pad_bottom(stateLabel, padVer, 0);
    lv_obj_set_height(stateLabel, lv_font_get_line_height(&ui_font_20) + 2 * padVer);
    lv_obj_align(stateLabel, LV_ALIGN_CENTER, 0, kStateY);
}

// The arc indicator (plan-step7.md §3.6, Q7-6): a hue's own colour, else grey when dimmed
// (also mid-turn, e.g. HA dropped), else white while the value is unconfirmed, else amber.
uint32_t arcColor(const core::presenter::ControlView &view) {
    if (view.arcRgb) {
        return *view.arcRgb;
    }
    if (view.dimmed) {
        return kArcDimColor;
    }
    return view.unconfirmed ? kArcUnconfirmedColor : kArcColor;
}

}  // namespace

lv_obj_t *create() {
    lv_obj_t *scr = createScreen();

    arc = lv_arc_create(scr);
    lv_obj_set_size(arc, kArcSize, kArcSize);
    lv_obj_center(arc);
    lv_arc_set_rotation(arc, 135);  // the 90° gap at the bottom, for the page indicator
    lv_arc_set_bg_angles(arc, 0, 270);
    lv_arc_set_range(arc, 0, kArcSteps);
    lv_arc_set_value(arc, 0);
    lv_obj_remove_style(arc, nullptr, LV_PART_KNOB);
    lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);  // read-only; touch gives swipes only
    lv_obj_set_style_arc_width(arc, kArcWidth, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, kArcWidth, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, lv_color_hex(kArcTrackColor), LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, lv_color_hex(kArcColor), LV_PART_INDICATOR);
    shownArcValue = 0;

    titleLabel = addClippedLabel(scr, &ui_font_28, -95, 250, "");
    valueLabel = addClippedLabel(scr, &ui_font_56, kValueY, kValueWidth, "");
    stateLabel = addClippedLabel(scr, &ui_font_20, kStateY, kStateWidth, "");
    lv_obj_set_style_bg_color(stateLabel, lv_color_hex(kBannerColor), 0);
    lv_obj_set_style_radius(stateLabel, kBannerRadius, 0);
    pageLabel = addCenteredLabel(scr, &ui_font_20, 150, "");
    return scr;
}

void show(const core::presenter::ControlView &view, size_t index, size_t count) {
    if (arc == nullptr) {
        return;
    }
    setText(titleLabel, shownTitle, view.title);
    setValueText(view.value + view.unit);
    setText(stateLabel, shownState, view.stateText);
    char page[24];
    std::snprintf(page, sizeof(page), "%u/%u", static_cast<unsigned>(index + 1),
                  static_cast<unsigned>(count));
    setText(pageLabel, shownPage, page);

    if (view.hasArc != shownArcVisible) {
        shownArcVisible = view.hasArc;
        if (view.hasArc) {
            lv_obj_remove_flag(arc, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(arc, LV_OBJ_FLAG_HIDDEN);
        }
    }
    const int32_t position = view.hasArc ? arcPosition(view) : 0;
    if (position != shownArcValue) {
        shownArcValue = position;
        lv_arc_set_value(arc, position);
    }
    if (view.banner != shownBanner) {
        setBanner(view.banner);
    }
    if (view.dimmed != shownDimmed || view.banner != shownBanner) {
        shownDimmed = view.dimmed;
        shownBanner = view.banner;
        setDimmed(view.dimmed, view.banner);
    }
    const uint32_t color = arcColor(view);
    if (color != shownArcColor) {
        shownArcColor = color;
        lv_obj_set_style_arc_color(arc, lv_color_hex(color), LV_PART_INDICATOR);
    }
}

}  // namespace controlScreen
}  // namespace ui
