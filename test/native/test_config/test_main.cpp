// Native tests for the config model and the DomainActions table
// (plan.md §3.4, §3.5, §4 Step 3, plus the approved README "Configuration" extensions).
#include <unity.h>

#include <ArduinoJson.h>

#include <cmath>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "config/ConfigModel.h"
#include "control/DomainActions.h"
#include "util/Base64.h"

namespace cfg = core::config;
namespace da = core::domain_actions;
using Kind = cfg::SecretChange::Kind;

// Unity double support is not enabled in this project; exact compare is intended here.
#define ASSERT_DOUBLE_EQ(expected, actual) TEST_ASSERT_TRUE((expected) == (actual))

void setUp() {}
void tearDown() {}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace {

cfg::LoadResult load(const std::string &json) { return cfg::fromJson(json.data(), json.size()); }

cfg::MergeResult merge(const cfg::Config &current, const std::string &patch) {
    return cfg::mergePatch(current, patch.data(), patch.size());
}

cfg::Control ctl(const char *entity, const char *action, double step, const char *press,
                 const char *label = "") {
    cfg::Control c;
    c.entityId = entity;
    c.label = label;
    c.action = action;
    c.step = step;
    c.press = press;
    return c;
}

// A fully populated valid config (non-default values everywhere).
cfg::Config fullConfig() {
    cfg::Config c;
    c.device.hostname = "kitchen-knob-2";
    c.device.brightness = 55;
    c.device.dimAfterS = 120;
    c.device.dimLevel = 7;
    c.device.offAfterS = 900;
    c.device.buzzer = false;
    c.device.encoderReverse = true;
    c.device.accel = false;
    c.device.sendIntervalMs = 400;
    c.wifi.ssid = "MyNetwork";
    c.ha.url = "http://192.168.1.10:8123";
    c.controls = {
        ctl("light.kitchen", "brightness", 5, "toggle"),
        ctl("light.kitchen", "color_temp", 100, "toggle", "Kitchen colour"),
        ctl("climate.living", "target_temp", 0.5, "none"),
        ctl("cover.blind", "position", 10, "stop"),
        ctl("fan.ceiling", "percentage", 10, "toggle"),
        ctl("media_player.lr", "volume", 2, "play_pause", "K\xC3\xB8kken \xC3\x86""ble \xC3\x98rn"),
        ctl("switch.plug", "none", 0, "toggle"),
        ctl("input_boolean.guest_mode", "none", 0, "none"),
    };
    return c;
}

// A structurally valid (DER shape only) CERTIFICATE block of exactly `total` bytes,
// already in normalised form.
std::string certShapedPem(size_t total) {
    const size_t bodyChars = 3980;  // multiple of 4
    const size_t lines = total - 28 - 26 - bodyChars;
    std::vector<uint8_t> der(bodyChars / 4 * 3, 0);
    const size_t inner = der.size() - 8;  // outer 30 82 hh ll, inner 30 82 hh ll
    der[0] = 0x30; der[1] = 0x82; der[2] = static_cast<uint8_t>((der.size() - 4) >> 8);
    der[3] = static_cast<uint8_t>((der.size() - 4) & 0xFF);
    der[4] = 0x30; der[5] = 0x82; der[6] = static_cast<uint8_t>(inner >> 8);
    der[7] = static_cast<uint8_t>(inner & 0xFF);
    const std::string b64 = core::util::base64Encode(der.data(), der.size());
    std::string out = "-----BEGIN CERTIFICATE-----\n";
    const size_t width = (b64.size() + lines - 1) / lines;
    for (size_t pos = 0; pos < b64.size(); pos += width) out += b64.substr(pos, width) + "\n";
    out += "-----END CERTIFICATE-----\n";
    return out;
}

std::string repeat(const std::string &s, size_t n) {
    std::string out;
    for (size_t i = 0; i < n; ++i) out += s;
    return out;
}

void assertInvalid(const cfg::Config &c, const char *field) {
    const cfg::ValidationResult r = cfg::validate(c);
    TEST_ASSERT_FALSE_MESSAGE(r.ok, field);
    TEST_ASSERT_EQUAL_STRING(field, r.field.c_str());
}

void assertValid(const cfg::Config &c) {
    const cfg::ValidationResult r = cfg::validate(c);
    if (!r.ok) {
        std::string msg = "unexpected failure: " + r.field + " / " + r.message;
        TEST_FAIL_MESSAGE(msg.c_str());
    }
}

void assertMergeFails(const cfg::Config &current, const std::string &patch, const char *field,
                      const char *message) {
    const cfg::MergeResult r = merge(current, patch);
    TEST_ASSERT_FALSE_MESSAGE(r.ok, patch.c_str());
    TEST_ASSERT_EQUAL_STRING_MESSAGE(field, r.error.field.c_str(), patch.c_str());
    if (message != nullptr) {
        TEST_ASSERT_EQUAL_STRING_MESSAGE(message, r.error.message.c_str(), patch.c_str());
    }
    TEST_ASSERT_TRUE_MESSAGE(r.config == current, "failed merge must leave config unchanged");
}

cfg::MergeResult mergeOk(const cfg::Config &current, const std::string &patch) {
    cfg::MergeResult r = merge(current, patch);
    if (!r.ok) {
        std::string msg = "merge failed: " + r.error.field + " / " + r.error.message + " for " + patch;
        TEST_FAIL_MESSAGE(msg.c_str());
    }
    return r;
}

cfg::Config configWithUrl(const char *url) {
    cfg::Config c;
    c.ha.url = url;
    return c;
}

cfg::Config configWithSsid(const char *ssid) {
    cfg::Config c;
    c.wifi.ssid = ssid;
    return c;
}

uint32_t gCounter = 0;
uint32_t counterRng() { return gCounter++; }
uint32_t constRng() { return 0xFFFFFFFFu; }

}  // namespace

// ---------------------------------------------------------------------------
// Defaults, round trip, loading
// ---------------------------------------------------------------------------

static void test_defaults_match_spec() {
    const cfg::Config c;
    TEST_ASSERT_EQUAL_STRING("ha-knob", c.device.hostname.c_str());
    TEST_ASSERT_EQUAL_INT32(80, c.device.brightness);
    TEST_ASSERT_EQUAL_UINT32(30, c.device.dimAfterS);
    TEST_ASSERT_EQUAL_INT32(15, c.device.dimLevel);
    TEST_ASSERT_EQUAL_UINT32(300, c.device.offAfterS);
    TEST_ASSERT_TRUE(c.device.buzzer);
    TEST_ASSERT_FALSE(c.device.encoderReverse);
    TEST_ASSERT_TRUE(c.device.accel);
    TEST_ASSERT_EQUAL_UINT32(250, c.device.sendIntervalMs);
    TEST_ASSERT_TRUE(c.wifi.ssid.empty());
    TEST_ASSERT_TRUE(c.ha.url.empty());
    TEST_ASSERT_EQUAL_size_t(0, c.controls.size());
    assertValid(c);
}

static void test_round_trip_spec_example_string_equal() {
    // plan.md §3.4 example, compacted, in the writer's key order.
    const std::string x =
        "{\"schema\":1,"
        "\"device\":{\"hostname\":\"ha-knob\",\"brightness\":80,\"dim_after_s\":30,\"dim_level\":15,"
        "\"off_after_s\":300,\"buzzer\":true,\"encoder_reverse\":false,\"accel\":true,"
        "\"send_interval_ms\":250},"
        "\"wifi\":{\"ssid\":\"MyNetwork\"},"
        "\"ha\":{\"url\":\"http://192.168.1.10:8123\",\"ca_pem\":\"\",\"tls_insecure\":false},"
        "\"controls\":["
        "{\"entity_id\":\"light.kitchen\",\"label\":\"\",\"action\":\"brightness\",\"step\":5,\"press\":\"toggle\"},"
        "{\"entity_id\":\"light.kitchen\",\"label\":\"Kitchen colour\",\"action\":\"color_temp\",\"step\":100,\"press\":\"toggle\"},"
        "{\"entity_id\":\"climate.living\",\"label\":\"\",\"action\":\"target_temp\",\"step\":0.5,\"press\":\"none\"},"
        "{\"entity_id\":\"cover.blind\",\"label\":\"\",\"action\":\"position\",\"step\":10,\"press\":\"toggle\"},"
        "{\"entity_id\":\"fan.ceiling\",\"label\":\"\",\"action\":\"percentage\",\"step\":10,\"press\":\"toggle\"},"
        "{\"entity_id\":\"media_player.lr\",\"label\":\"\",\"action\":\"volume\",\"step\":2,\"press\":\"play_pause\"},"
        "{\"entity_id\":\"switch.plug\",\"label\":\"\",\"action\":\"none\",\"step\":0,\"press\":\"toggle\"}"
        "]}";
    const cfg::LoadResult r = load(x);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(cfg::LoadStatus::Loaded), static_cast<int>(r.status));
    TEST_ASSERT_EQUAL_size_t(7, r.config.controls.size());
    TEST_ASSERT_EQUAL_STRING(x.c_str(), cfg::toJson(r.config).c_str());
}

static void test_round_trip_full_config_struct_equal() {
    const cfg::Config c = fullConfig();
    assertValid(c);
    const std::string json = cfg::toJson(c);
    const cfg::LoadResult r = load(json);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(cfg::LoadStatus::Loaded), static_cast<int>(r.status));
    TEST_ASSERT_TRUE(r.config == c);
    TEST_ASSERT_EQUAL_STRING(json.c_str(), cfg::toJson(r.config).c_str());
}

static void test_round_trip_json_escapes_in_labels() {
    cfg::Config c;
    c.controls = {ctl("light.a", "brightness", 5, "toggle", "Say \"hi\" \\ / ok")};
    const cfg::LoadResult r = load(cfg::toJson(c));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(cfg::LoadStatus::Loaded), static_cast<int>(r.status));
    TEST_ASSERT_TRUE(r.config == c);
}

static void test_to_json_has_schema_and_no_secrets() {
    const std::string json = cfg::toJson(fullConfig());
    TEST_ASSERT_TRUE(json.find("\"schema\":1") != std::string::npos);
    TEST_ASSERT_TRUE(json.find("password") == std::string::npos);
    TEST_ASSERT_TRUE(json.find("token") == std::string::npos);
    TEST_ASSERT_TRUE(json.find("\"web\"") == std::string::npos);
}

