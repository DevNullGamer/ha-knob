#pragma once

#include <cstdint>

namespace core {

// Encoder acceleration: consecutive detents less than 40 ms apart give x4, less than
// 80 ms apart give x2, otherwise x1. The first detent, and every detent while disabled,
// gives x1.
class Acceleration {
public:
    static constexpr uint32_t kFastMs = 40;
    static constexpr uint32_t kMediumMs = 80;

    void setEnabled(bool enabled) { enabled_ = enabled; }
    bool enabled() const { return enabled_; }

    // Call once per detent; returns the multiplier (1, 2 or 4) for that detent.
    int onDetent(uint32_t nowMs);

    // Call once per batch of detents (the main loop reads the encoder once per pass, plan-
    // step7.md Q7-2): the detents are taken as spread evenly over the time since the previous
    // call (onDetent or onDetents), so each gets the multiplier for (nowMs - last) / |detents|.
    // Returns the signed sum (detents x multiplier). The first call, and every call while
    // disabled, gives x1 each. 0 detents returns 0 and changes nothing.
    int onDetents(int detents, uint32_t nowMs);

    // Forgets the previous detent, so the next one gives x1.
    void reset() { hasLast_ = false; }

private:
    bool enabled_ = true;
    bool hasLast_ = false;
    uint32_t lastMs_ = 0;
};

}  // namespace core
