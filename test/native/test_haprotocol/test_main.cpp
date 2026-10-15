// Native tests for HaProtocol builders and parser (plan.md section 3.7).
#include <unity.h>

#include <ArduinoJson.h>

#include <string>
#include <vector>

#include "ha/HaProtocol.h"

namespace ha = core::ha;
using MT = ha::MessageType;

void setUp() {}
void tearDown() {}

static ha::Message parse(const std::string &s) { return ha::parseMessage(s.c_str(), s.size()); }

static void test_build_auth() {
    TEST_ASSERT_EQUAL_STRING("{\"type\":\"auth\",\"access_token\":\"T\"}", ha::buildAuth("T").c_str());
    TEST_ASSERT_EQUAL_STRING("{\"type\":\"auth\",\"access_token\":\"a\\\"b\"}", ha::buildAuth("a\"b").c_str());
}

static void test_build_ping() {
    TEST_ASSERT_EQUAL_STRING("{\"id\":1,\"type\":\"ping\"}", ha::buildPing(1).c_str());
}

static void test_build_subscribe() {
    TEST_ASSERT_EQUAL_STRING("{\"id\":2,\"type\":\"subscribe_entities\",\"entity_ids\":[\"a\",\"b\"]}",
                             ha::buildSubscribeEntities(2, {"a", "b"}).c_str());
    TEST_ASSERT_EQUAL_STRING("{\"id\":2,\"type\":\"subscribe_entities\",\"entity_ids\":[]}",
                             ha::buildSubscribeEntities(2, {}).c_str());
}

static void test_build_unsubscribe() {
    TEST_ASSERT_EQUAL_STRING("{\"id\":3,\"type\":\"unsubscribe_events\",\"subscription\":2}",
                             ha::buildUnsubscribeEvents(3, 2).c_str());
}

static void test_build_get_states() {
    TEST_ASSERT_EQUAL_STRING("{\"id\":4,\"type\":\"get_states\"}", ha::buildGetStates(4).c_str());
}

static void test_build_call_service() {
    JsonDocument data;
    data["brightness_pct"] = 40;
    TEST_ASSERT_EQUAL_STRING(
        "{\"id\":5,\"type\":\"call_service\",\"domain\":\"light\",\"service\":\"turn_on\","
        "\"service_data\":{\"brightness_pct\":40},\"target\":{\"entity_id\":\"light.kitchen\"}}",
        ha::buildCallService(5, "light", "turn_on", data.as<JsonVariantConst>(), "light.kitchen").c_str());
}

static void test_build_call_service_omissions() {
    JsonDocument none;  // null
    TEST_ASSERT_EQUAL_STRING(
        "{\"id\":6,\"type\":\"call_service\",\"domain\":\"switch\",\"service\":\"toggle\","
        "\"target\":{\"entity_id\":\"switch.x\"}}",
        ha::buildCallService(6, "switch", "toggle", none.as<JsonVariantConst>(), "switch.x").c_str());
    JsonDocument data;
    data["a"] = 1;
    TEST_ASSERT_EQUAL_STRING(
        "{\"id\":7,\"type\":\"call_service\",\"domain\":\"d\",\"service\":\"s\",\"service_data\":{\"a\":1}}",
        ha::buildCallService(7, "d", "s", data.as<JsonVariantConst>(), "").c_str());
    TEST_ASSERT_EQUAL_STRING("{\"id\":8,\"type\":\"call_service\",\"domain\":\"d\",\"service\":\"s\"}",
                             ha::buildCallService(8, "d", "s", none.as<JsonVariantConst>(), "").c_str());
}

static void test_parse_auth_required() {
    ha::Message m = parse("{\"type\":\"auth_required\",\"ha_version\":\"2024.1.0\"}");
    TEST_ASSERT_EQUAL(MT::AuthRequired, m.type);
    TEST_ASSERT_EQUAL_STRING("2024.1.0", m.haVersion.c_str());
    TEST_ASSERT_EQUAL_STRING("auth_required", ha::messageTypeName(m.type));
}

static void test_parse_auth_ok() {
    ha::Message m = parse("{\"type\":\"auth_ok\",\"ha_version\":\"2025.10.2\"}");
    TEST_ASSERT_EQUAL(MT::AuthOk, m.type);
    TEST_ASSERT_EQUAL_STRING("2025.10.2", m.haVersion.c_str());
    m = parse("{\"type\":\"auth_ok\"}");
    TEST_ASSERT_EQUAL(MT::AuthOk, m.type);
    TEST_ASSERT_EQUAL_STRING("", m.haVersion.c_str());
}

static void test_parse_auth_invalid() {
    ha::Message m = parse("{\"type\":\"auth_invalid\",\"message\":\"Invalid access token or password\"}");
    TEST_ASSERT_EQUAL(MT::AuthInvalid, m.type);
    TEST_ASSERT_EQUAL_STRING("Invalid access token or password", m.message.c_str());
}

