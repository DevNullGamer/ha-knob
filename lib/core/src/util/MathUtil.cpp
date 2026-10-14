#include "util/MathUtil.h"

namespace core {

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

}  // namespace core
