#include "app/Idle.h"

#include "hal/Display.h"
#include "util/Log.h"

namespace app {
namespace idle {

namespace {

core::ui::IdleTimer timer;
int32_t brightness = 80;  // device.brightness
int32_t dimLevel = 15;    // device.dim_level
// What the panel shows: on after hal::display::begin(), brightness not yet set from here.
core::ui::ScreenLevel shownLevel = core::ui::ScreenLevel::On;
core::ui::PanelSetting shown{true, -1};

const char *sourceName(Source source) {
    switch (source) {
        case Source::Encoder: return "encoder";
        case Source::Swipe: return "swipe";
        case Source::Button: return "button";
        case Source::Config: return "config";
        case Source::Toast: return "toast";
    }
    return "?";
}

void logWake(Source source, bool consumed) {
    util::logPrintf("IDLE wake by=%s consumed=%d\n", sourceName(source), consumed ? 1 : 0);
}

}  // namespace

void applyConfig(const core::config::DeviceConfig &device, uint32_t nowMs) {
    brightness = device.brightness;
    dimLevel = device.dimLevel;
    timer.configure(device.dimAfterS, device.offAfterS, nowMs);
}

bool input(Source source, uint32_t nowMs) {
    const bool act = timer.onInput(nowMs);
    if (!act) {
        logWake(source, true);
    }
    return act;
}

bool button(core::ButtonFsm::Event event, uint32_t nowMs) {
    const bool wasOn = timer.level() == core::ui::ScreenLevel::On;
    const bool act = timer.onButton(event, nowMs);
    if (!wasOn) {
        logWake(Source::Button, true);  // only the event that woke it, not the rest of the gesture
    }
    return act;
}

void keepAwake(Source source, uint32_t nowMs) {
    if (timer.level() != core::ui::ScreenLevel::On) {
        logWake(source, false);
    }
    timer.keepAwake(nowMs);
}

void loop(uint32_t nowMs) {
    timer.tick(nowMs);
    const core::ui::ScreenLevel level = timer.level();
    const core::ui::PanelSetting setting = core::ui::panelSetting(level, brightness, dimLevel);
    // The level counts too: Dim with dim_level >= brightness keeps the percentage but is logged.
    if (level == shownLevel && setting.on == shown.on && setting.percent == shown.percent) {
        return;
    }
    // Wake: panel on, then the brightness; off: brightness 0, then the panel off.
    if (setting.on && !shown.on) {
        hal::display::setPanelOn(true);
    }
    if (setting.percent != shown.percent) {
        hal::display::setBrightnessPercent(setting.percent);
    }
    if (!setting.on && shown.on) {
        hal::display::setPanelOn(false);
    }
    shownLevel = level;
    shown = setting;
    util::logPrintf("DISP level=%s pct=%d\n", core::ui::screenLevelName(level),
                    static_cast<int>(setting.percent));
}

core::ui::ScreenLevel level() { return timer.level(); }

}  // namespace idle
}  // namespace app
