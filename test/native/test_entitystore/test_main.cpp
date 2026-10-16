// Native tests for EntityStore (plan-step6.md section 3.3, Step 6.2), fed with the "event"
// object of subscribe_entities in HA's compressed format, also through parseMessage.
#include <unity.h>

#include <ArduinoJson.h>

#include <cstdlib>
#include <new>
#include <string>
#include <vector>

#include "ha/EntityStore.h"
#include "ha/HaProtocol.h"

namespace ha = core::ha;

// Counts global operator new calls (host heap) so a test can prove that a code path
// allocates only through the injected backend.
static long g_newCalls = 0;
static bool g_countNew = false;
void *operator new(std::size_t n) {
    if (g_countNew) {
        ++g_newCalls;
    }
    void *p = std::malloc(n ? n : 1);
    if (!p) {
        throw std::bad_alloc();
    }
    return p;
}
void operator delete(void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }

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

// ---- counting allocator (Step 6.5 audit carry-over: store memory goes to the backend) ----
namespace {
class CountingAllocator : public ArduinoJson::Allocator {
public:
    // Each block carries a 16-byte header with its size (no host-heap bookkeeping, so the
    // operator new counter only sees allocations that bypass the backend).
    void *allocate(size_t n) override {
        unsigned char *raw = static_cast<unsigned char *>(std::malloc(n + 16));
        if (!raw) {
            return nullptr;
        }
        *reinterpret_cast<size_t *>(raw) = n;
        ++allocs;
        live += static_cast<long>(n);
        return raw + 16;
    }
    void deallocate(void *p) override {
        if (!p) {
            return;
        }
        unsigned char *raw = static_cast<unsigned char *>(p) - 16;
        ++frees;
        live -= static_cast<long>(*reinterpret_cast<size_t *>(raw));
        std::free(raw);
    }
    void *reallocate(void *p, size_t n) override {
        deallocate(p);
        return allocate(n);
    }
    long allocs = 0, frees = 0, live = 0;
};
}  // namespace

static const char *kLongA =
    "{\"a\":{\"light.living_room_ceiling_lamp\":{\"s\":\"on\",\"a\":{\"friendly_name\":"
    "\"Living room ceiling lamp with a long name\",\"brightness\":200}},"
    "\"climate.upstairs_bedroom_thermostat\":{\"s\":\"heat\",\"a\":{\"friendly_name\":"
    "\"Upstairs bedroom thermostat long name\",\"temperature\":21.5}}}}";

static void test_counting_allocator_balanced() {
    CountingAllocator alloc;
    {
        ha::EntityStore st(&alloc);
        JsonDocument doc;
        TEST_ASSERT_TRUE(!deserializeJson(doc, std::string(kLongA)));
        g_newCalls = 0;
        g_countNew = true;
        st.apply(doc.as<JsonVariantConst>());
        g_countNew = false;
        TEST_ASSERT_EQUAL_INT(0, g_newCalls);  // nothing on the host heap
        TEST_ASSERT_EQUAL_UINT32(2, st.size());
        TEST_ASSERT_TRUE(alloc.allocs >= 4);  // 2 nodes + long ids/names go to the backend
        TEST_ASSERT_TRUE(alloc.live > 0);

        // Replace path ("a" again with new content), keeps the allocator
        JsonDocument doc2;
        TEST_ASSERT_TRUE(!deserializeJson(doc2, std::string(
            "{\"a\":{\"light.living_room_ceiling_lamp\":{\"s\":\"off\",\"a\":{\"friendly_name\":"
            "\"Another quite long friendly name here\",\"brightness\":10}}}}")));
        g_newCalls = 0;
        g_countNew = true;
        st.apply(doc2.as<JsonVariantConst>());
        g_countNew = false;
        TEST_ASSERT_EQUAL_INT(0, g_newCalls);
        TEST_ASSERT_EQUAL_STRING("Another quite long friendly name here",
                                 st.find("light.living_room_ceiling_lamp")->attrs.friendlyName.c_str());

        // "c" path: state and merged attribute, new long state string
        JsonDocument doc3;
        TEST_ASSERT_TRUE(!deserializeJson(doc3, std::string(
            "{\"c\":{\"climate.upstairs_bedroom_thermostat\":{\"+\":{\"s\":\"a_rather_long_state_value\","
            "\"a\":{\"friendly_name\":\"A third quite long friendly name\"}}}}}")));
        g_newCalls = 0;
        g_countNew = true;
        st.apply(doc3.as<JsonVariantConst>());
        g_countNew = false;
        TEST_ASSERT_EQUAL_INT(0, g_newCalls);
        TEST_ASSERT_EQUAL_STRING("a_rather_long_state_value",
                                 st.find("climate.upstairs_bedroom_thermostat")->state.c_str());

        // remove one, then clear
        JsonDocument doc4;
        TEST_ASSERT_TRUE(!deserializeJson(doc4, std::string("{\"r\":[\"light.living_room_ceiling_lamp\"]}")));
        st.apply(doc4.as<JsonVariantConst>());
        TEST_ASSERT_EQUAL_UINT32(1, st.size());
        st.clear();
        TEST_ASSERT_EQUAL_UINT32(0, st.size());
        TEST_ASSERT_EQUAL_INT(0, (int)alloc.live);  // balanced after clear()
        // repopulate, then destroy with entries still in the store
        st.apply(doc.as<JsonVariantConst>());
        TEST_ASSERT_TRUE(alloc.live > 0);
    }
    TEST_ASSERT_EQUAL_INT(0, (int)alloc.live);  // balanced after destruction
    TEST_ASSERT_EQUAL_INT((int)alloc.allocs, (int)alloc.frees);
}

static void test_counting_allocator_cap_64_balanced() {
    CountingAllocator alloc;
    {
        ha::EntityStore st(&alloc);
        std::string ev = "{\"a\":{";
        for (int i = 0; i < 70; ++i) {
            if (i) ev += ",";
            ev += "\"light.some_long_entity_name_" + std::to_string(i) + "\":{\"s\":\"on\",\"a\":{\"friendly_name\":\"Friendly name number " + std::to_string(i) + " long\"}}";
        }
        ev += "}}";
        applyJson(st, ev);
        TEST_ASSERT_EQUAL_UINT32(64, st.size());
    }
    TEST_ASSERT_EQUAL_INT(0, (int)alloc.live);
    TEST_ASSERT_EQUAL_INT((int)alloc.allocs, (int)alloc.frees);
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
    RUN_TEST(test_counting_allocator_balanced);
    RUN_TEST(test_counting_allocator_cap_64_balanced);
    return UNITY_END();
}
