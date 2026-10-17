#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "config/ConfigModel.h"
#include "control/LocalValue.h"
#include "ha/EntityStore.h"

namespace core {
namespace ha {
enum class State : uint8_t;  // ha/HaSession.h
}  // namespace ha

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

// Why HA is not usable, shown on the control page's state line while it is not Ready
// (plan-step8.md §3.3, Q8-15).
enum class OfflineReason : uint8_t { None, NoWifi, HaNotSetUp, Connecting, TokenRejected, Offline };

// Ready -> None (whatever wifiUp says); else !wifiUp -> NoWifi; NoConfig -> HaNotSetUp;
// Connecting / Authenticating / Subscribing -> Connecting; AuthFailed -> TokenRejected;
// Backoff (and anything else) -> Offline.
OfflineReason offlineReason(ha::State state, bool wifiUp);

// "", "No Wi-Fi", "HA not set up", "Connecting to HA...", "HA token rejected", "HA offline".
const char *offlineText(OfflineReason reason);

// What present() needs besides the control and its entity.
struct PresentContext {
    bool haReady = false;           // HA session state Ready
    // Why HA is not Ready; used only while !haReady. The default (and None) gives "HA offline".
    OfflineReason offline = OfflineReason::Offline;
    bool snapshotSeen = false;      // EntityStore::snapshotSeen()
    uint32_t msSinceSubscribe = 0;  // since the current subscription was started
    // The control's local value while it holds (ControlEngine::local(), plan-step7.md Q7-1);
    // shown instead of HA's value. Ignored for actions without a value ("none") and for a
    // control whose entity is missing.
    std::optional<control::LocalValue> local;
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
    std::string stateText;  // entity state / offlineText() / "waiting..." / "not found"
    bool dimmed = false;    // HA not Ready, entity missing, unavailable or unknown
    bool banner = false;    // HA not Ready: the state line is shown as the highlighted pill
    bool unconfirmed = false;  // the value is a local one not yet confirmed by HA (Q7-6)
    std::optional<uint32_t> arcRgb;  // hue action: the arc colour 0xRRGGBB of the shown hue

    bool operator==(const ControlView &o) const;
    bool operator!=(const ControlView &o) const { return !(*this == o); }
};

// The colour of a hue in degrees at full saturation and value (HSV(h, 100 %, 100 %)) as
// 0xRRGGBB: 0 -> 0xFF0000, 120 -> 0x00FF00, 240 -> 0x0000FF. Any hue is taken modulo 360
// (negative ones too); a non-finite hue counts as 0.
uint32_t hueToRgb(double hue);

// The page title of a control: its label, else the entity's friendly_name, else the
// entity id (entity nullptr = not in the store). Also used by the Select list.
std::string titleFor(const config::Control &control, const ha::EntityState *entity);

// The view of one control (plan-step6.md §3.7). entity = the store's state of
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
// Ready: the last values dimmed, banner set and the state text offlineText(context.offline)
// ("HA offline" by default). Unknown actions show as "none".
// HA's values come from domain_actions::currentValue (fan: absent and state "off" -> 0 %).
// context.local (Step 7.5, Q7-6): the action's value is the local one, formatted like HA's
// (fan percentage floored like the call sends it: 66.667 -> "66"), unconfirmed is copied,
// the state text stays and dimmed is only "HA not Ready". Hue: arcRgb = hueToRgb(shown hue).
ControlView present(const config::Control &control, const ha::EntityState *entity,
                    const PresentContext &context);

}  // namespace presenter
}  // namespace core
