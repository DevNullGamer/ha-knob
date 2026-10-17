// Native tests for util/JsonScan and ha/EntityCatalog (plan-step6.md sections 3.1, 3.5,
// Step 6.3).
#include <unity.h>

#include <ArduinoJson.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include "fixture_states.h"
#include "ha/EntityCatalog.h"
#include "util/JsonScan.h"

namespace ha = core::ha;
namespace js = core::util::json_scan;
using Status = ha::EntityCatalogBuilder::Status;

void setUp() {}
void tearDown() {}

constexpr uint32_t kId = 7;

// ---------------- helpers ----------------

static std::string envelope(const std::string &resultArray, uint32_t id = kId) {
    return "{\"id\":" + std::to_string(id) + ",\"type\":\"result\",\"success\":true,\"result\":" +
           resultArray + "}";
}

static Status runAll(ha::EntityCatalogBuilder &b, size_t budget) {
    for (int i = 0; i < 20000000; ++i) {
        if (b.step(budget) != Status::Running) break;
    }
    return b.status();
}

static std::string toJson(const ha::EntityCatalog &c) { return ha::catalogToJson(c, 0); }

static std::string lightEntity(const std::string &id, const std::string &name = "N",
                               const std::string &state = "on") {
    return "{\"entity_id\":\"" + id + "\",\"state\":\"" + state +
           "\",\"attributes\":{\"friendly_name\":\"" + name +
           "\",\"supported_color_modes\":[\"onoff\"]}}";
}

static std::string joinArray(const std::vector<std::string> &items) {
    std::string out = "[";
    for (size_t i = 0; i < items.size(); ++i) {
        if (i) out += ",";
        out += items[i];
    }
    return out + "]";
}

// Builds fully and returns the JSON of the catalog (Done expected).
static std::string buildJson(const std::string &text, size_t budget = (size_t)-1) {
    ha::EntityCatalogBuilder b(text.data(), text.size(), kId);
    TEST_ASSERT_EQUAL(Status::Done, runAll(b, budget));
    return toJson(b.entries());
}

static std::string fixtureText() { return envelope(kFixtureResultArray); }

static std::string actionsOf(const ha::EntityCatalog &c, const char *id) {
    for (size_t i = 0; i < c.size(); ++i) {
        ha::CatalogEntry e = c.entry(i);
        if (e.id == id) {
            std::string out;
            for (auto *a : e.actions()) {
                if (!out.empty()) out += ",";
                out += a->name;
            }
            return out;
        }
    }
    return "<absent>";
}

static ha::CatalogEntry findEntry(const ha::EntityCatalog &c, const char *id) {
    for (size_t i = 0; i < c.size(); ++i) {
        if (c.entry(i).id == id) return c.entry(i);
    }
    TEST_FAIL_MESSAGE(id);
    return ha::CatalogEntry();
}

// ---------------- JsonScan ----------------

static void test_scan_whitespace() {
    const char s[] = " \t\r\n x";
    TEST_ASSERT_EQUAL_UINT32(5, (uint32_t)js::skipWhitespace(s, 6, 0));
    TEST_ASSERT_EQUAL_UINT32(4, (uint32_t)js::skipWhitespace(s, 4, 0));  // all whitespace -> len
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)js::skipWhitespace(s, 0, 0));
}

static void test_scan_string() {
    size_t end = 0;
    const std::string a = "\"abc\" rest";
    TEST_ASSERT_EQUAL(js::ScanStatus::Ok, js::scanString(a.data(), a.size(), 0, end));
    TEST_ASSERT_EQUAL_UINT32(5, (uint32_t)end);
    const std::string esc = "\"a\\\"b\\\\\" x";  // "a\"b\\"
    TEST_ASSERT_EQUAL(js::ScanStatus::Ok, js::scanString(esc.data(), esc.size(), 0, end));
    TEST_ASSERT_EQUAL_UINT32(8, (uint32_t)end);
    const std::string empty = "\"\"";
    TEST_ASSERT_EQUAL(js::ScanStatus::Ok, js::scanString(empty.data(), empty.size(), 0, end));
    TEST_ASSERT_EQUAL_UINT32(2, (uint32_t)end);
    const std::string unterminated = "\"abc";
    TEST_ASSERT_EQUAL(js::ScanStatus::Truncated, js::scanString(unterminated.data(), unterminated.size(), 0, end));
    const std::string escQuoteLast = "\"abc\\\"";  // closing quote is escaped
    TEST_ASSERT_EQUAL(js::ScanStatus::Truncated, js::scanString(escQuoteLast.data(), escQuoteLast.size(), 0, end));
    const std::string trailingBackslash = "\"abc\\";
    TEST_ASSERT_EQUAL(js::ScanStatus::Truncated, js::scanString(trailingBackslash.data(), trailingBackslash.size(), 0, end));
    // offset start
    const std::string off = "xx\"q\"";
    TEST_ASSERT_EQUAL(js::ScanStatus::Ok, js::scanString(off.data(), off.size(), 2, end));
    TEST_ASSERT_EQUAL_UINT32(5, (uint32_t)end);
}

static void test_scan_skip_value() {
    size_t end = 0;
    auto skip = [&](const std::string &s) { return js::skipValue(s.data(), s.size(), 0, end); };
    TEST_ASSERT_EQUAL(js::ScanStatus::Ok, skip("{\"a\":[1,{\"b\":\"]}\"}],\"c\":2},"));
    TEST_ASSERT_EQUAL_UINT32(26, (uint32_t)end);
    TEST_ASSERT_EQUAL(js::ScanStatus::Ok, skip("[1,2,3]x"));
    TEST_ASSERT_EQUAL_UINT32(7, (uint32_t)end);
    TEST_ASSERT_EQUAL(js::ScanStatus::Ok, skip("-12.5e+3,"));
    TEST_ASSERT_EQUAL_UINT32(8, (uint32_t)end);
    TEST_ASSERT_EQUAL(js::ScanStatus::Ok, skip("true}"));
    TEST_ASSERT_EQUAL_UINT32(4, (uint32_t)end);
    TEST_ASSERT_EQUAL(js::ScanStatus::Ok, skip("null"));
    TEST_ASSERT_EQUAL_UINT32(4, (uint32_t)end);
    TEST_ASSERT_EQUAL(js::ScanStatus::Ok, skip("\"s\\\"]\""));
    TEST_ASSERT_EQUAL_UINT32(6, (uint32_t)end);
    TEST_ASSERT_EQUAL(js::ScanStatus::Ok, skip("[]"));
    TEST_ASSERT_EQUAL_UINT32(2, (uint32_t)end);
    TEST_ASSERT_EQUAL(js::ScanStatus::Ok, skip("{}"));
    // brackets in strings do not count
    TEST_ASSERT_EQUAL(js::ScanStatus::Ok, skip("[\"]]]\",\"[[\"]"));
    TEST_ASSERT_EQUAL_UINT32(12, (uint32_t)end);
}

static void test_scan_skip_value_errors() {
    size_t end = 0;
    auto skip = [&](const std::string &s) { return js::skipValue(s.data(), s.size(), 0, end); };
    TEST_ASSERT_EQUAL(js::ScanStatus::Truncated, skip("[1,2"));
    TEST_ASSERT_EQUAL(js::ScanStatus::Truncated, skip("{\"a\":[1]"));
    TEST_ASSERT_EQUAL(js::ScanStatus::Truncated, skip("{\"a\":\"x"));
    TEST_ASSERT_EQUAL(js::ScanStatus::Malformed, skip("[1}"));
    TEST_ASSERT_EQUAL(js::ScanStatus::Malformed, skip("{\"a\":1]"));
    TEST_ASSERT_EQUAL(js::ScanStatus::Malformed, skip("]"));
    TEST_ASSERT_EQUAL(js::ScanStatus::Malformed, skip("}"));
    TEST_ASSERT_EQUAL(js::ScanStatus::Malformed, skip(","));
    TEST_ASSERT_TRUE(skip("") != js::ScanStatus::Ok);
}

