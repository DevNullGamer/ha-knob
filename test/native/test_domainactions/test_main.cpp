// Native tests for EntityAttrs and DomainActions availability / step default / meta JSON
// (plan-step6.md sections 3.3, 3.4, Step 6.1).
#include <unity.h>

#include <ArduinoJson.h>

#include <string>
#include <vector>

#include "control/DomainActions.h"
#include "ha/EntityAttrs.h"

namespace da = core::domain_actions;
namespace ha = core::ha;

void setUp() {}
void tearDown() {}

// ---- helpers ----

static ha::EntityAttrs attrsFrom(const char *json, ha::AttrMode mode = ha::AttrMode::Replace,
                                 ha::EntityAttrs base = ha::EntityAttrs()) {
    JsonDocument doc;
    TEST_ASSERT_TRUE(!deserializeJson(doc, json));
    ha::applyAttrs(doc.as<JsonObjectConst>(), base, mode);
    return base;
}

static std::string available(const char *domain, const ha::EntityAttrs &a) {
    const da::DomainSpec *spec = da::findDomain(domain);
    TEST_ASSERT_NOT_NULL(spec);
    std::string out;
    for (const da::ActionSpec *act : da::availableActions(*spec, a)) {
        if (!out.empty()) out += ",";
        out += act->name;
    }
    return out;
}

static std::string availableJson(const char *domain, const char *attrsJson) {
    return available(domain, attrsFrom(attrsJson));
}

// ---- availableActions: light ----

static void test_light_onoff_only() {
    TEST_ASSERT_EQUAL_STRING("none", availableJson("light", "{\"supported_color_modes\":[\"onoff\"]}").c_str());
}

static void test_light_no_modes_or_unknown() {
    TEST_ASSERT_EQUAL_STRING("none", availableJson("light", "{}").c_str());
    TEST_ASSERT_EQUAL_STRING("none", availableJson("light", "{\"supported_color_modes\":[\"unknown\"]}").c_str());
    TEST_ASSERT_EQUAL_STRING("none", availableJson("light", "{\"supported_color_modes\":null}").c_str());
}

static void test_light_brightness_only() {
    TEST_ASSERT_EQUAL_STRING("brightness,none",
                             availableJson("light", "{\"supported_color_modes\":[\"brightness\"]}").c_str());
}

static void test_light_color_temp() {
    TEST_ASSERT_EQUAL_STRING("brightness,color_temp,none",
                             availableJson("light", "{\"supported_color_modes\":[\"color_temp\"]}").c_str());
}

static void test_light_hs() {
    TEST_ASSERT_EQUAL_STRING("brightness,hue,none",
                             availableJson("light", "{\"supported_color_modes\":[\"hs\"]}").c_str());
}

static void test_light_xy() {
    TEST_ASSERT_EQUAL_STRING("brightness,hue,none",
                             availableJson("light", "{\"supported_color_modes\":[\"xy\"]}").c_str());
}

static void test_light_rgb_rgbw() {
    TEST_ASSERT_EQUAL_STRING("brightness,hue,none",
                             availableJson("light", "{\"supported_color_modes\":[\"rgb\"]}").c_str());
    TEST_ASSERT_EQUAL_STRING("brightness,hue,none",
                             availableJson("light", "{\"supported_color_modes\":[\"rgbw\"]}").c_str());
}

static void test_light_rgbww() {
    TEST_ASSERT_EQUAL_STRING("brightness,hue,color_temp,none",
                             availableJson("light", "{\"supported_color_modes\":[\"color_temp\",\"rgbww\"]}").c_str());
    TEST_ASSERT_EQUAL_STRING("brightness,hue,none",
                             availableJson("light", "{\"supported_color_modes\":[\"rgbww\"]}").c_str());
}

static void test_light_white_mode_offers_brightness_only() {
    TEST_ASSERT_EQUAL_STRING("brightness,none",
                             availableJson("light", "{\"supported_color_modes\":[\"white\"]}").c_str());
}

static void test_light_onoff_plus_brightness() {
    TEST_ASSERT_EQUAL_STRING("brightness,none",
                             availableJson("light", "{\"supported_color_modes\":[\"onoff\",\"brightness\"]}").c_str());
}

