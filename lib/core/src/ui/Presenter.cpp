#include "ui/Presenter.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <optional>

#include "control/DomainActions.h"
#include "util/MathUtil.h"

namespace core {
namespace presenter {

namespace {
constexpr int kMaxDecimals = 6;
}  // namespace

int decimalsForStep(double step) {
    return core::decimalsForStep(step);
}

std::string formatNumber(double value, int decimals) {
    if (decimals < 0) decimals = 0;
    if (decimals > kMaxDecimals) decimals = kMaxDecimals;

    const double rounded = roundToDecimals(value, decimals);
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
constexpr double kDefaultTempStep = 0.5;
constexpr double kHueCycle = 360;

bool isFinite(const std::optional<double> &v) { return v.has_value() && std::isfinite(*v); }

// The DomainActions entry of an action with a value, looked up by name in any domain (like
// present() treats the action); nullptr for "none" and unknown actions.
const domain_actions::ActionSpec *valueAction(const std::string &action) {
    size_t count = 0;
    const domain_actions::DomainSpec *specs = domain_actions::domains(count);
    for (size_t i = 0; i < count; ++i) {
        const domain_actions::ActionSpec *spec = domain_actions::findAction(specs[i], action);
        if (spec != nullptr && spec->hasStep) {
            return spec;
        }
    }
    return nullptr;
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

bool isUnavailable(const std::string &state) { return state == "unavailable" || state == "unknown"; }

}  // namespace

bool waitOver(const PresentContext &context) {
    return context.snapshotSeen || context.msSinceSubscribe >= kNotFoundAfterMs;
}

bool ControlView::operator==(const ControlView &o) const {
    return title == o.title && value == o.value && unit == o.unit && hasArc == o.hasArc &&
           arcMin == o.arcMin && arcMax == o.arcMax && arcValue == o.arcValue &&
           stateText == o.stateText && dimmed == o.dimmed && unconfirmed == o.unconfirmed &&
           arcRgb == o.arcRgb;
}

uint32_t hueToRgb(double hue) {
    double h = std::isfinite(hue) ? std::fmod(hue, kHueCycle) : 0.0;
    if (h < 0) h += kHueCycle;
    if (h >= kHueCycle) h = 0;  // a tiny negative hue + 360 can round up to 360
    const double sector = h / 60.0;
    // The channel that rises or falls within the sector, 0-255.
    const double x = 255.0 * (1.0 - std::fabs(std::fmod(sector, 2.0) - 1.0));
    const uint32_t full = 255;
    const uint32_t part = static_cast<uint32_t>(std::lround(std::min(std::max(x, 0.0), 255.0)));
    uint32_t r = 0, g = 0, b = 0;
    switch (static_cast<int>(sector)) {
        case 0: r = full; g = part; break;
        case 1: r = part; g = full; break;
        case 2: g = full; b = part; break;
        case 3: g = part; b = full; break;
        case 4: r = part; b = full; break;
        default: r = full; b = part; break;
    }
    return (r << 16) | (g << 8) | b;
}

std::string titleFor(const config::Control &control, const ha::EntityState *entity) {
    if (!control.label.empty()) {
        return control.label;
    }
    if (entity != nullptr && !entity->attrs.friendlyName.empty()) {
        return std::string(entity->attrs.friendlyName.data(), entity->attrs.friendlyName.size());
    }
    return control.entityId;
}

ControlView present(const config::Control &control, const ha::EntityState *entity,
                    const PresentContext &context) {
    ControlView view;
    view.title = titleFor(control, entity);

    const std::string &action = control.action;
    const domain_actions::ActionSpec *spec = valueAction(action);
    view.hasArc = spec != nullptr;

    // Ranges first: they do not depend on a value.
    const ha::EntityAttrs empty;
    const ha::EntityAttrs &attrs = entity != nullptr ? entity->attrs : empty;
    if (spec != nullptr) {
        const domain_actions::ValueRange range = domain_actions::valueRange(*spec, attrs);
        view.arcMin = range.min;
        view.arcMax = range.max;
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

    // The shown value: the local one while the control holds, else HA's.
    const bool useLocal = spec != nullptr && context.local.has_value();
    std::optional<double> value;
    if (useLocal) {
        value = context.local->value;
        view.unconfirmed = context.local->unconfirmed;
    } else if (spec != nullptr) {
        value = domain_actions::currentValue(*spec, *entity);
    }
    const bool has = isFinite(value);

    if (action == "brightness") {
        setValue(view, value, has ? formatPercent(*value) : "", "%");
    } else if (action == "hue") {
        setValue(view, value, has ? formatNumber(*value, 0) : "", kDegree);
        if (has) {
            view.arcRgb = hueToRgb(*value);
        }
    } else if (action == "color_temp") {
        setValue(view, value, has ? formatNumber(*value, 0) : "", "K");
    } else if (action == "target_temp") {
        // The configured step (validated); a missing one falls back to HA's step, then 0.5.
        double step = control.step;
        if (!(step > 0)) {
            step = isFinite(attrs.targetTempStep) && *attrs.targetTempStep > 0 ? *attrs.targetTempStep
                                                                               : kDefaultTempStep;
        }
        setValue(view, value, has ? formatStepped(*value, step) : "", kDegree);
        if (isFinite(attrs.currentTemperature)) {
            const int decimals = std::max(decimalsForStep(step), 1);
            const std::string now = "now " + formatNumber(*attrs.currentTemperature, decimals) + kDegree;
            view.stateText = state.empty() ? now : state + ", " + now;
        }
    } else if (action == "percentage") {
        // A local value lies on HA's speed grid (e.g. 66.667) and is sent floored.
        if (useLocal && has) {
            value = domain_actions::sentPercentage(*value);
        }
        setValue(view, value, has ? formatPercent(*value) : "", "%");
    } else if (action == "position" || action == "volume") {
        setValue(view, value, has ? formatPercent(*value) : "", "%");
        if (action == "volume" && attrs.isVolumeMuted.value_or(false)) {
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

    // A local value is the user's own turn: dimmed only while HA is not Ready (Q7-6).
    view.dimmed = !context.haReady || (!useLocal && isUnavailable(state));
    if (!context.haReady) {
        view.stateText = "HA offline";
    }
    return view;
}

}  // namespace presenter
}  // namespace core