static void test_scan_deep_nesting_no_overflow() {
    size_t end = 0;
    const std::string deep = std::string(100000, '[') + std::string(100000, ']');
    TEST_ASSERT_EQUAL(js::ScanStatus::Ok, js::skipValue(deep.data(), deep.size(), 0, end));
    TEST_ASSERT_EQUAL_UINT32(200000, (uint32_t)end);
    const std::string unclosed(100000, '[');
    TEST_ASSERT_EQUAL(js::ScanStatus::Truncated, js::skipValue(unclosed.data(), unclosed.size(), 0, end));
    const std::string d100 = std::string(100, '[') + std::string(100, ']');
    TEST_ASSERT_EQUAL(js::ScanStatus::Ok, js::skipValue(d100.data(), d100.size(), 0, end));
    // mismatch inside the first 64 levels is caught
    const std::string bad = std::string(10, '[') + "}";
    TEST_ASSERT_EQUAL(js::ScanStatus::Malformed, js::skipValue(bad.data(), bad.size(), 0, end));
}

static void test_scan_parse_uint32() {
    uint32_t v = 99;
    TEST_ASSERT_TRUE(js::parseUint32("0", 1, v));
    TEST_ASSERT_EQUAL_UINT32(0, v);
    TEST_ASSERT_TRUE(js::parseUint32("42", 2, v));
    TEST_ASSERT_EQUAL_UINT32(42, v);
    TEST_ASSERT_TRUE(js::parseUint32("4294967295", 10, v));
    TEST_ASSERT_EQUAL_UINT32(4294967295u, v);
    TEST_ASSERT_FALSE(js::parseUint32("4294967296", 10, v));
    TEST_ASSERT_FALSE(js::parseUint32("99999999999", 11, v));
    TEST_ASSERT_FALSE(js::parseUint32("01", 2, v));
    TEST_ASSERT_FALSE(js::parseUint32("-1", 2, v));
    TEST_ASSERT_FALSE(js::parseUint32("+1", 2, v));
    TEST_ASSERT_FALSE(js::parseUint32("", 0, v));
    TEST_ASSERT_FALSE(js::parseUint32("1.0", 3, v));
    TEST_ASSERT_FALSE(js::parseUint32("1e3", 3, v));
    TEST_ASSERT_FALSE(js::parseUint32("12a", 3, v));
    TEST_ASSERT_TRUE(js::parseUint32("123xyz", 3, v));  // only len bytes are read
    TEST_ASSERT_EQUAL_UINT32(123, v);
}

static void test_scan_equals() {
    TEST_ASSERT_TRUE(js::equals("result", 6, "result"));
    TEST_ASSERT_FALSE(js::equals("result", 5, "result"));
    TEST_ASSERT_FALSE(js::equals("results", 7, "result"));
    TEST_ASSERT_TRUE(js::equals("", 0, ""));
    TEST_ASSERT_FALSE(js::equals("a", 1, ""));
}

// ---------------- peekEnvelope ----------------

static ha::EnvelopePeek peek(const std::string &s, size_t maxBytes = ha::kEnvelopePeekBytes) {
    return ha::peekEnvelope(s.data(), s.size(), maxBytes);
}

// String literals outlive the returned EnvelopePeek (its type is a view into the text); a
// temporary std::string would dangle.
static ha::EnvelopePeek peek(const char *literal, size_t maxBytes = ha::kEnvelopePeekBytes) {
    return ha::peekEnvelope(literal, strlen(literal), maxBytes);
}

static void test_peek_ha_order() {
    const std::string s = fixtureText();
    ha::EnvelopePeek p = peek(s);
    TEST_ASSERT_TRUE(p.hasId);
    TEST_ASSERT_EQUAL_UINT32(kId, p.id);
    TEST_ASSERT_TRUE(p.hasType);
    TEST_ASSERT_TRUE(p.type == "result");
}

static void test_peek_other_order() {
    // result first: stops at the array, id not seen
    std::string s = std::string("{\"result\":") + kFixtureResultArray + ",\"success\":true,\"type\":\"result\",\"id\":7}";
    ha::EnvelopePeek p = peek(s);
    TEST_ASSERT_FALSE(p.hasId);
    // scalars before the array in another order
    p = peek("{\"type\":\"result\",\"success\":true,\"id\":12,\"result\":[1]}");
    TEST_ASSERT_TRUE(p.hasId);
    TEST_ASSERT_EQUAL_UINT32(12, p.id);
    TEST_ASSERT_TRUE(p.type == "result");
    // whitespace
    p = peek(" { \"id\" : 5 , \"type\" : \"pong\" } ");
    TEST_ASSERT_TRUE(p.hasId);
    TEST_ASSERT_EQUAL_UINT32(5, p.id);
}

static void test_peek_short_messages() {
    ha::EnvelopePeek p = peek("{\"id\":7,\"type\":\"pong\"}");
    TEST_ASSERT_TRUE(p.hasId);
    TEST_ASSERT_EQUAL_UINT32(7, p.id);
    TEST_ASSERT_TRUE(p.type == "pong");
    p = peek("{\"id\":7}");
    TEST_ASSERT_TRUE(p.hasId);
    TEST_ASSERT_FALSE(p.hasType);
    p = peek("{\"type\":\"event\"}");
    TEST_ASSERT_FALSE(p.hasId);
    TEST_ASSERT_TRUE(p.hasType);
}

static void test_peek_garbage() {
    const char *bad[] = {"", "garbage", "{", "[1,2]", "42", "{\"id\":", "{\"id\":\"7\"}", "{\"id\":-1}",
                         "{\"id\":1.5}", "{\"id\":99999999999}", "{id:7}", "{\"id\" 7}",
                         "\xff\xfe{\"id\":7}", "{\"a\":\"unterminated"};
    for (const char *s : bad) {
        ha::EnvelopePeek p = ha::peekEnvelope(s, std::string(s).size());
        TEST_ASSERT_FALSE_MESSAGE(p.hasId, s);
    }
    ha::EnvelopePeek p = ha::peekEnvelope(nullptr, 0);
    TEST_ASSERT_FALSE(p.hasId);
    TEST_ASSERT_FALSE(p.hasType);
}

static void test_peek_limit_bytes() {
    // id after more than maxBytes of padding is not seen
    std::string s = "{\"pad\":\"" + std::string(2000, 'x') + "\",\"id\":3}";
    TEST_ASSERT_FALSE(peek(s).hasId);
    // a value cut by the limit is inconclusive
    TEST_ASSERT_FALSE(peek("{\"id\":7,\"type\":\"result\"}", 7).hasId);
    // within the limit it is found
    s = "{\"pad\":\"" + std::string(500, 'x') + "\",\"id\":3}";
    TEST_ASSERT_TRUE(peek(s).hasId);
    TEST_ASSERT_EQUAL_UINT32(3, peek(s).id);
}

// ---------------- builder: fixture ----------------

static void test_fixture_size_and_counts() {
    TEST_ASSERT_TRUE(kFixtureTotal >= 50);
    const std::string text = fixtureText();
    ha::EntityCatalogBuilder b(text.data(), text.size(), kId);
    TEST_ASSERT_EQUAL(Status::Done, runAll(b, 4096));
    TEST_ASSERT_EQUAL_UINT32(kFixtureTotal, b.total());
    TEST_ASSERT_EQUAL_UINT32(0, b.skipped());
    TEST_ASSERT_FALSE(b.truncated());
    TEST_ASSERT_EQUAL_UINT32(kFixtureSupported, (uint32_t)b.entries().size());
    TEST_ASSERT_EQUAL_UINT32(text.size(), (uint32_t)b.bytesProcessed());
}

