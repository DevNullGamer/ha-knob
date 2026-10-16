#include "control/DomainActions.h"

#include <cmath>
#include <cstring>

#include "config/ConfigModel.h"

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

bool isAction(const ActionSpec &action, const char *name) {
    return std::strcmp(action.name, name) == 0;
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
        // HA reports 100/count, e.g. 33.333..; the tolerance keeps 25.0000001 at 25
        const double rounded = std::ceil(*step - kStepTolerance);
        return rounded < action.stepMin   ? action.stepMin
               : rounded > action.stepMax ? action.stepMax
                                          : rounded;
    }
    return action.stepDefault;
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
