#pragma once

#include <cstddef>
#include <lvgl.h>
#include <string>
#include <vector>

// Select screen (plan-step7.md §3.6, Q7-9): a list (lv_roller, not clickable) of the control
// titles followed by "Status". The selection is driven by the knob through ui::; the
// roller never takes touch input.
namespace ui {
namespace selectScreen {

lv_obj_t *create();

// Replaces the items with `titles` plus "Status". Control characters (incl. '\n') become
// spaces; a title wider than the list ends in "…".
void setItems(const std::vector<std::string> &titles);

// Shows item `index` as the selected one (no animation).
void setSelected(size_t index);

}  // namespace selectScreen
}  // namespace ui
