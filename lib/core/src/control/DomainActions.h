#pragma once

#include <ArduinoJson.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "ha/EntityAttrs.h"

// The single source of truth for what each Home Assistant domain supports: encoder
// actions (with step default and range) and press actions. Config validation uses it;
// the entity picker uses availableActions() / stepDefaultFor() and the web UI gets the
// table from metaJson() (/api/meta/actions); the value math and service calls are added
// in Step 7.
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

// The domain's actions an entity with these attributes supports, in table order; "none"
// is always included (plan-step6.md §3.4):
//   light  brightness: any color mode other than onoff (HA's "unknown" sets no mode bit);
//          hue: hs, xy, rgb, rgbw or rgbww; color_temp: color_temp
//   climate target_temp: supported_features bit 0 and a "temperature" key (null counts)
//   cover position: bit 2 (4); fan percentage: bit 0 (1); media_player volume: bit 2 (4)
//   switch, input_boolean: "none" only
std::vector<const ActionSpec *> availableActions(const DomainSpec &domain,
                                                 const ha::EntityAttrs &attrs);

// Step default for an entity: climate target_temp uses target_temp_step if it lies within
// the action range (else the table default 0.5); fan percentage uses ceil(percentage_step)
// clamped to the action range 1-50 (else the table default 10); every other action its
// table default (0 for actions without a step).
double stepDefaultFor(const ActionSpec &action, const ha::EntityAttrs &attrs);

// The table for the web UI (/api/meta/actions), compact JSON in exactly this shape:
//   {"max_controls":24,"max_label_bytes":48,"domains":[{"domain":"light","actions":[
//    {"name":"brightness","step":{"default":5,"min":1,"max":25}},..,{"name":"none"}],
//    "presses":["toggle","none"]},..]}
// Actions without a step have no "step" member. Domains and lists are in table order.
std::string metaJson(ArduinoJson::Allocator *allocator = nullptr);

}  // namespace domain_actions
}  // namespace core