static void test_missing_sections_give_defaults() {
    cfg::LoadResult r = load("{}");
    TEST_ASSERT_EQUAL_INT(static_cast<int>(cfg::LoadStatus::Loaded), static_cast<int>(r.status));
    TEST_ASSERT_TRUE(r.config == cfg::Config());

    r = load("{\"schema\":1}");
    TEST_ASSERT_EQUAL_INT(static_cast<int>(cfg::LoadStatus::Loaded), static_cast<int>(r.status));
    TEST_ASSERT_TRUE(r.config == cfg::Config());

    // Only wifi present: device/ha/controls default.
    r = load("{\"schema\":1,\"wifi\":{\"ssid\":\"Net\"}}");
    TEST_ASSERT_EQUAL_INT(static_cast<int>(cfg::LoadStatus::Loaded), static_cast<int>(r.status));
    cfg::Config expected;
    expected.wifi.ssid = "Net";
    TEST_ASSERT_TRUE(r.config == expected);
}

static void test_missing_keys_inside_section_give_defaults() {
    const cfg::LoadResult r = load("{\"device\":{\"brightness\":50}}");
    TEST_ASSERT_EQUAL_INT(static_cast<int>(cfg::LoadStatus::Loaded), static_cast<int>(r.status));
    cfg::Config expected;
    expected.device.brightness = 50;
    TEST_ASSERT_TRUE(r.config == expected);
}

static void test_unknown_keys_ignored() {
    const cfg::LoadResult r = load(
        "{\"schema\":1,\"future\":{\"x\":1},\"device\":{\"hostname\":\"abc\",\"new_key\":true},"
        "\"controls\":[{\"entity_id\":\"light.a\",\"color\":\"red\"}]}");
    TEST_ASSERT_EQUAL_INT(static_cast<int>(cfg::LoadStatus::Loaded), static_cast<int>(r.status));
    TEST_ASSERT_EQUAL_STRING("abc", r.config.device.hostname.c_str());
    TEST_ASSERT_EQUAL_size_t(1, r.config.controls.size());
}

static void test_missing_or_empty_input_is_missing() {
    cfg::LoadResult r = cfg::fromJson(nullptr, 0);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(cfg::LoadStatus::Missing), static_cast<int>(r.status));
    TEST_ASSERT_TRUE(r.config == cfg::Config());
    r = cfg::fromJson("", 0);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(cfg::LoadStatus::Missing), static_cast<int>(r.status));
}

static void test_corrupt_json_gives_defaults() {
    const char *bad[] = {"{", "not json", "{\"device\":", "[1,2,3]", "42", "\"str\"", "null",
                         "{\"device\":{\"hostname\":\"x\"}"};
    for (const char *b : bad) {
        const cfg::LoadResult r = load(b);
        TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(cfg::LoadStatus::Corrupt),
                                      static_cast<int>(r.status), b);
        TEST_ASSERT_TRUE_MESSAGE(r.config == cfg::Config(), b);
    }
    // Truncated real config.
    const std::string full = cfg::toJson(fullConfig());
    const cfg::LoadResult r = load(full.substr(0, full.size() / 2));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(cfg::LoadStatus::Corrupt), static_cast<int>(r.status));
    TEST_ASSERT_TRUE(r.config == cfg::Config());
}

static void test_schema_2_rejected_keeps_defaults() {
    const cfg::LoadResult r = load("{\"schema\":2,\"device\":{\"hostname\":\"other\"}}");
    TEST_ASSERT_EQUAL_INT(static_cast<int>(cfg::LoadStatus::SchemaTooNew), static_cast<int>(r.status));
    TEST_ASSERT_TRUE(r.config == cfg::Config());
    const cfg::LoadResult r99 = load("{\"schema\":99}");
    TEST_ASSERT_EQUAL_INT(static_cast<int>(cfg::LoadStatus::SchemaTooNew), static_cast<int>(r99.status));
}

static void test_schema_non_numeric_is_invalid() {
    const char *bad[] = {"{\"schema\":\"1\"}", "{\"schema\":true}", "{\"schema\":1.5}",
                         "{\"schema\":{}}", "{\"schema\":[1]}"};
    for (const char *b : bad) {
        const cfg::LoadResult r = load(b);
        TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(cfg::LoadStatus::Invalid),
                                      static_cast<int>(r.status), b);
        TEST_ASSERT_TRUE_MESSAGE(r.config == cfg::Config(), b);
    }
}

static void test_schema_older_or_missing_is_migrated() {
    cfg::LoadResult r = load("{\"schema\":0,\"device\":{\"hostname\":\"old\"}}");
    TEST_ASSERT_EQUAL_INT(static_cast<int>(cfg::LoadStatus::Loaded), static_cast<int>(r.status));
    TEST_ASSERT_EQUAL_STRING("old", r.config.device.hostname.c_str());

    JsonDocument doc;
    deserializeJson(doc, "{\"device\":{}}");
    TEST_ASSERT_TRUE(cfg::migrate(doc));
    TEST_ASSERT_EQUAL_INT(cfg::kSchemaVersion, doc["schema"].as<int>());

    JsonDocument doc0;
    deserializeJson(doc0, "{\"schema\":0}");
    TEST_ASSERT_TRUE(cfg::migrate(doc0));
    TEST_ASSERT_EQUAL_INT(1, doc0["schema"].as<int>());

    JsonDocument doc2;
    deserializeJson(doc2, "{\"schema\":2}");
    TEST_ASSERT_FALSE(cfg::migrate(doc2));

    JsonDocument docS;
    deserializeJson(docS, "{\"schema\":\"x\"}");
    TEST_ASSERT_FALSE(cfg::migrate(docS));
}

static void test_wrong_types_invalid_give_defaults() {
    struct Case {
        const char *json;
        const char *field;
        const char *message;
    };
    const Case cases[] = {
        {"{\"device\":{\"brightness\":80.5}}", "device.brightness", "not_an_integer"},
        {"{\"device\":{\"dim_after_s\":1.25}}", "device.dim_after_s", "not_an_integer"},
        {"{\"device\":{\"send_interval_ms\":250.7}}", "device.send_interval_ms", "not_an_integer"},
        {"{\"device\":{\"brightness\":\"80\"}}", "device.brightness", "not_an_integer"},
        {"{\"device\":{\"hostname\":123}}", "device.hostname", "not_a_string"},
        {"{\"device\":{\"buzzer\":\"true\"}}", "device.buzzer", "not_a_boolean"},
        {"{\"device\":{\"accel\":1}}", "device.accel", "not_a_boolean"},
        {"{\"device\":{\"dim_after_s\":-1}}", "device.dim_after_s", nullptr},
        {"{\"wifi\":{\"ssid\":5}}", "wifi.ssid", "not_a_string"},
        {"{\"ha\":{\"url\":false}}", "ha.url", "not_a_string"},
        {"{\"device\":[]}", "device", "not_an_object"},
        {"{\"controls\":{}}", "controls", "not_an_array"},
        {"{\"controls\":[\"light.a\"]}", "controls[0]", "not_an_object"},
        {"{\"controls\":[{\"label\":\"x\"}]}", "controls[0].entity_id", "required"},
        {"{\"controls\":[{\"entity_id\":\"light.a\",\"step\":\"5\"}]}", "controls[0].step", "not_a_number"},
    };
    for (const Case &c : cases) {
        const cfg::LoadResult r = load(c.json);
        TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(cfg::LoadStatus::Invalid),
                                      static_cast<int>(r.status), c.json);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(c.field, r.error.field.c_str(), c.json);
        if (c.message != nullptr) {
            TEST_ASSERT_EQUAL_STRING_MESSAGE(c.message, r.error.message.c_str(), c.json);
        }
        TEST_ASSERT_TRUE_MESSAGE(r.config == cfg::Config(), c.json);
    }
}

static void test_stored_value_out_of_range_invalid_gives_defaults() {
    const cfg::LoadResult r = load("{\"device\":{\"brightness\":101,\"hostname\":\"x\"}}");
    TEST_ASSERT_EQUAL_INT(static_cast<int>(cfg::LoadStatus::Invalid), static_cast<int>(r.status));
    TEST_ASSERT_EQUAL_STRING("device.brightness", r.error.field.c_str());
    TEST_ASSERT_TRUE(r.config == cfg::Config());
}

static void test_load_status_names() {
    TEST_ASSERT_EQUAL_STRING("loaded", cfg::loadStatusName(cfg::LoadStatus::Loaded));
    TEST_ASSERT_NOT_NULL(cfg::loadStatusName(cfg::LoadStatus::Corrupt));
}

// ---------------------------------------------------------------------------
// Validation rules at their boundaries (§3.4 table)
// ---------------------------------------------------------------------------

static void test_validate_hostname() {
    cfg::Config c;
    c.device.hostname = std::string(32, 'a');
    assertValid(c);
    c.device.hostname = "a";
    assertValid(c);
    c.device.hostname = "abc-123-z";
    assertValid(c);
    const char *bad[] = {"", "Ha-knob", "ha_knob", "ha.knob", "ha knob", "h\xC3\xA6"};
    for (const char *b : bad) {
        c.device.hostname = b;
        assertInvalid(c, "device.hostname");
    }
    c.device.hostname = std::string(33, 'a');
    assertInvalid(c, "device.hostname");

    TEST_ASSERT_TRUE(cfg::isValidHostname(std::string(32, 'z')));
    TEST_ASSERT_FALSE(cfg::isValidHostname(std::string(33, 'z')));
    TEST_ASSERT_FALSE(cfg::isValidHostname(""));
    TEST_ASSERT_FALSE(cfg::isValidHostname("UPPER"));
}

static void test_validate_brightness() {
    cfg::Config c;
    c.device.brightness = 5;
    assertValid(c);
    c.device.brightness = 100;
    assertValid(c);
    c.device.brightness = 4;
    assertInvalid(c, "device.brightness");
    c.device.brightness = 101;
    assertInvalid(c, "device.brightness");
    c.device.brightness = -80;
    assertInvalid(c, "device.brightness");
}

static void test_validate_dim_level() {
    cfg::Config c;
    c.device.dimLevel = 0;
    assertValid(c);
    c.device.dimLevel = 100;
    assertValid(c);
    c.device.dimLevel = -1;
    assertInvalid(c, "device.dim_level");
    c.device.dimLevel = 101;
    assertInvalid(c, "device.dim_level");
}

static void test_validate_timers() {
    cfg::Config c;
    c.device.dimAfterS = 0;
    c.device.offAfterS = 0;
    assertValid(c);
    c.device.dimAfterS = 86400;
    c.device.offAfterS = 86400;
    assertValid(c);
    c.device.dimAfterS = 86401;
    assertInvalid(c, "device.dim_after_s");
    c.device.dimAfterS = 30;
    c.device.offAfterS = 86401;
    assertInvalid(c, "device.off_after_s");
}

