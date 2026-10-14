#include "control/DomainActions.h"

namespace core {
namespace domain_actions {

namespace {

constexpr double kStepTolerance = 1e-9;

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

}  // namespace domain_actions
}  // namespace core
