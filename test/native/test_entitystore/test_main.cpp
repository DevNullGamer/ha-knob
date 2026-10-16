// Native tests for EntityStore (plan-step6.md section 3.3, Step 6.2), fed with the "event"
// object of subscribe_entities in HA's compressed format, also through parseMessage.
#include <unity.h>

#include <ArduinoJson.h>

#include <string>

#include "ha/EntityStore.h"
#include "ha/HaProtocol.h"

namespace ha = core::ha;

void setUp() {}
void tearDown() {}

static void applyJson(ha::EntityStore &st, const std::string &event) {
    JsonDocument doc;
    TEST_ASSERT_TRUE(!deserializeJson(doc, event));
    st.apply(doc.as<JsonVariantConst>());
}

// Full path: wrap in an event message, parse (filter) and apply.
static void applyViaParse(ha::EntityStore &st, const std::string &event) {
    const std::string msg = "{\"id\":2,\"type\":\"event\",\"event\":" + event + "}";
    ha::Message m = ha::parseMessage(msg.c_str(), msg.size());
    TEST_ASSERT_EQUAL(ha::MessageType::Event, m.type);
    st.apply(m.event());
}

static const char *kLightA =
    "{\"a\":{\"light.x\":{\"s\":\"on\",\"a\":{\"friendly_name\":\"X\",\"brightness\":200,"
    "\"supported_color_modes\":[\"hs\"],\"hs_color\":[10,20]},\"c\":\"01ABC\"}}}";

static void test_initial_state() {
    ha::EntityStore st;
    TEST_ASSERT_EQUAL_UINT32(0, st.size());
    TEST_ASSERT_EQUAL_UINT32(0, st.version());
    TEST_ASSERT_FALSE(st.snapshotSeen());
    TEST_ASSERT_NULL(st.find("light.x"));
}

static void test_added_inserts() {
    ha::EntityStore st;
    applyJson(st, kLightA);
    TEST_ASSERT_EQUAL_UINT32(1, st.size());
    TEST_ASSERT_TRUE(st.snapshotSeen());
    const ha::EntityState *e = st.find("light.x");
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_STRING("on", e->state.c_str());
    TEST_ASSERT_EQUAL_STRING("X", e->attrs.friendlyName.c_str());
    TEST_ASSERT_EQUAL_FLOAT(200, *e->attrs.brightness);
    TEST_ASSERT_EQUAL_FLOAT(10, *e->attrs.hue);
    TEST_ASSERT_EQUAL_UINT16(ha::kColorHs, e->attrs.colorModes);
}

static void test_version_first_a_is_two() {
    // snapshotSeen (+1) and the new entity (+1)
    ha::EntityStore st;
    applyJson(st, kLightA);
    TEST_ASSERT_EQUAL_UINT32(2, st.version());
}

static void test_identical_reapply_no_version_change() {
    ha::EntityStore st;
    applyJson(st, kLightA);
    const uint32_t v = st.version();
    applyJson(st, kLightA);
    TEST_ASSERT_EQUAL_UINT32(v, st.version());
    TEST_ASSERT_EQUAL_UINT32(1, st.size());
}

static void test_added_replaces_attrs() {
    ha::EntityStore st;
    applyJson(st, kLightA);
    const uint32_t v = st.version();
    applyJson(st, "{\"a\":{\"light.x\":{\"s\":\"off\",\"a\":{\"friendly_name\":\"X\"}}}}");
    const ha::EntityState *e = st.find("light.x");
    TEST_ASSERT_EQUAL_STRING("off", e->state.c_str());
    TEST_ASSERT_FALSE(e->attrs.brightness.has_value());  // replaced, not merged
    TEST_ASSERT_FALSE(e->attrs.hue.has_value());
    TEST_ASSERT_TRUE(st.version() > v);
}