static void test_validate_send_interval() {
    cfg::Config c;
    c.device.sendIntervalMs = 100;
    assertValid(c);
    c.device.sendIntervalMs = 2000;
    assertValid(c);
    c.device.sendIntervalMs = 99;
    assertInvalid(c, "device.send_interval_ms");
    c.device.sendIntervalMs = 2001;
    assertInvalid(c, "device.send_interval_ms");
}

static void test_validate_ssid() {
    cfg::Config c;
    c.wifi.ssid = "";  // not configured
    assertValid(c);
    c.wifi.ssid = "x";
    assertValid(c);
    c.wifi.ssid = std::string(32, 'S');
    assertValid(c);
    c.wifi.ssid = std::string(30, 'S') + "\xC3\xB8";  // 32 bytes
    assertValid(c);
    c.wifi.ssid = std::string(33, 'S');
    assertInvalid(c, "wifi.ssid");
    c.wifi.ssid = std::string(31, 'S') + "\xC3\xB8";  // 31 chars, 33 bytes
    assertInvalid(c, "wifi.ssid");
    c.wifi.ssid = "My Net 2.4 GHz!";
    assertValid(c);
    const char *ctrl[] = {"Net\n", "Net\t1", "\x01Net", "Net\x7F"};
    for (const char *s : ctrl) {
        c.wifi.ssid = s;
        assertInvalid(c, "wifi.ssid");
    }
    c.wifi.ssid = std::string("Ne\0t", 4);
    assertInvalid(c, "wifi.ssid");
}

static void test_validate_ha_url() {
    cfg::Config c;
    const char *good[] = {"",
                          "http://192.168.1.10:8123",
                          "https://ha.example.com",
                          "http://homeassistant.local:8123/",
                          "http://ha:1",
                          "http://ha:65535",
                          "HTTP://HA.LOCAL:8123",
                          "https://ha.example.com/prefix/path"};
    for (const char *u : good) {
        c.ha.url = u;
        const cfg::ValidationResult r = cfg::validate(c);
        TEST_ASSERT_TRUE_MESSAGE(r.ok, u);
    }
    const char *bad[] = {"ftp://ha.local",
                         "ws://ha.local:8123",
                         "ha.local:8123",
                         "http://",
                         "http://:8123",
                         "http://ha:0",
                         "http://ha:65536",
                         "http://ha:99999",
                         "http://ha:",
                         "http://ha:80a",
                         "http://ha local",
                         "http://ha/pa th",
                         "http://user@ha.local",
                         " http://ha.local"};
    for (const char *u : bad) {
        c.ha.url = u;
        const cfg::ValidationResult r = cfg::validate(c);
        TEST_ASSERT_FALSE_MESSAGE(r.ok, u);
        TEST_ASSERT_EQUAL_STRING_MESSAGE("ha.url", r.field.c_str(), u);
    }
    // Host length: 253 OK, 254 rejected.
    std::string host253 = repeat(std::string(62, 'a') + ".", 4);  // 252
    host253 += "b";                                              // 253
    TEST_ASSERT_EQUAL_size_t(253, host253.size());
    c.ha.url = "http://" + host253 + ":8123";
    assertValid(c);
    c.ha.url = "http://" + host253 + "c:8123";
    assertInvalid(c, "ha.url");
}

static void test_parse_url_endpoint() {
    cfg::UrlEndpoint e = cfg::parseUrlEndpoint("http://HA.Local:8123/x");
    TEST_ASSERT_TRUE(e.ok);
    TEST_ASSERT_FALSE(e.tls);
    TEST_ASSERT_EQUAL_STRING("ha.local", e.host.c_str());
    TEST_ASSERT_EQUAL_UINT32(8123, e.port);

    e = cfg::parseUrlEndpoint("http://ha.local");
    TEST_ASSERT_TRUE(e.ok);
    TEST_ASSERT_EQUAL_UINT32(80, e.port);
    e = cfg::parseUrlEndpoint("https://ha.local/");
    TEST_ASSERT_TRUE(e.ok);
    TEST_ASSERT_TRUE(e.tls);
    TEST_ASSERT_EQUAL_UINT32(443, e.port);

    TEST_ASSERT_FALSE(cfg::parseUrlEndpoint("ftp://ha").ok);
    TEST_ASSERT_FALSE(cfg::parseUrlEndpoint("http://ha:0").ok);
    TEST_ASSERT_FALSE(cfg::parseUrlEndpoint("http://ha:65536").ok);
    TEST_ASSERT_TRUE(cfg::parseUrlEndpoint("http://ha:65535").ok);
    TEST_ASSERT_TRUE(cfg::parseUrlEndpoint("http://ha:1").ok);
}

static void test_validate_entity_id() {
    cfg::Config c;
    c.controls = {ctl("light.x", "brightness", 5, "toggle")};
    assertValid(c);
    const char *bad[] = {"Light.X", "light.X", "light", "light.", ".x", "light.kitchen.x",
                         "light.kit-chen", "light.k itchen", "light1.x", "light.\xC3\xA6"};
    for (const char *b : bad) {
        c.controls[0].entityId = b;
        const cfg::ValidationResult r = cfg::validate(c);
        TEST_ASSERT_FALSE_MESSAGE(r.ok, b);
        TEST_ASSERT_EQUAL_STRING_MESSAGE("controls[0].entity_id", r.field.c_str(), b);
        TEST_ASSERT_FALSE_MESSAGE(cfg::isValidEntityId(b), b);
    }
    // Length: 255 OK, 256 rejected.
    const std::string prefix = "light.";
    c.controls[0].entityId = prefix + std::string(255 - prefix.size(), 'a');
    assertValid(c);
    c.controls[0].entityId = prefix + std::string(256 - prefix.size(), 'a');
    assertInvalid(c, "controls[0].entity_id");

    TEST_ASSERT_TRUE(cfg::isValidEntityId("input_boolean.guest_mode_2"));
    TEST_ASSERT_TRUE(cfg::isValidEntityId("sensor.x"));  // pattern-valid
}

static void test_validate_unsupported_domain() {
    cfg::Config c;
    c.controls = {ctl("sensor.temperature", "none", 0, "none")};
    assertInvalid(c, "controls[0].entity_id");
}

static void test_validate_label() {
    cfg::Config c;
    c.controls = {ctl("light.x", "brightness", 5, "toggle", "")};
    assertValid(c);
    c.controls[0].label = std::string(48, 'L');
    assertValid(c);
    c.controls[0].label = repeat("\xC3\xA6", 24);  // 24 x 'æ' = 48 bytes
    assertValid(c);
    c.controls[0].label = std::string(49, 'L');
    assertInvalid(c, "controls[0].label");
    c.controls[0].label = repeat("\xC3\xB8", 24) + "x";  // 49 bytes
    assertInvalid(c, "controls[0].label");
    c.controls[0].label = "K\xC3\xB8kken \xC3\x86""ble \xC3\x98rn";
    assertValid(c);
    c.controls[0].label = "\xE2\x82\xAC 5 \xF0\x9F\x92\xA1";  // € and an emoji
    assertValid(c);

    const char *bad[] = {
        "line\nbreak", "tab\t", "\x7F",
        "\xC0\x80",          // overlong NUL
        "\xC1\xBF",          // overlong
        "\xE0\x80\xAF",      // overlong '/'
        "\xF0\x80\x80\xAF",  // overlong
        "\xED\xA0\x80",      // UTF-16 surrogate D800
        "\xED\xBF\xBF",      // surrogate DFFF
        "\xF4\x90\x80\x80",  // > U+10FFFF
        "\xC3",              // truncated
        "\xE2\x82",          // truncated
        "\x80",              // lone continuation
        "\xFF",              // invalid byte
        "\xC3\x28",          // bad continuation
    };
    for (const char *b : bad) {
        c.controls[0].label = b;
        const cfg::ValidationResult r = cfg::validate(c);
        TEST_ASSERT_FALSE_MESSAGE(r.ok, b);
        TEST_ASSERT_EQUAL_STRING("controls[0].label", r.field.c_str());
        TEST_ASSERT_FALSE(cfg::isValidLabelText(b));
    }
    TEST_ASSERT_TRUE(cfg::isValidLabelText("\xC3\xA6\xC3\xB8\xC3\xA5"));
    TEST_ASSERT_TRUE(cfg::isValidLabelText("\xF4\x8F\xBF\xBF"));  // U+10FFFF
}

static void test_validate_controls_count() {
    cfg::Config c;
    for (int i = 0; i < 24; ++i) c.controls.push_back(ctl("switch.s", "none", 0, "toggle"));
    assertValid(c);
    c.controls.push_back(ctl("switch.s", "none", 0, "toggle"));
    assertInvalid(c, "controls");
}

static void test_load_25_controls_rejected() {
    std::string json = "{\"controls\":[";
    for (int i = 0; i < 25; ++i) {
        if (i) json += ",";
        json += "{\"entity_id\":\"switch.s\"}";
    }
    json += "]}";
    const cfg::LoadResult r = load(json);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(cfg::LoadStatus::Invalid), static_cast<int>(r.status));
    TEST_ASSERT_EQUAL_STRING("controls", r.error.field.c_str());
    TEST_ASSERT_TRUE(r.config == cfg::Config());
}

static void test_validate_step_ranges() {
    struct Case {
        const char *entity;
        const char *action;
        double min;
        double max;
    };
    const Case cases[] = {
        {"light.a", "brightness", 1, 25},       {"light.a", "hue", 1, 60},
        {"light.a", "color_temp", 25, 1000},    {"climate.a", "target_temp", 0.1, 5},
        {"cover.a", "position", 1, 25},         {"fan.a", "percentage", 1, 50},
        {"media_player.a", "volume", 1, 20},
    };
    for (const Case &k : cases) {
        cfg::Config c;
        const char *press = da::findDomain(da::domainOf(k.entity))->presses[0];
        c.controls = {ctl(k.entity, k.action, k.min, press)};
        assertValid(c);
        c.controls[0].step = k.max;
        assertValid(c);
        c.controls[0].step = k.min - (k.min < 1 ? 0.01 : 0.5);
        assertInvalid(c, "controls[0].step");
        c.controls[0].step = k.max + (k.max < 10 ? 0.01 : 0.5);
        assertInvalid(c, "controls[0].step");
        c.controls[0].step = 0;
        assertInvalid(c, "controls[0].step");
        c.controls[0].step = -5;
        assertInvalid(c, "controls[0].step");
        c.controls[0].step = std::nan("");
        assertInvalid(c, "controls[0].step");
        c.controls[0].step = INFINITY;
        assertInvalid(c, "controls[0].step");
    }
}

