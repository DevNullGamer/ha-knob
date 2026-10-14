#pragma once

#include <cstdint>

namespace core {

enum class Swipe : uint8_t { None, Left, Right };

// Short name for logs: "left", "right", "none".
const char *swipeName(Swipe swipe);

// Classifies a touch stroke as a horizontal swipe when the finger lifts. Taps and
// vertical, short or slow strokes give Swipe::None. On this device the screen is the
// knob cap, so touch is used for swipes only and taps never trigger actions.
class SwipeClassifier {
public:
    struct Config {
        int32_t minDistancePx = 80;    // horizontal travel, start to last point
        uint32_t maxDurationMs = 700;  // first touch to lift
        int32_t minAxisRatio = 2;      // |dx| must be >= ratio * |dy|
    };

    SwipeClassifier() = default;
    explicit SwipeClassifier(const Config &config) : config_(config) {}

    // Feed every touch poll: whether the panel is touched, and the point (ignored when
    // not touched). Returns the swipe on the poll where the finger lifts, else None.
    Swipe update(bool touched, int16_t x, int16_t y, uint32_t nowMs);

private:
    Config config_;
    bool active_ = false;
    int16_t startX_ = 0;
    int16_t startY_ = 0;
    int16_t lastX_ = 0;
    int16_t lastY_ = 0;
    uint32_t startMs_ = 0;
};

}  // namespace core