static void test_empty_snapshot_bumps_once() {
    ha::EntityStore st;
    applyJson(st, "{\"a\":{}}");
    TEST_ASSERT_TRUE(st.snapshotSeen());
    TEST_ASSERT_EQUAL_UINT32(0, st.size());
    TEST_ASSERT_EQUAL_UINT32(1, st.version());
    applyJson(st, "{\"a\":{}}");
    TEST_ASSERT_EQUAL_UINT32(1, st.version());
}

static void test_change_state() {
    ha::EntityStore st;
    applyJson(st, kLightA);
    const uint32_t v = st.version();
    applyJson(st, "{\"c\":{\"light.x\":{\"+\":{\"s\":\"off\",\"lc\":1.5,\"lu\":2.5}}}}");
    const ha::EntityState *e = st.find("light.x");
    TEST_ASSERT_EQUAL_STRING("off", e->state.c_str());
    TEST_ASSERT_EQUAL_FLOAT(200, *e->attrs.brightness);  // attributes untouched
    TEST_ASSERT_EQUAL_UINT32(v + 1, st.version());
}

static void test_change_merges_only_changed_attrs() {
    ha::EntityStore st;
    applyJson(st, kLightA);
    applyJson(st, "{\"c\":{\"light.x\":{\"+\":{\"a\":{\"brightness\":50}}}}}");
    const ha::EntityState *e = st.find("light.x");
    TEST_ASSERT_EQUAL_STRING("on", e->state.c_str());
    TEST_ASSERT_EQUAL_FLOAT(50, *e->attrs.brightness);
    TEST_ASSERT_EQUAL_STRING("X", e->attrs.friendlyName.c_str());  // kept
    TEST_ASSERT_EQUAL_FLOAT(10, *e->attrs.hue);                   // kept
    TEST_ASSERT_EQUAL_UINT16(ha::kColorHs, e->attrs.colorModes);   // kept
}

static void test_change_attr_null_resets() {
    ha::EntityStore st;
    applyJson(st, kLightA);
    applyJson(st, "{\"c\":{\"light.x\":{\"+\":{\"s\":\"off\",\"a\":{\"brightness\":null}}}}}");
    const ha::EntityState *e = st.find("light.x");
    TEST_ASSERT_FALSE(e->attrs.brightness.has_value());
    TEST_ASSERT_EQUAL_STRING("off", e->state.c_str());
    TEST_ASSERT_EQUAL_FLOAT(10, *e->attrs.hue);
}

static void test_change_minus_resets_keys() {
    ha::EntityStore st;
    applyJson(st, kLightA);
    applyJson(st, "{\"c\":{\"light.x\":{\"-\":{\"a\":[\"brightness\",\"hs_color\",\"entity_picture\"]}}}}");
    const ha::EntityState *e = st.find("light.x");
    TEST_ASSERT_FALSE(e->attrs.brightness.has_value());
    TEST_ASSERT_FALSE(e->attrs.hue.has_value());
    TEST_ASSERT_FALSE(e->attrs.saturation.has_value());
    TEST_ASSERT_EQUAL_STRING("X", e->attrs.friendlyName.c_str());
    TEST_ASSERT_EQUAL_STRING("on", e->state.c_str());
}

static void test_change_plus_and_minus_together() {
    ha::EntityStore st;
    applyJson(st, kLightA);
    applyJson(st, "{\"c\":{\"light.x\":{\"+\":{\"s\":\"off\",\"a\":{\"brightness\":7}},"
                  "\"-\":{\"a\":[\"hs_color\"]}}}}");
    const ha::EntityState *e = st.find("light.x");
    TEST_ASSERT_EQUAL_STRING("off", e->state.c_str());
    TEST_ASSERT_EQUAL_FLOAT(7, *e->attrs.brightness);
    TEST_ASSERT_FALSE(e->attrs.hue.has_value());
}

