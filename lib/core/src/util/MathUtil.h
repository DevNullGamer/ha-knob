#pragma once

#include <cstdint>

namespace core {

// Clamps value into [lo, hi]. If lo > hi the bounds are swapped.
int32_t clampInt(int32_t value, int32_t lo, int32_t hi);

// Maps a percentage (clamped to 0..100) to 0..255, rounded: 0 -> 0, 50 -> 128, 100 -> 255.
uint8_t percentToByte(int32_t percent);

}  // namespace core
