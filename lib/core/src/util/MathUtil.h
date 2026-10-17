#pragma once

#include <cstdint>

namespace core {

// Clamps value into [lo, hi]. If lo > hi the bounds are swapped.
int32_t clampInt(int32_t value, int32_t lo, int32_t hi);

// Maps a percentage (clamped to 0..100) to 0..255, rounded: 0 -> 0, 50 -> 128, 100 -> 255.
uint8_t percentToByte(int32_t percent);

// Number of decimals needed to show multiples of step exactly (0 to 3).
// 1 -> 0, 0.5 -> 1, 0.1 -> 1, 0.25 -> 2. Non-positive steps give 0.
int decimalsForStep(double step);

// Rounds value to the given decimals (clamped to 0..6), half away from zero. Never returns
// -0.0 (it becomes 0.0).
double roundToDecimals(double value, int decimals);

}  // namespace core
