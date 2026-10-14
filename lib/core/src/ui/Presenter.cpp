#include "ui/Presenter.h"

#include <cmath>
#include <cstdio>

namespace core {
namespace presenter {

namespace {
constexpr int kMaxStepDecimals = 3;
constexpr int kMaxDecimals = 6;
constexpr double kStepEpsilon = 1e-6;
}  // namespace

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

std::string formatNumber(double value, int decimals) {
    if (decimals < 0) decimals = 0;
    if (decimals > kMaxDecimals) decimals = kMaxDecimals;

    const double scale = std::pow(10.0, decimals);
    double rounded = std::round(value * scale) / scale;
    if (rounded == 0.0) {
        rounded = 0.0;  // turns -0.0 into 0.0
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.*f", decimals, rounded);
    return buf;
}

std::string formatStepped(double value, double step) {
    return formatNumber(value, decimalsForStep(step));
}

std::string formatPercent(double value) {
    return formatNumber(value, 0);
}

}  // namespace presenter
}  // namespace core
