// Native tests for DomainActions value math and service mapping
// (plan-step7.md sections 3.1, Step 7.1, Q7-4, Q7-5, Amendment A1).
#include <unity.h>

#include <ArduinoJson.h>

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>

#include "control/DomainActions.h"
#include "ha/EntityAttrs.h"
#include "ha/EntityStore.h"
#include "ha/HaProtocol.h"

namespace da = core::domain_actions;
namespace ha = core::ha;

void setUp() {}
void tearDown() {}

namespace {

const double kNaN = std::numeric_limits<double>::quiet_NaN();

ha::EntityState ent(const char *state, const char *attrsJson = "{}") {
    ha::EntityState e;
    e.state = state;
    JsonDocument doc;
    TEST_ASSERT_TRUE(!deserializeJson(doc, attrsJson));
    ha::applyAttrs(doc.as<JsonObjectConst>(), e.attrs, ha::AttrMode::Replace);
    return e;
}

const da::DomainSpec &dom(const char *name) {
    const da::DomainSpec *d = da::findDomain(name);
    TEST_ASSERT_NOT_NULL(d);
    return *d;
}

const da::ActionSpec &act(const char *domain, const char *name) {
    const da::ActionSpec *a = da::findAction(dom(domain), name);
    TEST_ASSERT_NOT_NULL(a);
    return *a;
}

const da::ValueRange kPct = {0, 100, false};
const da::ValueRange kHue = {0, 360, true};

// "domain.service" or the NoCall reason.
std::string describe(const da::CallBuild &b) {
    if (!b.call) return "none:" + std::to_string((int)b.reason);
    return b.call->domain + "." + b.call->service;
}

std::string dataOf(const da::CallBuild &b) {
    TEST_ASSERT_TRUE(b.call.has_value());
    std::string out;
    serializeJson(b.call->data, out);
    return out;
}

// The complete call_service frame, as it would go to HA.
std::string frameOf(const da::CallBuild &b) {
    TEST_ASSERT_TRUE(b.call.has_value());
    return ha::buildCallService(1, b.call->domain, b.call->service, b.call->data, b.call->entityId);
}

da::CallBuild enc(const char *domain, const char *action, double value, const ha::EntityState *e,
                  const char *id = "x.y") {
    return da::encoderCall(dom(domain), act(domain, action), id, value, e);
}

void expectCall(const char *service, const char *data, const da::CallBuild &b) {
    TEST_ASSERT_EQUAL_STRING(service, describe(b).c_str());
    TEST_ASSERT_EQUAL_STRING(data, dataOf(b).c_str());
}

}  // namespace

// ---- valueRange ----

static void test_range_fixed() {
    for (const char *a : {"brightness"}) {
        da::ValueRange r = da::valueRange(act("light", a), ha::EntityAttrs());
        TEST_ASSERT_EQUAL_FLOAT(0, r.min);
        TEST_ASSERT_EQUAL_FLOAT(100, r.max);
        TEST_ASSERT_FALSE(r.wrap);
    }
    TEST_ASSERT_EQUAL_FLOAT(100, da::valueRange(act("cover", "position"), ha::EntityAttrs()).max);
    TEST_ASSERT_EQUAL_FLOAT(100, da::valueRange(act("fan", "percentage"), ha::EntityAttrs()).max);
    TEST_ASSERT_EQUAL_FLOAT(100, da::valueRange(act("media_player", "volume"), ha::EntityAttrs()).max);
    da::ValueRange hue = da::valueRange(act("light", "hue"), ha::EntityAttrs());
    TEST_ASSERT_EQUAL_FLOAT(0, hue.min);
    TEST_ASSERT_EQUAL_FLOAT(360, hue.max);
    TEST_ASSERT_TRUE(hue.wrap);
}

