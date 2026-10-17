#include "control/DomainActions.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

#include "config/ConfigModel.h"
#include "util/MathUtil.h"

namespace core {
namespace domain_actions {

namespace {

constexpr double kStepTolerance = 1e-9;

// supported_features bits (HA ClimateEntityFeature, CoverEntityFeature, FanEntityFeature,
// MediaPlayerEntityFeature).
constexpr uint32_t kClimateTargetTemperature = 1;
constexpr uint32_t kCoverSetPosition = 4;
constexpr uint32_t kFanSetSpeed = 1;
constexpr uint32_t kMediaPlayerVolumeSet = 4;

constexpr uint16_t kHueModes = ha::kColorHs | ha::kColorXy | ha::kColorRgb | ha::kColorRgbw |
                               ha::kColorRgbww;

// "none" is always a valid encoder action; it is listed last unless it is the only one.
constexpr ActionSpec kNone = {"none", false, 0, 0, 0};

constexpr ActionSpec kLightActions[] = {
    {"brightness", true, 5, 1, 25},     // %
    {"hue", true, 10, 1, 60},           // degrees, wraps 0-359
    {"color_temp", true, 100, 25, 1000},  // kelvin
    kNone,
};
constexpr ActionSpec kClimateActions[] = {
    {"target_temp", true, 0.5, 0.1, 5},  // default from target_temp_step at runtime (Step 7)
    kNone,
};
constexpr ActionSpec kCoverActions[] = {
    {"position", true, 10, 1, 25},
    kNone,
};
constexpr ActionSpec kFanActions[] = {
    {"percentage", true, 10, 1, 50},  // default from percentage_step at runtime (Step 7)
    kNone,
};
constexpr ActionSpec kMediaPlayerActions[] = {
    {"volume", true, 2, 1, 20},  // %
    kNone,
};
constexpr ActionSpec kSwitchActions[] = {
    kNone,
};

constexpr const char *kLightPresses[] = {"toggle", "none"};
constexpr const char *kClimatePresses[] = {"none", "toggle"};
constexpr const char *kCoverPresses[] = {"toggle", "stop", "none"};
constexpr const char *kFanPresses[] = {"toggle", "none"};
constexpr const char *kMediaPlayerPresses[] = {"play_pause", "toggle", "mute", "none"};
constexpr const char *kSwitchPresses[] = {"toggle", "none"};

template <typename T, size_t N>
constexpr size_t countOf(const T (&)[N]) {
    return N;
}

constexpr DomainSpec kDomains[] = {
    {"light", kLightActions, countOf(kLightActions), kLightPresses, countOf(kLightPresses)},
    {"climate", kClimateActions, countOf(kClimateActions), kClimatePresses,
     countOf(kClimatePresses)},
    {"cover", kCoverActions, countOf(kCoverActions), kCoverPresses, countOf(kCoverPresses)},
    {"fan", kFanActions, countOf(kFanActions), kFanPresses, countOf(kFanPresses)},
    {"media_player", kMediaPlayerActions, countOf(kMediaPlayerActions), kMediaPlayerPresses,
     countOf(kMediaPlayerPresses)},
    {"switch", kSwitchActions, countOf(kSwitchActions), kSwitchPresses, countOf(kSwitchPresses)},
    {"input_boolean", kSwitchActions, countOf(kSwitchActions), kSwitchPresses,
     countOf(kSwitchPresses)},
};

// Value ranges used when the entity reports none (HA's climate defaults for target_temp).
constexpr double kDefaultMinKelvin = 2000;
constexpr double kDefaultMaxKelvin = 6500;
constexpr double kDefaultMinTemp = 7;
constexpr double kDefaultMaxTemp = 35;
constexpr double kHueCycle = 360;
constexpr double kPercentFloorSlack = 1e-6;  // 99.9999999 (step 33.33333333) -> 100

bool isAction(const ActionSpec &action, const char *name) {
    return std::strcmp(action.name, name) == 0;
}

bool isFinite(const std::optional<double> &v) { return v.has_value() && std::isfinite(*v); }

// The entity's range if it is usable, else the default one.
ValueRange rangeOr(const std::optional<double> &lo, const std::optional<double> &hi,
                   double defaultLo, double defaultHi) {
    if (isFinite(lo) && isFinite(hi) && *hi > *lo) {
        return {*lo, *hi, false};
    }
    return {defaultLo, defaultHi, false};
}

std::optional<double> finiteOrNull(const std::optional<double> &v) {
    return isFinite(v) ? v : std::nullopt;
}

bool isUnavailableState(std::string_view state) {
    return state.empty() || state == "unavailable" || state == "unknown";
}

int toInt(double v) { return static_cast<int>(std::lround(v)); }

// The common part of encoderCall / pressCall: NotFound and Unavailable.
NoCall entityProblem(const ha::EntityState *entity) {
    if (entity == nullptr) {
        return NoCall::NotFound;
    }
    const std::string_view state(entity->state.data(), entity->state.size());
    return isUnavailableState(state) ? NoCall::Unavailable : NoCall::None;
}

CallBuild noCall(NoCall reason) {
    CallBuild build;
    build.reason = reason;
    return build;
}

CallBuild makeCall(const char *domain, const char *service, const std::string &entityId,
                   JsonDocument data = JsonDocument()) {
    CallBuild build;
    build.call.emplace();
    build.call->domain = domain;
    build.call->service = service;
    build.call->entityId = entityId;
    build.call->data = std::move(data);
    return build;
}

bool actionAvailable(const DomainSpec &domain, const ActionSpec &action,
                     const ha::EntityAttrs &attrs) {
    if (!action.hasStep) {
        return true;  // "none"
    }
    const std::string_view d = domain.domain;
    const uint32_t features = attrs.supportedFeatures;
    if (d == "light") {
        if (isAction(action, "brightness")) {
            return (attrs.colorModes & ~ha::kColorOnOff) != 0;
        }
        if (isAction(action, "hue")) {
            return (attrs.colorModes & kHueModes) != 0;
        }
        if (isAction(action, "color_temp")) {
            return (attrs.colorModes & ha::kColorTemp) != 0;
        }
    } else if (d == "climate" && isAction(action, "target_temp")) {
        return (features & kClimateTargetTemperature) != 0 && attrs.temperatureKey;
    } else if (d == "cover" && isAction(action, "position")) {
        return (features & kCoverSetPosition) != 0;
    } else if (d == "fan" && isAction(action, "percentage")) {
        return (features & kFanSetSpeed) != 0;
    } else if (d == "media_player" && isAction(action, "volume")) {
        return (features & kMediaPlayerVolumeSet) != 0;
    }
    return false;  // an action without a rule is never offered
}

}  // namespace

const DomainSpec *domains(size_t &count) {
    count = countOf(kDomains);
    return kDomains;
}

std::string_view domainOf(std::string_view entityId) {
    const size_t dot = entityId.find('.');
    return dot == std::string_view::npos ? std::string_view() : entityId.substr(0, dot);
}

const DomainSpec *findDomain(std::string_view domain) {
    for (const DomainSpec &spec : kDomains) {
        if (domain == spec.domain) {
            return &spec;
        }
    }
    return nullptr;
}

const ActionSpec *findAction(const DomainSpec &domain, std::string_view action) {
    for (size_t i = 0; i < domain.actionCount; ++i) {
        if (action == domain.actions[i].name) {
            return &domain.actions[i];
        }
    }
    return nullptr;
}

bool hasPress(const DomainSpec &domain, std::string_view press) {
    for (size_t i = 0; i < domain.pressCount; ++i) {
        if (press == domain.presses[i]) {
            return true;
        }
    }
    return false;
}

bool stepInRange(const ActionSpec &action, double step) {
    if (!action.hasStep) {
        return true;
    }
    return step >= action.stepMin - kStepTolerance && step <= action.stepMax + kStepTolerance;
}

std::vector<const ActionSpec *> availableActions(const DomainSpec &domain,
                                                 const ha::EntityAttrs &attrs) {
    std::vector<const ActionSpec *> out;
    for (size_t i = 0; i < domain.actionCount; ++i) {
        if (actionAvailable(domain, domain.actions[i], attrs)) {
            out.push_back(&domain.actions[i]);
        }
    }
    return out;
}

double stepDefaultFor(const ActionSpec &action, const ha::EntityAttrs &attrs) {
    if (isAction(action, "target_temp")) {
        const std::optional<double> &step = attrs.targetTempStep;
        return step && std::isfinite(*step) && stepInRange(action, *step) ? *step
                                                                          : action.stepDefault;
    }
    if (isAction(action, "percentage")) {
        const std::optional<double> &step = attrs.percentageStep;
        if (!step || !std::isfinite(*step) || *step <= 0) {
            return action.stepDefault;
        }
        // HA reports 100/count, e.g. 33.333..; kept exact so one detent is one speed
        // (Amendment A1), only clamped to the action range
        return *step < action.stepMin   ? action.stepMin
               : *step > action.stepMax ? action.stepMax
                                        : *step;
    }
    return action.stepDefault;
}

ValueRange valueRange(const ActionSpec &action, const ha::EntityAttrs &attrs) {
    if (isAction(action, "brightness") || isAction(action, "position") ||
        isAction(action, "percentage") || isAction(action, "volume")) {
        return {0, 100, false};
    }
    if (isAction(action, "hue")) {
        return {0, kHueCycle, true};
    }
    if (isAction(action, "color_temp")) {
        return rangeOr(attrs.minColorTempKelvin, attrs.maxColorTempKelvin, kDefaultMinKelvin,
                       kDefaultMaxKelvin);
    }
    if (isAction(action, "target_temp")) {
        return rangeOr(attrs.minTemp, attrs.maxTemp, kDefaultMinTemp, kDefaultMaxTemp);
    }
    return {0, 0, false};
}

std::optional<double> currentValue(const ActionSpec &action, const ha::EntityState &entity) {
    const ha::EntityAttrs &attrs = entity.attrs;
    const std::string_view state(entity.state.data(), entity.state.size());
    if (isAction(action, "brightness")) {
        if (state == "off" || !isFinite(attrs.brightness)) {
            return 0.0;
        }
        return std::min(std::max(std::round(*attrs.brightness * 100.0 / 255.0), 0.0), 100.0);
    }
    if (isAction(action, "hue")) {
        return finiteOrNull(attrs.hue);
    }
    if (isAction(action, "color_temp")) {
        return finiteOrNull(attrs.colorTempKelvin);
    }
    if (isAction(action, "target_temp")) {
        return finiteOrNull(attrs.temperature);
    }
    if (isAction(action, "position")) {
        return finiteOrNull(attrs.currentPosition);
    }
    if (isAction(action, "percentage")) {
        if (isFinite(attrs.percentage)) {
            return attrs.percentage;
        }
        return state == "off" ? std::optional<double>(0.0) : std::nullopt;
    }
    if (isAction(action, "volume")) {
        if (!isFinite(attrs.volumeLevel)) {
            return std::nullopt;
        }
        return *attrs.volumeLevel * 100.0;
    }
    return std::nullopt;
}

std::optional<double> baseValue(const ActionSpec &action, const ha::EntityState &entity,
                                double step) {
    const std::optional<double> current = currentValue(action, entity);
    if (current) {
        return current;
    }
    if (isAction(action, "hue")) {
        return 0.0;
    }
    if (isAction(action, "color_temp")) {
        const ValueRange range = valueRange(action, entity.attrs);
        return stepValue((range.min + range.max) / 2, 0, step, range);
    }
    return std::nullopt;
}

double stepValue(double base, int steps, double step, const ValueRange &range) {
    const bool validStep = step > 0 && std::isfinite(step);
    const double span = range.max - range.min;
    double v = std::isfinite(base) ? base : range.min;
    if (validStep) {
        v += steps * step;
        if (!range.wrap) {
            v = std::round(v / step) * step;  // Q7-4: back on the step grid
        }
    }
    if (range.wrap) {
        if (span > 0) {
            v = std::fmod(v - range.min, span);
            if (v < 0) v += span;
            v += range.min;
        }
    } else {
        v = std::min(std::max(v, range.min), range.max);
    }
    if (validStep) {
        v = roundToDecimals(v, decimalsForStep(step));
    }
    if (range.wrap && span > 0 && v >= range.max) {
        v -= span;  // e.g. 359.9996 rounded up to 360
    }
    return v;
}

CallBuild encoderCall(const DomainSpec &domain, const ActionSpec &action,
                      const std::string &entityId, double value, const ha::EntityState *entity) {
    if (!action.hasStep || findAction(domain, action.name) == nullptr) {
        return noCall(NoCall::NoAction);
    }
    const NoCall problem = entityProblem(entity);
    if (problem != NoCall::None) {
        return noCall(problem);
    }
    if (!std::isfinite(value) || !baseValue(action, *entity, action.stepDefault)) {
        return noCall(NoCall::NoValue);
    }

    JsonDocument data;
    if (isAction(action, "brightness")) {
        const int percent = toInt(value);
        if (percent <= 0) {
            return makeCall(domain.domain, "turn_off", entityId);
        }
        data["brightness_pct"] = percent;
        return makeCall(domain.domain, "turn_on", entityId, std::move(data));
    }
    if (isAction(action, "hue")) {
        int hue = toInt(value);
        if (hue >= static_cast<int>(kHueCycle)) hue = 0;
        const std::optional<double> &saturation = entity->attrs.saturation;
        JsonArray hs = data["hs_color"].to<JsonArray>();
        hs.add(hue);
        hs.add(isFinite(saturation) && *saturation > 0 ? *saturation : 100.0);
        return makeCall(domain.domain, "turn_on", entityId, std::move(data));
    }
    if (isAction(action, "color_temp")) {
        data["color_temp_kelvin"] = toInt(value);
        return makeCall(domain.domain, "turn_on", entityId, std::move(data));
    }
    if (isAction(action, "target_temp")) {
        data["temperature"] = value;
        return makeCall(domain.domain, "set_temperature", entityId, std::move(data));
    }
    if (isAction(action, "position")) {
        data["position"] = toInt(value);
        return makeCall(domain.domain, "set_cover_position", entityId, std::move(data));
    }
    if (isAction(action, "percentage")) {
        // HA's speed percentages are int(k * 100 // count) (33, 66, 100): floor, not round,
        // so a value on the 100/count grid maps to that speed (Amendment A1)
        data["percentage"] = sentPercentage(value);
        return makeCall(domain.domain, "set_percentage", entityId, std::move(data));
    }
    if (isAction(action, "volume")) {
        data["volume_level"] = std::round(value) / 100.0;
        return makeCall(domain.domain, "volume_set", entityId, std::move(data));
    }
    return noCall(NoCall::NoAction);  // an action without a mapping
}

int sentPercentage(double value) {
    return static_cast<int>(std::floor(value + kPercentFloorSlack));
}

CallBuild pressCall(const DomainSpec &domain, std::string_view press,
                    const std::string &entityId, const ha::EntityState *entity) {
    if (press == "none" || !hasPress(domain, press)) {
        return noCall(NoCall::NoAction);
    }
    const NoCall problem = entityProblem(entity);
    if (problem != NoCall::None) {
        return noCall(problem);
    }

    if (press == "toggle") {
        if (std::string_view(domain.domain) == "climate") {
            const std::string_view state(entity->state.data(), entity->state.size());
            return makeCall(domain.domain, state == "off" ? "turn_on" : "turn_off", entityId);
        }
        return makeCall(domain.domain, "toggle", entityId);
    }
    if (press == "stop") {
        return makeCall(domain.domain, "stop_cover", entityId);
    }
    if (press == "play_pause") {
        return makeCall(domain.domain, "media_play_pause", entityId);
    }
    if (press == "mute") {
        JsonDocument data;
        data["is_volume_muted"] = !entity->attrs.isVolumeMuted.value_or(false);
        return makeCall(domain.domain, "volume_mute", entityId, std::move(data));
    }
    return noCall(NoCall::NoAction);  // a press without a mapping
}

std::string metaJson(ArduinoJson::Allocator *allocator) {
    JsonDocument doc = allocator != nullptr ? JsonDocument(allocator) : JsonDocument();
    doc["max_controls"] = config::kMaxControls;
    doc["max_label_bytes"] = config::kMaxLabelBytes;
    JsonArray list = doc["domains"].to<JsonArray>();
    for (const DomainSpec &spec : kDomains) {
        JsonObject d = list.add<JsonObject>();
        d["domain"] = spec.domain;
        JsonArray actions = d["actions"].to<JsonArray>();
        for (size_t i = 0; i < spec.actionCount; ++i) {
            const ActionSpec &a = spec.actions[i];
            JsonObject action = actions.add<JsonObject>();
            action["name"] = a.name;
            if (a.hasStep) {
                JsonObject step = action["step"].to<JsonObject>();
                step["default"] = a.stepDefault;
                step["min"] = a.stepMin;
                step["max"] = a.stepMax;
            }
        }
        JsonArray presses = d["presses"].to<JsonArray>();
        for (size_t i = 0; i < spec.pressCount; ++i) {
            presses.add(spec.presses[i]);
        }
    }
    std::string out;
    serializeJson(doc, out);
    return out;
}

}  // namespace domain_actions
}  // namespace core
