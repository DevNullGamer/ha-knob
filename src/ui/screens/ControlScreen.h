#pragma once

#include <cstddef>
#include <lvgl.h>

#include "ui/Presenter.h"

// Control screen (plan-step6.md §3.7): the control's title, a 270° arc (not clickable;
// amber, grey when dimmed, white while the value is unconfirmed, the hue's own colour for
// the hue action; plan-step7.md Q7-6), the value with its unit, the state line and the page
// indicator "i/N". While HA is not Ready the state line is a dark red pill with white text
// (the banner, plan-step8.md Q8-15). One screen object serves every control page; labels
// and the arc are only touched when what they show changes.
namespace ui {
namespace controlScreen {

lv_obj_t *create();

// Shows the view of control `index` (0-based) of `count`.
void show(const core::presenter::ControlView &view, size_t index, size_t count);

}  // namespace controlScreen
}  // namespace ui
