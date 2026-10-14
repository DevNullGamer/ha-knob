#include "input/SwipeClassifier.h"

#include <cstdlib>

namespace core {

const char *swipeName(Swipe swipe) {
    switch (swipe) {
        case Swipe::Left: return "left";
        case Swipe::Right: return "right";
        default: return "none";
    }
}

Swipe SwipeClassifier::update(bool touched, int16_t x, int16_t y, uint32_t nowMs) {
    if (touched) {
        if (!active_) {
            active_ = true;
            startX_ = x;
            startY_ = y;
            startMs_ = nowMs;
        }
        lastX_ = x;
        lastY_ = y;
        return Swipe::None;
    }

    if (!active_) {
        return Swipe::None;
    }
    active_ = false;

    const int32_t dx = static_cast<int32_t>(lastX_) - startX_;
    const int32_t dy = static_cast<int32_t>(lastY_) - startY_;
    const int32_t absDx = std::abs(dx);
    if (nowMs - startMs_ > config_.maxDurationMs || absDx < config_.minDistancePx ||
        absDx < config_.minAxisRatio * std::abs(dy)) {
        return Swipe::None;
    }
    return dx < 0 ? Swipe::Left : Swipe::Right;
}

}  // namespace core