static void test_fixture_actions_and_steps() {
    const std::string text = fixtureText();
    ha::EntityCatalogBuilder b(text.data(), text.size(), kId);
    TEST_ASSERT_EQUAL(Status::Done, runAll(b, (size_t)-1));
    const ha::EntityCatalog &c = b.entries();
    struct Row { const char *id; const char *actions; double step; };
    const Row rows[] = {
        {"light.onoff", "none", 0},
        {"light.bright", "brightness,none", 5},
        {"light.ct", "brightness,color_temp,none", 5},
        {"light.hs", "brightness,hue,none", 5},
        {"light.xy", "brightness,hue,none", 5},
        {"light.kitchen", "brightness,hue,color_temp,none", 5},
        {"light.noname", "brightness,none", 5},
        {"climate.heat", "target_temp,none", 1},
        {"climate.off", "target_temp,none", 0.5},
        {"climate.notemp", "none", 0},
        {"cover.pos", "position,none", 10},
        {"cover.nopos", "none", 0},
        {"fan.f", "percentage,none", 34},
        {"media_player.tv", "volume,none", 2},
        {"switch.sw", "none", 0},
        {"input_boolean.ib", "none", 0},
    };
    for (const Row &r : rows) {
        TEST_ASSERT_EQUAL_STRING_MESSAGE(r.actions, actionsOf(c, r.id).c_str(), r.id);
        ha::CatalogEntry e = findEntry(c, r.id);
        TEST_ASSERT_EQUAL_FLOAT_MESSAGE((float)r.step, (float)e.stepDefault, r.id);
    }
}

static void test_fixture_unsupported_absent() {
    const std::string text = fixtureText();
    ha::EntityCatalogBuilder b(text.data(), text.size(), kId);
    runAll(b, (size_t)-1);
    TEST_ASSERT_EQUAL_STRING("<absent>", actionsOf(b.entries(), "sensor.temp").c_str());
    TEST_ASSERT_EQUAL_STRING("<absent>", actionsOf(b.entries(), "automation.a").c_str());
    TEST_ASSERT_EQUAL_STRING("<absent>", actionsOf(b.entries(), "binary_sensor.b0").c_str());
    TEST_ASSERT_EQUAL_STRING("<absent>", actionsOf(b.entries(), "sensor.s3").c_str());
}

static void test_fixture_names_states_order() {
    const std::string text = fixtureText();
    ha::EntityCatalogBuilder b(text.data(), text.size(), kId);
    runAll(b, (size_t)-1);
    const ha::EntityCatalog &c = b.entries();
    // HA order
    TEST_ASSERT_TRUE(c.entry(0).id == "light.onoff");
    TEST_ASSERT_TRUE(c.entry(1).id == "light.bright");
    TEST_ASSERT_TRUE(c.entry(15).id == "input_boolean.ib");
    TEST_ASSERT_TRUE(c.entry(16).id == "light.fill0");
    // Koekken survives (UTF-8)
    TEST_ASSERT_TRUE(findEntry(c, "light.kitchen").name == "K\xc3\xb8kken");
    // friendly_name falls back to the entity id
    TEST_ASSERT_TRUE(findEntry(c, "light.noname").name == "light.noname");
    TEST_ASSERT_TRUE(findEntry(c, "light.noname").state == "unavailable");
    TEST_ASSERT_TRUE(findEntry(c, "light.onoff").state == "on");
    TEST_ASSERT_TRUE(findEntry(c, "light.hs").state == "off");
    // escaped quotes and brackets in a name
    TEST_ASSERT_TRUE(findEntry(c, "switch.sw").name == "Switch \"Q\" [1]");
    TEST_ASSERT_TRUE(findEntry(c, "light.onoff").domain == core::domain_actions::findDomain("light"));
}

static void test_result_first_key_order() {
    const std::string normal = buildJson(fixtureText());
    const std::string resultFirst =
        std::string("{\"result\":") + kFixtureResultArray + ",\"success\":true,\"type\":\"result\",\"id\":7}";
    TEST_ASSERT_TRUE(normal == buildJson(resultFirst));
    const std::string middle =
        std::string("{\"type\":\"result\",\"result\":") + kFixtureResultArray + ",\"id\":7,\"success\":true}";
    TEST_ASSERT_TRUE(normal == buildJson(middle));
    // whitespace everywhere
    const std::string spaced =
        std::string(" {\n \"id\" : 7 ,\n \"type\" : \"result\" , \"success\" : true , \"result\" : ") +
        kFixtureResultArray + " \n} \n";
    TEST_ASSERT_TRUE(normal == buildJson(spaced));
}

// ---------------- slicing ----------------

static void test_slicing_identical_output() {
    const std::string text = fixtureText();
    const std::string ref = buildJson(text, (size_t)-1);
    const size_t budgets[] = {1, 100, 4096, (size_t)-1};
    for (size_t budget : budgets) {
        TEST_ASSERT_TRUE_MESSAGE(ref == buildJson(text, budget), "budget changed the output");
    }
    // also for the result-first order and with skipped / truncated content
    const std::string odd = std::string("{\"success\":true,\"result\":") +
        joinArray({lightEntity("light.a"), "5", lightEntity("light.b"), "{\"state\":\"on\"}"}) +
        ",\"type\":\"result\",\"id\":7}";
    const std::string ref2 = buildJson(odd, (size_t)-1);
    for (size_t budget : budgets) TEST_ASSERT_TRUE(ref2 == buildJson(odd, budget));
}

static void test_slicing_step_bounded() {
    const std::string text = fixtureText();
    // the largest unit is one element plus its separator
    size_t maxElement = 0;
    {
        JsonDocument d;
        TEST_ASSERT_TRUE(!deserializeJson(d, kFixtureResultArray));
        for (JsonVariantConst e : d.as<JsonArrayConst>()) {
            const size_t n = measureJson(e);
            if (n > maxElement) maxElement = n;
        }
    }
    const size_t unitBound = maxElement + 64;  // + separator / whitespace slack
    const size_t budgets[] = {1, 100, 4096};
    for (size_t budget : budgets) {
        ha::EntityCatalogBuilder b(text.data(), text.size(), kId);
        size_t steps = 0, before = 0;
        while (b.status() == Status::Running) {
            b.step(budget);
            ++steps;
            const size_t used = b.bytesProcessed() - before;
            before = b.bytesProcessed();
            TEST_ASSERT_TRUE_MESSAGE(used <= budget + unitBound, "step went beyond budget + one element");
            TEST_ASSERT_TRUE(steps < 100000);
        }
        TEST_ASSERT_EQUAL(Status::Done, b.status());
        if (budget == 1) TEST_ASSERT_TRUE(steps > kFixtureTotal);  // roughly one element per step
        if (budget == 4096) TEST_ASSERT_TRUE(steps > 1);
    }
    // budget 0 behaves like 1 (progress)
    ha::EntityCatalogBuilder z(text.data(), text.size(), kId);
    TEST_ASSERT_EQUAL(Status::Done, runAll(z, 0));
}

static void test_step_after_done_is_stable() {
    const std::string text = fixtureText();
    ha::EntityCatalogBuilder b(text.data(), text.size(), kId);
    runAll(b, (size_t)-1);
    const std::string j = toJson(b.entries());
    TEST_ASSERT_EQUAL(Status::Done, b.step(100));
    TEST_ASSERT_TRUE(j == toJson(b.entries()));
    ha::EntityCatalog taken = b.take();
    TEST_ASSERT_EQUAL_UINT32(kFixtureSupported, (uint32_t)taken.size());
    TEST_ASSERT_TRUE(j == toJson(taken));
}

// ---------------- envelope errors ----------------

static std::string failOf(const std::string &text, size_t budget = (size_t)-1, uint32_t id = kId) {
    ha::EntityCatalogBuilder b(text.data(), text.size(), id);
    TEST_ASSERT_EQUAL(Status::Failed, runAll(b, budget));
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)b.entries().size());  // Failed clears the list
    TEST_ASSERT_EQUAL(Status::Failed, b.step(10));
    return b.error();
}

