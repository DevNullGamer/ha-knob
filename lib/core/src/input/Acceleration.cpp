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

}  // namespace core
