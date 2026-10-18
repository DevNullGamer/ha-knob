#include "ui/Ui.h"

#include <lvgl.h>

#include <algorithm>
#include <optional>

#include "ui/PageRing.h"
#include "ui/SelectModel.h"
#include "ui/screens/BootScreen.h"
#include "ui/screens/ControlScreen.h"
#include "ui/screens/NoControlsScreen.h"
#include "ui/screens/ResetScreen.h"
#include "ui/screens/SelectScreen.h"
#include "ui/screens/SetupScreen.h"
#include "ui/screens/StatusScreen.h"
#include "util/Log.h"
#include "util/Text.h"

namespace ui {

namespace {

constexpr uint32_t kBootScreenMs = 1500;

// Toast (plan-step7.md §3.6): font 20, at most kToastWidth px wide and kToastMaxLines lines
// (longer texts end in "..."), centred kToastY px below the screen centre but moved up so
// its bottom stays at kToastMaxBottom, for kToastMs. Width and bottom keep even the largest
// box (260 x 98 px, radius 18) inside the 390 px round panel (corners <= 180 px from the
// centre); the spec's 300 px would reach the panel edge.
constexpr uint32_t kToastMs = 3000;
constexpr int32_t kToastWidth = 260;
constexpr int32_t kToastPad = 10;
constexpr int32_t kToastY = 100;
constexpr int32_t kToastMaxBottom = 136;
constexpr int32_t kToastMaxLines = 3;
constexpr int32_t kToastRadius = 18;
constexpr uint32_t kToastBgColor = 0x8B0000;  // dark red

lv_obj_t *bootScr = nullptr;
lv_obj_t *statusScr = nullptr;
lv_obj_t *setupScr = nullptr;
lv_obj_t *controlScr = nullptr;
lv_obj_t *noControlsScr = nullptr;
lv_obj_t *selectScr = nullptr;
lv_obj_t *resetScr = nullptr;
lv_obj_t *toastLabel = nullptr;  // on lv_layer_top(), over every screen
bool toastVisible = false;
uint32_t toastStartMs = 0;
lv_obj_t *shownScr = nullptr;  // screen loaded after boot
bool begun = false;            // begin() ran
bool booting = false;
uint32_t bootStartMs = 0;
bool wantSetup = false;
bool wantReset = false;          // the factory-reset confirmation is shown
uint32_t shownResetSeconds = 0;  // its countdown label (0 = not set since it opened)

core::ui::PageRing pages;     // the selected page
core::ui::SelectModel select;  // the Select list (its last item is "Status")

// The screen to show after boot, by precedence: reset confirmation, setup, Select list,
// the selected page.
lv_obj_t *targetScreen() {
    if (wantReset) return resetScr;
    if (wantSetup) return setupScr;
    if (select.isOpen()) return selectScr;
    if (pages.onStatus()) return statusScr;
    return pages.onNoControls() ? noControlsScr : controlScr;
}

// The pages are shown and take navigation (nothing of higher precedence is shown).
bool canNavigate() { return !booting && !wantReset && !wantSetup && !select.isOpen(); }

lv_obj_t *createToast() {
    lv_obj_t *label = lv_label_create(lv_layer_top());
    lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_text_font(label, &ui_font_20, 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_bg_color(label, lv_color_hex(kToastBgColor), 0);
    lv_obj_set_style_bg_opa(label, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(label, kToastRadius, 0);
    lv_obj_set_style_pad_all(label, kToastPad, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);
    return label;
}

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
    selectScr = selectScreen::create();
    resetScr = resetScreen::create();
    toastLabel = createToast();
    lv_obj_t *defaultScr = lv_screen_active();  // empty screen made by lv_display_create()
    lv_screen_load(bootScr);
    lv_obj_delete(defaultScr);
    booting = true;
    bootStartMs = nowMs;
    begun = true;
}

void tick(uint32_t nowMs) {
    if (toastVisible && nowMs - toastStartMs >= kToastMs) {
        toastVisible = false;
        lv_obj_add_flag(toastLabel, LV_OBJ_FLAG_HIDDEN);
    }
    if (select.tick(nowMs)) {
        util::logPrintf("UI select cancel reason=idle\n");
    }
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

bool pagesActive() { return begun && canNavigate(); }

bool statusPage() { return pages.onStatus(); }

void showControl(const core::presenter::ControlView &view) {
    const int page = controlPage();
    if (page >= 0) {
        controlScreen::show(view, static_cast<size_t>(page), pages.controlCount());
    }
}

bool selectOpen(const std::vector<std::string> &titles, uint32_t nowMs) {
    const size_t count = pages.controlCount();
    if (!pagesActive() || count == 0 || titles.size() != count) {
        return false;
    }
    const size_t selected = pages.onStatus() ? count : static_cast<size_t>(pages.controlPage());
    selectScreen::setItems(titles);
    select.open(count + 1, selected, nowMs);  // + "Status"
    selectScreen::setSelected(select.selected());
    util::logPrintf("UI select open\n");
    return true;
}

bool selectIsOpen() { return select.isOpen(); }

void selectMove(int delta, uint32_t nowMs) {
    if (!select.isOpen()) {
        return;
    }
    select.move(delta, nowMs);
    selectScreen::setSelected(select.selected());
}

bool selectChoose() {
    const std::optional<size_t> chosen = select.choose();
    if (!chosen) {
        return false;
    }
    if (*chosen < pages.controlCount()) {
        util::logPrintf("UI select choose=%u\n", static_cast<unsigned>(*chosen + 1));
        const bool changed = pages.controlPage() != static_cast<int>(*chosen);
        pages.selectControl(*chosen);
        return changed;
    }
    util::logPrintf("UI select status\n");
    const bool changed = !pages.onStatus();
    pages.selectStatus();
    return changed;
}

void selectCancel(const char *reason) {
    if (!select.isOpen()) {
        return;
    }
    select.close();
    util::logPrintf("UI select cancel reason=%s\n", reason);
}

void showResetConfirm(uint32_t secondsLeft) {
    if (!begun) {
        return;
    }
    if (!wantReset) {
        wantReset = true;
        shownResetSeconds = 0;
        resetScreen::showQuestion();
    }
    if (secondsLeft != shownResetSeconds) {
        shownResetSeconds = secondsLeft;
        resetScreen::setSecondsLeft(secondsLeft);
    }
}

void showResetting() {
    if (!begun) {
        return;
    }
    wantReset = true;
    resetScreen::showResetting();
}

void hideResetConfirm() { wantReset = false; }

void showToast(const std::string &text, uint32_t nowMs) {
    if (!begun) {
        return;
    }
    util::logPrintf("UI toast text=%s\n", core::util::logSafe(text, 60).c_str());
    // As many lines as the text needs at the full width, at most kToastMaxLines; the box is
    // only as wide as the widest line, so a short notice stays clear of the round panel's edge.
    constexpr int32_t maxTextWidth = kToastWidth - 2 * kToastPad;
    lv_point_t size;
    lv_text_get_size(&size, text.c_str(), &ui_font_20, 0, 0, maxTextWidth, LV_TEXT_FLAG_NONE);
    const int32_t lineHeight = lv_font_get_line_height(&ui_font_20);
    // + 2 px against rounding: the label must wrap exactly where the measurement did.
    const int32_t textWidth = std::min<int32_t>(size.x + 2, maxTextWidth);
    const int32_t textHeight =
        std::min<int32_t>(std::max<int32_t>(size.y, lineHeight), kToastMaxLines * lineHeight);
    lv_label_set_text(toastLabel, text.c_str());
    const int32_t boxHeight = textHeight + 2 * kToastPad;
    lv_obj_set_size(toastLabel, textWidth + 2 * kToastPad, boxHeight);
    lv_obj_align(toastLabel, LV_ALIGN_CENTER, 0, std::min(kToastY, kToastMaxBottom - boxHeight / 2));
    lv_obj_remove_flag(toastLabel, LV_OBJ_FLAG_HIDDEN);
    toastVisible = true;
    toastStartMs = nowMs;
}

void showNetwork(const NetworkView &view) {
    wantSetup = view.apActive && !view.staConnected;
    if (wantSetup) {
        selectCancel("setup");
    }

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