static void test_success_false() {
    TEST_ASSERT_EQUAL_STRING("states_failed: boom",
        failOf("{\"id\":7,\"type\":\"result\",\"success\":false,\"error\":{\"code\":\"x\",\"message\":\"boom\"}}").c_str());
    // error before success
    TEST_ASSERT_EQUAL_STRING("states_failed: boom",
        failOf("{\"id\":7,\"type\":\"result\",\"error\":{\"code\":\"x\",\"message\":\"boom\"},\"success\":false}").c_str());
    // without a message
    TEST_ASSERT_EQUAL_STRING("states_failed",
        failOf("{\"id\":7,\"type\":\"result\",\"success\":false,\"error\":{\"code\":\"x\"}}").c_str());
    TEST_ASSERT_EQUAL_STRING("states_failed", failOf("{\"id\":7,\"type\":\"result\",\"success\":false}").c_str());
    // with a result array present too
    TEST_ASSERT_EQUAL_STRING("states_failed: m",
        failOf("{\"id\":7,\"type\":\"result\",\"success\":false,\"result\":[],\"error\":{\"message\":\"m\"}}").c_str());
    // every budget
    TEST_ASSERT_EQUAL_STRING("states_failed: boom",
        failOf("{\"id\":7,\"type\":\"result\",\"success\":false,\"error\":{\"code\":\"x\",\"message\":\"boom\"}}", 1).c_str());
}

static void test_success_false_huge_message() {
    const std::string text = "{\"id\":7,\"type\":\"result\",\"success\":false,\"error\":{\"code\":\"x\",\"message\":\"" +
                             std::string(70000, 'm') + "\"}}";
    TEST_ASSERT_EQUAL_STRING("states_failed", failOf(text).c_str());
    TEST_ASSERT_EQUAL_STRING("states_failed", failOf(text, 1000).c_str());
}

static void test_id_mismatch_and_type() {
    TEST_ASSERT_EQUAL_STRING("states_unexpected", failOf(envelope("[]", 8)).c_str());
    TEST_ASSERT_EQUAL_STRING("states_unexpected", failOf(envelope("[]", 7), (size_t)-1, 9).c_str());
    // mismatch with result first
    TEST_ASSERT_EQUAL_STRING("states_unexpected",
        failOf("{\"result\":[],\"success\":true,\"type\":\"result\",\"id\":8}").c_str());
    // wrong type / missing members / non-array result
    TEST_ASSERT_EQUAL_STRING("states_unexpected", failOf("{\"id\":7,\"type\":\"event\",\"success\":true,\"result\":[]}").c_str());
    TEST_ASSERT_EQUAL_STRING("states_unexpected", failOf("{\"id\":7,\"success\":true,\"result\":[]}").c_str());
    TEST_ASSERT_EQUAL_STRING("states_unexpected", failOf("{\"type\":\"result\",\"success\":true,\"result\":[]}").c_str());
    TEST_ASSERT_EQUAL_STRING("states_unexpected", failOf("{\"id\":7,\"type\":\"result\",\"result\":[]}").c_str());
    TEST_ASSERT_EQUAL_STRING("states_unexpected", failOf("{\"id\":7,\"type\":\"result\",\"success\":true}").c_str());
    TEST_ASSERT_EQUAL_STRING("states_unexpected", failOf("{\"id\":7,\"type\":\"result\",\"success\":true,\"result\":{}}").c_str());
    TEST_ASSERT_EQUAL_STRING("states_unexpected", failOf("{\"id\":7,\"type\":\"result\",\"success\":true,\"result\":null}").c_str());
    TEST_ASSERT_EQUAL_STRING("states_unexpected", failOf("{\"id\":\"7\",\"type\":\"result\",\"success\":true,\"result\":[]}").c_str());
}

static void test_malformed_texts() {
    const char *bad[] = {
        "", "garbage", "{", "[]", "[1,2]", "null", "42",
        "{\"id\":7,\"type\":\"result\",\"success\":true,\"result\":[}",
        "{\"id\":7,\"type\":\"result\",\"success\":true,\"result\":[1,]}x",
        "{\"id\":7,\"type\":\"result\",\"success\":true,\"result\":[]} trailing",
        "{\"id\":7,\"type\":\"result\",\"success\":true,\"result\":[]}}",
        "{\"id\":7,\"type\":\"result\",\"success\":true,\"result\":[{\"a\":1}",
        "{\"id\":7 \"type\":\"result\"}",
        "{\"id\":7,\"type\":\"result\",\"success\":true,\"result\":[{\"entity_id\":\"light.x\"} {\"a\":1}]}",
    };
    for (const char *s : bad) {
        const std::string t = s;
        ha::EntityCatalogBuilder b(t.data(), t.size(), kId);
        TEST_ASSERT_EQUAL_MESSAGE(Status::Failed, runAll(b, 5), s);
        TEST_ASSERT_EQUAL_STRING_MESSAGE("states_unexpected", b.error().c_str(), s);
    }
}

static void test_truncation_sweep() {
    const std::string full = fixtureText();
    size_t count = 0;
    for (size_t cut = 0; cut < full.size(); cut += 97) {
        const std::string t = full.substr(0, cut);  // exact-size heap copy
        for (size_t budget : {(size_t)1, (size_t)4096}) {
            ha::EntityCatalogBuilder b(t.data(), t.size(), kId);
            TEST_ASSERT_EQUAL_MESSAGE(Status::Failed, runAll(b, budget), "truncated text must fail");
            TEST_ASSERT_EQUAL_STRING("states_unexpected", b.error().c_str());
            TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)b.entries().size());
            TEST_ASSERT_TRUE(b.bytesProcessed() <= t.size());
        }
        ++count;
    }
    TEST_ASSERT_TRUE(count > 50);
    // and every cut at every byte near the end of a small text
    const std::string small = envelope(joinArray({lightEntity("light.a"), lightEntity("light.b")}));
    for (size_t cut = 0; cut < small.size(); ++cut) {
        const std::string t = small.substr(0, cut);
        ha::EntityCatalogBuilder b(t.data(), t.size(), kId);
        TEST_ASSERT_EQUAL(Status::Failed, runAll(b, 3));
    }
}

// ---------------- per-element handling ----------------

static void test_escapes_and_brackets_in_strings() {
    const std::string text = envelope(joinArray({
        "{\"entity_id\":\"light.a\",\"state\":\"on\",\"attributes\":{\"friendly_name\":\"q\\\" ] } [ { \\\\\",\"supported_color_modes\":[\"onoff\"]}}",
        lightEntity("light.b", "B"),
        "{\"entity_id\":\"light.c\",\"state\":\"}]\",\"attributes\":{\"friendly_name\":\"\\\\\"}}",
        lightEntity("light.d", "D"),
    }));
    for (size_t budget : {(size_t)1, (size_t)100, (size_t)-1}) {
        ha::EntityCatalogBuilder b(text.data(), text.size(), kId);
        TEST_ASSERT_EQUAL(Status::Done, runAll(b, budget));
        TEST_ASSERT_EQUAL_UINT32(4, (uint32_t)b.entries().size());
        TEST_ASSERT_EQUAL_UINT32(0, b.skipped());
        TEST_ASSERT_TRUE(b.entries().entry(0).name == "q\" ] } [ { \\");
        TEST_ASSERT_TRUE(b.entries().entry(2).state == "}]");
        TEST_ASSERT_TRUE(b.entries().entry(2).name == "\\");
        TEST_ASSERT_TRUE(b.entries().entry(3).id == "light.d");
    }
}

static void test_nesting_25_skipped_others_kept() {
    const std::string deep = "{\"entity_id\":\"light.deep\",\"state\":\"on\",\"attributes\":{\"foo\":" +
                             std::string(25, '[') + std::string(25, ']') + "}}";
    const std::string text = envelope(joinArray({lightEntity("light.a"), deep, lightEntity("light.b")}));
    ha::EntityCatalogBuilder b(text.data(), text.size(), kId);
    TEST_ASSERT_EQUAL(Status::Done, runAll(b, 50));
    TEST_ASSERT_EQUAL_UINT32(3, b.total());
    TEST_ASSERT_EQUAL_UINT32(1, b.skipped());
    TEST_ASSERT_EQUAL_UINT32(2, (uint32_t)b.entries().size());
    TEST_ASSERT_TRUE(b.entries().entry(0).id == "light.a");
    TEST_ASSERT_TRUE(b.entries().entry(1).id == "light.b");
}

