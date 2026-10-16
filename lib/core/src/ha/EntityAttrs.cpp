#include "ha/EntityAttrs.h"

namespace core {
namespace ha {

namespace {

// The optional<double> attributes, by JSON key ("hs_color" and "temperature" are special).
struct NumberKey {
    const char *key;
    std::optional<double> EntityAttrs::*member;
};

constexpr NumberKey kNumberKeys[] = {
    {"brightness", &EntityAttrs::brightness},
    {"color_temp_kelvin", &EntityAttrs::colorTempKelvin},
    {"min_color_temp_kelvin", &EntityAttrs::minColorTempKelvin},
    {"max_color_temp_kelvin", &EntityAttrs::maxColorTempKelvin},
    {"current_temperature", &EntityAttrs::currentTemperature},
    {"min_temp", &EntityAttrs::minTemp},
    {"max_temp", &EntityAttrs::maxTemp},
    {"target_temp_step", &EntityAttrs::targetTempStep},
    {"current_position", &EntityAttrs::currentPosition},
    {"percentage", &EntityAttrs::percentage},
    {"percentage_step", &EntityAttrs::percentageStep},
    {"volume_level", &EntityAttrs::volumeLevel},
};

struct ColorModeName {
    const char *name;
    uint16_t bit;
};

constexpr ColorModeName kColorModeNames[] = {
    {"onoff", kColorOnOff}, {"brightness", kColorBrightness}, {"color_temp", kColorTemp},
    {"hs", kColorHs},       {"xy", kColorXy},                 {"rgb", kColorRgb},
    {"rgbw", kColorRgbw},   {"rgbww", kColorRgbww},           {"white", kColorWhite},
};

std::optional<double> EntityAttrs::*numberMember(std::string_view key) {
    for (const NumberKey &nk : kNumberKeys) {
        if (key == nk.key) {
            return nk.member;
        }
    }
    return nullptr;
}

uint16_t colorModeMask(JsonArrayConst modes) {
    uint16_t mask = 0;
    for (JsonVariantConst mode : modes) {
        if (!mode.is<const char *>()) {
            continue;
        }
        const std::string_view name = mode.as<const char *>();
        if (name == "unknown") {
            continue;
        }
        uint16_t bit = kColorOther;
        for (const ColorModeName &cm : kColorModeNames) {
            if (name == cm.name) {
                bit = cm.bit;
                break;
            }
        }
        mask |= bit;
    }
    return mask;
}

// Applies one non-null attribute value; wrong types and unknown keys are ignored.
void applyValue(std::string_view key, JsonVariantConst value, EntityAttrs &out) {
    if (std::optional<double> EntityAttrs::*member = numberMember(key)) {
        if (value.is<double>()) {
            out.*member = value.as<double>();
        }
    } else if (key == "temperature") {
        out.temperatureKey = true;
        if (value.is<double>()) {
            out.temperature = value.as<double>();
        }
    } else if (key == "friendly_name") {
        if (value.is<const char *>()) {
            out.friendlyName = value.as<const char *>();
        }
    } else if (key == "supported_features") {
        if (value.is<uint32_t>()) {
            out.supportedFeatures = value.as<uint32_t>();
        }
    } else if (key == "supported_color_modes") {
        if (value.is<JsonArrayConst>()) {
            out.colorModes = colorModeMask(value.as<JsonArrayConst>());
        }
    } else if (key == "hs_color") {
        JsonArrayConst hs = value.as<JsonArrayConst>();
        if (hs.size() >= 2 && hs[0].is<double>() && hs[1].is<double>()) {
            out.hue = hs[0].as<double>();
            out.saturation = hs[1].as<double>();
        }
    } else if (key == "is_volume_muted") {
        if (value.is<bool>()) {
            out.isVolumeMuted = value.as<bool>();
        }
    }
}

}  // namespace

bool EntityAttrs::operator==(const EntityAttrs &o) const {
    return friendlyName == o.friendlyName && supportedFeatures == o.supportedFeatures &&
           colorModes == o.colorModes && brightness == o.brightness &&
           colorTempKelvin == o.colorTempKelvin && minColorTempKelvin == o.minColorTempKelvin &&
           maxColorTempKelvin == o.maxColorTempKelvin && hue == o.hue && saturation == o.saturation &&
           temperature == o.temperature && currentTemperature == o.currentTemperature &&
           minTemp == o.minTemp && maxTemp == o.maxTemp && targetTempStep == o.targetTempStep &&
           currentPosition == o.currentPosition && percentage == o.percentage &&
           percentageStep == o.percentageStep && volumeLevel == o.volumeLevel &&
           isVolumeMuted == o.isVolumeMuted && temperatureKey == o.temperatureKey;
}

void applyAttrs(JsonObjectConst attrs, EntityAttrs &out, AttrMode mode) {
    if (mode == AttrMode::Replace) {
        out = EntityAttrs();
    }
    if (attrs.isNull()) {
        return;
    }
    for (JsonPairConst kv : attrs) {
        const std::string_view key(kv.key().c_str(), kv.key().size());
        if (kv.value().isNull()) {
            // null resets the attribute; "temperature": null still counts as present
            resetAttr(key, out);
            if (key == "temperature") {
                out.temperatureKey = true;
            }
        } else {
            applyValue(key, kv.value(), out);
        }
    }
}

bool resetAttr(std::string_view key, EntityAttrs &out) {
    if (std::optional<double> EntityAttrs::*member = numberMember(key)) {
        (out.*member).reset();
    } else if (key == "temperature") {
        out.temperature.reset();
        out.temperatureKey = false;
    } else if (key == "friendly_name") {
        out.friendlyName.clear();
    } else if (key == "supported_features") {
        out.supportedFeatures = 0;
    } else if (key == "supported_color_modes") {
        out.colorModes = 0;
    } else if (key == "hs_color") {
        out.hue.reset();
        out.saturation.reset();
    } else if (key == "is_volume_muted") {
        out.isVolumeMuted.reset();
    } else {
        return false;
    }
    return true;
}

void addAttributeFilter(JsonObject filter) {
    for (const char *key : kAttributeKeys) {
        filter[key] = true;
    }
}

}  // namespace ha
}  // namespace core
