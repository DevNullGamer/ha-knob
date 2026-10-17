#include "ui/Ui.h"

#include <lvgl.h>

#include "ui/PageRing.h"
#include "ui/screens/BootScreen.h"
#include "ui/screens/ControlScreen.h"
#include "ui/screens/NoControlsScreen.h"
#include "ui/screens/SetupScreen.h"
#include "ui/screens/StatusScreen.h"

namespace ui {

namespace {

constexpr uint32_t kBootScreenMs = 1500;

lv_obj_t *bootScr = nullptr;
lv_obj_t *statusScr = nullptr;
lv_obj_t *setupScr = nullptr;
lv_obj_t *controlScr = nullptr;
lv_obj_t *noControlsScr = nullptr;
lv_obj_t *shownScr = nullptr;  // screen loaded after boot
bool booting = false;
uint32_t bootStartMs = 0;
bool wantSetup = false;

core::ui::PageRing pages;  // the selected page

lv_obj_t *targetScreen() {
    if (wantSetup) return setupScr;
    if (pages.onStatus()) return statusScr;
    return pages.onNoControls() ? noControlsScr : controlScr;
}

bool canNavigate() { return !booting && !wantSetup; }

}  // namespace

bool NetworkView::operator==(const NetworkView &o) const {
    return ssid == o.ssid && staConnected == o.staConnected && staIp == o.staIp &&
           apActive == o.apActive && apSsid == o.apSsid && apPassword == o.apPassword &&
           hostname == o.hostname;
}

void begin(uint32_t nowMs) {
    bootScr = bootScreen::create();
    statusScr = statusScreen::create();
    setupScr = setupScreen::create();
    controlScr = controlScreen::create();
    noControlsScr = noControlsScreen::create();
    lv_obj_t *defaultScr = lv_screen_active();  // empty screen made by lv_display_create()
    lv_screen_load(bootScr);
    lv_obj_delete(defaultScr);
    booting = true;
    bootStartMs = nowMs;
}

void tick(uint32_t nowMs) {
    if (booting) {
        if (nowMs - bootStartMs >= kBootScreenMs) {
            booting = false;
            shownScr = targetScreen();
            lv_screen_load_anim(shownScr, LV_SCR_LOAD_ANIM_FADE_IN, 300, 0, true /* delete boot */);
            bootScr = nullptr;
        }
        return;
    }
    if (shownScr != targetScreen()) {
        shownScr = targetScreen();
        lv_screen_load(shownScr);
    }
}

void setControlCount(size_t count) { pages.setControlCount(count); }

bool nextPage() {
    if (!canNavigate()) {
        return false;
    }
    pages.next();
    return true;
}

bool previousPage() {
    if (!canNavigate()) {
        return false;
    }
    pages.previous();
    return true;
}

int controlPage() { return pages.controlPage(); }

bool statusPage() { return pages.onStatus(); }

void showControl(const core::presenter::ControlView &view) {
    const int page = controlPage();
    if (page >= 0) {
        controlScreen::show(view, static_cast<size_t>(page), pages.controlCount());
    }
}

void showNetwork(const NetworkView &view) {
    wantSetup = view.apActive && !view.staConnected;

    setupScreen::setAccessPoint(view.apSsid.c_str(), view.apPassword.c_str());
    std::string setupLine;
    if (!view.ssid.empty()) {
        setupLine = "Connecting to " + view.ssid + "...";  // retried while the AP is up
    }
    setupScreen::setStatusLine(setupLine.c_str());

    std::string wifiLine;
    std::string ipLine;
    std::string hostLine;
    if (view.ssid.empty()) {
        wifiLine = "Wi-Fi: not configured";
    } else if (view.staConnected) {
        wifiLine = "Wi-Fi: " + view.ssid;
        ipLine = "http://" + view.staIp;
        hostLine = "http://" + view.hostname + ".local";
    } else {
        wifiLine = "Wi-Fi: connecting to " + view.ssid;
    }
    statusScreen::setNetwork(wifiLine.c_str(), ipLine.c_str(), hostLine.c_str());

    // The NoControls page is never shown while the setup screen wins (AP up, station down).
    const std::string addressLine = view.staConnected ? "Open http://" + view.staIp
                                    : view.ssid.empty() ? std::string("Set up Wi-Fi first")
                                                        : std::string("Waiting for Wi-Fi...");
    noControlsScreen::setAddress(addressLine.c_str());
}

void showHa(const HaView &view) {
    using core::ha::State;
    std::string line;
    switch (view.state) {
        case State::NoConfig:
            line = view.lastError.empty() ? "HA: not configured" : "HA: invalid address";
            break;
        case State::Connecting:
        case State::Authenticating:
        case State::Subscribing:
            line = "HA: connecting...";
            break;
        case State::Ready:
            if (view.tls == core::ha::TlsMode::Insecure) {
                // " (TLS unverified)" instead of the version, so the warning fits the label
                // (the version is shown in the web UI).
                line = "HA: connected (TLS unverified)";
            } else {
                line = view.haVersion.empty() ? "HA: connected" : "HA: connected (" + view.haVersion + ")";
            }
            break;
        case State::AuthFailed:
            line = "HA: token rejected";
            break;
        case State::Backoff:
        default:
            line = view.networkUp ? "HA: offline, retrying" : "HA: waiting for Wi-Fi";
            break;
    }
    statusScreen::setHa(line.c_str());
    noControlsScreen::setHa(line.c_str());
}

void showEncoderCount(int32_t count) {
    statusScreen::setEncoderCount(count);
}

void showLastInput(const char *text) {
    statusScreen::setLastInput(text);
}

}  // namespace ui