static void test_parse_result() {
    ha::Message m = parse("{\"id\":7,\"type\":\"result\",\"success\":true,\"result\":null}");
    TEST_ASSERT_EQUAL(MT::Result, m.type);
    TEST_ASSERT_EQUAL_UINT32(7, m.id);
    TEST_ASSERT_TRUE(m.success);
    m = parse("{\"id\":9,\"type\":\"result\",\"success\":false,\"error\":{\"code\":\"not_found\",\"message\":\"Nope\"}}");
    TEST_ASSERT_EQUAL(MT::Result, m.type);
    TEST_ASSERT_EQUAL_UINT32(9, m.id);
    TEST_ASSERT_FALSE(m.success);
    TEST_ASSERT_EQUAL_STRING("Nope", m.message.c_str());
    TEST_ASSERT_EQUAL_STRING("not_found", m.errorCode.c_str());
}

static void test_parse_pong() {
    ha::Message m = parse("{\"id\":3,\"type\":\"pong\"}");
    TEST_ASSERT_EQUAL(MT::Pong, m.type);
    TEST_ASSERT_EQUAL_UINT32(3, m.id);
}

static void test_parse_event() {
    ha::Message m = parse("{\"id\":2,\"type\":\"event\",\"event\":{\"a\":{\"light.x\":{\"s\":\"on\"}}}}");
    TEST_ASSERT_EQUAL(MT::Event, m.type);
    TEST_ASSERT_EQUAL_UINT32(2, m.id);
    TEST_ASSERT_TRUE(m.hasAdded);
    TEST_ASSERT_FALSE(m.hasChanged);
    TEST_ASSERT_FALSE(m.hasRemoved);
    m = parse("{\"id\":2,\"type\":\"event\",\"event\":{\"c\":{\"light.x\":{\"+\":{\"s\":\"off\"}}}}}");
    TEST_ASSERT_TRUE(m.hasChanged);
    TEST_ASSERT_FALSE(m.hasAdded);
    m = parse("{\"id\":2,\"type\":\"event\",\"event\":{\"r\":[\"light.x\"]}}");
    TEST_ASSERT_TRUE(m.hasRemoved);
    m = parse("{\"id\":2,\"type\":\"event\",\"event\":{\"a\":{},\"c\":{},\"r\":[]}}");
    TEST_ASSERT_TRUE(m.hasAdded && m.hasChanged && m.hasRemoved);
    m = parse("{\"id\":2,\"type\":\"event\"}");
    TEST_ASSERT_EQUAL(MT::Event, m.type);
    TEST_ASSERT_FALSE(m.hasAdded || m.hasChanged || m.hasRemoved);
}

static void test_parse_unknown_ignored() {
    ha::Message m = parse("{\"type\":\"something_else\",\"id\":1}");
    TEST_ASSERT_EQUAL(MT::Ignored, m.type);
}

static void test_parse_errors() {
    const char *bad[] = {"", "not json", "{", "[1,2]", "42", "\"str\"", "null", "{}", "{\"type\":5}",
                         "{\"id\":1}", "{\"type\":null}", "[{\"type\":\"pong\"}]"};
    for (const char *s : bad) {
        const ha::Message m = ha::parseMessage(s, std::string(s).size());
        TEST_ASSERT_EQUAL_MESSAGE(MT::Error, m.type, s);
    }
    TEST_ASSERT_EQUAL(MT::Error, ha::parseMessage(nullptr, 0).type);
    TEST_ASSERT_EQUAL_STRING("error", ha::messageTypeName(MT::Error));
}

static void test_parse_odd_id() {
    TEST_ASSERT_EQUAL_UINT32(0, parse("{\"id\":\"x\",\"type\":\"pong\"}").id);
    TEST_ASSERT_EQUAL_UINT32(0, parse("{\"id\":-1,\"type\":\"pong\"}").id);
    TEST_ASSERT_EQUAL_UINT32(0, parse("{\"type\":\"pong\"}").id);
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_build_auth);
    RUN_TEST(test_build_ping);
    RUN_TEST(test_build_subscribe);
    RUN_TEST(test_build_unsubscribe);
    RUN_TEST(test_build_get_states);
    RUN_TEST(test_build_call_service);
    RUN_TEST(test_build_call_service_omissions);
    RUN_TEST(test_parse_auth_required);
    RUN_TEST(test_parse_auth_ok);
    RUN_TEST(test_parse_auth_invalid);
    RUN_TEST(test_parse_result);
    RUN_TEST(test_parse_pong);
    RUN_TEST(test_parse_event);
    RUN_TEST(test_parse_unknown_ignored);
    RUN_TEST(test_parse_errors);
    RUN_TEST(test_parse_odd_id);
    return UNITY_END();
}
