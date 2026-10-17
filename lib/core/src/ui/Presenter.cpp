#include "ui/Presenter.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <optional>

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

namespace {

constexpr const char *kNoValue = "--";
constexpr const char *kDegree = "\xC2\xB0";  // U+00B0, in the project fonts
constexpr double kDefaultMinKelvin = 2000;
constexpr double kDefaultMaxKelvin = 6500;
constexpr double kDefaultMinTemp = 7;   // HA's climate defaults
constexpr double kDefaultMaxTemp = 35;
constexpr double kDefaultTempStep = 0.5;

bool isFinite(const std::optional<double> &v) { return v.has_value() && std::isfinite(*v); }

// The configured range if it is usable, else the default one.
void setRange(ControlView &view, const std::optional<double> &lo, const std::optional<double> &hi,
              double defaultLo, double defaultHi) {
    if (isFinite(lo) && isFinite(hi) && *hi > *lo) {
        view.arcMin = *lo;
        view.arcMax = *hi;
    } else {
        view.arcMin = defaultLo;
        view.arcMax = defaultHi;
    }
}

// Sets value / unit / arc from an optional number (shown as `text`); absent -> "--".
void setValue(ControlView &view, const std::optional<double> &arcValue, const std::string &text,
              const char *unit) {
    if (!isFinite(arcValue)) {
        view.value = kNoValue;
        view.unit.clear();
        view.arcValue = view.arcMin;
        return;
    }
    view.value = text;
    view.unit = unit;
    view.arcValue = std::min(std::max(*arcValue, view.arcMin), view.arcMax);
}

std::optional<double> scaled(const std::optional<double> &v, double factor) {
    if (!isFinite(v)) return std::nullopt;
    return *v * factor;
}

bool isUnavailable(const std::string &state) { return state == "unavailable" || state == "unknown"; }

std::string percentText(const std::optional<double> &v) {
    return isFinite(v) ? formatPercent(*v) : std::string();
}

}  // namespace

bool waitOver(const PresentContext &context) {
    return context.snapshotSeen || context.msSinceSubscribe >= kNotFoundAfterMs;
}

bool ControlView::operator==(const ControlView &o) const {
    return title == o.title && value == o.value && unit == o.unit && hasArc == o.hasArc &&
           arcMin == o.arcMin && arcMax == o.arcMax && arcValue == o.arcValue &&
           stateText == o.stateText && dimmed == o.dimmed;
}

ControlView present(const config::Control &control, const ha::EntityState *entity,
                    const PresentContext &context) {
    ControlView view;
    if (!control.label.empty()) {
        view.title = control.label;
    } else if (entity != nullptr && !entity->attrs.friendlyName.empty()) {
        view.title.assign(entity->attrs.friendlyName.data(), entity->attrs.friendlyName.size());
    } else {
        view.title = control.entityId;
    }

    const std::string &action = control.action;
    const bool isBrightness = action == "brightness";
    const bool isHue = action == "hue";
    const bool isColorTemp = action == "color_temp";
    const bool isTargetTemp = action == "target_temp";
    const bool isPercent = action == "position" || action == "percentage" || action == "volume";
    view.hasArc = isBrightness || isHue || isColorTemp || isTargetTemp || isPercent;

    // Ranges first: they do not depend on a value.
    const ha::EntityAttrs empty;
    const ha::EntityAttrs &attrs = entity != nullptr ? entity->attrs : empty;
    if (isHue) {
        view.arcMax = 360;
    } else if (isColorTemp) {
        setRange(view, attrs.minColorTempKelvin, attrs.maxColorTempKelvin, kDefaultMinKelvin,
                 kDefaultMaxKelvin);
    } else if (isTargetTemp) {
        setRange(view, attrs.minTemp, attrs.maxTemp, kDefaultMinTemp, kDefaultMaxTemp);
    }

    if (entity == nullptr) {
        view.value = kNoValue;
        view.arcValue = view.arcMin;
        view.dimmed = true;
        view.stateText = !context.haReady   ? "HA offline"
                         : waitOver(context) ? "not found"
                                             : "waiting...";
        if (!view.hasArc) {
            view.arcMin = view.arcMax = view.arcValue = 0;
        }
        return view;
    }

    const std::string state(entity->state.data(), entity->state.size());
    view.stateText = state;

    if (isBrightness) {
        double percent = 0;  // off or absent -> 0 %
        if (state != "off" && isFinite(attrs.brightness)) {
            percent = std::min(std::max(std::round(*attrs.brightness * 100.0 / 255.0), 0.0), 100.0);
        }
        setValue(view, percent, formatPercent(percent), "%");
    } else if (isHue) {
        setValue(view, attrs.hue, isFinite(attrs.hue) ? formatNumber(*attrs.hue, 0) : "", kDegree);
    } else if (isColorTemp) {
        setValue(view, attrs.colorTempKelvin,
                 isFinite(attrs.colorTempKelvin) ? formatNumber(*attrs.colorTempKelvin, 0) : "", "K");
    } else if (isTargetTemp) {
        // The configured step (validated); a missing one falls back to HA's step, then 0.5.
        double step = control.step;
        if (!(step > 0)) {
            step = isFinite(attrs.targetTempStep) && *attrs.targetTempStep > 0 ? *attrs.targetTempStep
                                                                               : kDefaultTempStep;
        }
        setValue(view, attrs.temperature,
                 isFinite(attrs.temperature) ? formatStepped(*attrs.temperature, step) : "", kDegree);
        if (isFinite(attrs.currentTemperature)) {
            const int decimals = std::max(decimalsForStep(step), 1);
            const std::string now = "now " + formatNumber(*attrs.currentTemperature, decimals) + kDegree;
            view.stateText = state.empty() ? now : state + ", " + now;
        }
    } else if (action == "position") {
        setValue(view, attrs.currentPosition, percentText(attrs.currentPosition), "%");
    } else if (action == "percentage") {
        setValue(view, attrs.percentage, percentText(attrs.percentage), "%");
    } else if (action == "volume") {
        const std::optional<double> percent = scaled(attrs.volumeLevel, 100.0);
        setValue(view, percent, percentText(percent), "%");
        if (attrs.isVolumeMuted.value_or(false)) {
            view.stateText = state.empty() ? "muted" : state + ", muted";
        }
    } else {
        // "none" (or an action this version does not know): the state is the value, except
        // unavailable / unknown, which go to the state line ("unavailable" does not fit the
        // value label).
        view.arcMin = view.arcMax = view.arcValue = 0;
        if (state.empty() || isUnavailable(state)) {
            view.value = kNoValue;
        } else {
            view.value = state;
            view.stateText.clear();
        }
    }

    view.dimmed = !context.haReady || isUnavailable(state);
    if (!context.haReady) {
        view.stateText = "HA offline";
    }
    return view;
}

}  // namespace presenter
}  // namespace core
