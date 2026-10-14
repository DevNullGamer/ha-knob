#include "ui/Ui.h"

#include <lvgl.h>

#include "ui/screens/BootScreen.h"
#include "ui/screens/SetupScreen.h"
#include "ui/screens/StatusScreen.h"

namespace ui {

namespace {

constexpr uint32_t kBootScreenMs = 1500;

lv_obj_t *bootScr = nullptr;
lv_obj_t *statusScr = nullptr;
lv_obj_t *setupScr = nullptr;
lv_obj_t *shownScr = nullptr;  // screen loaded after boot
bool booting = false;
uint32_t bootStartMs = 0;
bool wantSetup = false;

lv_obj_t *targetScreen() { return wantSetup ? setupScr : statusScr; }

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
}

void showEncoderCount(int32_t count) {
    statusScreen::setEncoderCount(count);
}

void showLastInput(const char *text) {
    statusScreen::setLastInput(text);
}

}  // namespace ui