static void test_range_color_temp() {
    const da::ActionSpec &a = act("light", "color_temp");
    ha::EntityState e = ent("on", "{\"min_color_temp_kelvin\":2202,\"max_color_temp_kelvin\":6535}");
    da::ValueRange r = da::valueRange(a, e.attrs);
    TEST_ASSERT_EQUAL_FLOAT(2202, r.min);
    TEST_ASSERT_EQUAL_FLOAT(6535, r.max);
    TEST_ASSERT_FALSE(r.wrap);
    r = da::valueRange(a, ha::EntityAttrs());
    TEST_ASSERT_EQUAL_FLOAT(2000, r.min);
    TEST_ASSERT_EQUAL_FLOAT(6500, r.max);
    e = ent("on", "{\"min_color_temp_kelvin\":3000}");  // only one bound
    TEST_ASSERT_EQUAL_FLOAT(2000, da::valueRange(a, e.attrs).min);
    e = ent("on", "{\"min_color_temp_kelvin\":5000,\"max_color_temp_kelvin\":5000}");  // max <= min
    TEST_ASSERT_EQUAL_FLOAT(6500, da::valueRange(a, e.attrs).max);
    e = ent("on", "{\"min_color_temp_kelvin\":6000,\"max_color_temp_kelvin\":3000}");
    TEST_ASSERT_EQUAL_FLOAT(2000, da::valueRange(a, e.attrs).min);
    ha::EntityAttrs nan;
    nan.minColorTempKelvin = kNaN;
    nan.maxColorTempKelvin = 6000;
    TEST_ASSERT_EQUAL_FLOAT(2000, da::valueRange(a, nan).min);
}

static void test_range_target_temp() {
    const da::ActionSpec &a = act("climate", "target_temp");
    ha::EntityState e = ent("heat", "{\"min_temp\":10,\"max_temp\":30.5}");
    da::ValueRange r = da::valueRange(a, e.attrs);
    TEST_ASSERT_EQUAL_FLOAT(10, r.min);
    TEST_ASSERT_EQUAL_FLOAT(30.5, r.max);
    r = da::valueRange(a, ha::EntityAttrs());
    TEST_ASSERT_EQUAL_FLOAT(7, r.min);
    TEST_ASSERT_EQUAL_FLOAT(35, r.max);
    e = ent("heat", "{\"min_temp\":30,\"max_temp\":10}");
    TEST_ASSERT_EQUAL_FLOAT(7, da::valueRange(a, e.attrs).min);
}

static void test_range_none() {
    da::ValueRange r = da::valueRange(act("light", "none"), ha::EntityAttrs());
    TEST_ASSERT_EQUAL_FLOAT(0, r.min);
    TEST_ASSERT_EQUAL_FLOAT(0, r.max);
    TEST_ASSERT_FALSE(r.wrap);
}

// ---- stepValue ----

static void test_step_basic_grid() {
    TEST_ASSERT_EQUAL_FLOAT(65, da::stepValue(50, 3, 5, kPct));
    TEST_ASSERT_EQUAL_FLOAT(100, da::stepValue(98, 1, 5, kPct));
    TEST_ASSERT_EQUAL_FLOAT(0, da::stepValue(2, -1, 5, kPct));
    TEST_ASSERT_EQUAL_FLOAT(50, da::stepValue(47, 1, 5, kPct));
    TEST_ASSERT_EQUAL_FLOAT(40, da::stepValue(47, -1, 5, kPct));
    TEST_ASSERT_EQUAL_FLOAT(55, da::stepValue(47.5, 1, 5, kPct));
    TEST_ASSERT_EQUAL_FLOAT(45, da::stepValue(47.5, -1, 5, kPct));
    TEST_ASSERT_EQUAL_FLOAT(50, da::stepValue(50, 0, 5, kPct));
}

static void test_step_color_temp() {
    const da::ValueRange r = {2202, 6535, false};
    TEST_ASSERT_EQUAL_FLOAT(2800, da::stepValue(2702, 1, 100, r));
    TEST_ASSERT_EQUAL_FLOAT(6400, da::stepValue(6535, -1, 100, r));
    TEST_ASSERT_EQUAL_FLOAT(6535, da::stepValue(6500, 5, 100, r));
    TEST_ASSERT_EQUAL_FLOAT(2202, da::stepValue(2250, -1, 100, r));
}

static void test_base_color_temp_absent_is_snapped_mid_range() {
    const da::ActionSpec &a = act("light", "color_temp");
    ha::EntityState e = ent("on", "{\"min_color_temp_kelvin\":2202,\"max_color_temp_kelvin\":6535}");
    std::optional<double> b = da::baseValue(a, e, 100);
    TEST_ASSERT_TRUE(b.has_value());
    TEST_ASSERT_EQUAL_FLOAT(4400, *b);
    b = da::baseValue(a, e, 25);
    TEST_ASSERT_TRUE(b.has_value());
    TEST_ASSERT_EQUAL_FLOAT(4375, *b);
}