static void test_nesting_moderate_kept() {
    const std::string ok = "{\"entity_id\":\"light.ok\",\"state\":\"on\",\"attributes\":{\"foo\":" +
                           std::string(15, '[') + std::string(15, ']') + "}}";
    const std::string text = envelope(joinArray({ok}));
    ha::EntityCatalogBuilder b(text.data(), text.size(), kId);
    TEST_ASSERT_EQUAL(Status::Done, runAll(b, 50));
    TEST_ASSERT_EQUAL_UINT32(0, b.skipped());
    TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)b.entries().size());  // no modes: listed with "none" only
}

static void test_oversized_element_skipped() {
    const std::string big = "{\"entity_id\":\"light.big\",\"state\":\"on\",\"attributes\":{\"foo\":\"" +
                            std::string(300u * 1024u, 'z') + "\"}}";
    const std::string text = envelope(joinArray({lightEntity("light.a"), big, lightEntity("light.b")}));
    for (size_t budget : {(size_t)4096, (size_t)-1}) {
        ha::EntityCatalogBuilder b(text.data(), text.size(), kId);
        TEST_ASSERT_EQUAL(Status::Done, runAll(b, budget));
        TEST_ASSERT_EQUAL_UINT32(3, b.total());
        TEST_ASSERT_EQUAL_UINT32(1, b.skipped());
        TEST_ASSERT_EQUAL_UINT32(2, (uint32_t)b.entries().size());
        TEST_ASSERT_TRUE(b.entries().entry(1).id == "light.b");
    }
}

static void test_huge_friendly_name_skips_only_that_element() {
    const std::string huge = "{\"entity_id\":\"light.huge\",\"state\":\"on\",\"attributes\":{\"friendly_name\":\"" +
                             std::string(70000, 'n') + "\"}}";
    const std::string text = envelope(joinArray({huge, lightEntity("light.b", "B")}));
    for (size_t budget : {(size_t)1, (size_t)-1}) {
        ha::EntityCatalogBuilder b(text.data(), text.size(), kId);
        TEST_ASSERT_EQUAL(Status::Done, runAll(b, budget));
        TEST_ASSERT_EQUAL_UINT32(2, b.total());
        TEST_ASSERT_EQUAL_UINT32(1, b.skipped());
        TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)b.entries().size());
        TEST_ASSERT_TRUE(b.entries().entry(0).id == "light.b");
        TEST_ASSERT_TRUE(b.entries().entry(0).name == "B");
    }
}

static void test_huge_state_skips_only_that_element() {
    const std::string huge = "{\"entity_id\":\"light.huge\",\"state\":\"" + std::string(70000, 's') +
                             "\",\"attributes\":{\"friendly_name\":\"H\"}}";
    const std::string text = envelope(joinArray({huge, lightEntity("light.b", "B")}));
    for (size_t budget : {(size_t)1, (size_t)-1}) {
        ha::EntityCatalogBuilder b(text.data(), text.size(), kId);
        TEST_ASSERT_EQUAL(Status::Done, runAll(b, budget));
        TEST_ASSERT_EQUAL_UINT32(2, b.total());
        TEST_ASSERT_EQUAL_UINT32(1, b.skipped());
        TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)b.entries().size());
        TEST_ASSERT_TRUE(b.entries().entry(0).id == "light.b");
    }
}

static void test_unneeded_big_string_not_a_problem() {
    // filtered-out strings are never stored, so they hit no limit
    const std::string e = "{\"entity_id\":\"light.p\",\"state\":\"on\",\"attributes\":{\"entity_picture\":\"" +
                          std::string(70000, 'p') + "\",\"friendly_name\":\"P\"}}";
    const std::string text = envelope(joinArray({e, lightEntity("light.b")}));
    ha::EntityCatalogBuilder b(text.data(), text.size(), kId);
    TEST_ASSERT_EQUAL(Status::Done, runAll(b, 1000));
    TEST_ASSERT_EQUAL_UINT32(0, b.skipped());
    TEST_ASSERT_EQUAL_UINT32(2, (uint32_t)b.entries().size());
    TEST_ASSERT_TRUE(b.entries().entry(0).name == "P");
}

static void test_bad_elements_skipped() {
    const std::string text = envelope(joinArray({
        lightEntity("light.a"),
        "5",                                                     // not an object
        "\"str\"",
        "null",
        "{\"state\":\"on\"}",                                    // no entity_id
        "{\"entity_id\":5,\"state\":\"on\"}",                    // entity_id not a string
        "{\"entity_id\":\"\",\"state\":\"on\"}",                 // empty entity_id
        "{\"entity_id\":\"light.broken\",\"state\":}",           // not valid JSON
        "{\"entity_id\":\"light.x\" \"state\":\"on\"}",          // missing comma
        lightEntity("light.b"),
    }));
    ha::EntityCatalogBuilder b(text.data(), text.size(), kId);
    TEST_ASSERT_EQUAL(Status::Done, runAll(b, 7));
    TEST_ASSERT_EQUAL_UINT32(10, b.total());
    TEST_ASSERT_EQUAL_UINT32(8, b.skipped());
    TEST_ASSERT_EQUAL_UINT32(2, (uint32_t)b.entries().size());
    TEST_ASSERT_TRUE(b.entries().entry(0).id == "light.a");
    TEST_ASSERT_TRUE(b.entries().entry(1).id == "light.b");
}

static void test_missing_state_and_attributes() {
    const std::string text = envelope(joinArray({
        "{\"entity_id\":\"switch.s\"}",
        "{\"entity_id\":\"switch.t\",\"state\":\"on\",\"attributes\":null}",
        "{\"entity_id\":\"nodot\",\"state\":\"on\"}",
    }));
    ha::EntityCatalogBuilder b(text.data(), text.size(), kId);
    TEST_ASSERT_EQUAL(Status::Done, runAll(b, 100));
    TEST_ASSERT_EQUAL_UINT32(3, b.total());
    TEST_ASSERT_EQUAL_UINT32(2, (uint32_t)b.entries().size());
    TEST_ASSERT_TRUE(b.entries().entry(0).state == "");
    TEST_ASSERT_TRUE(b.entries().entry(0).name == "switch.s");
    TEST_ASSERT_TRUE(b.entries().entry(1).state == "on");
}

static void test_empty_result() {
    const std::string text = envelope("[]");
    ha::EntityCatalogBuilder b(text.data(), text.size(), kId);
    TEST_ASSERT_EQUAL(Status::Done, runAll(b, 1));
    TEST_ASSERT_EQUAL_UINT32(0, b.total());
    TEST_ASSERT_TRUE(b.entries().empty());
    TEST_ASSERT_EQUAL_STRING(
        "{\"fetched_at_s\":0,\"total\":0,\"skipped\":0,\"truncated\":false,\"entities\":[]}",
        toJson(b.entries()).c_str());
}

static void test_name_and_state_utf8_cuts() {
    const std::string oe = "\xc3\xb8";
    const std::string name97 = std::string(95, 'a') + oe;         // o-slash would straddle byte 96
    const std::string name96 = std::string(94, 'a') + oe;         // exactly 96 bytes
    const std::string state256 = std::string(254, 's') + oe;      // straddles 255
    const std::string state255 = std::string(253, 's') + oe;      // exactly 255
    const std::string text = envelope(joinArray({
        lightEntity("light.n97", name97, "on"),
        lightEntity("light.n96", name96, "on"),
        lightEntity("light.long", std::string(200, 'x'), state256),
        lightEntity("light.s255", "S", state255),
    }));
    ha::EntityCatalogBuilder b(text.data(), text.size(), kId);
    TEST_ASSERT_EQUAL(Status::Done, runAll(b, (size_t)-1));
    const ha::EntityCatalog &c = b.entries();
    TEST_ASSERT_EQUAL_UINT32(95, (uint32_t)findEntry(c, "light.n97").name.size());
    TEST_ASSERT_EQUAL_UINT32(96, (uint32_t)findEntry(c, "light.n96").name.size());
    TEST_ASSERT_EQUAL_UINT32(96, (uint32_t)findEntry(c, "light.long").name.size());
    TEST_ASSERT_EQUAL_UINT32(254, (uint32_t)findEntry(c, "light.long").state.size());
    TEST_ASSERT_EQUAL_UINT32(255, (uint32_t)findEntry(c, "light.s255").state.size());
    // every kept string is still valid UTF-8 JSON
    JsonDocument d;
    TEST_ASSERT_TRUE(!deserializeJson(d, toJson(c)));
    // a long entity id name fallback is cut too
    const std::string longId = "light." + std::string(150, 'i');
    const std::string t2 = envelope(joinArray({"{\"entity_id\":\"" + longId + "\",\"state\":\"on\"}"}));
    ha::EntityCatalogBuilder b2(t2.data(), t2.size(), kId);
    TEST_ASSERT_EQUAL(Status::Done, runAll(b2, (size_t)-1));
    TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)b2.entries().size());
    TEST_ASSERT_EQUAL_UINT32(96, (uint32_t)b2.entries().entry(0).name.size());
}

