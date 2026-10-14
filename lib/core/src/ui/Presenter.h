#pragma once

#include <string>

namespace core {
namespace presenter {

// Number of decimals needed to show multiples of step exactly (0 to 3).
// 1 -> 0, 0.5 -> 1, 0.1 -> 1, 0.25 -> 2. Non-positive steps give 0.
int decimalsForStep(double step);

// Rounds value to the given decimals (0 to 6) and formats it with exactly that many.
// Never returns "-0" / "-0.0".
std::string formatNumber(double value, int decimals);

// Formats value with the precision of step, e.g. (21.5, 0.5) -> "21.5", (20, 0.5) -> "20.0",
// (3200, 100) -> "3200".
std::string formatStepped(double value, double step);

// Formats a percentage as an integer, e.g. 45.4 -> "45", 99.5 -> "100". No unit.
std::string formatPercent(double value);

}  // namespace presenter
}  // namespace core