static void test_step_target_temp() {
    const da::ValueRange r = {7, 30, false};
    TEST_ASSERT_EQUAL_FLOAT(22.0, da::stepValue(21.3, 1, 0.5, r));
    TEST_ASSERT_EQUAL_FLOAT(22.0, da::stepValue(21.5, 1, 0.5, r));
    TEST_ASSERT_EQUAL_FLOAT(30, da::stepValue(30, 1, 0.5, r));
    TEST_ASSERT_EQUAL_FLOAT(30, da::stepValue(29.8, 1, 0.5, r));
    TEST_ASSERT_EQUAL_FLOAT(7, da::stepValue(7.2, -1, 0.5, r));
}

static void test_step_no_drift() {
    const da::ValueRange r = {7, 35, false};
    double v = 20.0;
    for (int i = 0; i < 100; ++i) v = da::stepValue(v, 1, 0.1, r);
    TEST_ASSERT_TRUE(v == 30.0);
    for (int i = 0; i < 100; ++i) v = da::stepValue(v, -1, 0.1, r);
    TEST_ASSERT_TRUE(v == 20.0);

    double acc = 20.0;
    for (int i = 0; i < 100; ++i) acc = da::stepValue(acc, 1, 0.3, r);
    TEST_ASSERT_TRUE(acc == da::stepValue(20.0, 100, 0.3, r));

    TEST_ASSERT_TRUE(da::stepValue(20.1, 1, 0.25, r) == 20.25);
    TEST_ASSERT_TRUE(da::stepValue(20.25, 1, 0.25, r) == 20.5);
}

static void test_step_hue_wraps_unsnapped() {
    TEST_ASSERT_EQUAL_FLOAT(15, da::stepValue(355, 2, 10, kHue));
    TEST_ASSERT_EQUAL_FLOAT(355, da::stepValue(5, -1, 10, kHue));
    TEST_ASSERT_EQUAL_FLOAT(16, da::stepValue(355.5, 2, 10, kHue));  // 375.5 -> 15.5 -> 16
    TEST_ASSERT_EQUAL_FLOAT(0, da::stepValue(359.6, 0, 1, kHue));      // never 360
    TEST_ASSERT_EQUAL_FLOAT(0, da::stepValue(358.6, 1, 1, kHue));
    TEST_ASSERT_EQUAL_FLOAT(359, da::stepValue(0, -1, 1, kHue));
    TEST_ASSERT_EQUAL_FLOAT(0, da::stepValue(-720, 0, 10, kHue));
    TEST_ASSERT_EQUAL_FLOAT(350, da::stepValue(-10, 0, 10, kHue));
    // unsnapped: a base off the grid keeps its offset
    TEST_ASSERT_EQUAL_FLOAT(127, da::stepValue(117, 1, 10, kHue));
}

static void test_step_hue_never_returns_max() {
    for (double base = 358.0; base < 360.0; base += 0.07) {
        const double v = da::stepValue(base, 0, 1, kHue);
        TEST_ASSERT_TRUE(v >= 0 && v < 360);
    }
}

static void test_step_non_finite_and_bad_step() {
    TEST_ASSERT_EQUAL_FLOAT(0, da::stepValue(kNaN, 0, 5, kPct));
    TEST_ASSERT_EQUAL_FLOAT(5, da::stepValue(kNaN, 1, 5, kPct));
    TEST_ASSERT_EQUAL_FLOAT(0, da::stepValue(-std::numeric_limits<double>::infinity(), 0, 5, kPct));
    // step <= 0: clamp / wrap only
    TEST_ASSERT_EQUAL_FLOAT(50, da::stepValue(50, 3, 0, kPct));
    TEST_ASSERT_EQUAL_FLOAT(50.5, da::stepValue(50.5, 3, -5, kPct));
    TEST_ASSERT_EQUAL_FLOAT(100, da::stepValue(150, 1, 0, kPct));
    TEST_ASSERT_EQUAL_FLOAT(0, da::stepValue(-5, 1, 0, kPct));
    TEST_ASSERT_EQUAL_FLOAT(10, da::stepValue(370, 0, 0, kHue));
    TEST_ASSERT_EQUAL_FLOAT(50, da::stepValue(50, 3, kNaN, kPct));
}