// ---------------- cap ----------------

static std::string manyLights(int n) {
    std::string arr = "[";
    for (int i = 0; i < n; ++i) {
        if (i) arr += ",";
        arr += "{\"entity_id\":\"light.l" + std::to_string(i) + "\",\"state\":\"on\",\"attributes\":{}}";
    }
    return envelope(arr + "]");
}

static void test_cap_1500() {
    const std::string at = manyLights(1500);
    ha::EntityCatalogBuilder a(at.data(), at.size(), kId);
    TEST_ASSERT_EQUAL(Status::Done, runAll(a, 32768));
    TEST_ASSERT_EQUAL_UINT32(1500, (uint32_t)a.entries().size());
    TEST_ASSERT_FALSE(a.truncated());
    TEST_ASSERT_EQUAL_UINT32(1500, a.total());

    const std::string over = manyLights(1600);
    ha::EntityCatalogBuilder b(over.data(), over.size(), kId);
    TEST_ASSERT_EQUAL(Status::Done, runAll(b, 32768));
    TEST_ASSERT_EQUAL_UINT32(1500, (uint32_t)b.entries().size());
    TEST_ASSERT_TRUE(b.truncated());
    TEST_ASSERT_EQUAL_UINT32(1600, b.total());
    TEST_ASSERT_EQUAL_UINT32(0, b.skipped());
    TEST_ASSERT_TRUE(b.entries().entry(1499).id == "light.l1499");
    // JSON carries truncated:true and total
    const std::string j = toJson(b.entries());
    TEST_ASSERT_TRUE(j.find("\"truncated\":true") != std::string::npos);
    TEST_ASSERT_TRUE(j.find("\"total\":1600") != std::string::npos);
    JsonDocument d;
    TEST_ASSERT_TRUE(!deserializeJson(d, j));
    TEST_ASSERT_EQUAL_UINT32(1500, (uint32_t)d["entities"].size());
}

static void test_cap_counting_continues() {
    // unsupported entities after the cap do not set truncated; broken ones are still counted
    std::string body = manyLights(1500);
    body.resize(body.size() - 2);  // drop "]}"
    body += ",{\"entity_id\":\"sensor.z\",\"state\":\"1\"},5,{\"entity_id\":\"light.over\",\"state\":\"on\"}]}";
    ha::EntityCatalogBuilder b(body.data(), body.size(), kId);
    TEST_ASSERT_EQUAL(Status::Done, runAll(b, 4096));
    TEST_ASSERT_EQUAL_UINT32(1500, (uint32_t)b.entries().size());
    TEST_ASSERT_EQUAL_UINT32(1503, b.total());
    TEST_ASSERT_EQUAL_UINT32(1, b.skipped());
    TEST_ASSERT_TRUE(b.truncated());  // light.over did not fit

    std::string body2 = manyLights(1500);
    body2.resize(body2.size() - 2);
    body2 += ",{\"entity_id\":\"sensor.z\",\"state\":\"1\"}]}";
    ha::EntityCatalogBuilder c(body2.data(), body2.size(), kId);
    TEST_ASSERT_EQUAL(Status::Done, runAll(c, 4096));
    TEST_ASSERT_FALSE(c.truncated());
    TEST_ASSERT_EQUAL_UINT32(1501, c.total());
}

// ---------------- catalogToJson ----------------

static void test_json_exact_body() {
    const std::string text = envelope(joinArray({
        "{\"entity_id\":\"light.k\",\"state\":\"on\",\"attributes\":{\"friendly_name\":\"K\xc3\xb8kken\",\"supported_color_modes\":[\"color_temp\"]}}",
        "{\"entity_id\":\"switch.s\",\"state\":\"off\",\"attributes\":{\"friendly_name\":\"S\"}}",
        "{\"entity_id\":\"climate.c\",\"state\":\"heat\",\"attributes\":{\"supported_features\":1,\"temperature\":null,\"target_temp_step\":0.5}}",
        "{\"entity_id\":\"sensor.x\",\"state\":\"1\"}",
        "5",
    }));
    ha::EntityCatalogBuilder b(text.data(), text.size(), kId);
    TEST_ASSERT_EQUAL(Status::Done, runAll(b, 100));
    const std::string j = ha::catalogToJson(b.entries(), 12);
    const char *expected =
        "{\"fetched_at_s\":12,\"total\":5,\"skipped\":1,\"truncated\":false,\"entities\":["
        "{\"id\":\"light.k\",\"name\":\"K\xc3\xb8kken\",\"state\":\"on\",\"actions\":[\"brightness\",\"color_temp\",\"none\"],\"step_default\":5},"
        "{\"id\":\"switch.s\",\"name\":\"S\",\"state\":\"off\",\"actions\":[\"none\"],\"step_default\":0},"
        "{\"id\":\"climate.c\",\"name\":\"climate.c\",\"state\":\"heat\",\"actions\":[\"target_temp\",\"none\"],\"step_default\":0.5}"
        "]}";
    TEST_ASSERT_EQUAL_STRING(expected, j.c_str());
}

static void test_json_escapes_and_parses() {
    const std::string j = buildJson(fixtureText());
    JsonDocument d;
    TEST_ASSERT_TRUE(!deserializeJson(d, j));
    TEST_ASSERT_EQUAL_UINT32(kFixtureTotal, d["total"].as<uint32_t>());
    TEST_ASSERT_EQUAL_UINT32(kFixtureSupported, (uint32_t)d["entities"].size());
    TEST_ASSERT_EQUAL_STRING("Switch \"Q\" [1]", d["entities"][14]["name"].as<const char *>());
    TEST_ASSERT_EQUAL_STRING("switch.sw", d["entities"][14]["id"].as<const char *>());
    // default action first, step_default belongs to it
    TEST_ASSERT_EQUAL_STRING("brightness", d["entities"][1]["actions"][0].as<const char *>());
    TEST_ASSERT_EQUAL_INT(5, d["entities"][1]["step_default"].as<int>());
    // an empty catalog object round-trips too
    ha::EntityCatalog empty;
    TEST_ASSERT_TRUE(!deserializeJson(d, ha::catalogToJson(empty, 3)));
    TEST_ASSERT_EQUAL_UINT32(3, d["fetched_at_s"].as<uint32_t>());
}

// ---------------- CatalogJsonWriter (Step 6.5 R8) ----------------

static std::string writerJson(const ha::EntityCatalog &cat, size_t slice, size_t *regrow = nullptr,
                              size_t *capacityMinusSize = nullptr, size_t *steps = nullptr) {
    ha::CatalogJsonWriter w(cat, 77);
    size_t n = 0;
    while (!w.step(slice)) {
        ++n;
        TEST_ASSERT_TRUE(n < 100000);
    }
    if (steps) *steps = n + 1;
    TEST_ASSERT_TRUE(w.done());
    if (regrow) *regrow = w.regrowths();
    std::string out = w.take();
    (void)capacityMinusSize;
    return out;
}

static ha::EntityCatalog catalogOf(const std::string &text) {
    ha::EntityCatalogBuilder b(text.data(), text.size(), kId);
    TEST_ASSERT_EQUAL(Status::Done, runAll(b, 100000));
    return b.take();
}

