#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "ha/HaSession.h"
#include "ui/Presenter.h"

// Screen manager. All calls must come from the main loop (LVGL is not thread-safe).
namespace ui {

// What the screens show about the network (filled by main.cpp from net::wifi::Status).
struct NetworkView {
    std::string ssid;          // configured STA SSID ("" = not configured)
    bool staConnected = false;
    std::string staIp;
    bool apActive = false;
    std::string apSsid;
    std::string apPassword;    // shown on the setup screen
    std::string hostname;

    bool operator==(const NetworkView &o) const;
    bool operator!=(const NetworkView &o) const { return !(*this == o); }
};

// What the screens show about the Home Assistant connection (filled by main.cpp from
// ha::Status).
struct HaView {
    core::ha::State state = core::ha::State::NoConfig;
    std::string haVersion;
    std::string lastError;
    core::ha::TlsMode tls = core::ha::TlsMode::None;
    bool networkUp = false;  // Wi-Fi station connected
};

// Creates the screens and shows the boot screen. Requires hal::display::begin().
void begin(uint32_t nowMs);

// Leaves the boot screen after a short delay, then shows the screen of highest precedence
// (below). Also hides the toast after 3 s and closes the Select list after 8 s without input.
void tick(uint32_t nowMs);

// Screen precedence (highest first): boot, setup, Select list, pages. Step 8 adds its reset
// confirmation between boot and setup.

// Pages (plan-step6.md §3.7, Q6-1): a ring of the control pages (or the NoControls page
// while there are none) followed by the Status page. Navigation is ignored while the boot
// screen, the setup screen or the Select list is shown.
//
// Sets the number of controls. A control page stays selected while its index is still in
// range (else the first page is), the Status page stays the Status page.
void setControlCount(size_t count);
// Next / previous page, wrapping. False if the page did not change.
bool nextPage();
bool previousPage();
// Index of the selected control page; -1 for the Status and the NoControls page.
int controlPage();
// True while the pages take input (not the boot or the setup screen, no Select list);
// rotation and presses act on a control page only then.
bool pagesActive();
// True while the Status page is selected.
bool statusPage();
// Shows the view of the selected control page (call when controlPage() >= 0).
void showControl(const core::presenter::ControlView &view);

// Select list (plan-step7.md Q7-9): the control titles plus "Status", the current page
// selected. Logs `UI select open`, `UI select choose=<i>` (1-based control page),
// `UI select status`, `UI select cancel reason=<long|idle|config|setup>`.
//
// Opens the list while the pages are active and there is at least one control; `titles`
// has one entry per control (presenter::titleFor). False if it did not open.
bool selectOpen(const std::vector<std::string> &titles, uint32_t nowMs);
bool selectIsOpen();
// Moves the selection by delta items (1 per detent, clamped at the ends); restarts the 8 s
// idle time.
void selectMove(int delta, uint32_t nowMs);
// Closes the list and shows the selected page. True if the selected page changed.
bool selectChoose();
// Closes the list without a change (reason for the log: "long", "config", ...). No-op when
// closed. The setup screen and 8 s without input close it by themselves.
void selectCancel(const char *reason);

// A short message over every screen (control notices, plan-step7.md Q7-6): a label on
// lv_layer_top() for 3 s (hidden in tick()); a new toast replaces the old one. Logs
// `UI toast text=<t>`. Ignored before begin().
void showToast(const std::string &text, uint32_t nowMs);

void showNetwork(const NetworkView &view);
void showHa(const HaView &view);
void showEncoderCount(int32_t count);
void showLastInput(const char *text);

}  // namespace ui
