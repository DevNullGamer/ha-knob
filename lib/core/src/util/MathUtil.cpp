#include "util/MathUtil.h"

#include <cmath>

namespace core {

namespace {
constexpr int kMaxStepDecimals = 3;
constexpr int kMaxDecimals = 6;
constexpr double kStepEpsilon = 1e-6;
}  // namespace

int32_t clampInt(int32_t value, int32_t lo, int32_t hi) {
    if (lo > hi) {
        const int32_t tmp = lo;
        lo = hi;
        hi = tmp;
    }
    if (value < lo) return lo;
    if (value > hi) return hi;
    return value;
}

uint8_t percentToByte(int32_t percent) {
    const int32_t p = clampInt(percent, 0, 100);
    return static_cast<uint8_t>((p * 255 + 50) / 100);
}

int decimalsForStep(double step) {
    if (!(step > 0.0)) {
        return 0;
    }
    double scaled = step;
    for (int decimals = 0; decimals < kMaxStepDecimals; ++decimals) {
        if (std::fabs(scaled - std::round(scaled)) < kStepEpsilon * scaled + kStepEpsilon) {
            return decimals;
        }
        scaled *= 10.0;
    }
    return kMaxStepDecimals;
}

double roundToDecimals(double value, int decimals) {
    if (decimals < 0) decimals = 0;
    if (decimals > kMaxDecimals) decimals = kMaxDecimals;

    const double scale = std::pow(10.0, decimals);
    double rounded = std::round(value * scale) / scale;
    if (rounded == 0.0) {
        rounded = 0.0;  // turns -0.0 into 0.0
    }
    return rounded;
}

}  // namespace core
