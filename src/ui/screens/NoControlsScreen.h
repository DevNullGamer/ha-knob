#pragma once

#include <lvgl.h>

// Shown instead of the control pages while no control is configured (plan-step6.md §3.7):
// "No controls yet", where to add them, and the Home Assistant line.
namespace ui {
namespace noControlsScreen {

lv_obj_t *create();

// Where the web UI is, e.g. "Open http://192.168.1.143" (the next line reads
// "to add controls").
void setAddress(const char *line);

// Home Assistant line, e.g. "HA: connected (2026.10.1)".
void setHa(const char *haLine);

}  // namespace noControlsScreen
}  // namespace ui