static void test_validate_action_invalid_for_domain() {
    cfg::Config c;
    c.controls = {ctl("switch.plug", "brightness", 5, "toggle")};
    assertInvalid(c, "controls[0].action");
    c.controls = {ctl("input_boolean.x", "position", 10, "toggle")};
    assertInvalid(c, "controls[0].action");
    c.controls = {ctl("cover.x", "brightness", 5, "toggle")};
    assertInvalid(c, "controls[0].action");
    c.controls = {ctl("climate.x", "volume", 2, "none")};
    assertInvalid(c, "controls[0].action");
    c.controls = {ctl("light.x", "", 5, "toggle")};
    assertInvalid(c, "controls[0].action");
    c.controls = {ctl("light.x", "Brightness", 5, "toggle")};
    assertInvalid(c, "controls[0].action");
}

static void test_validate_press_invalid_for_domain() {
    cfg::Config c;
    c.controls = {ctl("light.x", "brightness", 5, "stop")};
    assertInvalid(c, "controls[0].press");
    c.controls = {ctl("switch.x", "none", 0, "play_pause")};
    assertInvalid(c, "controls[0].press");
    c.controls = {ctl("climate.x", "target_temp", 0.5, "mute")};
    assertInvalid(c, "controls[0].press");
    c.controls = {ctl("light.x", "brightness", 5, "")};
    assertInvalid(c, "controls[0].press");
    // Valid per §3.5.
    c.controls = {ctl("cover.x", "position", 10, "stop")};
    assertValid(c);
    c.controls = {ctl("media_player.x", "volume", 2, "mute")};
    assertValid(c);
    c.controls = {ctl("climate.x", "target_temp", 0.5, "toggle")};
    assertValid(c);
}

static void test_validate_step_ignored_for_none_action() {
    cfg::Config c;
    c.controls = {ctl("switch.plug", "none", 0, "toggle")};
    assertValid(c);
    c.controls[0].step = 999;
    assertValid(c);
}

static void test_validate_reports_index_of_bad_control() {
    cfg::Config c;
    c.controls = {ctl("light.a", "brightness", 5, "toggle"), ctl("light.b", "brightness", 5, "toggle"),
                  ctl("switch.c", "brightness", 5, "toggle")};
    assertInvalid(c, "controls[2].action");
}

static void test_duplicate_entity_ids_allowed() {
    cfg::Config c;
    c.controls = {ctl("light.kitchen", "brightness", 5, "toggle"),
                  ctl("light.kitchen", "color_temp", 100, "toggle"),
                  ctl("light.kitchen", "hue", 10, "none"),
                  ctl("light.kitchen", "brightness", 5, "toggle")};
    assertValid(c);
    const cfg::LoadResult r = load(cfg::toJson(c));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(cfg::LoadStatus::Loaded), static_cast<int>(r.status));
    TEST_ASSERT_EQUAL_size_t(4, r.config.controls.size());
}

// ---------------------------------------------------------------------------
// Size limit
// ---------------------------------------------------------------------------

static void test_24_controls_48_byte_labels_fit_limit() {
    cfg::Config c = fullConfig();
    c.controls.clear();
    const char *entities[] = {"media_player.living_room_speaker_group", "light.kitchen_ceiling_spots",
                              "climate.living_room_thermostat", "cover.bedroom_window_blind"};
    const char *actions[] = {"volume", "brightness", "target_temp", "position"};
    const double steps[] = {2, 5, 0.5, 10};
    const char *presses[] = {"play_pause", "toggle", "none", "toggle"};
    for (int i = 0; i < 24; ++i) {
        // Alternate ASCII and 2-byte UTF-8 labels, all exactly 48 bytes.
        const std::string label = (i % 2) ? std::string(48, 'W') : repeat("\xC3\x98", 24);
        c.controls.push_back(ctl(entities[i % 4], actions[i % 4], steps[i % 4], presses[i % 4],
                                 label.c_str()));
    }
    const std::string json = cfg::toJson(c);
    TEST_ASSERT_TRUE_MESSAGE(json.size() <= cfg::kMaxConfigJsonBytes, std::to_string(json.size()).c_str());
    assertValid(c);
    const cfg::LoadResult r = load(json);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(cfg::LoadStatus::Loaded), static_cast<int>(r.status));
    TEST_ASSERT_TRUE(r.config == c);

    // Redacted output (API) is also well-formed with all 24 controls.
    const std::string red = cfg::toRedactedJson(c, {true, true, true});
    JsonDocument doc;
    TEST_ASSERT_TRUE(deserializeJson(doc, red) == DeserializationError::Ok);
    TEST_ASSERT_EQUAL_size_t(24, doc["controls"].size());
}

static void test_oversize_config_too_large() {
    cfg::Config c;
    const std::string id = "media_player." + std::string(255 - 13, 'x');
    for (int i = 0; i < 24; ++i) {
        c.controls.push_back(ctl(id.c_str(), "volume", 2, "play_pause", std::string(48, 'L').c_str()));
    }
    // 24 x 255-char ids is 9361 bytes: valid since the limit went from 8192 to 12288.
    TEST_ASSERT_TRUE(cfg::toJson(c).size() > 8192);
    assertValid(c);
    // Add a large (4096-byte, structurally valid) CA on an https URL to push it over
    // kMaxConfigJsonBytes.
    c.ha.url = "https://ha.example.com";
    c.ha.caPem = certShapedPem(4096);
    TEST_ASSERT_EQUAL_size_t(4096, c.ha.caPem.size());
    TEST_ASSERT_TRUE(cfg::toJson(c).size() > cfg::kMaxConfigJsonBytes);
    const cfg::ValidationResult r = cfg::validate(c);
    TEST_ASSERT_FALSE(r.ok);
    TEST_ASSERT_EQUAL_STRING("controls", r.field.c_str());
    TEST_ASSERT_EQUAL_STRING("too_large", r.message.c_str());

    // The same through mergePatch (ha + controls sections of the oversize config).
    JsonDocument patchDoc;
    TEST_ASSERT_TRUE(deserializeJson(patchDoc, cfg::toJson(c)) == DeserializationError::Ok);
    patchDoc.remove("schema");
    patchDoc.remove("device");
    patchDoc.remove("wifi");
    patchDoc["ha"]["token"] = "tok";
    std::string trimmed;
    serializeJson(patchDoc, trimmed);
    const cfg::MergeResult m = merge(cfg::Config(), trimmed);
    TEST_ASSERT_FALSE(m.ok);
    TEST_ASSERT_EQUAL_STRING("controls", m.error.field.c_str());
    TEST_ASSERT_EQUAL_STRING("too_large", m.error.message.c_str());
}

// ---------------------------------------------------------------------------
// Redaction
// ---------------------------------------------------------------------------

static void test_redacted_flags_and_no_secret_values() {
    // Secrets are set through mergePatch so we know their values and can search for them.
    const cfg::Config base = fullConfig();
    const char *wifiPw = "SuperSecretWifi99";
    const char *token = "eyJhbGciOiJIUzI1NiJ9.secretTokenValue.xyz";
    const char *webPw = "WebPassw0rd!";
    const std::string patch = std::string("{\"wifi\":{\"ssid\":\"MyNetwork\",\"password\":\"") + wifiPw +
                              "\"},\"ha\":{\"url\":\"http://192.168.1.10:8123\",\"token\":\"" + token +
                              "\"},\"web\":{\"password\":\"" + webPw + "\"}}";
    const cfg::MergeResult m = mergeOk(base, patch);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Kind::Set), static_cast<int>(m.secrets.wifiPassword.kind));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Kind::Set), static_cast<int>(m.secrets.haToken.kind));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Kind::Set), static_cast<int>(m.secrets.webPassword.kind));

    const bool combos[][3] = {{true, true, true}, {false, false, false}, {true, false, true},
                              {false, true, false}};
    for (const auto &f : combos) {
        cfg::SecretsPresent p;
        p.wifiPassword = f[0];
        p.haToken = f[1];
        p.webPassword = f[2];
        const std::string red = cfg::toRedactedJson(m.config, p);
        TEST_ASSERT_TRUE(red.find(wifiPw) == std::string::npos);
        TEST_ASSERT_TRUE(red.find(token) == std::string::npos);
        TEST_ASSERT_TRUE(red.find(webPw) == std::string::npos);
        TEST_ASSERT_TRUE(red.find("Secret") == std::string::npos);

        JsonDocument doc;
        TEST_ASSERT_TRUE(deserializeJson(doc, red) == DeserializationError::Ok);
        TEST_ASSERT_TRUE(doc["wifi"]["password_set"].is<bool>());
        TEST_ASSERT_TRUE(doc["ha"]["token_set"].is<bool>());
        TEST_ASSERT_TRUE(doc["web"]["password_set"].is<bool>());
        TEST_ASSERT_EQUAL(f[0], doc["wifi"]["password_set"].as<bool>());
        TEST_ASSERT_EQUAL(f[1], doc["ha"]["token_set"].as<bool>());
        TEST_ASSERT_EQUAL(f[2], doc["web"]["password_set"].as<bool>());
        // No raw secret keys at all.
        TEST_ASSERT_TRUE(doc["wifi"]["password"].isNull());
        TEST_ASSERT_TRUE(doc["ha"]["token"].isNull());
        TEST_ASSERT_TRUE(doc["web"]["password"].isNull());
        // Non-secret content is still there.
        TEST_ASSERT_EQUAL_STRING("MyNetwork", doc["wifi"]["ssid"].as<const char *>());
        TEST_ASSERT_EQUAL_STRING("http://192.168.1.10:8123", doc["ha"]["url"].as<const char *>());
        TEST_ASSERT_EQUAL_size_t(m.config.controls.size(), doc["controls"].size());
        TEST_ASSERT_EQUAL_INT(1, doc["schema"].as<int>());
    }
}

static void test_redacted_json_loads_back_as_config() {
    // The redacted JSON is a superset of the stored one; loading it ignores the *_set keys.
    const cfg::Config c = fullConfig();
    const std::string red = cfg::toRedactedJson(c, {true, true, false});
    const cfg::LoadResult r = load(red);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(cfg::LoadStatus::Loaded), static_cast<int>(r.status));
    TEST_ASSERT_TRUE(r.config == c);
}

// ---------------------------------------------------------------------------
// mergePatch: sections and secrets
// ---------------------------------------------------------------------------

