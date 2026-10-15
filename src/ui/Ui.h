#pragma once

#include <cstdint>
#include <string>

#include "ha/HaSession.h"

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
    bool networkUp = false;  // Wi-Fi station connected
};

// Creates the screens and shows the boot screen. Requires hal::display::begin().
void begin(uint32_t nowMs);

// Leaves the boot screen after a short delay, for the setup screen (setup AP up and
// station not connected) or the status screen.
void tick(uint32_t nowMs);

void showNetwork(const NetworkView &view);
void showHa(const HaView &view);
void showEncoderCount(int32_t count);
void showLastInput(const char *text);

}  // namespace ui