// ---- currentValue / baseValue ----

static void test_current_brightness() {
    const da::ActionSpec &a = act("light", "brightness");
    ha::EntityState e = ent("on", "{\"brightness\":255}");
    TEST_ASSERT_EQUAL_FLOAT(100, *da::currentValue(a, e));
    e = ent("on", "{\"brightness\":128}");
    TEST_ASSERT_EQUAL_FLOAT(50, *da::currentValue(a, e));
    e = ent("off", "{\"brightness\":128}");
    TEST_ASSERT_EQUAL_FLOAT(0, *da::currentValue(a, e));
    e = ent("on");
    TEST_ASSERT_EQUAL_FLOAT(0, *da::currentValue(a, e));
    e = ent("on", "{\"brightness\":0}");
    TEST_ASSERT_EQUAL_FLOAT(0, *da::currentValue(a, e));
    e = ent("on", "{\"brightness\":1}");
    TEST_ASSERT_EQUAL_FLOAT(0, *da::currentValue(a, e));  // round(0.39)
    e = ent("on", "{\"brightness\":300}");
    TEST_ASSERT_EQUAL_FLOAT(100, *da::currentValue(a, e));  // clamped
}

static void test_current_other_values() {
    ha::EntityState e = ent("on", "{\"hs_color\":[200.5,40],\"color_temp_kelvin\":3000}");
    TEST_ASSERT_EQUAL_FLOAT(200.5, *da::currentValue(act("light", "hue"), e));
    TEST_ASSERT_EQUAL_FLOAT(3000, *da::currentValue(act("light", "color_temp"), e));
    e = ent("heat", "{\"temperature\":21.5}");
    TEST_ASSERT_EQUAL_FLOAT(21.5, *da::currentValue(act("climate", "target_temp"), e));
    e = ent("open", "{\"current_position\":40}");
    TEST_ASSERT_EQUAL_FLOAT(40, *da::currentValue(act("cover", "position"), e));
    e = ent("on", "{\"percentage\":66}");
    TEST_ASSERT_EQUAL_FLOAT(66, *da::currentValue(act("fan", "percentage"), e));
    e = ent("playing", "{\"volume_level\":0.42}");
    TEST_ASSERT_EQUAL_FLOAT(42, *da::currentValue(act("media_player", "volume"), e));
}

static void test_current_absent_values() {
    ha::EntityState on = ent("on");
    TEST_ASSERT_FALSE(da::currentValue(act("light", "hue"), on).has_value());
    TEST_ASSERT_FALSE(da::currentValue(act("light", "color_temp"), on).has_value());
    TEST_ASSERT_FALSE(da::currentValue(act("fan", "percentage"), on).has_value());
    TEST_ASSERT_EQUAL_FLOAT(0, *da::currentValue(act("fan", "percentage"), ent("off")));
    TEST_ASSERT_FALSE(da::currentValue(act("climate", "target_temp"), ent("heat", "{\"temperature\":null}")).has_value());
    TEST_ASSERT_FALSE(da::currentValue(act("cover", "position"), ent("open")).has_value());
    TEST_ASSERT_FALSE(da::currentValue(act("media_player", "volume"), ent("playing")).has_value());
    TEST_ASSERT_FALSE(da::currentValue(act("light", "none"), on).has_value());
    ha::EntityState nan = ent("on");
    nan.attrs.hue = kNaN;
    nan.attrs.currentPosition = kNaN;
    TEST_ASSERT_FALSE(da::currentValue(act("light", "hue"), nan).has_value());
    TEST_ASSERT_FALSE(da::currentValue(act("cover", "position"), nan).has_value());
}