static void test_change_unknown_id_ignored() {
    ha::EntityStore st;
    applyJson(st, kLightA);
    const uint32_t v = st.version();
    applyJson(st, "{\"c\":{\"light.nope\":{\"+\":{\"s\":\"off\"}}}}");
    TEST_ASSERT_EQUAL_UINT32(1, st.size());
    TEST_ASSERT_NULL(st.find("light.nope"));
    TEST_ASSERT_EQUAL_UINT32(v, st.version());
}

static void test_change_without_effect_no_version() {
    ha::EntityStore st;
    applyJson(st, kLightA);
    const uint32_t v = st.version();
    applyJson(st, "{\"c\":{\"light.x\":{\"+\":{\"s\":\"on\",\"a\":{\"brightness\":200}}}}}");
    TEST_ASSERT_EQUAL_UINT32(v, st.version());
    applyJson(st, "{\"c\":{\"light.x\":{\"+\":{\"lc\":9.9}}}}");  // only timestamps
    TEST_ASSERT_EQUAL_UINT32(v, st.version());
    applyJson(st, "{\"c\":{\"light.x\":{\"-\":{\"a\":[\"percentage\"]}}}}");  // key not set
    TEST_ASSERT_EQUAL_UINT32(v, st.version());
}

static void test_context_string_and_object() {
    ha::EntityStore st;
    applyJson(st, "{\"a\":{\"light.x\":{\"s\":\"on\",\"a\":{},\"c\":{\"id\":\"01\",\"parent_id\":null,\"user_id\":null}}}}");
    TEST_ASSERT_NOT_NULL(st.find("light.x"));
    applyJson(st, "{\"c\":{\"light.x\":{\"+\":{\"s\":\"off\",\"c\":\"01XYZ\"}}}}");
    TEST_ASSERT_EQUAL_STRING("off", st.find("light.x")->state.c_str());
    applyJson(st, "{\"c\":{\"light.x\":{\"+\":{\"s\":\"on\",\"c\":{\"id\":\"02\",\"parent_id\":null,\"user_id\":\"u\"}}}}}");
    TEST_ASSERT_EQUAL_STRING("on", st.find("light.x")->state.c_str());
}

static void test_removed() {
    ha::EntityStore st;
    applyJson(st, kLightA);
    const uint32_t v = st.version();
    applyJson(st, "{\"r\":[\"light.x\"]}");
    TEST_ASSERT_EQUAL_UINT32(0, st.size());
    TEST_ASSERT_NULL(st.find("light.x"));
    TEST_ASSERT_EQUAL_UINT32(v + 1, st.version());
    TEST_ASSERT_TRUE(st.snapshotSeen());  // removal does not reset the snapshot flag
    // unknown id: no change
    applyJson(st, "{\"r\":[\"light.x\",\"light.never\"]}");
    TEST_ASSERT_EQUAL_UINT32(v + 1, st.version());
}

static void test_a_c_r_in_one_event() {
    ha::EntityStore st;
    applyJson(st, "{\"a\":{\"light.a\":{\"s\":\"on\"},\"light.b\":{\"s\":\"on\"}}}");
    applyJson(st, "{\"a\":{\"light.c\":{\"s\":\"on\"}},\"c\":{\"light.a\":{\"+\":{\"s\":\"off\"}}},\"r\":[\"light.b\"]}");
    TEST_ASSERT_EQUAL_UINT32(2, st.size());
    TEST_ASSERT_EQUAL_STRING("off", st.find("light.a")->state.c_str());
    TEST_ASSERT_NULL(st.find("light.b"));
    TEST_ASSERT_NOT_NULL(st.find("light.c"));
}

static void test_non_object_events_ignored() {
    ha::EntityStore st;
    applyJson(st, "null");
    applyJson(st, "[1,2]");
    applyJson(st, "5");
    applyJson(st, "{\"a\":[1],\"c\":5,\"r\":\"x\"}");
    TEST_ASSERT_EQUAL_UINT32(0, st.size());
    TEST_ASSERT_EQUAL_UINT32(0, st.version());
    TEST_ASSERT_FALSE(st.snapshotSeen());
    applyJson(st, "{\"a\":{\"light.x\":5,\"light.y\":null,\"light.z\":{\"s\":\"on\"}}}");
    TEST_ASSERT_EQUAL_UINT32(1, st.size());
    TEST_ASSERT_NOT_NULL(st.find("light.z"));
}

