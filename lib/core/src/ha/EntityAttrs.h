#pragma once

#include <ArduinoJson.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// The typed subset of Home Assistant entity attributes the device uses (plan-step6.md §3.3).
// The one attribute model for the entity catalog, the live EntityStore and the presenter.
// Pure. Attributes not listed in kAttributeKeys are never parsed or stored.
namespace core {
namespace ha {

// Bits of EntityAttrs::colorModes, one per entry of a light's "supported_color_modes".
// HA's "unknown" sets no bit; any other unrecognised mode string sets kColorOther.
enum ColorModeBit : uint16_t {
    kColorOnOff = 1u << 0,
    kColorBrightness = 1u << 1,
    kColorTemp = 1u << 2,
    kColorHs = 1u << 3,
    kColorXy = 1u << 4,
    kColorRgb = 1u << 5,
    kColorRgbw = 1u << 6,
    kColorRgbww = 1u << 7,
    kColorWhite = 1u << 8,
    kColorOther = 1u << 9,
};

struct EntityAttrs {
    std::string friendlyName;        // "friendly_name" ("" if absent)
    uint32_t supportedFeatures = 0;  // "supported_features" bitmask (0 if absent)
    uint16_t colorModes = 0;         // "supported_color_modes" as ColorModeBit mask
    std::optional<double> brightness;            // 0-255
    std::optional<double> colorTempKelvin;       // "color_temp_kelvin"
    std::optional<double> minColorTempKelvin;    // "min_color_temp_kelvin"
    std::optional<double> maxColorTempKelvin;    // "max_color_temp_kelvin"
    std::optional<double> hue;                   // "hs_color"[0], degrees
    std::optional<double> saturation;            // "hs_color"[1], %
    std::optional<double> temperature;           // climate target
    std::optional<double> currentTemperature;    // "current_temperature"
    std::optional<double> minTemp;               // "min_temp"
    std::optional<double> maxTemp;               // "max_temp"
    std::optional<double> targetTempStep;        // "target_temp_step"
    std::optional<double> currentPosition;       // cover "current_position", %
    std::optional<double> percentage;            // fan, %
    std::optional<double> percentageStep;        // fan "percentage_step"
    std::optional<double> volumeLevel;           // media_player "volume_level", 0-1
    std::optional<bool> isVolumeMuted;           // "is_volume_muted"
    bool temperatureKey = false;  // "temperature" is present (even when null)

    bool operator==(const EntityAttrs &other) const;
    bool operator!=(const EntityAttrs &other) const { return !(*this == other); }
};

// Exactly the JSON keys EntityAttrs reads (used to build ArduinoJson filters).
inline constexpr const char *kAttributeKeys[] = {
    "friendly_name",       "supported_features",    "supported_color_modes",
    "brightness",          "color_temp_kelvin",     "min_color_temp_kelvin",
    "max_color_temp_kelvin", "hs_color",            "temperature",
    "current_temperature", "min_temp",              "max_temp",
    "target_temp_step",    "current_position",      "percentage",
    "percentage_step",     "volume_level",          "is_volume_muted",
};
inline constexpr size_t kAttributeKeyCount = sizeof(kAttributeKeys) / sizeof(kAttributeKeys[0]);

enum class AttrMode : uint8_t {
    Replace,  // start from defaults, then take every known key of the object
    Merge,    // keep the current values, take only the keys present in the object
};

// Applies an attributes object. A JSON null resets that attribute (optional -> empty,
// friendly_name -> "", masks -> 0); a value of the wrong type is ignored (the attribute
// keeps its value, which in Replace mode is the default). "hs_color" sets hue and
// saturation only from an array whose first two items are numbers; null resets both.
// A null or non-object `attrs` is an empty object (Replace still resets everything).
void applyAttrs(JsonObjectConst attrs, EntityAttrs &out, AttrMode mode);

// Resets one attribute by its JSON key (the "-" part of a subscribe_entities change).
// Returns false for keys not in kAttributeKeys (nothing changes).
bool resetAttr(std::string_view key, EntityAttrs &out);

// Sets filter[key] = true for every kAttributeKeys entry (an ArduinoJson filter object
// for an "attributes" / "a" object).
void addAttributeFilter(JsonObject filter);

}  // namespace ha
}  // namespace core