static void test_base_values() {
    TEST_ASSERT_EQUAL_FLOAT(0, *da::baseValue(act("light", "hue"), ent("on"), 10));
    TEST_ASSERT_EQUAL_FLOAT(200, *da::baseValue(act("light", "hue"), ent("on", "{\"hs_color\":[200,50]}"), 10));
    TEST_ASSERT_EQUAL_FLOAT(0, *da::baseValue(act("light", "brightness"), ent("on"), 5));
    TEST_ASSERT_FALSE(da::baseValue(act("climate", "target_temp"), ent("heat", "{\"temperature\":null}"), 0.5).has_value());
    TEST_ASSERT_FALSE(da::baseValue(act("cover", "position"), ent("open"), 10).has_value());
    TEST_ASSERT_FALSE(da::baseValue(act("media_player", "volume"), ent("on"), 2).has_value());
    TEST_ASSERT_FALSE(da::baseValue(act("fan", "percentage"), ent("on"), 10).has_value());
    TEST_ASSERT_EQUAL_FLOAT(0, *da::baseValue(act("fan", "percentage"), ent("off"), 10));
}

// ---- encoderCall ----

static void test_enc_brightness() {
    ha::EntityState e = ent("on", "{\"brightness\":128}");
    da::CallBuild b = enc("light", "brightness", 0, &e, "light.k");
    TEST_ASSERT_EQUAL_STRING("light.turn_off", describe(b).c_str());
    TEST_ASSERT_EQUAL_STRING("light.k", b.call->entityId.c_str());
    TEST_ASSERT_TRUE(b.call->data.isNull());
    TEST_ASSERT_EQUAL_STRING(
        "{\"id\":1,\"type\":\"call_service\",\"domain\":\"light\",\"service\":\"turn_off\","
        "\"target\":{\"entity_id\":\"light.k\"}}",
        frameOf(b).c_str());
    b = enc("light", "brightness", 0.4, &e);
    TEST_ASSERT_EQUAL_STRING("light.turn_off", describe(b).c_str());
    TEST_ASSERT_TRUE(b.call->data.isNull());
    expectCall("light.turn_on", "{\"brightness_pct\":5}", enc("light", "brightness", 5, &e));
    expectCall("light.turn_on", "{\"brightness_pct\":100}", enc("light", "brightness", 100, &e));
    expectCall("light.turn_on", "{\"brightness_pct\":1}", enc("light", "brightness", 0.6, &e));
    TEST_ASSERT_EQUAL_STRING(
        "{\"id\":1,\"type\":\"call_service\",\"domain\":\"light\",\"service\":\"turn_on\","
        "\"service_data\":{\"brightness_pct\":40},\"target\":{\"entity_id\":\"light.k\"}}",
        frameOf(enc("light", "brightness", 40, &e, "light.k")).c_str());
}

static void test_enc_hue() {
    ha::EntityState e = ent("on", "{\"hs_color\":[10,40.5]}");
    expectCall("light.turn_on", "{\"hs_color\":[120,40.5]}", enc("light", "hue", 120, &e));
    expectCall("light.turn_on", "{\"hs_color\":[0,40.5]}", enc("light", "hue", 359.6, &e));
    expectCall("light.turn_on", "{\"hs_color\":[0,40.5]}", enc("light", "hue", 0, &e));
    e = ent("on", "{\"hs_color\":[10,0]}");
    expectCall("light.turn_on", "{\"hs_color\":[120,100]}", enc("light", "hue", 120, &e));
    e = ent("on");  // hue and saturation absent
    expectCall("light.turn_on", "{\"hs_color\":[30,100]}", enc("light", "hue", 30, &e));
}

static void test_enc_color_temp() {
    ha::EntityState e = ent("on", "{\"color_temp_kelvin\":3000}");
    expectCall("light.turn_on", "{\"color_temp_kelvin\":2800}", enc("light", "color_temp", 2800, &e));
    e = ent("on");  // absent: Q7-5 start value
    expectCall("light.turn_on", "{\"color_temp_kelvin\":4300}", enc("light", "color_temp", 4300, &e));
}