static void test_merge_invalid_json() {
    const cfg::Config cur = fullConfig();
    assertMergeFails(cur, "{", "", "invalid_json");
    assertMergeFails(cur, "[]", "", "invalid_json");
    assertMergeFails(cur, "", "", "invalid_json");
    assertMergeFails(cur, "nope", "", "invalid_json");
}

static void test_merge_empty_patch_keeps_everything() {
    const cfg::Config cur = fullConfig();
    const cfg::MergeResult r = mergeOk(cur, "{}");
    TEST_ASSERT_TRUE(r.config == cur);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Kind::Keep), static_cast<int>(r.secrets.wifiPassword.kind));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Kind::Keep), static_cast<int>(r.secrets.haToken.kind));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Kind::Keep), static_cast<int>(r.secrets.webPassword.kind));
    TEST_ASSERT_FALSE(r.restartWifi);
}

static void test_merge_absent_sections_kept() {
    const cfg::Config cur = fullConfig();
    const cfg::MergeResult r = mergeOk(cur, "{\"device\":{\"hostname\":\"new-name\"}}");
    TEST_ASSERT_TRUE(r.config.wifi == cur.wifi);
    TEST_ASSERT_TRUE(r.config.ha == cur.ha);
    TEST_ASSERT_TRUE(r.config.controls == cur.controls);
    TEST_ASSERT_EQUAL_STRING("new-name", r.config.device.hostname.c_str());
}

static void test_merge_device_section_replace_is_literal() {
    const cfg::Config cur = fullConfig();  // brightness 55, buzzer false, ...
    const cfg::MergeResult r = mergeOk(cur, "{\"device\":{\"brightness\":90}}");
    cfg::DeviceConfig expected;
    expected.brightness = 90;
    TEST_ASSERT_TRUE(r.config.device == expected);
}

static void test_merge_controls_section_replaces_list() {
    const cfg::Config cur = fullConfig();
    cfg::MergeResult r = mergeOk(
        cur, "{\"controls\":[{\"entity_id\":\"fan.attic\",\"action\":\"percentage\",\"step\":20,\"press\":\"none\"}]}");
    TEST_ASSERT_EQUAL_size_t(1, r.config.controls.size());
    TEST_ASSERT_TRUE(r.config.controls[0] == ctl("fan.attic", "percentage", 20, "none"));
    TEST_ASSERT_TRUE(r.config.device == cur.device);

    r = mergeOk(cur, "{\"controls\":[]}");
    TEST_ASSERT_EQUAL_size_t(0, r.config.controls.size());
}

static void test_merge_controls_get_domain_defaults() {
    const cfg::MergeResult r = mergeOk(
        cfg::Config(),
        "{\"controls\":["
        "{\"entity_id\":\"light.a\"},"
        "{\"entity_id\":\"climate.a\"},"
        "{\"entity_id\":\"cover.a\"},"
        "{\"entity_id\":\"fan.a\"},"
        "{\"entity_id\":\"media_player.a\"},"
        "{\"entity_id\":\"switch.a\"},"
        "{\"entity_id\":\"input_boolean.a\"},"
        "{\"entity_id\":\"light.b\",\"action\":\"color_temp\"},"
        "{\"entity_id\":\"light.c\",\"action\":\"hue\",\"press\":\"none\"},"
        "{\"entity_id\":\"light.d\",\"step\":7}"
        "]}");
    const std::vector<cfg::Control> &c = r.config.controls;
    TEST_ASSERT_EQUAL_size_t(10, c.size());
    TEST_ASSERT_TRUE(c[0] == ctl("light.a", "brightness", 5, "toggle"));
    TEST_ASSERT_TRUE(c[1] == ctl("climate.a", "target_temp", 0.5, "none"));
    TEST_ASSERT_TRUE(c[2] == ctl("cover.a", "position", 10, "toggle"));
    TEST_ASSERT_TRUE(c[3] == ctl("fan.a", "percentage", 10, "toggle"));
    TEST_ASSERT_TRUE(c[4] == ctl("media_player.a", "volume", 2, "play_pause"));
    TEST_ASSERT_EQUAL_STRING("none", c[5].action.c_str());
    TEST_ASSERT_EQUAL_STRING("toggle", c[5].press.c_str());
    TEST_ASSERT_EQUAL_STRING("none", c[6].action.c_str());
    TEST_ASSERT_EQUAL_STRING("toggle", c[6].press.c_str());
    TEST_ASSERT_TRUE(c[7] == ctl("light.b", "color_temp", 100, "toggle"));
    TEST_ASSERT_TRUE(c[8] == ctl("light.c", "hue", 10, "none"));
    TEST_ASSERT_TRUE(c[9] == ctl("light.d", "brightness", 7, "toggle"));
}

static void test_merge_controls_invalid_rejected() {
    const cfg::Config cur = fullConfig();
    assertMergeFails(cur, "{\"controls\":[{\"entity_id\":\"switch.plug\",\"action\":\"brightness\"}]}",
                     "controls[0].action", nullptr);
    assertMergeFails(cur, "{\"controls\":[{\"entity_id\":\"light.a\",\"step\":26}]}", "controls[0].step",
                     nullptr);
    assertMergeFails(cur, "{\"controls\":[{\"entity_id\":\"Light.X\"}]}", "controls[0].entity_id", nullptr);
    std::string many = "{\"controls\":[";
    for (int i = 0; i < 25; ++i) many += std::string(i ? "," : "") + "{\"entity_id\":\"switch.s\"}";
    many += "]}";
    assertMergeFails(cur, many, "controls", nullptr);
}

static void test_merge_device_out_of_range_rejected() {
    const cfg::Config cur = fullConfig();
    assertMergeFails(cur, "{\"device\":{\"brightness\":4}}", "device.brightness", nullptr);
    assertMergeFails(cur, "{\"device\":{\"brightness\":50.5}}", "device.brightness", "not_an_integer");
    assertMergeFails(cur, "{\"device\":{\"hostname\":\"" + std::string(33, 'a') + "\"}}", "device.hostname",
                     nullptr);
}

static void test_merge_duplicates_allowed() {
    const cfg::MergeResult r = mergeOk(
        cfg::Config(),
        "{\"controls\":[{\"entity_id\":\"light.k\"},{\"entity_id\":\"light.k\",\"action\":\"color_temp\"},"
        "{\"entity_id\":\"light.k\"}]}");
    TEST_ASSERT_EQUAL_size_t(3, r.config.controls.size());
}

static void test_merge_web_password_rules() {
    const cfg::Config cur = fullConfig();
    cfg::MergeResult r = mergeOk(cur, "{\"web\":{\"password\":\"\"}}");
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Kind::Keep), static_cast<int>(r.secrets.webPassword.kind));
    r = mergeOk(cur, "{\"web\":{}}");
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Kind::Keep), static_cast<int>(r.secrets.webPassword.kind));
    r = mergeOk(cur, "{\"web\":{\"password\":null}}");
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Kind::Clear), static_cast<int>(r.secrets.webPassword.kind));
    r = mergeOk(cur, "{\"web\":{\"password\":\"x\"}}");
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Kind::Set), static_cast<int>(r.secrets.webPassword.kind));
    TEST_ASSERT_EQUAL_STRING("x", r.secrets.webPassword.value.c_str());
    // 64 OK, 65 rejected; UTF-8 OK; control chars rejected.
    r = mergeOk(cur, "{\"web\":{\"password\":\"" + std::string(64, 'p') + "\"}}");
    TEST_ASSERT_EQUAL_size_t(64, r.secrets.webPassword.value.size());
    assertMergeFails(cur, "{\"web\":{\"password\":\"" + std::string(65, 'p') + "\"}}", "web.password", nullptr);
    r = mergeOk(cur, "{\"web\":{\"password\":\"p\xC3\xA6ss word\"}}");
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Kind::Set), static_cast<int>(r.secrets.webPassword.kind));
    assertMergeFails(cur, "{\"web\":{\"password\":\"pass\\nword\"}}", "web.password", nullptr);
    assertMergeFails(cur, "{\"web\":{\"password\":\"pass\\u0001\"}}", "web.password", nullptr);
    assertMergeFails(cur, "{\"web\":{\"password\":123}}", "web.password", nullptr);
    // Secret changes do not alter the non-secret config.
    TEST_ASSERT_TRUE(r.config == cur);
    TEST_ASSERT_FALSE(r.restartWifi);
}

static void test_merge_ha_token_format() {
    const cfg::Config cur = fullConfig();
    const std::string url = "\"url\":\"http://192.168.1.10:8123\"";
    cfg::MergeResult r = mergeOk(cur, "{\"ha\":{" + url + ",\"token\":\"" + std::string(512, 't') + "\"}}");
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Kind::Set), static_cast<int>(r.secrets.haToken.kind));
    TEST_ASSERT_EQUAL_size_t(512, r.secrets.haToken.value.size());
    assertMergeFails(cur, "{\"ha\":{" + url + ",\"token\":\"" + std::string(513, 't') + "\"}}", "ha.token",
                     nullptr);
    assertMergeFails(cur, "{\"ha\":{" + url + ",\"token\":\"abc def\"}}", "ha.token", nullptr);
    assertMergeFails(cur, "{\"ha\":{" + url + ",\"token\":\"abc\\tdef\"}}", "ha.token", nullptr);
    assertMergeFails(cur, "{\"ha\":{" + url + ",\"token\":\"abc\xC3\xA6\"}}", "ha.token", nullptr);
    assertMergeFails(cur, "{\"ha\":{" + url + ",\"token\":5}}", "ha.token", nullptr);
    // Printable ASCII incl. JWT punctuation is fine.
    r = mergeOk(cur, "{\"ha\":{" + url + ",\"token\":\"eyJ0.eXAi-_~!#$%&'()*+,/:;<=>?@[]^`{|}\"}}");
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Kind::Set), static_cast<int>(r.secrets.haToken.kind));
}

// ---------------------------------------------------------------------------
// mergePatch: token guard
// ---------------------------------------------------------------------------

static void test_token_guard_same_url_keeps_token() {
    const cfg::Config cur = configWithUrl("http://192.168.1.10:8123");
    cfg::MergeResult r = mergeOk(cur, "{\"ha\":{\"url\":\"http://192.168.1.10:8123\"}}");
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Kind::Keep), static_cast<int>(r.secrets.haToken.kind));
    r = mergeOk(cur, "{\"ha\":{\"url\":\"http://192.168.1.10:8123\",\"token\":\"\"}}");
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Kind::Keep), static_cast<int>(r.secrets.haToken.kind));
    r = mergeOk(cur, "{\"ha\":{\"url\":\"http://192.168.1.10:8123\",\"token\":null}}");
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Kind::Clear), static_cast<int>(r.secrets.haToken.kind));
    r = mergeOk(cur, "{\"ha\":{\"url\":\"http://192.168.1.10:8123\",\"token\":\"newtok\"}}");
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Kind::Set), static_cast<int>(r.secrets.haToken.kind));
    TEST_ASSERT_EQUAL_STRING("newtok", r.secrets.haToken.value.c_str());
}