static void test_writer_identical_for_all_slices() {
    const std::string text = fixtureText();
    const ha::EntityCatalog cat = catalogOf(text);
    const std::string ref = ha::catalogToJson(cat, 77);
    TEST_ASSERT_TRUE(ref.size() > 100);
    const size_t slices[] = {0, 1, 3, 8, SIZE_MAX};
    for (size_t sl : slices) {
        TEST_ASSERT_EQUAL_STRING(ref.c_str(), writerJson(cat, sl).c_str());
    }
    JsonDocument d;
    TEST_ASSERT_TRUE(!deserializeJson(d, ref));
    TEST_ASSERT_EQUAL_UINT32(77, d["fetched_at_s"].as<uint32_t>());
}

static void test_writer_step0_is_1() {
    const ha::EntityCatalog cat = catalogOf(fixtureText());
    ha::CatalogJsonWriter w(cat, 1);
    TEST_ASSERT_FALSE(w.step(0));
    TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)w.entriesWritten());
    TEST_ASSERT_FALSE(w.step(0));
    TEST_ASSERT_EQUAL_UINT32(2, (uint32_t)w.entriesWritten());
    size_t steps = 0;
    writerJson(cat, 0, nullptr, nullptr, &steps);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)cat.size(), (uint32_t)steps);  // one entry per step
}

static void test_writer_progress_and_done_idempotent() {
    const ha::EntityCatalog cat = catalogOf(fixtureText());
    ha::CatalogJsonWriter w(cat, 1);
    TEST_ASSERT_TRUE(w.step(SIZE_MAX));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)cat.size(), (uint32_t)w.entriesWritten());
    const size_t cap = w.capacity();
    TEST_ASSERT_TRUE(w.step(5));  // after done: does nothing
    const std::string once = w.take();
    TEST_ASSERT_EQUAL_STRING(ha::catalogToJson(cat, 1).c_str(), once.c_str());
    TEST_ASSERT_TRUE(cap >= once.size());
}

static void test_writer_empty_catalog() {
    ha::EntityCatalog empty;
    for (size_t sl : {size_t(0), size_t(1), size_t(8), SIZE_MAX}) {
        ha::CatalogJsonWriter w(empty, 3);
        TEST_ASSERT_TRUE(w.step(sl));  // the first step also closes an empty catalog
        TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)w.regrowths());
        const std::string j = w.take();
        TEST_ASSERT_EQUAL_STRING(ha::catalogToJson(empty, 3).c_str(), j.c_str());
        JsonDocument d;
        TEST_ASSERT_TRUE(!deserializeJson(d, j));
        TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)d["entities"].size());
    }
}

static void test_writer_escape_heavy_names_and_long_steps() {
    std::vector<std::string> els;
    for (int i = 0; i < 60; ++i) {
        // 90 escaped quotes/backslashes/control chars per name (name cap is 96 bytes)
        std::string nm;
        for (int k = 0; k < 20; ++k) nm += "\\\\\\\"\\u0001\\n";
        els.push_back("{\"entity_id\":\"climate.c" + std::to_string(i) +
                      "\",\"state\":\"heat\",\"attributes\":{\"friendly_name\":\"" + nm +
                      "\",\"supported_features\":1,\"temperature\":20,\"target_temp_step\":0.5}}");
    }
    const ha::EntityCatalog cat = catalogOf(envelope(joinArray(els)));
    TEST_ASSERT_EQUAL_UINT32(60, (uint32_t)cat.size());
    const std::string ref = ha::catalogToJson(cat, 77);
    JsonDocument d;
    TEST_ASSERT_TRUE(!deserializeJson(d, ref));
    TEST_ASSERT_EQUAL_UINT32(60, (uint32_t)d["entities"].size());
    const size_t slices[] = {0, 1, 3, 8, SIZE_MAX};
    for (size_t sl : slices) {
        size_t regrow = 99;
        const std::string j = writerJson(cat, sl, &regrow);
        if (j != ref) {
            size_t k = 0;
            while (k < j.size() && k < ref.size() && j[k] == ref[k]) ++k;
            printf("    MISMATCH slice=%zu sizes %zu vs %zu first diff at %zu: [%s] vs [%s]\n", sl, j.size(),
                   ref.size(), k, j.substr(k > 20 ? k - 20 : 0, 60).c_str(),
                   ref.substr(k > 20 ? k - 20 : 0, 60).c_str());
        }
        TEST_ASSERT_TRUE(j == ref);
        printf("    escape-heavy slice=%zu bytes=%zu regrowths=%zu\n", sl, j.size(), regrow);
        TEST_ASSERT_TRUE(regrow < 60);  // grows exactly, never once per entry
    }
}

static void test_writer_capacity_no_doubling() {
    std::vector<std::string> els;
    for (int i = 0; i < 400; ++i) {
        els.push_back("{\"entity_id\":\"light.l" + std::to_string(i) +
                      "\",\"state\":\"on\",\"attributes\":{\"friendly_name\":\"Lamp number " +
                      std::to_string(i) + "\",\"supported_color_modes\":[\"hs\"]}}");
    }
    const ha::EntityCatalog cat = catalogOf(envelope(joinArray(els)));
    for (size_t sl : {size_t(1), size_t(8), SIZE_MAX}) {
        ha::CatalogJsonWriter w(cat, 9);
        while (!w.step(sl)) {
        }
        const size_t size = w.take().size();
        (void)size;
    }
    ha::CatalogJsonWriter w(cat, 9);
    while (!w.step(8)) {
    }
    const size_t cap = w.capacity();
    const std::string out = w.take();
    TEST_ASSERT_TRUE(cap >= out.size());
    TEST_ASSERT_TRUE(cap - out.size() < out.size() / 2);  // far from a doubling (>= 100 % slack)
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)w.regrowths());
}

// ---------------- 1 MB synthetic ----------------

static void test_one_megabyte_synthetic() {
    std::string arr = "[";
    int n = 0;
    while (arr.size() < 1024u * 1024u) {
        if (n) arr += ",";
        arr += "{\"entity_id\":\"light.syn" + std::to_string(n) +
               "\",\"state\":\"on\",\"attributes\":{\"supported_color_modes\":[\"hs\",\"color_temp\"],"
               "\"brightness\":200,\"hs_color\":[10.5,50.5],\"friendly_name\":\"Synthetic light " +
               std::to_string(n) + "\",\"entity_picture\":\"/local/some/picture/path.png\","
               "\"effect_list\":[\"a\",\"b\",\"c\",\"d\"],\"icon\":\"mdi:lamp\"},"
               "\"context\":{\"id\":\"01HXXXXXXXXXXXXXXXXXXXXXXX\",\"parent_id\":null,\"user_id\":null},"
               "\"last_changed\":\"2026-10-01T10:00:00.123456+00:00\",\"last_updated\":\"2026-10-01T10:00:00.123456+00:00\"}";
        ++n;
    }
    arr += "]";
    const std::string text = envelope(arr);
    TEST_ASSERT_TRUE(text.size() >= 1024u * 1024u);
    const auto t0 = std::chrono::steady_clock::now();
    ha::EntityCatalogBuilder b(text.data(), text.size(), kId);
    size_t steps = 0;
    while (b.step(32768) == Status::Running) ++steps;
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    std::printf("[1 MB synthetic] %u bytes, %d entities, %u steps (32 KB), host time %lld ms\n",
                (unsigned)text.size(), n, (unsigned)steps, (long long)ms);
    TEST_ASSERT_EQUAL(Status::Done, b.status());
    TEST_ASSERT_EQUAL_UINT32((uint32_t)n, b.total());
    TEST_ASSERT_EQUAL_UINT32(0, b.skipped());
    TEST_ASSERT_EQUAL_UINT32(n > 1500 ? 1500u : (uint32_t)n, (uint32_t)b.entries().size());
    TEST_ASSERT_EQUAL_UINT32(n > 1500, b.truncated() ? 1u : 0u);
    TEST_ASSERT_TRUE(steps > 10);
}

// ---------------- Step 6.5c: element limit per step ----------------

