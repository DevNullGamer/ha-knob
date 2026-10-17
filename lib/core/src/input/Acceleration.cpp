#include "input/Acceleration.h"

namespace core {

int Acceleration::onDetent(uint32_t nowMs) {
    const bool hadLast = hasLast_;
    const uint32_t intervalMs = nowMs - lastMs_;
    hasLast_ = true;
    lastMs_ = nowMs;

    if (!enabled_ || !hadLast) {
        return 1;
    }
    if (intervalMs < kFastMs) {
        return 4;
    }
    if (intervalMs < kMediumMs) {
        return 2;
    }
    return 1;
}

int Acceleration::onDetents(int detents, uint32_t nowMs) {
    if (detents == 0) {
        return 0;
    }
    const bool hadLast = hasLast_;
    const uint32_t intervalMs = nowMs - lastMs_;
    hasLast_ = true;
    lastMs_ = nowMs;

    const uint32_t count = static_cast<uint32_t>(detents < 0 ? -detents : detents);
    int multiplier = 1;
    if (enabled_ && hadLast) {
        // interval / count < threshold, without the integer division's truncation
        if (intervalMs < kFastMs * count) {
            multiplier = 4;
        } else if (intervalMs < kMediumMs * count) {
            multiplier = 2;
        }
    }
    return detents * multiplier;
}

}  // namespace core
