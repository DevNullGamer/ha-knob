// Native tests for HaProtocol builders and parser (plan.md section 3.7).
#include <unity.h>

#include <ArduinoJson.h>

#include <cstdlib>
#include <cstring>
#include <map>
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

// Allocator that records the peak number of live bytes (ArduinoJson 7 has no memoryUsage()).
struct PeakAllocator : ArduinoJson::Allocator {
    size_t live = 0, peak = 0;
    std::map<void *, size_t> sizes;
    void *allocate(size_t n) override {
        void *p = malloc(n);
        if (p) { sizes[p] = n; live += n; if (live > peak) peak = live; }
        return p;
    }
    void deallocate(void *p) override {
        if (!p) return;
        live -= sizes[p];
        sizes.erase(p);
        free(p);
    }
    void *reallocate(void *p, size_t n) override {
        void *q = malloc(n);
        if (!q) return nullptr;
        if (p) { size_t old = sizes[p]; memcpy(q, p, old < n ? old : n); deallocate(p); }
        sizes[q] = n; live += n; if (live > peak) peak = live;
        return q;
    }
};

// Builds {"id":42,"type":"result","success":true,"result":["xxx..","xxx..",..]} of exactly `total`
// bytes. The payload is an array of 1000-char strings: ArduinoJson's default string length
// limit is 65535, so one multi-100-KB string would not parse even without the envelope filter.
static std::string bigResult(size_t total) {
    const std::string head = "{\"id\":42,\"type\":\"result\",\"success\":true,\"result\":[";
    const std::string tail = "]}";
    std::string body;
    const std::string item(1000, 'x');
    const size_t room = total - head.size() - tail.size();
    while (body.size() + item.size() + 3 <= room) body += "\"" + item + "\",";
    // fill the rest with one shorter last string so the total is exact
    const size_t rest = room - body.size();  // >= 2 : "..." with 0..n chars
    body += "\"" + std::string(rest - 2, 'y') + "\"";
    return head + body + tail;
}

static void test_envelope_only_boundary() {
    // exactly kEnvelopeOnlyBytes: still parsed fully
    std::string at = bigResult(ha::kEnvelopeOnlyBytes);
    TEST_ASSERT_EQUAL_UINT32(ha::kEnvelopeOnlyBytes, (uint32_t)at.size());
    ha::Message a = parse(at);
    TEST_ASSERT_EQUAL(MT::Result, a.type);
    TEST_ASSERT_FALSE(a.envelopeOnly);
    TEST_ASSERT_TRUE(a.result().is<JsonArrayConst>());
    // one byte more: envelope only
    std::string over = bigResult(ha::kEnvelopeOnlyBytes + 1);
    ha::Message b = parse(over);
    TEST_ASSERT_EQUAL(MT::Result, b.type);
    TEST_ASSERT_TRUE(b.envelopeOnly);
    TEST_ASSERT_TRUE(b.success);
}

static void test_small_message_not_envelope_only() {
    ha::Message m = parse("{\"id\":1,\"type\":\"result\",\"success\":true,\"result\":{\"a\":[1,2]}}");
    TEST_ASSERT_FALSE(m.envelopeOnly);
    TEST_ASSERT_EQUAL_INT(2, (int)m.result()["a"].size());
}

static void test_200kb_parsed_fully() {
    std::string s = bigResult(200u * 1024u);
    ha::Message m = parse(s);
    TEST_ASSERT_EQUAL(MT::Result, m.type);
    TEST_ASSERT_FALSE(m.envelopeOnly);
    TEST_ASSERT_TRUE(m.success);
    TEST_ASSERT_EQUAL_UINT32(42, m.id);
    TEST_ASSERT_TRUE(m.result().is<JsonArrayConst>());
    TEST_ASSERT_TRUE(m.result().size() > 190u);
}