static void test_enc_position_temperature_volume() {
    ha::EntityState cover = ent("open", "{\"current_position\":30}");
    expectCall("cover.set_cover_position", "{\"position\":40}", enc("cover", "position", 40, &cover));
    expectCall("cover.set_cover_position", "{\"position\":0}", enc("cover", "position", 0, &cover));

    ha::EntityState climate = ent("heat", "{\"temperature\":21}");
    expectCall("climate.set_temperature", "{\"temperature\":22}", enc("climate", "target_temp", 22.0, &climate));
    expectCall("climate.set_temperature", "{\"temperature\":21.4}", enc("climate", "target_temp", 21.4, &climate));
    expectCall("climate.set_temperature", "{\"temperature\":21.5}", enc("climate", "target_temp", 21.5, &climate));

    ha::EntityState mp = ent("playing", "{\"volume_level\":0.4}");
    expectCall("media_player.volume_set", "{\"volume_level\":0}", enc("media_player", "volume", 0, &mp));
    expectCall("media_player.volume_set", "{\"volume_level\":0.07}", enc("media_player", "volume", 7, &mp));
    expectCall("media_player.volume_set", "{\"volume_level\":0.29}", enc("media_player", "volume", 29, &mp));
    expectCall("media_player.volume_set", "{\"volume_level\":0.42}", enc("media_player", "volume", 42, &mp));
    expectCall("media_player.volume_set", "{\"volume_level\":1}", enc("media_player", "volume", 100, &mp));
    // 40 + 1 x 2 -> 0.42
    const double v = da::stepValue(*da::baseValue(act("media_player", "volume"), mp, 2), 1, 2, kPct);
    expectCall("media_player.volume_set", "{\"volume_level\":0.42}", enc("media_player", "volume", v, &mp));
}

static void test_enc_percentage_floor() {
    ha::EntityState fan = ent("on", "{\"percentage\":33}");
    expectCall("fan.set_percentage", "{\"percentage\":66}", enc("fan", "percentage", 66.667, &fan));
    expectCall("fan.set_percentage", "{\"percentage\":100}", enc("fan", "percentage", 99.99999999, &fan));
    expectCall("fan.set_percentage", "{\"percentage\":30}", enc("fan", "percentage", 29.9999999, &fan));
    expectCall("fan.set_percentage", "{\"percentage\":30}", enc("fan", "percentage", 30.0000004, &fan));
    expectCall("fan.set_percentage", "{\"percentage\":0}", enc("fan", "percentage", 0, &fan));
    expectCall("fan.set_percentage", "{\"percentage\":29}", enc("fan", "percentage", 29.99, &fan));
}

static void test_fan_speed_grid_one_detent_one_speed() {
    for (int n = 2; n <= 10; ++n) {
        // step as HA reports it, through the ArduinoJson serialisation round trip
        JsonDocument sd;
        sd["percentage_step"] = 100.0 / n;
        std::string stepJson;
        serializeJson(sd, stepJson);
        ha::EntityState probe = ent("on", stepJson.c_str());
        const double step = da::stepDefaultFor(act("fan", "percentage"), probe.attrs);
        TEST_ASSERT_TRUE(step > 0);

        for (int k = 0; k <= n; ++k) {
            const int p = k == 0 ? 0 : (int)(std::floor(k * 100.0 / n + 1e-9));  // int(k * 100 // n)
            for (int dir = -1; dir <= 1; dir += 2) {
                const int expected = k + dir < 0 ? 0 : (k + dir > n ? n : k + dir);
                ha::EntityState fan = k == 0 ? ent("off") : ent("on", "{}");
                fan.attrs.percentage = p;
                const std::optional<double> base = da::baseValue(act("fan", "percentage"), fan, step);
                TEST_ASSERT_TRUE(base.has_value());
                const double value = da::stepValue(*base, dir, step, kPct);
                da::CallBuild b = enc("fan", "percentage", value, &fan);
                TEST_ASSERT_TRUE(b.call.has_value());
                const int q = b.call->data["percentage"].as<int>();
                const int speed = (n * q + 99) / 100;  // ceil(n * q / 100)
                char msg[64];
                std::snprintf(msg, sizeof(msg), "n=%d k=%d dir=%d p=%d q=%d", n, k, dir, p, q);
                TEST_ASSERT_EQUAL_INT_MESSAGE(expected, speed, msg);
            }
        }
    }
}

