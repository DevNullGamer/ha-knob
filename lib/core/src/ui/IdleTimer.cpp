#include "ui/IdleTimer.h"

#include <algorithm>

namespace core {
namespace ui {

namespace {

uint32_t secondsToMs(uint32_t s) {
    const uint64_t ms = static_cast<uint64_t>(s) * 1000u;
    return ms > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(ms);
}

}  // namespace

const char *screenLevelName(ScreenLevel level) {
    switch (level) {
        case ScreenLevel::On: return "on";
        case ScreenLevel::Dim: return "dim";
        case ScreenLevel::Off: return "off";
    }
    return "on";
}

void IdleTimer::wake(uint32_t nowMs) {
    level_ = ScreenLevel::On;
    lastActivityMs_ = nowMs;
}

void IdleTimer::configure(uint32_t dimAfterS, uint32_t offAfterS, uint32_t nowMs) {
    dimAfterMs_ = secondsToMs(dimAfterS);
    offAfterMs_ = secondsToMs(offAfterS);
    wake(nowMs);
}

void IdleTimer::keepAwake(uint32_t nowMs) {
    wake(nowMs);
}

bool IdleTimer::onInput(uint32_t nowMs) {
    const bool wasOn = level_ == ScreenLevel::On;
    wake(nowMs);
    return wasOn;
}

bool IdleTimer::onButton(ButtonFsm::Event event, uint32_t nowMs) {
    using Event = ButtonFsm::Event;
    if (event == Event::None) return true;
    const bool wasOn = level_ == ScreenLevel::On;
    wake(nowMs);
    switch (event) {
        case Event::Pressed:
            held_ = true;
            swallowing_ = !wasOn;
            return wasOn;
        case Event::LongPress:
        case Event::VeryLongPress:
            if (swallowing_) return false;
            if (!wasOn) {
                swallowing_ = true;
                held_ = true;
                return false;
            }
            return true;
        case Event::ShortPress:
        case Event::Released:
            held_ = false;
            if (swallowing_) {
                swallowing_ = false;
                return false;
            }
            return wasOn;
        case Event::None:
            break;
    }
    return true;
}

bool IdleTimer::tick(uint32_t nowMs) {
    if (held_) {
        lastActivityMs_ = nowMs;
        return false;
    }
    const uint32_t idle = nowMs - lastActivityMs_;
    ScreenLevel target = ScreenLevel::On;
    if (offAfterMs_ > 0 && idle >= offAfterMs_) {
        target = ScreenLevel::Off;
    } else if (dimAfterMs_ > 0 && idle >= dimAfterMs_) {
        target = ScreenLevel::Dim;
    }
    // Forward only: sticky across the millis() wrap.
    if (static_cast<uint8_t>(target) <= static_cast<uint8_t>(level_)) return false;
    level_ = target;
    return true;
}

PanelSetting panelSetting(ScreenLevel level, int32_t brightness, int32_t dimLevel) {
    switch (level) {
        case ScreenLevel::Off: return {false, 0};
        case ScreenLevel::Dim: return {true, std::min(dimLevel, brightness)};
        case ScreenLevel::On: break;
    }
    return {true, brightness};
}

}  // namespace ui
}  // namespace core