static void test_light_unknown_mixed_with_real_mode() {
    TEST_ASSERT_EQUAL_STRING("brightness,hue,none",
                             availableJson("light", "{\"supported_color_modes\":[\"unknown\",\"hs\"]}").c_str());
}

// ---- availableActions: other domains ----

static void test_climate() {
    TEST_ASSERT_EQUAL_STRING("target_temp,none",
                             availableJson("climate", "{\"supported_features\":1,\"temperature\":21.5}").c_str());
    // temperature key present but null (climate in "off") still counts
    TEST_ASSERT_EQUAL_STRING("target_temp,none",
                             availableJson("climate", "{\"supported_features\":1,\"temperature\":null}").c_str());
    // feature bit among others
    TEST_ASSERT_EQUAL_STRING("target_temp,none",
                             availableJson("climate", "{\"supported_features\":385,\"temperature\":20}").c_str());
    // no temperature key
    TEST_ASSERT_EQUAL_STRING("none", availableJson("climate", "{\"supported_features\":1}").c_str());
    // no bit 0 (e.g. only target range = 2)
    TEST_ASSERT_EQUAL_STRING("none",
                             availableJson("climate", "{\"supported_features\":2,\"temperature\":21}").c_str());
    TEST_ASSERT_EQUAL_STRING("none", availableJson("climate", "{\"temperature\":21}").c_str());
}

static void test_cover() {
    TEST_ASSERT_EQUAL_STRING("position,none", availableJson("cover", "{\"supported_features\":4}").c_str());
    TEST_ASSERT_EQUAL_STRING("position,none", availableJson("cover", "{\"supported_features\":15}").c_str());
    TEST_ASSERT_EQUAL_STRING("none", availableJson("cover", "{\"supported_features\":11}").c_str());
    TEST_ASSERT_EQUAL_STRING("none", availableJson("cover", "{}").c_str());
}

static void test_fan() {
    TEST_ASSERT_EQUAL_STRING("percentage,none", availableJson("fan", "{\"supported_features\":1}").c_str());
    TEST_ASSERT_EQUAL_STRING("percentage,none", availableJson("fan", "{\"supported_features\":57}").c_str());
    TEST_ASSERT_EQUAL_STRING("none", availableJson("fan", "{\"supported_features\":2}").c_str());
    TEST_ASSERT_EQUAL_STRING("none", availableJson("fan", "{}").c_str());
}

static void test_media_player() {
    TEST_ASSERT_EQUAL_STRING("volume,none", availableJson("media_player", "{\"supported_features\":4}").c_str());
    TEST_ASSERT_EQUAL_STRING("volume,none", availableJson("media_player", "{\"supported_features\":152461}").c_str());
    TEST_ASSERT_EQUAL_STRING("none", availableJson("media_player", "{\"supported_features\":3}").c_str());
    TEST_ASSERT_EQUAL_STRING("none", availableJson("media_player", "{}").c_str());
}

static void test_switch_and_input_boolean() {
    // attributes never add actions to switch / input_boolean
    const char *rich = "{\"supported_features\":65535,\"supported_color_modes\":[\"hs\"],\"temperature\":1}";
    TEST_ASSERT_EQUAL_STRING("none", availableJson("switch", "{}").c_str());
    TEST_ASSERT_EQUAL_STRING("none", availableJson("switch", rich).c_str());
    TEST_ASSERT_EQUAL_STRING("none", availableJson("input_boolean", "{}").c_str());
    TEST_ASSERT_EQUAL_STRING("none", availableJson("input_boolean", rich).c_str());
}

static void test_none_always_included_for_every_domain() {
    size_t n = 0;
    const da::DomainSpec *all = da::domains(n);
    TEST_ASSERT_TRUE(n >= 7);
    for (size_t i = 0; i < n; ++i) {
        auto acts = da::availableActions(all[i], ha::EntityAttrs());
        TEST_ASSERT_TRUE_MESSAGE(!acts.empty(), all[i].domain);
        TEST_ASSERT_EQUAL_STRING_MESSAGE("none", acts.back()->name, all[i].domain);
    }
}

// ---- stepDefaultFor ----

static const da::ActionSpec *action(const char *domain, const char *name) {
    const da::DomainSpec *d = da::findDomain(domain);
    TEST_ASSERT_NOT_NULL(d);
    const da::ActionSpec *a = da::findAction(*d, name);
    TEST_ASSERT_NOT_NULL(a);
    return a;
}

