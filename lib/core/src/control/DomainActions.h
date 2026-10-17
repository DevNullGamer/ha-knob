#pragma once

#include <ArduinoJson.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ha/EntityAttrs.h"
#include "ha/EntityStore.h"

// The single source of truth for what each Home Assistant domain supports: encoder
// actions (with step default and range) and press actions. Config validation uses it;
// the entity picker uses availableActions() / stepDefaultFor() and the web UI gets the
// table from metaJson() (/api/meta/actions); the presenter and the control engine use the
// value math and service mapping below (plan-step7.md §3.1).
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
// the action range (else the table default 0.5); fan percentage uses percentage_step
// unrounded (e.g. 33.333.., one speed per detent) clamped to the action range 1-50 (else the
// table default 10); every other action its table default (0 for actions without a step).
double stepDefaultFor(const ActionSpec &action, const ha::EntityAttrs &attrs);

// The table for the web UI (/api/meta/actions), compact JSON in exactly this shape:
//   {"max_controls":24,"max_label_bytes":48,"domains":[{"domain":"light","actions":[
//    {"name":"brightness","step":{"default":5,"min":1,"max":25}},..,{"name":"none"}],
//    "presses":["toggle","none"]},..]}
// Actions without a step have no "step" member. Domains and lists are in table order.
std::string metaJson(ArduinoJson::Allocator *allocator = nullptr);

// ---- Value math (plan-step7.md §3.1, Q7-4, Q7-5) -------------------------------------
// Values are in display units: % for brightness / position / percentage / volume, degrees
// for hue, kelvin for color_temp, the climate's unit for target_temp.

struct ValueRange {
    double min;
    double max;
    bool wrap;  // hue: values live in [min, max) and wrap around
};

// brightness, position, percentage, volume 0-100; hue 0-360 wrap; color_temp
// min/max_color_temp_kelvin if both are finite and max > min, else 2000-6500; target_temp
// min_temp/max_temp likewise, else 7-35. "none" and unknown actions: {0, 0, false}.
ValueRange valueRange(const ActionSpec &action, const ha::EntityAttrs &attrs);

// HA's current value in display units: brightness round(brightness * 100 / 255) clamped to
// 0-100 (state "off" or no brightness -> 0); hue hs_color[0]; color_temp color_temp_kelvin;
// target_temp temperature; position current_position; percentage percentage (absent and
// state "off" -> 0); volume volume_level * 100. nullopt when the value is absent or not
// finite, and for "none" / unknown actions.
std::optional<double> currentValue(const ActionSpec &action, const ha::EntityState &entity);

// The base of the next turn: currentValue(), else the start value of Q7-5 (hue 0, color_temp
// the middle of its range snapped to step), else nullopt (no value to change).
std::optional<double> baseValue(const ActionSpec &action, const ha::EntityState &entity,
                                double step);

// base moved by steps x step (Q7-4): snapped to the step grid (round((base + steps * step) /
// step) * step), clamped to [min, max], rounded to decimalsForStep(step) decimals. Wrapping
// ranges (hue) are not snapped (a 360 cycle is not a multiple of every step): base +
// steps * step modulo the span, never negative, in [min, max). A non-positive or non-finite
// step only clamps / wraps base; a non-finite base counts as min.
double stepValue(double base, int steps, double step, const ValueRange &range);

// ---- Service mapping (plan-step7.md §3.1) ---------------------------------------------

// Why a build produced no call.
enum class NoCall : uint8_t {
    None,         // a call was built
    NoAction,     // action / press "none", or not one of the domain's
    NotFound,     // the entity is not in the store
    Unavailable,  // state "unavailable", "unknown" or ""
    NoValue,      // no value to change (Q7-5: setpoint null, no position, no volume, ...)
};

struct ServiceCall {
    std::string domain;
    std::string service;
    std::string entityId;
    JsonDocument data;  // null = no service_data
};

struct CallBuild {
    std::optional<ServiceCall> call;
    NoCall reason = NoCall::None;
};

// The service call that sets the encoder action to value (already stepped):
//   brightness  value 0 -> light.turn_off (no data); else light.turn_on {"brightness_pct":int}
//   hue         light.turn_on {"hs_color":[int, s]}, s = saturation if > 0 else 100
//   color_temp  light.turn_on {"color_temp_kelvin":int}
//   target_temp climate.set_temperature {"temperature":value}
//   position    cover.set_cover_position {"position":int}
//   percentage  fan.set_percentage {"percentage":floor(value + 1e-6)} (HA's speed grid)
//   volume      media_player.volume_set {"volume_level":round(value) / 100}
// Checked in this order: "none" or an action the domain does not offer -> NoAction;
// entity nullptr -> NotFound; state unavailable / unknown / "" -> Unavailable; no base
// value (baseValue() would be nullopt) or a non-finite value -> NoValue.
CallBuild encoderCall(const DomainSpec &domain, const ActionSpec &action,
                      const std::string &entityId, double value, const ha::EntityState *entity);

// The service call of a press: toggle -> <domain>.toggle, except climate: state "off" ->
// climate.turn_on, else climate.turn_off; stop -> cover.stop_cover; play_pause ->
// media_player.media_play_pause; mute -> media_player.volume_mute {"is_volume_muted":
// !is_volume_muted} (absent -> true). No service_data otherwise. "none" or a press the
// domain does not offer -> NoAction; then the NotFound / Unavailable rules of encoderCall.
CallBuild pressCall(const DomainSpec &domain, std::string_view press,
                    const std::string &entityId, const ha::EntityState *entity);

}  // namespace domain_actions
}  // namespace core