static void test_clear() {
    ha::EntityStore st;
    applyJson(st, kLightA);
    const uint32_t v = st.version();
    st.clear();
    TEST_ASSERT_EQUAL_UINT32(0, st.size());
    TEST_ASSERT_FALSE(st.snapshotSeen());
    TEST_ASSERT_EQUAL_UINT32(v + 1, st.version());
    st.clear();  // nothing to drop
    TEST_ASSERT_EQUAL_UINT32(v + 1, st.version());
    // snapshot flag only (empty snapshot) also counts
    applyJson(st, "{\"a\":{}}");
    const uint32_t v2 = st.version();
    st.clear();
    TEST_ASSERT_EQUAL_UINT32(v2 + 1, st.version());
    TEST_ASSERT_FALSE(st.snapshotSeen());
    // fresh store: clear is a no-op
    ha::EntityStore fresh;
    fresh.clear();
    TEST_ASSERT_EQUAL_UINT32(0, fresh.version());
}

static void test_cap_64() {
    ha::EntityStore st;
    std::string ev = "{\"a\":{";
    for (int i = 0; i < 70; ++i) {
        if (i) ev += ",";
        ev += "\"light.e" + std::to_string(i) + "\":{\"s\":\"on\"}";
    }
    ev += "}}";
    applyJson(st, ev);
    TEST_ASSERT_EQUAL_UINT32(64, st.size());
    TEST_ASSERT_EQUAL_UINT32(ha::EntityStore::kMaxEntries, st.size());
    // an existing entity can still be updated at the cap
    const uint32_t v = st.version();
    applyJson(st, "{\"c\":{\"light.e0\":{\"+\":{\"s\":\"off\"}}}}");
    TEST_ASSERT_EQUAL_STRING("off", st.find("light.e0")->state.c_str());
    TEST_ASSERT_EQUAL_UINT32(v + 1, st.version());
    // a new one is still refused
    applyJson(st, "{\"a\":{\"light.extra\":{\"s\":\"on\"}}}");
    TEST_ASSERT_EQUAL_UINT32(64, st.size());
    TEST_ASSERT_NULL(st.find("light.extra"));
    // after a removal there is room again
    applyJson(st, "{\"r\":[\"light.e1\"]}");
    applyJson(st, "{\"a\":{\"light.extra\":{\"s\":\"on\"}}}");
    TEST_ASSERT_NOT_NULL(st.find("light.extra"));
    TEST_ASSERT_EQUAL_UINT32(64, st.size());
}

// ---- HA's exact compressed format, through parseMessage ----