static void test_step_default_climate() {
    const da::ActionSpec *a = action("climate", "target_temp");
    TEST_ASSERT_EQUAL_FLOAT(1.0, da::stepDefaultFor(*a, attrsFrom("{\"target_temp_step\":1}")));
    TEST_ASSERT_EQUAL_FLOAT(0.5, da::stepDefaultFor(*a, attrsFrom("{\"target_temp_step\":0.5}")));
    TEST_ASSERT_EQUAL_FLOAT(0.5, da::stepDefaultFor(*a, attrsFrom("{\"target_temp_step\":0.05}")));  // below 0.1
    TEST_ASSERT_EQUAL_FLOAT(0.5, da::stepDefaultFor(*a, attrsFrom("{\"target_temp_step\":10}")));    // above 5
    TEST_ASSERT_EQUAL_FLOAT(0.5, da::stepDefaultFor(*a, attrsFrom("{}")));
    TEST_ASSERT_EQUAL_FLOAT(0.1, da::stepDefaultFor(*a, attrsFrom("{\"target_temp_step\":0.1}")));
    TEST_ASSERT_EQUAL_FLOAT(5.0, da::stepDefaultFor(*a, attrsFrom("{\"target_temp_step\":5}")));
}

static void test_step_default_fan() {
    const da::ActionSpec *a = action("fan", "percentage");
    TEST_ASSERT_EQUAL_FLOAT(34.0, da::stepDefaultFor(*a, attrsFrom("{\"percentage_step\":33.33}")));
    TEST_ASSERT_EQUAL_FLOAT(10.0, da::stepDefaultFor(*a, attrsFrom("{}")));
    TEST_ASSERT_EQUAL_FLOAT(25.0, da::stepDefaultFor(*a, attrsFrom("{\"percentage_step\":25}")));
    TEST_ASSERT_EQUAL_FLOAT(1.0, da::stepDefaultFor(*a, attrsFrom("{\"percentage_step\":1}")));
    TEST_ASSERT_EQUAL_FLOAT(50.0, da::stepDefaultFor(*a, attrsFrom("{\"percentage_step\":100}")));  // clamp
    TEST_ASSERT_EQUAL_FLOAT(50.0, da::stepDefaultFor(*a, attrsFrom("{\"percentage_step\":50}")));
    TEST_ASSERT_EQUAL_FLOAT(1.0, da::stepDefaultFor(*a, attrsFrom("{\"percentage_step\":0.5}")));   // ceil -> 1
    TEST_ASSERT_EQUAL_FLOAT(10.0, da::stepDefaultFor(*a, attrsFrom("{\"percentage_step\":0}")));    // invalid
    TEST_ASSERT_EQUAL_FLOAT(10.0, da::stepDefaultFor(*a, attrsFrom("{\"percentage_step\":-5}")));   // invalid
}

static void test_step_default_others_use_table() {
    ha::EntityAttrs a = attrsFrom("{\"target_temp_step\":1,\"percentage_step\":33}");
    TEST_ASSERT_EQUAL_FLOAT(5.0, da::stepDefaultFor(*action("light", "brightness"), a));
    TEST_ASSERT_EQUAL_FLOAT(10.0, da::stepDefaultFor(*action("light", "hue"), a));
    TEST_ASSERT_EQUAL_FLOAT(100.0, da::stepDefaultFor(*action("light", "color_temp"), a));
    TEST_ASSERT_EQUAL_FLOAT(10.0, da::stepDefaultFor(*action("cover", "position"), a));
    TEST_ASSERT_EQUAL_FLOAT(2.0, da::stepDefaultFor(*action("media_player", "volume"), a));
    TEST_ASSERT_EQUAL_FLOAT(0.0, da::stepDefaultFor(*action("light", "none"), a));
    TEST_ASSERT_EQUAL_FLOAT(0.0, da::stepDefaultFor(*action("switch", "none"), a));
}

// ---- applyAttrs ----