static void test_fan_grid_examples() {
    ha::EntityState fan = ent("on", "{\"percentage\":33,\"percentage_step\":33.33333333}");
    const da::ActionSpec &a = act("fan", "percentage");
    const double step = da::stepDefaultFor(a, fan.attrs);
    TEST_ASSERT_EQUAL_FLOAT(33.33333333, step);
    // n=3: 66.667 -> 66
    expectCall("fan.set_percentage", "{\"percentage\":66}", enc("fan", "percentage", 66.667, &fan));
    // 3 x 33.33333333 from off -> 100
    ha::EntityState off = ent("off");
    const double v = da::stepValue(*da::baseValue(a, off, step), 3, step, kPct);
    expectCall("fan.set_percentage", "{\"percentage\":100}", enc("fan", "percentage", v, &off));
    // n=7: 57.143 -> 57
    expectCall("fan.set_percentage", "{\"percentage\":57}", enc("fan", "percentage", 57.143, &fan));
}

static void test_enc_nocall_order() {
    ha::EntityState on = ent("on", "{\"brightness\":100}");
    // NoAction first: "none", an action of another domain, even with entity nullptr
    da::CallBuild b = da::encoderCall(dom("switch"), act("switch", "none"), "switch.s", 1, nullptr);
    TEST_ASSERT_FALSE(b.call.has_value());
    TEST_ASSERT_TRUE(b.reason == da::NoCall::NoAction);
    b = da::encoderCall(dom("cover"), act("light", "brightness"), "cover.c", 50, &on);
    TEST_ASSERT_TRUE(b.reason == da::NoCall::NoAction);
    b = da::encoderCall(dom("cover"), act("light", "brightness"), "cover.c", 50, nullptr);
    TEST_ASSERT_TRUE(b.reason == da::NoCall::NoAction);
    // NotFound
    b = enc("light", "brightness", 50, nullptr);
    TEST_ASSERT_TRUE(b.reason == da::NoCall::NotFound);
    // Unavailable
    for (const char *s : {"unavailable", "unknown", ""}) {
        ha::EntityState e = ent(s, "{\"brightness\":100}");
        b = enc("light", "brightness", 50, &e);
        TEST_ASSERT_FALSE(b.call.has_value());
        TEST_ASSERT_TRUE_MESSAGE(b.reason == da::NoCall::Unavailable, s);
    }
    // Unavailable comes before NoValue
    ha::EntityState na = ent("unavailable");
    TEST_ASSERT_TRUE(enc("cover", "position", 50, &na).reason == da::NoCall::Unavailable);
    // NoValue
    ha::EntityState cover = ent("open");
    TEST_ASSERT_TRUE(enc("cover", "position", 50, &cover).reason == da::NoCall::NoValue);
    ha::EntityState climate = ent("heat", "{\"temperature\":null}");
    TEST_ASSERT_TRUE(enc("climate", "target_temp", 21, &climate).reason == da::NoCall::NoValue);
    ha::EntityState mp = ent("playing");
    TEST_ASSERT_TRUE(enc("media_player", "volume", 20, &mp).reason == da::NoCall::NoValue);
    ha::EntityState fan = ent("on");
    TEST_ASSERT_TRUE(enc("fan", "percentage", 20, &fan).reason == da::NoCall::NoValue);
    TEST_ASSERT_TRUE(enc("light", "brightness", kNaN, &on).reason == da::NoCall::NoValue);
    // a call was built
    TEST_ASSERT_TRUE(enc("light", "brightness", 50, &on).reason == da::NoCall::None);
}

// ---- pressCall ----

static da::CallBuild press(const char *domain, const char *p, const ha::EntityState *e) {
    return da::pressCall(dom(domain), p, "x.y", e);
}

static void test_press_toggle() {
    ha::EntityState on = ent("on");
    for (const char *d : {"light", "fan", "cover", "media_player", "switch", "input_boolean"}) {
        da::CallBuild b = press(d, "toggle", &on);
        TEST_ASSERT_EQUAL_STRING((std::string(d) + ".toggle").c_str(), describe(b).c_str());
        TEST_ASSERT_TRUE(b.call->data.isNull());
        TEST_ASSERT_EQUAL_STRING("x.y", b.call->entityId.c_str());
    }
}