static void test_1mb_result_envelope_only() {
    std::string s = bigResult(1024u * 1024u);
    PeakAllocator pa;
    ha::Message m = ha::parseMessage(s.c_str(), s.size(), &pa);
    TEST_ASSERT_TRUE_MESSAGE(pa.peak < 64u * 1024u, "peak JSON allocation must stay below 64 KB");
    TEST_ASSERT_EQUAL(MT::Result, m.type);
    TEST_ASSERT_TRUE(m.envelopeOnly);
    TEST_ASSERT_TRUE(m.success);
    TEST_ASSERT_EQUAL_UINT32(42, m.id);
    TEST_ASSERT_TRUE(m.result().isNull());  // no result payload kept
    TEST_ASSERT_FALSE(m.hasAdded);
    TEST_ASSERT_FALSE(m.hasChanged);
    TEST_ASSERT_FALSE(m.hasRemoved);
    // the document did not grow to message size
}

static void test_big_result_with_array_payload_envelope_only() {
    std::string head = "{\"id\":7,\"type\":\"result\",\"success\":true,\"result\":[";
    std::string body;
    while (body.size() < 300u * 1024u)
        body += "{\"entity_id\":\"light.x\",\"state\":\"on\",\"attributes\":{\"b\":1}},";
    body.pop_back();
    const std::string all = head + body + "]}";
    PeakAllocator pa;
    ha::Message m = ha::parseMessage(all.c_str(), all.size(), &pa);
    TEST_ASSERT_TRUE_MESSAGE(pa.peak < 64u * 1024u, "peak JSON allocation must stay below 64 KB");
    TEST_ASSERT_EQUAL(MT::Result, m.type);
    TEST_ASSERT_TRUE(m.envelopeOnly);
    TEST_ASSERT_TRUE(m.success);
    TEST_ASSERT_EQUAL_UINT32(7, m.id);
    TEST_ASSERT_TRUE(m.result().isNull());
}

static void test_big_failed_result_keeps_error() {
    std::string s = "{\"id\":9,\"type\":\"result\",\"success\":false,\"error\":{\"code\":\"not_found\","
                    "\"message\":\"Entity not found\",\"extra\":\"" + std::string(400u * 1024u, 'y') + "\"}}";
    ha::Message m = parse(s);
    TEST_ASSERT_EQUAL(MT::Result, m.type);
    TEST_ASSERT_TRUE(m.envelopeOnly);
    TEST_ASSERT_FALSE(m.success);
    TEST_ASSERT_EQUAL_UINT32(9, m.id);
    TEST_ASSERT_EQUAL_STRING("not_found", m.errorCode.c_str());
    TEST_ASSERT_EQUAL_STRING("Entity not found", m.message.c_str());
    TEST_ASSERT_TRUE(m.doc["error"]["extra"].isNull());
}

static void test_big_other_types_keep_envelope() {
    std::string pad(300u * 1024u, 'p');
    ha::Message ok = parse("{\"type\":\"auth_ok\",\"ha_version\":\"2026.1.2\",\"pad\":\"" + pad + "\"}");
    TEST_ASSERT_EQUAL(MT::AuthOk, ok.type);
    TEST_ASSERT_TRUE(ok.envelopeOnly);
    TEST_ASSERT_EQUAL_STRING("2026.1.2", ok.haVersion.c_str());
    ha::Message bad = parse("{\"type\":\"auth_invalid\",\"message\":\"nope\",\"pad\":\"" + pad + "\"}");
    TEST_ASSERT_EQUAL(MT::AuthInvalid, bad.type);
    TEST_ASSERT_EQUAL_STRING("nope", bad.message.c_str());
    ha::Message req = parse("{\"type\":\"auth_required\",\"ha_version\":\"1.0\",\"pad\":\"" + pad + "\"}");
    TEST_ASSERT_EQUAL(MT::AuthRequired, req.type);
    TEST_ASSERT_EQUAL_STRING("1.0", req.haVersion.c_str());
    ha::Message pong = parse("{\"id\":5,\"type\":\"pong\",\"pad\":\"" + pad + "\"}");
    TEST_ASSERT_EQUAL(MT::Pong, pong.type);
    TEST_ASSERT_EQUAL_UINT32(5, pong.id);
    ha::Message other = parse("{\"type\":\"whatever\",\"pad\":\"" + pad + "\"}");
    TEST_ASSERT_EQUAL(MT::Ignored, other.type);
}