// Mixed list: supported, unsupported, skipped, non-object and bad elements.
static std::string mixedElements(int n) {
    std::vector<std::string> items;
    for (int i = 0; i < n; ++i) {
        switch (i % 6) {
            case 0: items.push_back(lightEntity("light.m" + std::to_string(i), "L" + std::to_string(i))); break;
            case 1: items.push_back("{\"entity_id\":\"sensor.m" + std::to_string(i) + "\",\"state\":\"1\"}"); break;  // unsupported
            case 2: items.push_back("5"); break;                                  // non-object
            case 3: items.push_back("{\"state\":\"on\"}"); break;                // no entity_id: skipped
            case 4: items.push_back("{\"entity_id\":\"switch.m" + std::to_string(i) + "\",\"state\":\"on\"}"); break;
            default: items.push_back("{\"entity_id\":\"light.bad\",\"state\":}"); break;  // invalid JSON
        }
    }
    return envelope(joinArray(items));
}

static void test_element_limit_all_combinations_identical() {
    const std::string text = mixedElements(60);
    const std::string reference = buildJson(text);
    const size_t elementLimits[] = {0, 1, 2, 3, 7, 16, 59, 60, 61, 1000, (size_t)-1};
    const size_t budgets[] = {0, 1, 50, 300, 4096, (size_t)-1};
    for (size_t maxEl : elementLimits) {
        for (size_t budget : budgets) {
            ha::EntityCatalogBuilder b(text.data(), text.size(), kId);
            const size_t limit = maxEl == 0 ? 1 : maxEl;
            size_t steps = 0;
            uint32_t prevTotal = 0;
            size_t prevBytes = 0;
            Status st = Status::Running;
            while (st == Status::Running) {
                st = b.step(budget, maxEl);
                ++steps;
                TEST_ASSERT_TRUE_MESSAGE(b.total() - prevTotal <= limit, "step exceeded maxElements");
                if (st == Status::Running) {  // the final (Done) step may only finish
                    TEST_ASSERT_TRUE_MESSAGE(b.bytesProcessed() > prevBytes, "step made no progress");
                }
                prevTotal = b.total();
                prevBytes = b.bytesProcessed();
                TEST_ASSERT_TRUE(steps < 100000);
            }
            TEST_ASSERT_EQUAL(Status::Done, st);
            TEST_ASSERT_EQUAL_UINT32(60, b.total());
            TEST_ASSERT_EQUAL_UINT32(30, b.skipped());  // cases 2, 3, 5 of every 6 (unsupported are not skipped)
            TEST_ASSERT_EQUAL_UINT32(20, (uint32_t)b.entries().size());  // cases 0 and 4
            TEST_ASSERT_TRUE_MESSAGE(reference == toJson(b.entries()), "output depends on the element limit");
        }
    }
}

static void test_element_limit_exact_step_count() {
    std::vector<std::string> items;
    for (int i = 0; i < 20; ++i) items.push_back(lightEntity("light.e" + std::to_string(i)));
    const std::string text = envelope(joinArray(items));
    // byte budget unlimited: only the element limit ends a Running step
    ha::EntityCatalogBuilder b(text.data(), text.size(), kId);
    uint32_t k = 0;
    while (b.step((size_t)-1, 4) == Status::Running) {
        ++k;
        TEST_ASSERT_EQUAL_UINT32(4 * k, b.total());
    }
    TEST_ASSERT_EQUAL_UINT32(5, k);  // 20 elements: 5 Running steps of 4 (the limit is hit before "]"), the 6th is Done
    TEST_ASSERT_EQUAL_UINT32(20, b.total());
    TEST_ASSERT_EQUAL_UINT32(20, (uint32_t)b.entries().size());
    // maxElements 0 behaves like 1
    ha::EntityCatalogBuilder z(text.data(), text.size(), kId);
    TEST_ASSERT_EQUAL(Status::Running, z.step((size_t)-1, 0));
    TEST_ASSERT_EQUAL_UINT32(1, z.total());
    // default argument = unlimited elements
    ha::EntityCatalogBuilder d(text.data(), text.size(), kId);
    TEST_ASSERT_EQUAL(Status::Done, d.step((size_t)-1));
}

static void test_element_limit_counts_skipped_and_unsupported() {
    // 3 unsupported + 3 skipped/non-object elements, then one light; limit 2
    const std::string text = envelope(joinArray({
        "{\"entity_id\":\"sensor.a\",\"state\":\"1\"}",
        "{\"entity_id\":\"sensor.b\",\"state\":\"1\"}",
        "7",
        "\"s\"",
        "{\"entity_id\":5}",
        "{\"entity_id\":\"sensor.c\",\"state\":\"1\"}",
        lightEntity("light.z"),
    }));
    ha::EntityCatalogBuilder b(text.data(), text.size(), kId);
    TEST_ASSERT_EQUAL(Status::Running, b.step((size_t)-1, 2));
    TEST_ASSERT_EQUAL_UINT32(2, b.total());
    TEST_ASSERT_EQUAL(Status::Running, b.step((size_t)-1, 2));
    TEST_ASSERT_EQUAL_UINT32(4, b.total());
    TEST_ASSERT_EQUAL(Status::Running, b.step((size_t)-1, 2));
    TEST_ASSERT_EQUAL_UINT32(6, b.total());
    TEST_ASSERT_EQUAL(Status::Done, b.step((size_t)-1, 2));
    TEST_ASSERT_EQUAL_UINT32(7, b.total());
    TEST_ASSERT_EQUAL_UINT32(3, b.skipped());  // 7, "s", {"entity_id":5}
    TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)b.entries().size());
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_scan_whitespace);
    RUN_TEST(test_scan_string);
    RUN_TEST(test_scan_skip_value);
    RUN_TEST(test_scan_skip_value_errors);
    RUN_TEST(test_scan_deep_nesting_no_overflow);
    RUN_TEST(test_scan_parse_uint32);
    RUN_TEST(test_scan_equals);
    RUN_TEST(test_peek_ha_order);
    RUN_TEST(test_peek_other_order);
    RUN_TEST(test_peek_short_messages);
    RUN_TEST(test_peek_garbage);
    RUN_TEST(test_peek_limit_bytes);
    RUN_TEST(test_fixture_size_and_counts);
    RUN_TEST(test_fixture_actions_and_steps);
    RUN_TEST(test_fixture_unsupported_absent);
    RUN_TEST(test_fixture_names_states_order);
    RUN_TEST(test_result_first_key_order);
    RUN_TEST(test_slicing_identical_output);
    RUN_TEST(test_slicing_step_bounded);
    RUN_TEST(test_step_after_done_is_stable);
    RUN_TEST(test_success_false);
    RUN_TEST(test_success_false_huge_message);
    RUN_TEST(test_id_mismatch_and_type);
    RUN_TEST(test_malformed_texts);
    RUN_TEST(test_truncation_sweep);
    RUN_TEST(test_escapes_and_brackets_in_strings);
    RUN_TEST(test_nesting_25_skipped_others_kept);
    RUN_TEST(test_nesting_moderate_kept);
    RUN_TEST(test_oversized_element_skipped);
    RUN_TEST(test_huge_friendly_name_skips_only_that_element);
    RUN_TEST(test_huge_state_skips_only_that_element);
    RUN_TEST(test_unneeded_big_string_not_a_problem);
    RUN_TEST(test_bad_elements_skipped);
    RUN_TEST(test_missing_state_and_attributes);
    RUN_TEST(test_empty_result);
    RUN_TEST(test_name_and_state_utf8_cuts);
    RUN_TEST(test_cap_1500);
    RUN_TEST(test_cap_counting_continues);
    RUN_TEST(test_json_exact_body);
    RUN_TEST(test_json_escapes_and_parses);
    RUN_TEST(test_writer_identical_for_all_slices);
    RUN_TEST(test_writer_step0_is_1);
    RUN_TEST(test_writer_progress_and_done_idempotent);
    RUN_TEST(test_writer_empty_catalog);
    RUN_TEST(test_writer_escape_heavy_names_and_long_steps);
    RUN_TEST(test_writer_capacity_no_doubling);
    RUN_TEST(test_one_megabyte_synthetic);
    RUN_TEST(test_element_limit_all_combinations_identical);
    RUN_TEST(test_element_limit_exact_step_count);
    RUN_TEST(test_element_limit_counts_skipped_and_unsupported);
    return UNITY_END();
}