static void test_token_guard_new_host_without_token_rejected() {
    const cfg::Config cur = configWithUrl("http://192.168.1.10:8123");
    assertMergeFails(cur, "{\"ha\":{\"url\":\"http://evil.example.com:8123\"}}", "ha.token", "token_required");
    assertMergeFails(cur, "{\"ha\":{\"url\":\"http://evil.example.com:8123\",\"token\":\"\"}}", "ha.token",
                     "token_required");
    assertMergeFails(cur, "{\"ha\":{\"url\":\"http://evil.example.com:8123\",\"token\":null}}", "ha.token",
                     "token_required");
    // Different port on the same host.
    assertMergeFails(cur, "{\"ha\":{\"url\":\"http://192.168.1.10:8124\"}}", "ha.token", "token_required");
    // Same host, default port 80 instead of 8123.
    assertMergeFails(cur, "{\"ha\":{\"url\":\"http://192.168.1.10\"}}", "ha.token", "token_required");
    // Section replace without url -> url "" -> clears token (not an error).
    const cfg::MergeResult r = mergeOk(cur, "{\"ha\":{}}");
    TEST_ASSERT_TRUE(r.config.ha.url.empty());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Kind::Clear), static_cast<int>(r.secrets.haToken.kind));
}

static void test_token_guard_new_host_with_token_accepted() {
    const cfg::Config cur = configWithUrl("http://192.168.1.10:8123");
    const cfg::MergeResult r =
        mergeOk(cur, "{\"ha\":{\"url\":\"https://ha.example.com\",\"token\":\"abc.def.ghi\"}}");
    TEST_ASSERT_EQUAL_STRING("https://ha.example.com", r.config.ha.url.c_str());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Kind::Set), static_cast<int>(r.secrets.haToken.kind));
    TEST_ASSERT_EQUAL_STRING("abc.def.ghi", r.secrets.haToken.value.c_str());
}

static void test_token_guard_first_url_requires_token() {
    const cfg::Config cur;  // no URL yet
    assertMergeFails(cur, "{\"ha\":{\"url\":\"http://ha.local:8123\"}}", "ha.token", "token_required");
    const cfg::MergeResult r = mergeOk(cur, "{\"ha\":{\"url\":\"http://ha.local:8123\",\"token\":\"t\"}}");
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Kind::Set), static_cast<int>(r.secrets.haToken.kind));
}

static void test_token_guard_url_cleared_clears_token() {
    const cfg::Config cur = configWithUrl("http://192.168.1.10:8123");
    const cfg::MergeResult r = mergeOk(cur, "{\"ha\":{\"url\":\"\"}}");
    TEST_ASSERT_TRUE(r.config.ha.url.empty());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Kind::Clear), static_cast<int>(r.secrets.haToken.kind));
}

static void test_token_guard_path_only_change_keeps_token() {
    const cfg::Config cur = configWithUrl("http://192.168.1.10:8123");
    cfg::MergeResult r = mergeOk(cur, "{\"ha\":{\"url\":\"http://192.168.1.10:8123/ha\"}}");
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Kind::Keep), static_cast<int>(r.secrets.haToken.kind));
    TEST_ASSERT_EQUAL_STRING("http://192.168.1.10:8123/ha", r.config.ha.url.c_str());
    r = mergeOk(cur, "{\"ha\":{\"url\":\"http://192.168.1.10:8123/\"}}");
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Kind::Keep), static_cast<int>(r.secrets.haToken.kind));
}

static void test_token_guard_host_case_and_default_port_normalised() {
    cfg::Config cur = configWithUrl("http://HA.Local");
    cfg::MergeResult r = mergeOk(cur, "{\"ha\":{\"url\":\"http://ha.local:80\"}}");
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Kind::Keep), static_cast<int>(r.secrets.haToken.kind));
    r = mergeOk(cur, "{\"ha\":{\"url\":\"HTTP://ha.LOCAL/\"}}");
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Kind::Keep), static_cast<int>(r.secrets.haToken.kind));

    cur = configWithUrl("https://ha.example.com:443/");
    r = mergeOk(cur, "{\"ha\":{\"url\":\"https://HA.example.com\"}}");
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Kind::Keep), static_cast<int>(r.secrets.haToken.kind));
}

static void test_token_guard_tls_upgrade_keeps_token() {
    const cfg::Config cur = configWithUrl("http://ha.local:8123");
    const cfg::MergeResult r = mergeOk(cur, "{\"ha\":{\"url\":\"https://ha.local:8123\"}}");
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Kind::Keep), static_cast<int>(r.secrets.haToken.kind));
}

static void test_token_guard_tls_downgrade_requires_token() {
    const cfg::Config cur = configWithUrl("https://ha.local:8123");
    assertMergeFails(cur, "{\"ha\":{\"url\":\"http://ha.local:8123\"}}", "ha.token", "token_required");
    const cfg::MergeResult r = mergeOk(cur, "{\"ha\":{\"url\":\"http://ha.local:8123\",\"token\":\"t2\"}}");
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Kind::Set), static_cast<int>(r.secrets.haToken.kind));
}

static void test_token_guard_invalid_url_rejected() {
    const cfg::Config cur = configWithUrl("http://ha.local:8123");
    const cfg::MergeResult r = merge(cur, "{\"ha\":{\"url\":\"ftp://ha.local\",\"token\":\"t\"}}");
    TEST_ASSERT_FALSE(r.ok);
    TEST_ASSERT_EQUAL_STRING("ha.url", r.error.field.c_str());
    TEST_ASSERT_TRUE(r.config == cur);
}

// ---------------------------------------------------------------------------
// mergePatch: Wi-Fi guard
// ---------------------------------------------------------------------------

static void test_wifi_unchanged_ssid_secret_rules() {
    const cfg::Config cur = configWithSsid("Home");
    cfg::MergeResult r = mergeOk(cur, "{\"wifi\":{\"ssid\":\"Home\"}}");
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Kind::Keep), static_cast<int>(r.secrets.wifiPassword.kind));
    TEST_ASSERT_FALSE(r.restartWifi);
    r = mergeOk(cur, "{\"wifi\":{\"ssid\":\"Home\",\"password\":\"\"}}");  // empty keeps
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Kind::Keep), static_cast<int>(r.secrets.wifiPassword.kind));
    TEST_ASSERT_FALSE(r.restartWifi);
    r = mergeOk(cur, "{\"wifi\":{\"ssid\":\"Home\",\"password\":\"newpass123\"}}");
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Kind::Set), static_cast<int>(r.secrets.wifiPassword.kind));
    TEST_ASSERT_EQUAL_STRING("newpass123", r.secrets.wifiPassword.value.c_str());
    TEST_ASSERT_TRUE(r.restartWifi);
    r = mergeOk(cur, "{\"wifi\":{\"ssid\":\"Home\",\"password\":null}}");
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Kind::Clear), static_cast<int>(r.secrets.wifiPassword.kind));
    TEST_ASSERT_TRUE(r.restartWifi);
}

static void test_wifi_password_format() {
    const cfg::Config cur = configWithSsid("Home");
    auto patch = [](const std::string &pw) { return "{\"wifi\":{\"ssid\":\"Home\",\"password\":\"" + pw + "\"}}"; };
    mergeOk(cur, patch(std::string(8, 'p')));
    mergeOk(cur, patch(std::string(63, 'p')));
    mergeOk(cur, patch("p a s s ~!"));  // printable ASCII incl. space
    assertMergeFails(cur, patch(std::string(7, 'p')), "wifi.password", nullptr);
    assertMergeFails(cur, patch(std::string(64, 'p')), "wifi.password", nullptr);
    assertMergeFails(cur, patch("p\xC3\xA6ssword1"), "wifi.password", nullptr);
    assertMergeFails(cur, patch("password\\n1"), "wifi.password", nullptr);
    assertMergeFails(cur, "{\"wifi\":{\"ssid\":\"Home\",\"password\":12345678}}", "wifi.password", nullptr);
}

static void test_wifi_ssid_change_requires_password() {
    const cfg::Config cur = configWithSsid("Home");
    assertMergeFails(cur, "{\"wifi\":{\"ssid\":\"EvilTwin\"}}", "wifi.password", "password_required");
    assertMergeFails(cur, "{\"wifi\":{\"ssid\":\"EvilTwin\",\"password\":\"\"}}", "wifi.password",
                     "password_required");
    // Byte-for-byte: case change and trailing space count as changes.
    assertMergeFails(cur, "{\"wifi\":{\"ssid\":\"home\"}}", "wifi.password", "password_required");
    assertMergeFails(cur, "{\"wifi\":{\"ssid\":\"Home \"}}", "wifi.password", "password_required");
    // First configuration (from "") also needs a password or null.
    assertMergeFails(cfg::Config(), "{\"wifi\":{\"ssid\":\"Home\"}}", "wifi.password", "password_required");
}

static void test_wifi_ssid_change_with_password_or_null_accepted() {
    const cfg::Config cur = configWithSsid("Home");
    cfg::MergeResult r = mergeOk(cur, "{\"wifi\":{\"ssid\":\"Office\",\"password\":\"office-pass\"}}");
    TEST_ASSERT_EQUAL_STRING("Office", r.config.wifi.ssid.c_str());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Kind::Set), static_cast<int>(r.secrets.wifiPassword.kind));
    TEST_ASSERT_TRUE(r.restartWifi);
    r = mergeOk(cur, "{\"wifi\":{\"ssid\":\"OpenCafe\",\"password\":null}}");
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Kind::Clear), static_cast<int>(r.secrets.wifiPassword.kind));
    TEST_ASSERT_TRUE(r.restartWifi);
    // Invalid new password with a new SSID: format error, not accepted.
    assertMergeFails(cur, "{\"wifi\":{\"ssid\":\"Office\",\"password\":\"short\"}}", "wifi.password", nullptr);
}