static void test_big_event_is_envelope_only_without_payload_flags() {
    std::string s = "{\"id\":3,\"type\":\"event\",\"event\":{\"a\":{\"light.x\":{\"s\":\"" +
                    std::string(300u * 1024u, 'z') + "\"}}}}";
    ha::Message m = parse(s);
    TEST_ASSERT_EQUAL(MT::Event, m.type);
    TEST_ASSERT_EQUAL_UINT32(3, m.id);
    TEST_ASSERT_TRUE(m.envelopeOnly);
    TEST_ASSERT_FALSE(m.hasAdded);
    TEST_ASSERT_TRUE(m.event().isNull());
}

static void test_big_invalid_json_is_error() {
    std::string s = "{\"type\":\"result\",\"x\":\"" + std::string(300u * 1024u, 'q');  // truncated
    ha::Message m = parse(s);
    TEST_ASSERT_EQUAL(MT::Error, m.type);
}

// ---- Step 6.2: message filter and nesting limit (plan-step6.md section 3.3) ----

static bool hasKey(JsonVariantConst v, const char *k) {
    for (JsonPairConst kv : v.as<JsonObjectConst>()) if (std::string(kv.key().c_str()) == k) return true;
    return false;
}

static std::string nested(int levels) {
    return std::string(levels, '[') + std::string(levels, ']');
}

static std::string aEvent(const std::string &extraAttr) {
    return "{\"id\":2,\"type\":\"event\",\"event\":{\"a\":{\"light.x\":{\"s\":\"on\",\"a\":{"
           "\"brightness\":100,\"friendly_name\":\"X\"," + extraAttr + "}}}}}";
}

static std::string cEvent(const std::string &extraAttr) {
    return "{\"id\":2,\"type\":\"event\",\"event\":{\"c\":{\"light.x\":{\"+\":{\"s\":\"off\",\"a\":{"
           "\"brightness\":50," + extraAttr + "}}}}}}";
}

static void test_event_filter_keeps_needed_drops_unneeded() {
    ha::Message m = parse(aEvent("\"entity_picture\":\"/local/x.png\",\"source_list\":[\"a\",\"b\"],\"foo\":{\"bar\":1}"));
    TEST_ASSERT_EQUAL(MT::Event, m.type);
    TEST_ASSERT_TRUE(m.hasAdded);
    JsonVariantConst e = m.event()["a"]["light.x"];
    TEST_ASSERT_EQUAL_STRING("on", e["s"].as<const char *>());
    TEST_ASSERT_EQUAL_INT(100, e["a"]["brightness"].as<int>());
    TEST_ASSERT_EQUAL_STRING("X", e["a"]["friendly_name"].as<const char *>());
    TEST_ASSERT_TRUE(e["a"]["entity_picture"].isNull());
    TEST_ASSERT_TRUE(e["a"]["source_list"].isNull());
    TEST_ASSERT_TRUE(e["a"]["foo"].isNull());
    TEST_ASSERT_FALSE(hasKey(m.doc["event"]["a"]["light.x"]["a"], "entity_picture"));
}

static void test_event_context_and_timestamps_dropped() {
    ha::Message m = parse("{\"id\":2,\"type\":\"event\",\"event\":{\"a\":{\"light.x\":{\"s\":\"on\",\"a\":{},"
                          "\"c\":{\"id\":\"1\",\"parent_id\":null,\"user_id\":null},\"lc\":1.5,\"lu\":2.5}}}}");
    TEST_ASSERT_EQUAL(MT::Event, m.type);
    JsonVariantConst e = m.event()["a"]["light.x"];
    TEST_ASSERT_EQUAL_STRING("on", e["s"].as<const char *>());
    TEST_ASSERT_TRUE(e["c"].isNull());
    TEST_ASSERT_TRUE(e["lc"].isNull());
}

