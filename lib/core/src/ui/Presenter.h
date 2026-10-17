#pragma once

#include <cstdint>
#include <string>

#include "config/ConfigModel.h"
#include "ha/EntityStore.h"

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

// An entity that is not in the store is "waiting..." until the subscription's snapshot
// arrived or for this long after subscribing, whichever comes first; then "not found".
constexpr uint32_t kNotFoundAfterMs = 3000;

// What present() needs besides the control and its entity.
struct PresentContext {
    bool haReady = false;           // HA session state Ready
    bool snapshotSeen = false;      // EntityStore::snapshotSeen()
    uint32_t msSinceSubscribe = 0;  // since the current subscription was started
};

// True once a missing entity counts as "not found" (snapshot seen or kNotFoundAfterMs over).
bool waitOver(const PresentContext &context);

// One ControlScreen page (plan-step6.md §3.7). All texts are UTF-8.
struct ControlView {
    std::string title;      // label, else friendly_name, else entity_id
    std::string value;      // "50", "21.5", "on"; "--" if there is no value
    std::string unit;       // "%", "°" (U+00B0), "K" or ""; "" when value is "--"
    bool hasArc = false;    // false for the action "none"
    double arcMin = 0;
    double arcMax = 100;
    double arcValue = 0;    // within [arcMin, arcMax]; arcMin when there is no value
    std::string stateText;  // entity state / "HA offline" / "waiting..." / "not found"
    bool dimmed = false;    // HA not Ready, entity missing, unavailable or unknown

    bool operator==(const ControlView &o) const;
    bool operator!=(const ControlView &o) const { return !(*this == o); }
};

// The read-only view of one control (plan-step6.md §3.7). entity = the store's state of
// control.entityId (nullptr = not in the store). Per action:
//   brightness  round(brightness * 100 / 255) %, 0 % when the light is off or the value is
//               absent; arc 0-100
//   hue         hs_color[0] °; arc 0-360
//   color_temp  color_temp_kelvin K; arc min/max_color_temp_kelvin (else 2000-6500)
//   target_temp formatStepped(temperature, step) °; arc min_temp-max_temp (else 7-35);
//               state text "<state>, now <current_temperature>°"
//   position    current_position %; percentage %; volume volume_level * 100 % (", muted"
//               after the state when is_volume_muted); arcs 0-100
//   none        no arc, value = the entity state; for unavailable / unknown (or no state)
//               the value is "--" and the state goes to the state text
// An absent value (other than brightness) shows "--" with the arc at its minimum. Not
// Ready: the last values dimmed, state text "HA offline". Unknown actions show as "none".
ControlView present(const config::Control &control, const ha::EntityState *entity,
                    const PresentContext &context);

}  // namespace presenter
}  // namespace core
