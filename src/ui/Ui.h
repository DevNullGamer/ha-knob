#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

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

// Leaves the boot screen after a short delay, then shows the setup screen (setup AP up and
// station not connected) or the current page.
void tick(uint32_t nowMs);

// Pages (plan-step6.md §3.7, Q6-1): a ring of the control pages (or the NoControls page
// while there are none) followed by the Status page. Navigation is ignored while the boot
// or the setup screen is shown.
//
// Sets the number of controls. A control page stays selected while its index is still in
// range (else the first page is), the Status page stays the Status page.
void setControlCount(size_t count);
// Next / previous page, wrapping. False if the page did not change.
bool nextPage();
bool previousPage();
// Index of the selected control page; -1 for the Status and the NoControls page.
int controlPage();
// True while the Status page is selected.
bool statusPage();
// Shows the view of the selected control page (call when controlPage() >= 0).
void showControl(const core::presenter::ControlView &view);

void showNetwork(const NetworkView &view);
void showHa(const HaView &view);
void showEncoderCount(int32_t count);
void showLastInput(const char *text);

}  // namespace ui