static void test_change_event_filter_keeps_plus_and_minus() {
    ha::Message m = parse("{\"id\":2,\"type\":\"event\",\"event\":{\"c\":{\"light.x\":{\"+\":{\"s\":\"off\","
                          "\"a\":{\"brightness\":null,\"entity_picture\":\"x\"},\"c\":\"01\",\"lc\":1.0},"
                          "\"-\":{\"a\":[\"hs_color\"]}}}}}");
    TEST_ASSERT_EQUAL(MT::Event, m.type);
    TEST_ASSERT_TRUE(m.hasChanged);
    JsonVariantConst d = m.event()["c"]["light.x"];
    TEST_ASSERT_EQUAL_STRING("off", d["+"]["s"].as<const char *>());
    TEST_ASSERT_TRUE(hasKey(d["+"]["a"], "brightness"));
    TEST_ASSERT_TRUE(d["+"]["a"]["brightness"].isNull());
    TEST_ASSERT_TRUE(d["+"]["a"]["entity_picture"].isNull());
    TEST_ASSERT_TRUE(d["+"]["c"].isNull());
    TEST_ASSERT_TRUE(d["+"]["lc"].isNull());
    TEST_ASSERT_EQUAL_STRING("hs_color", d["-"]["a"][0].as<const char *>());
}

static void test_removed_event_filter() {
    ha::Message m = parse("{\"id\":2,\"type\":\"event\",\"event\":{\"r\":[\"light.x\",\"light.y\"]}}");
    TEST_ASSERT_TRUE(m.hasRemoved);
    TEST_ASSERT_EQUAL_INT(2, (int)m.event()["r"].size());
    TEST_ASSERT_EQUAL_STRING("light.y", m.event()["r"][1].as<const char *>());
}

static void test_event_unneeded_attr_70000_bytes_parses() {
    ha::Message m = parse(aEvent("\"entity_picture\":\"" + std::string(70000, 'p') + "\""));
    TEST_ASSERT_EQUAL(MT::Event, m.type);
    TEST_ASSERT_TRUE(m.hasAdded);
    JsonVariantConst a = m.event()["a"]["light.x"]["a"];
    TEST_ASSERT_EQUAL_INT(100, a["brightness"].as<int>());
    TEST_ASSERT_EQUAL_STRING("X", a["friendly_name"].as<const char *>());
    TEST_ASSERT_TRUE(a["entity_picture"].isNull());
    TEST_ASSERT_FALSE(hasKey(a, "entity_picture"));
    ha::Message c = parse(cEvent("\"entity_picture\":\"" + std::string(70000, 'p') + "\""));
    TEST_ASSERT_EQUAL(MT::Event, c.type);
    TEST_ASSERT_TRUE(c.hasChanged);
    TEST_ASSERT_FALSE(hasKey(c.event()["c"]["light.x"]["+"]["a"], "entity_picture"));
}

static void test_nesting_boundaries_a_event() {
    // an unneeded attribute value nested 15 levels in an "a" event parses
    ha::Message ok = parse(aEvent("\"foo\":" + nested(15)));
    TEST_ASSERT_EQUAL(MT::Event, ok.type);
    TEST_ASSERT_TRUE(ok.hasAdded);
    TEST_ASSERT_EQUAL_INT(100, ok.event()["a"]["light.x"]["a"]["brightness"].as<int>());
    TEST_ASSERT_TRUE(ok.event()["a"]["light.x"]["a"]["foo"].isNull());
    // 25 levels: Error, no crash
    TEST_ASSERT_EQUAL(MT::Error, parse(aEvent("\"foo\":" + nested(25))).type);
}