static void test_press_climate_toggle() {
    ha::EntityState heat = ent("heat");
    ha::EntityState on = ent("on");
    ha::EntityState off = ent("off");
    TEST_ASSERT_EQUAL_STRING("climate.turn_on", describe(press("climate", "toggle", &off)).c_str());
    TEST_ASSERT_EQUAL_STRING("climate.turn_off", describe(press("climate", "toggle", &heat)).c_str());
    TEST_ASSERT_EQUAL_STRING("climate.turn_off", describe(press("climate", "toggle", &on)).c_str());
    TEST_ASSERT_TRUE(press("climate", "toggle", &off).call->data.isNull());
}

static void test_press_stop_and_play_pause() {
    ha::EntityState e = ent("opening");
    da::CallBuild b = press("cover", "stop", &e);
    TEST_ASSERT_EQUAL_STRING("cover.stop_cover", describe(b).c_str());
    TEST_ASSERT_TRUE(b.call->data.isNull());
    e = ent("playing");
    b = press("media_player", "play_pause", &e);
    TEST_ASSERT_EQUAL_STRING("media_player.media_play_pause", describe(b).c_str());
    TEST_ASSERT_TRUE(b.call->data.isNull());
}

static void test_press_mute() {
    ha::EntityState e = ent("playing");
    expectCall("media_player.volume_mute", "{\"is_volume_muted\":true}", press("media_player", "mute", &e));
    e = ent("playing", "{\"is_volume_muted\":true}");
    expectCall("media_player.volume_mute", "{\"is_volume_muted\":false}", press("media_player", "mute", &e));
    e = ent("playing", "{\"is_volume_muted\":false}");
    expectCall("media_player.volume_mute", "{\"is_volume_muted\":true}", press("media_player", "mute", &e));
}

static void test_press_no_action_and_problems() {
    ha::EntityState on = ent("on");
    TEST_ASSERT_TRUE(press("light", "none", &on).reason == da::NoCall::NoAction);
    TEST_ASSERT_TRUE(press("light", "none", nullptr).reason == da::NoCall::NoAction);
    TEST_ASSERT_TRUE(press("light", "stop", &on).reason == da::NoCall::NoAction);
    TEST_ASSERT_TRUE(press("light", "mute", nullptr).reason == da::NoCall::NoAction);
    TEST_ASSERT_TRUE(press("climate", "none", &on).reason == da::NoCall::NoAction);
    TEST_ASSERT_TRUE(press("switch", "bogus", &on).reason == da::NoCall::NoAction);
    TEST_ASSERT_TRUE(press("light", "toggle", nullptr).reason == da::NoCall::NotFound);
    for (const char *s : {"unavailable", "unknown", ""}) {
        ha::EntityState e = ent(s);
        TEST_ASSERT_TRUE_MESSAGE(press("light", "toggle", &e).reason == da::NoCall::Unavailable, s);
    }
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_range_fixed);
    RUN_TEST(test_range_color_temp);
    RUN_TEST(test_range_target_temp);
    RUN_TEST(test_range_none);
    RUN_TEST(test_step_basic_grid);
    RUN_TEST(test_step_color_temp);
    RUN_TEST(test_base_color_temp_absent_is_snapped_mid_range);
    RUN_TEST(test_step_target_temp);
    RUN_TEST(test_step_no_drift);
    RUN_TEST(test_step_hue_wraps_unsnapped);
    RUN_TEST(test_step_hue_never_returns_max);
    RUN_TEST(test_step_non_finite_and_bad_step);
    RUN_TEST(test_current_brightness);
    RUN_TEST(test_current_other_values);
    RUN_TEST(test_current_absent_values);
    RUN_TEST(test_base_values);
    RUN_TEST(test_enc_brightness);
    RUN_TEST(test_enc_hue);
    RUN_TEST(test_enc_color_temp);
    RUN_TEST(test_enc_position_temperature_volume);
    RUN_TEST(test_enc_percentage_floor);
    RUN_TEST(test_fan_speed_grid_one_detent_one_speed);
    RUN_TEST(test_fan_grid_examples);
    RUN_TEST(test_enc_nocall_order);
    RUN_TEST(test_press_toggle);
    RUN_TEST(test_press_climate_toggle);
    RUN_TEST(test_press_stop_and_play_pause);
    RUN_TEST(test_press_mute);
    RUN_TEST(test_press_no_action_and_problems);
    return UNITY_END();
}