static void test_ha_format_end_to_end() {
    ha::EntityStore st;
    applyViaParse(st,
        "{\"a\":{\"light.kitchen\":{\"s\":\"on\",\"a\":{\"supported_color_modes\":[\"color_temp\",\"hs\"],"
        "\"color_temp_kelvin\":2700,\"min_color_temp_kelvin\":2000,\"max_color_temp_kelvin\":6500,"
        "\"brightness\":255,\"hs_color\":[30.1,60.2],\"rgb_color\":[255,200,100],"
        "\"xy_color\":[0.5,0.4],\"entity_picture\":\"/local/p.png\",\"friendly_name\":\"K\\u00f8kken\","
        "\"supported_features\":44},\"c\":\"01HABC\",\"lc\":1700000000.1,\"lu\":1700000000.2},"
        "\"climate.hall\":{\"s\":\"heat\",\"a\":{\"hvac_modes\":[\"off\",\"heat\"],\"min_temp\":7,"
        "\"max_temp\":35,\"target_temp_step\":0.5,\"current_temperature\":19.5,\"temperature\":21,"
        "\"friendly_name\":\"Hall\",\"supported_features\":401},\"c\":{\"id\":\"01C\",\"parent_id\":null,\"user_id\":null}}}}");
    TEST_ASSERT_TRUE(st.snapshotSeen());
    TEST_ASSERT_EQUAL_UINT32(2, st.size());
    const ha::EntityState *k = st.find("light.kitchen");
    TEST_ASSERT_NOT_NULL(k);
    TEST_ASSERT_EQUAL_STRING("K\xc3\xb8kken", k->attrs.friendlyName.c_str());
    TEST_ASSERT_EQUAL_FLOAT(255, *k->attrs.brightness);
    TEST_ASSERT_EQUAL_FLOAT(2700, *k->attrs.colorTempKelvin);
    TEST_ASSERT_EQUAL_FLOAT(30.1, *k->attrs.hue);
    TEST_ASSERT_EQUAL_UINT16(ha::kColorTemp | ha::kColorHs, k->attrs.colorModes);
    const ha::EntityState *c = st.find("climate.hall");
    TEST_ASSERT_EQUAL_STRING("heat", c->state.c_str());
    TEST_ASSERT_EQUAL_FLOAT(21, *c->attrs.temperature);
    TEST_ASSERT_EQUAL_FLOAT(19.5, *c->attrs.currentTemperature);

    // compressed change: state + one attribute, timestamps, context string
    applyViaParse(st,
        "{\"c\":{\"light.kitchen\":{\"+\":{\"s\":\"off\",\"a\":{\"brightness\":null},\"c\":\"01NEW\","
        "\"lc\":1700000001.5,\"lu\":1700000001.5}},"
        "\"climate.hall\":{\"+\":{\"a\":{\"current_temperature\":20.0},\"lu\":1.0},"
        "\"-\":{\"a\":[\"temperature\"]}}}}");
    k = st.find("light.kitchen");
    TEST_ASSERT_EQUAL_STRING("off", k->state.c_str());
    TEST_ASSERT_FALSE(k->attrs.brightness.has_value());
    TEST_ASSERT_EQUAL_FLOAT(2700, *k->attrs.colorTempKelvin);
    c = st.find("climate.hall");
    TEST_ASSERT_EQUAL_FLOAT(20.0, *c->attrs.currentTemperature);
    TEST_ASSERT_FALSE(c->attrs.temperature.has_value());
    TEST_ASSERT_FALSE(c->attrs.temperatureKey);
    TEST_ASSERT_EQUAL_STRING("Hall", c->attrs.friendlyName.c_str());

    applyViaParse(st, "{\"r\":[\"climate.hall\"]}");
    TEST_ASSERT_EQUAL_UINT32(1, st.size());
    TEST_ASSERT_NULL(st.find("climate.hall"));
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_initial_state);
    RUN_TEST(test_added_inserts);
    RUN_TEST(test_version_first_a_is_two);
    RUN_TEST(test_identical_reapply_no_version_change);
    RUN_TEST(test_added_replaces_attrs);
    RUN_TEST(test_empty_snapshot_bumps_once);
    RUN_TEST(test_change_state);
    RUN_TEST(test_change_merges_only_changed_attrs);
    RUN_TEST(test_change_attr_null_resets);
    RUN_TEST(test_change_minus_resets_keys);
    RUN_TEST(test_change_plus_and_minus_together);
    RUN_TEST(test_change_unknown_id_ignored);
    RUN_TEST(test_change_without_effect_no_version);
    RUN_TEST(test_context_string_and_object);
    RUN_TEST(test_removed);
    RUN_TEST(test_a_c_r_in_one_event);
    RUN_TEST(test_non_object_events_ignored);
    RUN_TEST(test_clear);
    RUN_TEST(test_cap_64);
    RUN_TEST(test_ha_format_end_to_end);
    return UNITY_END();
}