static void test_nesting_boundaries_c_event() {
    ha::Message ok = parse(cEvent("\"foo\":" + nested(14)));
    TEST_ASSERT_EQUAL(MT::Event, ok.type);
    TEST_ASSERT_TRUE(ok.hasChanged);
    TEST_ASSERT_EQUAL_INT(50, ok.event()["c"]["light.x"]["+"]["a"]["brightness"].as<int>());
    TEST_ASSERT_EQUAL(MT::Error, parse(cEvent("\"foo\":" + nested(15))).type);
    TEST_ASSERT_EQUAL(MT::Error, parse(cEvent("\"foo\":" + nested(25))).type);
}

static void test_deep_nesting_everywhere_is_error_not_crash() {
    // deep value in an unrelated envelope key and in a result
    TEST_ASSERT_EQUAL(MT::Error, parse("{\"type\":\"pong\",\"x\":" + nested(100) + "}").type);
    TEST_ASSERT_EQUAL(MT::Error, parse("{\"id\":1,\"type\":\"result\",\"success\":true,\"result\":" + nested(30) + "}").type);
    // 5000 levels, unterminated
    TEST_ASSERT_EQUAL(MT::Error, parse("{\"type\":\"pong\",\"x\":" + std::string(5000, '[')).type);
}

static void test_result_payload_still_kept_small() {
    ha::Message m = parse("{\"id\":1,\"type\":\"result\",\"success\":true,\"result\":{\"a\":{\"b\":[1,{\"c\":2}]}}}");
    TEST_ASSERT_EQUAL(MT::Result, m.type);
    TEST_ASSERT_EQUAL_INT(2, m.result()["a"]["b"][1]["c"].as<int>());
}

static void test_parse_error_reasons() {
    TEST_ASSERT_EQUAL_STRING("IncompleteInput", parse("{\"id\":1,\"type\":\"event\",\"event\":{\"a\":").parseError);
    TEST_ASSERT_EQUAL_STRING("InvalidInput", parse("{]").parseError);
    TEST_ASSERT_EQUAL_STRING("not_object", parse("garbage").parseError);
    TEST_ASSERT_TRUE(parse("").parseError[0] != 0);  // some ArduinoJson error name
    TEST_ASSERT_EQUAL_STRING("not_object", parse("[1,2]").parseError);
    TEST_ASSERT_EQUAL_STRING("no_type", parse("{\"id\":1}").parseError);
    TEST_ASSERT_EQUAL_STRING("no_type", parse("{\"type\":5}").parseError);
    TEST_ASSERT_EQUAL(MT::Error, parse("{\"id\":1}").type);
    // valid messages (also Ignored) carry no parse error
    TEST_ASSERT_EQUAL_STRING("", parse("{\"id\":1,\"type\":\"pong\"}").parseError);
    TEST_ASSERT_EQUAL_STRING("", parse("{\"type\":\"other\"}").parseError);
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_event_filter_keeps_needed_drops_unneeded);
    RUN_TEST(test_event_context_and_timestamps_dropped);
    RUN_TEST(test_change_event_filter_keeps_plus_and_minus);
    RUN_TEST(test_removed_event_filter);
    RUN_TEST(test_event_unneeded_attr_70000_bytes_parses);
    RUN_TEST(test_nesting_boundaries_a_event);
    RUN_TEST(test_nesting_boundaries_c_event);
    RUN_TEST(test_deep_nesting_everywhere_is_error_not_crash);
    RUN_TEST(test_result_payload_still_kept_small);
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
    RUN_TEST(test_envelope_only_boundary);
    RUN_TEST(test_small_message_not_envelope_only);
    RUN_TEST(test_200kb_parsed_fully);
    RUN_TEST(test_1mb_result_envelope_only);
    RUN_TEST(test_big_result_with_array_payload_envelope_only);
    RUN_TEST(test_big_failed_result_keeps_error);
    RUN_TEST(test_big_other_types_keep_envelope);
    RUN_TEST(test_big_event_is_envelope_only_without_payload_flags);
    RUN_TEST(test_big_invalid_json_is_error);
    RUN_TEST(test_parse_error_reasons);
    return UNITY_END();
}