static void test_apply_replace_full() {
    ha::EntityAttrs a = attrsFrom(
        "{\"friendly_name\":\"K\\u00f8kken\",\"supported_features\":44,"
        "\"supported_color_modes\":[\"color_temp\",\"hs\"],\"brightness\":128,"
        "\"color_temp_kelvin\":2700,\"min_color_temp_kelvin\":2000,\"max_color_temp_kelvin\":6500,"
        "\"hs_color\":[30.5,80],\"temperature\":21.5,\"current_temperature\":19.5,\"min_temp\":7,"
        "\"max_temp\":35,\"target_temp_step\":0.5,\"current_position\":40,\"percentage\":66,"
        "\"percentage_step\":33.3,\"volume_level\":0.42,\"is_volume_muted\":true}");
    TEST_ASSERT_EQUAL_STRING("K\xc3\xb8kken", a.friendlyName.c_str());
    TEST_ASSERT_EQUAL_UINT32(44, a.supportedFeatures);
    TEST_ASSERT_EQUAL_UINT16(ha::kColorTemp | ha::kColorHs, a.colorModes);
    TEST_ASSERT_EQUAL_FLOAT(128, *a.brightness);
    TEST_ASSERT_EQUAL_FLOAT(2700, *a.colorTempKelvin);
    TEST_ASSERT_EQUAL_FLOAT(2000, *a.minColorTempKelvin);
    TEST_ASSERT_EQUAL_FLOAT(6500, *a.maxColorTempKelvin);
    TEST_ASSERT_EQUAL_FLOAT(30.5, *a.hue);
    TEST_ASSERT_EQUAL_FLOAT(80, *a.saturation);
    TEST_ASSERT_EQUAL_FLOAT(21.5, *a.temperature);
    TEST_ASSERT_TRUE(a.temperatureKey);
    TEST_ASSERT_EQUAL_FLOAT(19.5, *a.currentTemperature);
    TEST_ASSERT_EQUAL_FLOAT(7, *a.minTemp);
    TEST_ASSERT_EQUAL_FLOAT(35, *a.maxTemp);
    TEST_ASSERT_EQUAL_FLOAT(0.5, *a.targetTempStep);
    TEST_ASSERT_EQUAL_FLOAT(40, *a.currentPosition);
    TEST_ASSERT_EQUAL_FLOAT(66, *a.percentage);
    TEST_ASSERT_EQUAL_FLOAT(33.3, *a.percentageStep);
    TEST_ASSERT_EQUAL_FLOAT(0.42, *a.volumeLevel);
    TEST_ASSERT_TRUE(a.isVolumeMuted.has_value() && *a.isVolumeMuted);
}

static void test_apply_color_modes_bits() {
    ha::EntityAttrs a = attrsFrom(
        "{\"supported_color_modes\":[\"onoff\",\"brightness\",\"color_temp\",\"hs\",\"xy\",\"rgb\","
        "\"rgbw\",\"rgbww\",\"white\",\"unknown\",\"weird\"]}");
    const uint16_t all = ha::kColorOnOff | ha::kColorBrightness | ha::kColorTemp | ha::kColorHs |
                         ha::kColorXy | ha::kColorRgb | ha::kColorRgbw | ha::kColorRgbww |
                         ha::kColorWhite | ha::kColorOther;
    TEST_ASSERT_EQUAL_UINT16(all, a.colorModes);
    TEST_ASSERT_EQUAL_UINT16(0, attrsFrom("{\"supported_color_modes\":[\"unknown\"]}").colorModes);
}

static void test_apply_replace_resets_missing() {
    ha::EntityAttrs base = attrsFrom("{\"brightness\":100,\"friendly_name\":\"A\",\"supported_features\":4}");
    ha::EntityAttrs r = attrsFrom("{\"percentage\":50}", ha::AttrMode::Replace, base);
    TEST_ASSERT_FALSE(r.brightness.has_value());
    TEST_ASSERT_EQUAL_STRING("", r.friendlyName.c_str());
    TEST_ASSERT_EQUAL_UINT32(0, r.supportedFeatures);
    TEST_ASSERT_EQUAL_FLOAT(50, *r.percentage);
}

static void test_apply_merge_keeps_others() {
    ha::EntityAttrs base = attrsFrom("{\"brightness\":100,\"friendly_name\":\"A\",\"supported_features\":4}");
    ha::EntityAttrs m = attrsFrom("{\"brightness\":50}", ha::AttrMode::Merge, base);
    TEST_ASSERT_EQUAL_FLOAT(50, *m.brightness);
    TEST_ASSERT_EQUAL_STRING("A", m.friendlyName.c_str());
    TEST_ASSERT_EQUAL_UINT32(4, m.supportedFeatures);
}

