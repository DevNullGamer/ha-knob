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

}  // namespace core
