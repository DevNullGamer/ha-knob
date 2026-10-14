#pragma once

#include <cstdint>

namespace core {

// Debounced push-button state machine. Feed it the raw level and the time; call it
// often (every few ms) so the long-press timing is accurate.
//
// Events:
//  Pressed        debounced press (raw level stable for 30 ms)
//  ShortPress     release of a press that did not reach LongPress
//  LongPress      held >= 600 ms (measured from the press edge), fired once while held;
//                 a press whose contact opens before 600 ms ends as ShortPress
//  VeryLongPress  held >= 10 s, fired once while held
//  Released       release after LongPress / VeryLongPress (no ShortPress then)
// At most one event is returned per call.
class ButtonFsm {
public:
    enum class Event : uint8_t { None, Pressed, Released, ShortPress, LongPress, VeryLongPress };

    static constexpr uint32_t kDebounceMs = 30;
    static constexpr uint32_t kLongPressMs = 600;
    static constexpr uint32_t kVeryLongPressMs = 10000;

    Event update(bool rawDown, uint32_t nowMs);

    // Debounced state.
    bool isDown() const { return stableDown_; }

private:
    bool rawDown_ = false;
    bool stableDown_ = false;
    uint32_t rawSinceMs_ = 0;
    uint32_t pressStartMs_ = 0;
    bool longFired_ = false;
    bool veryLongFired_ = false;
};

// Short name for logs: "down", "up", "short", "long", "verylong", "none".
const char *buttonEventName(ButtonFsm::Event event);

}  // namespace core
