#pragma once

#include <cstdint>

namespace core {

// Clamps value into [lo, hi]. If lo > hi the bounds are swapped.
int32_t clampInt(int32_t value, int32_t lo, int32_t hi);

}  // namespace core
