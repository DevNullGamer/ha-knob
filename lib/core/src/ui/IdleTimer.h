#pragma once

#include <cstdint>

#include "input/ButtonFsm.h"

// Screen idle timer (plan-step8.md §3.1): On -> Dim -> Off by time since the last
// activity, and the "wake without acting" rule for inputs and button gestures (Q8-3, Q8-4).
// Pure; all time math is modulo 2^32.
namespace core {
namespace ui {

enum class ScreenLevel : uint8_t { On, Dim, Off };

// "on" | "dim" | "off"
const char *screenLevelName(ScreenLevel level);

class IdleTimer {
public:
    // 0 = never. Counts as activity (keepAwake). Seconds are converted to ms (clamped).
    void configure(uint32_t dimAfterS, uint32_t offAfterS, uint32_t nowMs);
    // Encoder detent batch / completed swipe. true = act on it; false = it only woke the screen.
    bool onInput(uint32_t nowMs);
    // Button events (None -> true, no effect).
    bool onButton(ButtonFsm::Event event, uint32_t nowMs);
    // Non-input activity (config saved, toast): level On, timer reset, nothing swallowed.
    void keepAwake(uint32_t nowMs);
    // Moves the level forward by elapsed time. true when the level changed in this call.
    bool tick(uint32_t nowMs);
    ScreenLevel level() const { return level_; }

private:
    void wake(uint32_t nowMs);

    uint32_t dimAfterMs_ = 0;
    uint32_t offAfterMs_ = 0;
    uint32_t lastActivityMs_ = 0;
    ScreenLevel level_ = ScreenLevel::On;
    bool held_ = false;
    bool swallowing_ = false;
};

struct PanelSetting {
    bool on;
    int32_t percent;
};

// On -> {true, brightness}; Dim -> {true, min(dimLevel, brightness)}; Off -> {false, 0}.
PanelSetting panelSetting(ScreenLevel level, int32_t brightness, int32_t dimLevel);

}  // namespace ui
}  // namespace core
