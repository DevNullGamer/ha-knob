#include "input/ButtonFsm.h"

namespace core {

ButtonFsm::Event ButtonFsm::update(bool rawDown, uint32_t nowMs) {
    if (rawDown != rawDown_) {
        rawDown_ = rawDown;
        rawSinceMs_ = nowMs;
    }

    if (rawDown_ != stableDown_ && nowMs - rawSinceMs_ >= kDebounceMs) {
        stableDown_ = rawDown_;
        if (stableDown_) {
            pressStartMs_ = rawSinceMs_;
            longFired_ = false;
            veryLongFired_ = false;
            return Event::Pressed;
        }
        return longFired_ ? Event::Released : Event::ShortPress;
    }

    // Only while the raw level is still down: once the contact has opened, the press is
    // over (pending the release debounce) and must not turn into a long press.
    if (stableDown_ && rawDown_) {
        const uint32_t heldMs = nowMs - pressStartMs_;
        if (!longFired_ && heldMs >= kLongPressMs) {
            longFired_ = true;
            return Event::LongPress;
        }
        if (!veryLongFired_ && heldMs >= kVeryLongPressMs) {
            veryLongFired_ = true;
            return Event::VeryLongPress;
        }
    }
    return Event::None;
}

const char *buttonEventName(ButtonFsm::Event event) {
    switch (event) {
        case ButtonFsm::Event::Pressed: return "down";
        case ButtonFsm::Event::Released: return "up";
        case ButtonFsm::Event::ShortPress: return "short";
        case ButtonFsm::Event::LongPress: return "long";
        case ButtonFsm::Event::VeryLongPress: return "verylong";
        default: return "none";
    }
}

}  // namespace core
