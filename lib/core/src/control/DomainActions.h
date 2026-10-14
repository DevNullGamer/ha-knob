#pragma once

#include <cstddef>
#include <string_view>

// The single source of truth for what each Home Assistant domain supports: encoder
// actions (with step default and range) and press actions. Config validation uses it
// now; the web UI gets it from /api/meta/actions (Step 6); the value math and service
// calls are added in Step 7.
namespace core {
namespace domain_actions {

struct ActionSpec {
    const char *name;
    bool hasStep;        // false for "none": the step is ignored
    double stepDefault;
    double stepMin;
    double stepMax;
};

struct DomainSpec {
    const char *domain;
    const ActionSpec *actions;  // default first
    size_t actionCount;
    const char *const *presses;  // default first
    size_t pressCount;
};

// All supported domains (light, climate, cover, fan, media_player, switch, input_boolean).
const DomainSpec *domains(size_t &count);

// Domain part of an entity id ("light.kitchen" -> "light"); empty if there is no '.'.
std::string_view domainOf(std::string_view entityId);

// nullptr if the domain is not supported.
const DomainSpec *findDomain(std::string_view domain);

// nullptr if the domain does not offer the action.
const ActionSpec *findAction(const DomainSpec &domain, std::string_view action);

bool hasPress(const DomainSpec &domain, std::string_view press);

// True if step lies within the action's range (a tiny tolerance absorbs float noise).
// Always true for actions without a step.
bool stepInRange(const ActionSpec &action, double step);

}  // namespace domain_actions
}  // namespace core