static void test_wifi_ssid_cleared_clears_password() {
    const cfg::Config cur = configWithSsid("Home");
    cfg::MergeResult r = mergeOk(cur, "{\"wifi\":{\"ssid\":\"\"}}");
    TEST_ASSERT_TRUE(r.config.wifi.ssid.empty());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Kind::Clear), static_cast<int>(r.secrets.wifiPassword.kind));
    TEST_ASSERT_TRUE(r.restartWifi);
    // Literal section replace: a wifi section without ssid unconfigures Wi-Fi.
    r = mergeOk(cur, "{\"wifi\":{}}");
    TEST_ASSERT_TRUE(r.config.wifi.ssid.empty());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Kind::Clear), static_cast<int>(r.secrets.wifiPassword.kind));
}

static void test_wifi_ssid_invalid_rejected_in_patch() {
    const cfg::Config cur = configWithSsid("Home");
    assertMergeFails(cur, "{\"wifi\":{\"ssid\":\"" + std::string(33, 's') + "\",\"password\":null}}", "wifi.ssid",
                     nullptr);
    assertMergeFails(cur, "{\"wifi\":{\"ssid\":\"bad\\nssid\",\"password\":null}}", "wifi.ssid", nullptr);
}

// Round-4 rule: an empty merged URL / SSID never stores a token / password.
static void test_empty_url_forces_token_clear_even_if_supplied() {
    const cfg::Config cur = configWithUrl("http://192.168.1.10:8123");
    cfg::MergeResult r = mergeOk(cur, "{\"ha\":{\"url\":\"\",\"token\":\"abc\"}}");
    TEST_ASSERT_TRUE(r.config.ha.url.empty());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Kind::Clear), static_cast<int>(r.secrets.haToken.kind));
    TEST_ASSERT_TRUE(r.secrets.haToken.value.empty());
    // URL was already empty: a supplied token is still not stored.
    r = mergeOk(cfg::Config(), "{\"ha\":{\"url\":\"\",\"token\":\"abc\"}}");
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Kind::Clear), static_cast<int>(r.secrets.haToken.kind));
    TEST_ASSERT_TRUE(r.secrets.haToken.value.empty());
    // Section without url (literal replace -> "") plus a token.
    r = mergeOk(cur, "{\"ha\":{\"token\":\"abc\"}}");
    TEST_ASSERT_TRUE(r.config.ha.url.empty());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Kind::Clear), static_cast<int>(r.secrets.haToken.kind));
    // Empty URL, token "" or null: also Clear.
    r = mergeOk(cfg::Config(), "{\"ha\":{\"url\":\"\",\"token\":\"\"}}");
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Kind::Clear), static_cast<int>(r.secrets.haToken.kind));
    r = mergeOk(cfg::Config(), "{\"ha\":{\"url\":\"\",\"token\":null}}");
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Kind::Clear), static_cast<int>(r.secrets.haToken.kind));
    // An invalid token format is still rejected, not silently dropped.
    assertMergeFails(cur, "{\"ha\":{\"url\":\"\",\"token\":\"a b\"}}", "ha.token", nullptr);
}

static void test_empty_ssid_forces_password_clear_even_if_supplied() {
    const cfg::Config cur = configWithSsid("Home");
    cfg::MergeResult r = mergeOk(cur, "{\"wifi\":{\"ssid\":\"\",\"password\":\"newpass123\"}}");
    TEST_ASSERT_TRUE(r.config.wifi.ssid.empty());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Kind::Clear), static_cast<int>(r.secrets.wifiPassword.kind));
    TEST_ASSERT_TRUE(r.secrets.wifiPassword.value.empty());
    // SSID already empty: a supplied password is still not stored.
    r = mergeOk(cfg::Config(), "{\"wifi\":{\"ssid\":\"\",\"password\":\"newpass123\"}}");
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Kind::Clear), static_cast<int>(r.secrets.wifiPassword.kind));
    TEST_ASSERT_TRUE(r.secrets.wifiPassword.value.empty());
    r = mergeOk(cfg::Config(), "{\"wifi\":{\"password\":\"newpass123\"}}");
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Kind::Clear), static_cast<int>(r.secrets.wifiPassword.kind));
    r = mergeOk(cfg::Config(), "{\"wifi\":{\"ssid\":\"\"}}");
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Kind::Clear), static_cast<int>(r.secrets.wifiPassword.kind));
    // Invalid password format is still rejected.
    assertMergeFails(cur, "{\"wifi\":{\"ssid\":\"\",\"password\":\"short\"}}", "wifi.password", nullptr);
}

static void test_non_empty_url_and_ssid_not_forced_clear() {
    // Guard against over-application: a configured URL/SSID keeps the normal rules.
    cfg::MergeResult r = mergeOk(configWithUrl("http://ha.local:8123"),
                                 "{\"ha\":{\"url\":\"http://ha.local:8123\"}}");
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Kind::Keep), static_cast<int>(r.secrets.haToken.kind));
    r = mergeOk(configWithSsid("Home"), "{\"wifi\":{\"ssid\":\"Home\"}}");
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Kind::Keep), static_cast<int>(r.secrets.wifiPassword.kind));
    // A patch without ha/wifi sections never touches those secrets.
    r = mergeOk(cfg::Config(), "{\"device\":{}}");
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Kind::Keep), static_cast<int>(r.secrets.haToken.kind));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Kind::Keep), static_cast<int>(r.secrets.wifiPassword.kind));
}

// Step 4: restartWifi = ssidChanged || (!staysUnconfigured && password kind != Keep).
static void test_restart_wifi_flag() {
    // "" -> "" (stays unconfigured): no restart, although the password is forced to Clear.
    cfg::MergeResult r = mergeOk(cfg::Config(), "{\"wifi\":{\"ssid\":\"\"}}");
    TEST_ASSERT_FALSE(r.restartWifi);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Kind::Clear), static_cast<int>(r.secrets.wifiPassword.kind));
    r = mergeOk(cfg::Config(), "{\"wifi\":{\"ssid\":\"\",\"password\":\"newpass123\"}}");
    TEST_ASSERT_FALSE(r.restartWifi);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Kind::Clear), static_cast<int>(r.secrets.wifiPassword.kind));
    r = mergeOk(cfg::Config(), "{\"wifi\":{\"password\":null}}");
    TEST_ASSERT_FALSE(r.restartWifi);
    r = mergeOk(cfg::Config(), "{\"wifi\":{}}");
    TEST_ASSERT_FALSE(r.restartWifi);
    // SSID change: restart (new SSID, first SSID, and unconfigure).
    r = mergeOk(configWithSsid("Home"), "{\"wifi\":{\"ssid\":\"Office\",\"password\":\"office-pass\"}}");
    TEST_ASSERT_TRUE(r.restartWifi);
    r = mergeOk(cfg::Config(), "{\"wifi\":{\"ssid\":\"Home\",\"password\":null}}");
    TEST_ASSERT_TRUE(r.restartWifi);
    r = mergeOk(configWithSsid("Home"), "{\"wifi\":{\"ssid\":\"\"}}");
    TEST_ASSERT_TRUE(r.restartWifi);
    // Same SSID + new password / cleared password: restart.
    r = mergeOk(configWithSsid("Home"), "{\"wifi\":{\"ssid\":\"Home\",\"password\":\"newpass123\"}}");
    TEST_ASSERT_TRUE(r.restartWifi);
    r = mergeOk(configWithSsid("Home"), "{\"wifi\":{\"ssid\":\"Home\",\"password\":null}}");
    TEST_ASSERT_TRUE(r.restartWifi);
    // Same SSID, password kept (absent or ""): no restart.
    r = mergeOk(configWithSsid("Home"), "{\"wifi\":{\"ssid\":\"Home\"}}");
    TEST_ASSERT_FALSE(r.restartWifi);
    r = mergeOk(configWithSsid("Home"), "{\"wifi\":{\"ssid\":\"Home\",\"password\":\"\"}}");
    TEST_ASSERT_FALSE(r.restartWifi);
    // No wifi section at all: no restart.
    r = mergeOk(configWithSsid("Home"), "{\"device\":{\"brightness\":50}}");
    TEST_ASSERT_FALSE(r.restartWifi);
    r = mergeOk(configWithSsid("Home"), "{\"web\":{\"password\":\"x\"},\"ha\":{}}");
    TEST_ASSERT_FALSE(r.restartWifi);
}

static void test_merge_failure_resets_secret_changes() {
    // A valid secret followed by an invalid section: nothing must be applied.
    const cfg::Config cur = configWithSsid("Home");
    const cfg::MergeResult r =
        merge(cur, "{\"web\":{\"password\":\"abc\"},\"device\":{\"brightness\":1}}");
    TEST_ASSERT_FALSE(r.ok);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Kind::Keep), static_cast<int>(r.secrets.webPassword.kind));
    TEST_ASSERT_FALSE(r.restartWifi);
    TEST_ASSERT_TRUE(r.config == cur);
}

// ---------------------------------------------------------------------------
// makeApPassword
// ---------------------------------------------------------------------------

static void test_ap_password() {
    gCounter = 0;
    const std::string pw = cfg::makeApPassword(counterRng);
    TEST_ASSERT_EQUAL_size_t(10, pw.size());
    TEST_ASSERT_TRUE(pw.size() >= 8);  // WPA2 minimum
    // Deterministic with an injected RNG.
    gCounter = 0;
    TEST_ASSERT_EQUAL_STRING(pw.c_str(), cfg::makeApPassword(counterRng).c_str());

    // Over 32 consecutive values every alphabet character appears; no look-alikes.
    gCounter = 0;
    const std::string all = cfg::makeApPassword(counterRng, 32);
    std::set<char> chars(all.begin(), all.end());
    TEST_ASSERT_EQUAL_size_t(32, chars.size());
    for (char c : all) {
        TEST_ASSERT_TRUE((c >= 'a' && c <= 'z') || (c >= '2' && c <= '9'));
        TEST_ASSERT_TRUE(c != 'l' && c != 'o' && c != '0' && c != '1');
    }
    TEST_ASSERT_EQUAL_size_t(10, cfg::makeApPassword(constRng).size());
    TEST_ASSERT_EQUAL_size_t(16, cfg::makeApPassword(constRng, 16).size());
}

// ---------------------------------------------------------------------------
// DomainActions table (§3.5)
// ---------------------------------------------------------------------------

