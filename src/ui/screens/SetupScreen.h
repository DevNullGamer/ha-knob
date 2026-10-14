#pragma once

#include <lvgl.h>

// Setup screen, shown while the setup AP is up and the station is not connected: the AP
// name and password to join, the URL of the setup page, and a status line (e.g. that the
// configured network cannot be reached).
namespace ui {
namespace setupScreen {

lv_obj_t *create();

void setAccessPoint(const char *ssid, const char *password);

// "" hides the line.
void setStatusLine(const char *text);

}  // namespace setupScreen
}  // namespace ui