static void test_apply_null_resets() {
    ha::EntityAttrs base = attrsFrom(
        "{\"brightness\":100,\"friendly_name\":\"A\",\"supported_features\":4,"
        "\"supported_color_modes\":[\"hs\"],\"hs_color\":[1,2],\"is_volume_muted\":true,"
        "\"percentage\":10}");
    ha::EntityAttrs m = attrsFrom(
        "{\"brightness\":null,\"friendly_name\":null,\"supported_features\":null,"
        "\"supported_color_modes\":null,\"hs_color\":null,\"is_volume_muted\":null}",
        ha::AttrMode::Merge, base);
    TEST_ASSERT_FALSE(m.brightness.has_value());
    TEST_ASSERT_EQUAL_STRING("", m.friendlyName.c_str());
    TEST_ASSERT_EQUAL_UINT32(0, m.supportedFeatures);
    TEST_ASSERT_EQUAL_UINT16(0, m.colorModes);
    TEST_ASSERT_FALSE(m.hue.has_value());
    TEST_ASSERT_FALSE(m.saturation.has_value());
    TEST_ASSERT_FALSE(m.isVolumeMuted.has_value());
    TEST_ASSERT_EQUAL_FLOAT(10, *m.percentage);  // untouched
}

static void test_apply_temperature_null_sets_key() {
    ha::EntityAttrs a = attrsFrom("{\"temperature\":null}");
    TEST_ASSERT_TRUE(a.temperatureKey);
    TEST_ASSERT_FALSE(a.temperature.has_value());
    // merge null over a value: value gone, key still present
    ha::EntityAttrs b = attrsFrom("{\"temperature\":null}", ha::AttrMode::Merge, attrsFrom("{\"temperature\":20}"));
    TEST_ASSERT_TRUE(b.temperatureKey);
    TEST_ASSERT_FALSE(b.temperature.has_value());
    TEST_ASSERT_FALSE(attrsFrom("{}").temperatureKey);
}

static void test_apply_wrong_types_ignored() {
    ha::EntityAttrs a = attrsFrom(
        "{\"brightness\":\"high\",\"friendly_name\":5,\"supported_features\":\"x\","
        "\"supported_color_modes\":\"hs\",\"hs_color\":\"red\",\"is_volume_muted\":\"yes\","
        "\"percentage\":[1],\"volume_level\":{}}");
    TEST_ASSERT_FALSE(a.brightness.has_value());
    TEST_ASSERT_EQUAL_STRING("", a.friendlyName.c_str());
    TEST_ASSERT_EQUAL_UINT32(0, a.supportedFeatures);
    TEST_ASSERT_EQUAL_UINT16(0, a.colorModes);
    TEST_ASSERT_FALSE(a.hue.has_value());
    TEST_ASSERT_FALSE(a.isVolumeMuted.has_value());
    TEST_ASSERT_FALSE(a.percentage.has_value());
    TEST_ASSERT_FALSE(a.volumeLevel.has_value());
    // in Merge mode a wrong type keeps the current value
    ha::EntityAttrs base = attrsFrom("{\"brightness\":77,\"friendly_name\":\"N\"}");
    ha::EntityAttrs m = attrsFrom("{\"brightness\":\"x\",\"friendly_name\":false}", ha::AttrMode::Merge, base);
    TEST_ASSERT_EQUAL_FLOAT(77, *m.brightness);
    TEST_ASSERT_EQUAL_STRING("N", m.friendlyName.c_str());
}