static void test_domain_table_contents() {
    size_t n = 0;
    const da::DomainSpec *all = da::domains(n);
    TEST_ASSERT_NOT_NULL(all);
    TEST_ASSERT_EQUAL_size_t(7, n);
    const char *names[] = {"light", "climate", "cover", "fan", "media_player", "switch", "input_boolean"};
    for (const char *name : names) TEST_ASSERT_NOT_NULL_MESSAGE(da::findDomain(name), name);
    TEST_ASSERT_NULL(da::findDomain("sensor"));
    TEST_ASSERT_NULL(da::findDomain(""));
    TEST_ASSERT_NULL(da::findDomain("Light"));

    struct Expect {
        const char *domain;
        const char *defaultAction;
        double stepDefault, stepMin, stepMax;
        const char *defaultPress;
    };
    const Expect ex[] = {
        {"light", "brightness", 5, 1, 25, "toggle"},
        {"climate", "target_temp", 0.5, 0.1, 5, "none"},
        {"cover", "position", 10, 1, 25, "toggle"},
        {"fan", "percentage", 10, 1, 50, "toggle"},
        {"media_player", "volume", 2, 1, 20, "play_pause"},
    };
    for (const Expect &e : ex) {
        const da::DomainSpec *d = da::findDomain(e.domain);
        TEST_ASSERT_EQUAL_STRING(e.defaultAction, d->actions[0].name);
        TEST_ASSERT_TRUE(d->actions[0].hasStep);
        ASSERT_DOUBLE_EQ(e.stepDefault, d->actions[0].stepDefault);
        ASSERT_DOUBLE_EQ(e.stepMin, d->actions[0].stepMin);
        ASSERT_DOUBLE_EQ(e.stepMax, d->actions[0].stepMax);
        TEST_ASSERT_EQUAL_STRING(e.defaultPress, d->presses[0]);
    }
    const da::DomainSpec *light = da::findDomain("light");
    const da::ActionSpec *hue = da::findAction(*light, "hue");
    TEST_ASSERT_NOT_NULL(hue);
    ASSERT_DOUBLE_EQ(10, hue->stepDefault);
    ASSERT_DOUBLE_EQ(1, hue->stepMin);
    ASSERT_DOUBLE_EQ(60, hue->stepMax);
    const da::ActionSpec *ct = da::findAction(*light, "color_temp");
    TEST_ASSERT_NOT_NULL(ct);
    ASSERT_DOUBLE_EQ(100, ct->stepDefault);
    ASSERT_DOUBLE_EQ(25, ct->stepMin);
    ASSERT_DOUBLE_EQ(1000, ct->stepMax);

    for (const char *sw : {"switch", "input_boolean"}) {
        const da::DomainSpec *d = da::findDomain(sw);
        TEST_ASSERT_EQUAL_STRING("none", d->actions[0].name);
        TEST_ASSERT_FALSE(d->actions[0].hasStep);
        TEST_ASSERT_NULL(da::findAction(*d, "brightness"));
        TEST_ASSERT_EQUAL_STRING("toggle", d->presses[0]);
        TEST_ASSERT_TRUE(da::hasPress(*d, "none"));
    }

    // Press options per §3.5.
    TEST_ASSERT_TRUE(da::hasPress(*da::findDomain("cover"), "stop"));
    TEST_ASSERT_TRUE(da::hasPress(*da::findDomain("cover"), "none"));
    TEST_ASSERT_TRUE(da::hasPress(*da::findDomain("climate"), "toggle"));
    TEST_ASSERT_TRUE(da::hasPress(*da::findDomain("media_player"), "mute"));
    TEST_ASSERT_TRUE(da::hasPress(*da::findDomain("media_player"), "toggle"));
    TEST_ASSERT_FALSE(da::hasPress(*da::findDomain("light"), "stop"));
    TEST_ASSERT_FALSE(da::hasPress(*da::findDomain("fan"), "mute"));
    TEST_ASSERT_FALSE(da::hasPress(*light, ""));
}

static void test_domain_defaults_within_range_and_presses_unique() {
    size_t n = 0;
    const da::DomainSpec *all = da::domains(n);
    for (size_t i = 0; i < n; ++i) {
        const da::DomainSpec &d = all[i];
        TEST_ASSERT_TRUE(d.actionCount >= 1);
        TEST_ASSERT_TRUE(d.pressCount >= 1);
        TEST_ASSERT_NOT_NULL(da::findAction(d, "none"));
        TEST_ASSERT_TRUE(da::hasPress(d, "none"));
        for (size_t a = 0; a < d.actionCount; ++a) {
            const da::ActionSpec &s = d.actions[a];
            if (s.hasStep) {
                TEST_ASSERT_TRUE(s.stepMin > 0);
                TEST_ASSERT_TRUE(s.stepMin <= s.stepDefault && s.stepDefault <= s.stepMax);
                TEST_ASSERT_TRUE(da::stepInRange(s, s.stepDefault));
            }
        }
    }
}

static void test_domain_of_and_step_in_range() {
    TEST_ASSERT_TRUE(da::domainOf("light.kitchen") == "light");
    TEST_ASSERT_TRUE(da::domainOf("media_player.lr") == "media_player");
    TEST_ASSERT_TRUE(da::domainOf("nodot").empty());
    TEST_ASSERT_TRUE(da::domainOf("").empty());
    TEST_ASSERT_TRUE(da::domainOf(".x").empty());

    const da::ActionSpec *t = da::findAction(*da::findDomain("climate"), "target_temp");
    TEST_ASSERT_TRUE(da::stepInRange(*t, 0.1));
    TEST_ASSERT_TRUE(da::stepInRange(*t, 0.30000000000000004));
    TEST_ASSERT_TRUE(da::stepInRange(*t, 5));
    TEST_ASSERT_FALSE(da::stepInRange(*t, 0.09));
    TEST_ASSERT_FALSE(da::stepInRange(*t, 5.01));
    const da::ActionSpec *none = da::findAction(*da::findDomain("switch"), "none");
    TEST_ASSERT_TRUE(da::stepInRange(*none, -100));
}

int main() {
    UNITY_BEGIN();
    // Defaults, round trip, loading
    RUN_TEST(test_defaults_match_spec);
    RUN_TEST(test_round_trip_spec_example_string_equal);
    RUN_TEST(test_round_trip_full_config_struct_equal);
    RUN_TEST(test_round_trip_json_escapes_in_labels);
    RUN_TEST(test_to_json_has_schema_and_no_secrets);
    RUN_TEST(test_missing_sections_give_defaults);
    RUN_TEST(test_missing_keys_inside_section_give_defaults);
    RUN_TEST(test_unknown_keys_ignored);
    RUN_TEST(test_missing_or_empty_input_is_missing);
    RUN_TEST(test_corrupt_json_gives_defaults);
    RUN_TEST(test_schema_2_rejected_keeps_defaults);
    RUN_TEST(test_schema_non_numeric_is_invalid);
    RUN_TEST(test_schema_older_or_missing_is_migrated);
    RUN_TEST(test_wrong_types_invalid_give_defaults);
    RUN_TEST(test_stored_value_out_of_range_invalid_gives_defaults);
    RUN_TEST(test_load_status_names);
    // Validation
    RUN_TEST(test_validate_hostname);
    RUN_TEST(test_validate_brightness);
    RUN_TEST(test_validate_dim_level);
    RUN_TEST(test_validate_timers);
    RUN_TEST(test_validate_send_interval);
    RUN_TEST(test_validate_ssid);
    RUN_TEST(test_validate_ha_url);
    RUN_TEST(test_parse_url_endpoint);
    RUN_TEST(test_validate_entity_id);
    RUN_TEST(test_validate_unsupported_domain);
    RUN_TEST(test_validate_label);
    RUN_TEST(test_validate_controls_count);
    RUN_TEST(test_load_25_controls_rejected);
    RUN_TEST(test_validate_step_ranges);
    RUN_TEST(test_validate_action_invalid_for_domain);
    RUN_TEST(test_validate_press_invalid_for_domain);
    RUN_TEST(test_validate_step_ignored_for_none_action);
    RUN_TEST(test_validate_reports_index_of_bad_control);
    RUN_TEST(test_duplicate_entity_ids_allowed);
    // Size
    RUN_TEST(test_24_controls_48_byte_labels_fit_limit);
    RUN_TEST(test_oversize_config_too_large);
    // Redaction
    RUN_TEST(test_redacted_flags_and_no_secret_values);
    RUN_TEST(test_redacted_json_loads_back_as_config);
    // mergePatch
    RUN_TEST(test_merge_invalid_json);
    RUN_TEST(test_merge_empty_patch_keeps_everything);
    RUN_TEST(test_merge_absent_sections_kept);
    RUN_TEST(test_merge_device_section_replace_is_literal);
    RUN_TEST(test_merge_controls_section_replaces_list);
    RUN_TEST(test_merge_controls_get_domain_defaults);
    RUN_TEST(test_merge_controls_invalid_rejected);
    RUN_TEST(test_merge_device_out_of_range_rejected);
    RUN_TEST(test_merge_duplicates_allowed);
    RUN_TEST(test_merge_web_password_rules);
    RUN_TEST(test_merge_ha_token_format);
    // Token guard
    RUN_TEST(test_token_guard_same_url_keeps_token);
    RUN_TEST(test_token_guard_new_host_without_token_rejected);
    RUN_TEST(test_token_guard_new_host_with_token_accepted);
    RUN_TEST(test_token_guard_first_url_requires_token);
    RUN_TEST(test_token_guard_url_cleared_clears_token);
    RUN_TEST(test_token_guard_path_only_change_keeps_token);
    RUN_TEST(test_token_guard_host_case_and_default_port_normalised);
    RUN_TEST(test_token_guard_tls_upgrade_keeps_token);
    RUN_TEST(test_token_guard_tls_downgrade_requires_token);
    RUN_TEST(test_token_guard_invalid_url_rejected);
    // Wi-Fi guard
    RUN_TEST(test_wifi_unchanged_ssid_secret_rules);
    RUN_TEST(test_wifi_password_format);
    RUN_TEST(test_wifi_ssid_change_requires_password);
    RUN_TEST(test_wifi_ssid_change_with_password_or_null_accepted);
    RUN_TEST(test_wifi_ssid_cleared_clears_password);
    RUN_TEST(test_wifi_ssid_invalid_rejected_in_patch);
    RUN_TEST(test_empty_url_forces_token_clear_even_if_supplied);
    RUN_TEST(test_empty_ssid_forces_password_clear_even_if_supplied);
    RUN_TEST(test_non_empty_url_and_ssid_not_forced_clear);
    RUN_TEST(test_restart_wifi_flag);
    RUN_TEST(test_merge_failure_resets_secret_changes);
    // AP password
    RUN_TEST(test_ap_password);
    // DomainActions
    RUN_TEST(test_domain_table_contents);
    RUN_TEST(test_domain_defaults_within_range_and_presses_unique);
    RUN_TEST(test_domain_of_and_step_in_range);
    return UNITY_END();
}