static void test_apply_hs_color_short_or_bad() {
    ha::EntityAttrs a = attrsFrom("{\"hs_color\":[120]}");
    TEST_ASSERT_FALSE(a.hue.has_value());
    TEST_ASSERT_FALSE(a.saturation.has_value());
    a = attrsFrom("{\"hs_color\":[]}");
    TEST_ASSERT_FALSE(a.hue.has_value());
    a = attrsFrom("{\"hs_color\":[\"a\",\"b\"]}");
    TEST_ASSERT_FALSE(a.hue.has_value());
    a = attrsFrom("{\"hs_color\":[10,20,30]}");  // extra items fine
    TEST_ASSERT_EQUAL_FLOAT(10, *a.hue);
    TEST_ASSERT_EQUAL_FLOAT(20, *a.saturation);
    // a short array in Merge mode leaves the previous colour
    ha::EntityAttrs m = attrsFrom("{\"hs_color\":[5]}", ha::AttrMode::Merge, attrsFrom("{\"hs_color\":[1,2]}"));
    TEST_ASSERT_EQUAL_FLOAT(1, *m.hue);
    TEST_ASSERT_EQUAL_FLOAT(2, *m.saturation);
}

static void test_apply_unknown_keys_ignored_and_null_object() {
    ha::EntityAttrs a = attrsFrom("{\"entity_picture\":\"/x.png\",\"whatever\":1,\"brightness\":3}");
    TEST_ASSERT_EQUAL_FLOAT(3, *a.brightness);
    ha::EntityAttrs base = attrsFrom("{\"brightness\":9}");
    ha::applyAttrs(JsonObjectConst(), base, ha::AttrMode::Merge);
    TEST_ASSERT_EQUAL_FLOAT(9, *base.brightness);
    ha::applyAttrs(JsonObjectConst(), base, ha::AttrMode::Replace);
    TEST_ASSERT_FALSE(base.brightness.has_value());
}

static void test_reset_attr() {
    ha::EntityAttrs a = attrsFrom("{\"temperature\":20,\"hs_color\":[1,2],\"brightness\":5}");
    TEST_ASSERT_TRUE(ha::resetAttr("temperature", a));
    TEST_ASSERT_FALSE(a.temperature.has_value());
    TEST_ASSERT_FALSE(a.temperatureKey);
    TEST_ASSERT_TRUE(ha::resetAttr("hs_color", a));
    TEST_ASSERT_FALSE(a.hue.has_value());
    TEST_ASSERT_FALSE(a.saturation.has_value());
    TEST_ASSERT_FALSE(ha::resetAttr("entity_picture", a));
    TEST_ASSERT_EQUAL_FLOAT(5, *a.brightness);
}

static void test_attribute_keys_and_filter() {
    TEST_ASSERT_EQUAL_UINT32(18, (uint32_t)ha::kAttributeKeyCount);
    JsonDocument f;
    ha::addAttributeFilter(f.to<JsonObject>());
    TEST_ASSERT_EQUAL_UINT32(18, (uint32_t)f.size());
    for (const char *k : ha::kAttributeKeys) {
        TEST_ASSERT_TRUE_MESSAGE(f[k].as<bool>(), k);
    }
    TEST_ASSERT_TRUE(f["entity_picture"].isNull());
}

static void test_attrs_equality() {
    ha::EntityAttrs a = attrsFrom("{\"brightness\":5,\"temperature\":null}");
    ha::EntityAttrs b = attrsFrom("{\"brightness\":5,\"temperature\":null}");
    TEST_ASSERT_TRUE(a == b);
    b.brightness = 6;
    TEST_ASSERT_TRUE(a != b);
    ha::EntityAttrs c = attrsFrom("{\"brightness\":5}");
    TEST_ASSERT_TRUE(a != c);  // temperatureKey differs
}

// ---- metaJson ----

static void test_meta_json_exact() {
    const char *expected =
        "{\"max_controls\":24,\"max_label_bytes\":48,\"domains\":["
        "{\"domain\":\"light\",\"actions\":["
        "{\"name\":\"brightness\",\"step\":{\"default\":5,\"min\":1,\"max\":25}},"
        "{\"name\":\"hue\",\"step\":{\"default\":10,\"min\":1,\"max\":60}},"
        "{\"name\":\"color_temp\",\"step\":{\"default\":100,\"min\":25,\"max\":1000}},"
        "{\"name\":\"none\"}],\"presses\":[\"toggle\",\"none\"]},"
        "{\"domain\":\"climate\",\"actions\":["
        "{\"name\":\"target_temp\",\"step\":{\"default\":0.5,\"min\":0.1,\"max\":5}},"
        "{\"name\":\"none\"}],\"presses\":[\"none\",\"toggle\"]},"
        "{\"domain\":\"cover\",\"actions\":["
        "{\"name\":\"position\",\"step\":{\"default\":10,\"min\":1,\"max\":25}},"
        "{\"name\":\"none\"}],\"presses\":[\"toggle\",\"stop\",\"none\"]},"
        "{\"domain\":\"fan\",\"actions\":["
        "{\"name\":\"percentage\",\"step\":{\"default\":10,\"min\":1,\"max\":50}},"
        "{\"name\":\"none\"}],\"presses\":[\"toggle\",\"none\"]},"
        "{\"domain\":\"media_player\",\"actions\":["
        "{\"name\":\"volume\",\"step\":{\"default\":2,\"min\":1,\"max\":20}},"
        "{\"name\":\"none\"}],\"presses\":[\"play_pause\",\"toggle\",\"mute\",\"none\"]},"
        "{\"domain\":\"switch\",\"actions\":[{\"name\":\"none\"}],\"presses\":[\"toggle\",\"none\"]},"
        "{\"domain\":\"input_boolean\",\"actions\":[{\"name\":\"none\"}],\"presses\":[\"toggle\",\"none\"]}"
        "]}";
    TEST_ASSERT_EQUAL_STRING(expected, da::metaJson().c_str());
}

static void test_meta_json_parses_and_covers_table() {
    const std::string s = da::metaJson();
    JsonDocument doc;
    TEST_ASSERT_TRUE(!deserializeJson(doc, s));
    size_t n = 0;
    const da::DomainSpec *all = da::domains(n);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)n, (uint32_t)doc["domains"].size());
    for (size_t i = 0; i < n; ++i) {
        JsonObjectConst d = doc["domains"][i];
        TEST_ASSERT_EQUAL_STRING(all[i].domain, d["domain"].as<const char *>());
        TEST_ASSERT_EQUAL_UINT32((uint32_t)all[i].actionCount, (uint32_t)d["actions"].size());
        TEST_ASSERT_EQUAL_UINT32((uint32_t)all[i].pressCount, (uint32_t)d["presses"].size());
        for (size_t j = 0; j < all[i].actionCount; ++j) {
            JsonObjectConst a = d["actions"][j];
            TEST_ASSERT_EQUAL_STRING(all[i].actions[j].name, a["name"].as<const char *>());
            TEST_ASSERT_EQUAL(all[i].actions[j].hasStep, a["step"].is<JsonObjectConst>());
        }
    }
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_light_onoff_only);
    RUN_TEST(test_light_no_modes_or_unknown);
    RUN_TEST(test_light_brightness_only);
    RUN_TEST(test_light_color_temp);
    RUN_TEST(test_light_hs);
    RUN_TEST(test_light_xy);
    RUN_TEST(test_light_rgb_rgbw);
    RUN_TEST(test_light_rgbww);
    RUN_TEST(test_light_white_mode_offers_brightness_only);
    RUN_TEST(test_light_onoff_plus_brightness);
    RUN_TEST(test_light_unknown_mixed_with_real_mode);
    RUN_TEST(test_climate);
    RUN_TEST(test_cover);
    RUN_TEST(test_fan);
    RUN_TEST(test_media_player);
    RUN_TEST(test_switch_and_input_boolean);
    RUN_TEST(test_none_always_included_for_every_domain);
    RUN_TEST(test_step_default_climate);
    RUN_TEST(test_step_default_fan);
    RUN_TEST(test_step_default_others_use_table);
    RUN_TEST(test_apply_replace_full);
    RUN_TEST(test_apply_color_modes_bits);
    RUN_TEST(test_apply_replace_resets_missing);
    RUN_TEST(test_apply_merge_keeps_others);
    RUN_TEST(test_apply_null_resets);
    RUN_TEST(test_apply_temperature_null_sets_key);
    RUN_TEST(test_apply_wrong_types_ignored);
    RUN_TEST(test_apply_hs_color_short_or_bad);
    RUN_TEST(test_apply_unknown_keys_ignored_and_null_object);
    RUN_TEST(test_reset_attr);
    RUN_TEST(test_attribute_keys_and_filter);
    RUN_TEST(test_attrs_equality);
    RUN_TEST(test_meta_json_exact);
    RUN_TEST(test_meta_json_parses_and_covers_table);
    return UNITY_END();
}
