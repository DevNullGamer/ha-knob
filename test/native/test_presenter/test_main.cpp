// Native tests for Presenter number formatting and MathUtil::percentToByte
// (plan.md §4 Step 2).
#include <unity.h>

#include <cstdint>
#include <limits>
#include <string>

#include "ui/Presenter.h"
#include "util/MathUtil.h"

using core::presenter::decimalsForStep;
using core::presenter::formatNumber;
using core::presenter::formatPercent;
using core::presenter::formatStepped;

void setUp() {}
void tearDown() {}

static void test_stepped_climate_half_degree() {
    TEST_ASSERT_EQUAL_STRING("21.5", formatStepped(21.5, 0.5).c_str());
    TEST_ASSERT_EQUAL_STRING("20.0", formatStepped(20.0, 0.5).c_str());
    TEST_ASSERT_EQUAL_STRING("20.0", formatStepped(20, 0.5).c_str());
}

static void test_stepped_integer_steps() {
    TEST_ASSERT_EQUAL_STRING("3200", formatStepped(3200, 100).c_str());
    TEST_ASSERT_EQUAL_STRING("21", formatStepped(21, 1).c_str());
    TEST_ASSERT_EQUAL_STRING("120", formatStepped(120, 10).c_str());
}

static void test_stepped_other_fractional_steps() {
    TEST_ASSERT_EQUAL_STRING("21.0", formatStepped(21, 0.1).c_str());
    TEST_ASSERT_EQUAL_STRING("21.3", formatStepped(21.3, 0.1).c_str());
    TEST_ASSERT_EQUAL_STRING("21.25", formatStepped(21.25, 0.25).c_str());
    TEST_ASSERT_EQUAL_STRING("21.50", formatStepped(21.5, 0.25).c_str());
}

static void test_stepped_float_noise_is_rounded_away() {
    // 0.1 + 0.2 style accumulation must not leak into the display.
    TEST_ASSERT_EQUAL_STRING("0.3", formatStepped(0.1 + 0.2, 0.1).c_str());
    TEST_ASSERT_EQUAL_STRING("21.5", formatStepped(21.499999999, 0.5).c_str());
}

static void test_stepped_negative_values() {
    TEST_ASSERT_EQUAL_STRING("-5.5", formatStepped(-5.5, 0.5).c_str());
    TEST_ASSERT_EQUAL_STRING("-10", formatStepped(-10, 1).c_str());
}

static void test_no_negative_zero() {
    TEST_ASSERT_EQUAL_STRING("0.0", formatStepped(-0.0, 0.5).c_str());
    TEST_ASSERT_EQUAL_STRING("0.0", formatStepped(-0.04, 0.5).c_str());
    TEST_ASSERT_EQUAL_STRING("0", formatStepped(-0.4, 1).c_str());
    TEST_ASSERT_EQUAL_STRING("0", formatPercent(-0.0).c_str());
    TEST_ASSERT_EQUAL_STRING("0", formatPercent(-0.4).c_str());
    TEST_ASSERT_EQUAL_STRING("0.00", formatNumber(-0.001, 2).c_str());
}

static void test_percent_is_integer() {
    TEST_ASSERT_EQUAL_STRING("45", formatPercent(45).c_str());
    TEST_ASSERT_EQUAL_STRING("45", formatPercent(45.4).c_str());
    TEST_ASSERT_EQUAL_STRING("46", formatPercent(45.6).c_str());
    TEST_ASSERT_EQUAL_STRING("100", formatPercent(99.5).c_str());
    TEST_ASSERT_EQUAL_STRING("0", formatPercent(0).c_str());
    TEST_ASSERT_EQUAL_STRING("100", formatPercent(100).c_str());
    const std::string s = formatPercent(33.333);
    TEST_ASSERT_EQUAL_STRING("33", s.c_str());
    TEST_ASSERT_TRUE(s.find('.') == std::string::npos);
}

static void test_decimals_for_step() {
    TEST_ASSERT_EQUAL_INT(0, decimalsForStep(1));
    TEST_ASSERT_EQUAL_INT(0, decimalsForStep(100));
    TEST_ASSERT_EQUAL_INT(1, decimalsForStep(0.5));
    TEST_ASSERT_EQUAL_INT(1, decimalsForStep(0.1));
    TEST_ASSERT_EQUAL_INT(2, decimalsForStep(0.25));
    TEST_ASSERT_EQUAL_INT(2, decimalsForStep(0.01));
    TEST_ASSERT_EQUAL_INT(0, decimalsForStep(0));
    TEST_ASSERT_EQUAL_INT(0, decimalsForStep(-0.5));
}

static void test_format_number_clamps_decimals() {
    TEST_ASSERT_EQUAL_STRING("2", formatNumber(1.6, -3).c_str());
    TEST_ASSERT_EQUAL_STRING("1.500000", formatNumber(1.5, 99).c_str());
}

// ---------------------------------------------------------------------------
// percentToByte: brightness 0-100 % -> 0-255 (rounded, clamped).
// ---------------------------------------------------------------------------

static void test_percent_to_byte_endpoints() {
    TEST_ASSERT_EQUAL_UINT8(0, core::percentToByte(0));
    TEST_ASSERT_EQUAL_UINT8(255, core::percentToByte(100));
}

static void test_percent_to_byte_rounding() {
    TEST_ASSERT_EQUAL_UINT8(3, core::percentToByte(1));      // 2.55 -> 3
    TEST_ASSERT_EQUAL_UINT8(128, core::percentToByte(50));   // 127.5 -> 128
    TEST_ASSERT_EQUAL_UINT8(64, core::percentToByte(25));    // 63.75 -> 64
    TEST_ASSERT_EQUAL_UINT8(191, core::percentToByte(75));   // 191.25 -> 191
    TEST_ASSERT_EQUAL_UINT8(252, core::percentToByte(99));   // 252.45 -> 252
    TEST_ASSERT_EQUAL_UINT8(204, core::percentToByte(80));   // 204 exact
}

static void test_percent_to_byte_monotonic() {
    int prev = -1;
    for (int p = 0; p <= 100; ++p) {
        const int b = core::percentToByte(p);
        TEST_ASSERT_TRUE(b > prev);  // strictly increasing: each % step is distinct
        prev = b;
    }
}

static void test_percent_to_byte_clamps_out_of_range() {
    TEST_ASSERT_EQUAL_UINT8(0, core::percentToByte(-1));
    TEST_ASSERT_EQUAL_UINT8(0, core::percentToByte(-500));
    TEST_ASSERT_EQUAL_UINT8(255, core::percentToByte(101));
    TEST_ASSERT_EQUAL_UINT8(255, core::percentToByte(1000));
    TEST_ASSERT_EQUAL_UINT8(0, core::percentToByte(std::numeric_limits<int32_t>::min()));
    TEST_ASSERT_EQUAL_UINT8(255, core::percentToByte(std::numeric_limits<int32_t>::max()));
}

// ---------------------------------------------------------------------------
// present() / ControlView (plan-step6.md section 3.7, Step 6.8)
// ---------------------------------------------------------------------------

using core::presenter::ControlView;
using core::presenter::PresentContext;
using core::presenter::present;
using core::presenter::waitOver;

static core::config::Control ctl(const char *action, const char *entityId = "light.x",
                                 const char *label = "", double step = 0) {
    core::config::Control c;
    c.entityId = entityId;
    c.label = label;
    c.action = action;
    c.step = step;
    return c;
}

static core::ha::EntityState ent(const char *state) {
    core::ha::EntityState e;
    e.state.assign(state);
    return e;
}

static PresentContext ready() {
    PresentContext c;
    c.haReady = true;
    c.snapshotSeen = true;
    return c;
}

#define ASSERT_STR(exp, act) TEST_ASSERT_EQUAL_STRING((exp), (act).c_str())
#define DEG "\xC2\xB0"

static void test_brightness() {
    core::ha::EntityState e = ent("on");
    e.attrs.brightness = 255;
    ControlView v = present(ctl("brightness"), &e, ready());
    ASSERT_STR("100", v.value);
    ASSERT_STR("%", v.unit);
    TEST_ASSERT_TRUE(v.hasArc);
    TEST_ASSERT_EQUAL_FLOAT(0, v.arcMin);
    TEST_ASSERT_EQUAL_FLOAT(100, v.arcMax);
    TEST_ASSERT_EQUAL_FLOAT(100, v.arcValue);
    TEST_ASSERT_FALSE(v.dimmed);
    ASSERT_STR("on", v.stateText);
    e.attrs.brightness = 128;
    v = present(ctl("brightness"), &e, ready());
    ASSERT_STR("50", v.value);
    TEST_ASSERT_EQUAL_FLOAT(50, v.arcValue);
    e.attrs.brightness = 0;
    v = present(ctl("brightness"), &e, ready());
    ASSERT_STR("0", v.value);
}

static void test_brightness_off_and_absent() {
    core::ha::EntityState e = ent("off");
    e.attrs.brightness = 200;
    ControlView v = present(ctl("brightness"), &e, ready());
    ASSERT_STR("0", v.value);
    ASSERT_STR("%", v.unit);
    TEST_ASSERT_EQUAL_FLOAT(0, v.arcValue);
    core::ha::EntityState on = ent("on");  // null / absent brightness
    v = present(ctl("brightness"), &on, ready());
    ASSERT_STR("0", v.value);
    ASSERT_STR("%", v.unit);
    TEST_ASSERT_EQUAL_FLOAT(0, v.arcValue);
}

static void test_brightness_unavailable_dimmed() {
    core::ha::EntityState e = ent("unavailable");
    ControlView v = present(ctl("brightness"), &e, ready());
    ASSERT_STR("0", v.value);
    TEST_ASSERT_TRUE(v.dimmed);
    ASSERT_STR("unavailable", v.stateText);
}

static void test_hue() {
    core::ha::EntityState e = ent("on");
    e.attrs.hue = 120;
    ControlView v = present(ctl("hue"), &e, ready());
    ASSERT_STR("120", v.value);
    ASSERT_STR(DEG, v.unit);
    TEST_ASSERT_TRUE(v.hasArc);
    TEST_ASSERT_EQUAL_FLOAT(0, v.arcMin);
    TEST_ASSERT_EQUAL_FLOAT(360, v.arcMax);
    TEST_ASSERT_EQUAL_FLOAT(120, v.arcValue);
    e.attrs.hue = 359.6;
    v = present(ctl("hue"), &e, ready());
    ASSERT_STR("360", v.value);
    TEST_ASSERT_TRUE(v.arcValue <= 360.0);
}

static void test_hue_absent() {
    core::ha::EntityState e = ent("on");
    ControlView v = present(ctl("hue"), &e, ready());
    ASSERT_STR("--", v.value);
    ASSERT_STR("", v.unit);
    TEST_ASSERT_EQUAL_FLOAT(0, v.arcValue);
    TEST_ASSERT_TRUE(v.hasArc);
}

static void test_color_temp_range() {
    core::ha::EntityState e = ent("on");
    e.attrs.colorTempKelvin = 4000;
    e.attrs.minColorTempKelvin = 2202;
    e.attrs.maxColorTempKelvin = 6535;
    ControlView v = present(ctl("color_temp"), &e, ready());
    ASSERT_STR("4000", v.value);
    ASSERT_STR("K", v.unit);
    TEST_ASSERT_EQUAL_FLOAT(2202, v.arcMin);
    TEST_ASSERT_EQUAL_FLOAT(6535, v.arcMax);
    TEST_ASSERT_EQUAL_FLOAT(4000, v.arcValue);
}

static void test_color_temp_default_and_bad_ranges() {
    core::ha::EntityState e = ent("on");
    e.attrs.colorTempKelvin = 3000;
    ControlView v = present(ctl("color_temp"), &e, ready());
    TEST_ASSERT_EQUAL_FLOAT(2000, v.arcMin);
    TEST_ASSERT_EQUAL_FLOAT(6500, v.arcMax);
    e.attrs.minColorTempKelvin = 5000;
    e.attrs.maxColorTempKelvin = 5000;  // min >= max
    v = present(ctl("color_temp"), &e, ready());
    TEST_ASSERT_EQUAL_FLOAT(2000, v.arcMin);
    TEST_ASSERT_EQUAL_FLOAT(6500, v.arcMax);
    e.attrs.minColorTempKelvin = 6000;
    e.attrs.maxColorTempKelvin = 3000;
    v = present(ctl("color_temp"), &e, ready());
    TEST_ASSERT_EQUAL_FLOAT(2000, v.arcMin);
    TEST_ASSERT_EQUAL_FLOAT(6500, v.arcMax);
}

static void test_color_temp_clamped_to_range() {
    core::ha::EntityState e = ent("on");
    e.attrs.minColorTempKelvin = 2202;
    e.attrs.maxColorTempKelvin = 6535;
    e.attrs.colorTempKelvin = 2000;
    ControlView v = present(ctl("color_temp"), &e, ready());
    ASSERT_STR("2000", v.value);
    TEST_ASSERT_EQUAL_FLOAT(2202, v.arcValue);
    e.attrs.colorTempKelvin = 7000;
    v = present(ctl("color_temp"), &e, ready());
    TEST_ASSERT_EQUAL_FLOAT(6535, v.arcValue);
}

static void test_target_temp_formatting() {
    core::ha::EntityState e = ent("heat");
    e.attrs.temperature = 21.5;
    ControlView v = present(ctl("target_temp", "climate.x", "", 0.5), &e, ready());
    ASSERT_STR("21.5", v.value);
    ASSERT_STR(DEG, v.unit);
    e.attrs.temperature = 20;
    v = present(ctl("target_temp", "climate.x", "", 0.5), &e, ready());
    ASSERT_STR("20.0", v.value);
    e.attrs.temperature = 21.5;
    v = present(ctl("target_temp", "climate.x", "", 1), &e, ready());
    ASSERT_STR("22", v.value);
}

static void test_target_temp_step_fallbacks() {
    core::ha::EntityState e = ent("heat");
    e.attrs.temperature = 21;
    e.attrs.targetTempStep = 1;
    ControlView v = present(ctl("target_temp", "climate.x", "", 0), &e, ready());
    ASSERT_STR("21", v.value);  // step 1 from target_temp_step
    e.attrs.targetTempStep.reset();
    v = present(ctl("target_temp", "climate.x", "", 0), &e, ready());
    ASSERT_STR("21.0", v.value);  // 0.5
}

static void test_target_temp_null_and_current() {
    core::ha::EntityState e = ent("heat");
    ControlView v = present(ctl("target_temp", "climate.x", "", 0.5), &e, ready());
    ASSERT_STR("--", v.value);
    ASSERT_STR("", v.unit);
    e.attrs.currentTemperature = 20.25;
    e.attrs.temperature = 21;
    v = present(ctl("target_temp", "climate.x", "", 0.5), &e, ready());
    ASSERT_STR("heat, now 20.3" DEG, v.stateText);
    core::ha::EntityState e2 = ent("");
    e2.attrs.currentTemperature = 20.25;
    v = present(ctl("target_temp", "climate.x", "", 0.5), &e2, ready());
    ASSERT_STR("now 20.3" DEG, v.stateText);
}

static void test_target_temp_range() {
    core::ha::EntityState e = ent("heat");
    e.attrs.temperature = 21;
    ControlView v = present(ctl("target_temp", "climate.x", "", 0.5), &e, ready());
    TEST_ASSERT_EQUAL_FLOAT(7, v.arcMin);
    TEST_ASSERT_EQUAL_FLOAT(35, v.arcMax);
    e.attrs.minTemp = 10;
    e.attrs.maxTemp = 30;
    v = present(ctl("target_temp", "climate.x", "", 0.5), &e, ready());
    TEST_ASSERT_EQUAL_FLOAT(10, v.arcMin);
    TEST_ASSERT_EQUAL_FLOAT(30, v.arcMax);
    TEST_ASSERT_EQUAL_FLOAT(21, v.arcValue);
}

static void test_position_percentage_volume() {
    core::ha::EntityState e = ent("open");
    e.attrs.currentPosition = 30;
    ControlView v = present(ctl("position", "cover.x"), &e, ready());
    ASSERT_STR("30", v.value);
    ASSERT_STR("%", v.unit);
    TEST_ASSERT_EQUAL_FLOAT(30, v.arcValue);
    core::ha::EntityState none = ent("open");
    v = present(ctl("position", "cover.x"), &none, ready());
    ASSERT_STR("--", v.value);

    core::ha::EntityState f = ent("on");
    f.attrs.percentage = 33.3;
    v = present(ctl("percentage", "fan.x"), &f, ready());
    ASSERT_STR("33", v.value);
    ASSERT_STR("%", v.unit);

    core::ha::EntityState m = ent("playing");
    m.attrs.volumeLevel = 0.42;
    v = present(ctl("volume", "media_player.x"), &m, ready());
    ASSERT_STR("42", v.value);
    ASSERT_STR("playing", v.stateText);
    m.attrs.isVolumeMuted = true;
    v = present(ctl("volume", "media_player.x"), &m, ready());
    ASSERT_STR("playing, muted", v.stateText);
    m.attrs.isVolumeMuted = false;
    v = present(ctl("volume", "media_player.x"), &m, ready());
    ASSERT_STR("playing", v.stateText);
    core::ha::EntityState m2 = ent("playing");
    v = present(ctl("volume", "media_player.x"), &m2, ready());
    ASSERT_STR("--", v.value);
}

static void test_none_action() {
    core::ha::EntityState e = ent("on");
    ControlView v = present(ctl("none", "switch.x"), &e, ready());
    ASSERT_STR("on", v.value);
    TEST_ASSERT_FALSE(v.hasArc);
    ASSERT_STR("", v.stateText);
    TEST_ASSERT_FALSE(v.dimmed);
    // an unknown action behaves like none
    v = present(ctl("foo", "switch.x"), &e, ready());
    ASSERT_STR("on", v.value);
    TEST_ASSERT_FALSE(v.hasArc);
    ASSERT_STR("", v.stateText);
}

static void test_none_unavailable_unknown_empty() {
    const char *states[] = {"unavailable", "unknown", ""};
    for (const char *st : states) {
        core::ha::EntityState e = ent(st);
        ControlView v = present(ctl("none", "switch.x"), &e, ready());
        ASSERT_STR("--", v.value);
        ASSERT_STR(st, v.stateText);
        TEST_ASSERT_FALSE(v.hasArc);
        TEST_ASSERT_EQUAL(st[0] != 0, v.dimmed);
    }
}

static void test_title_precedence() {
    core::ha::EntityState e = ent("on");
    e.attrs.friendlyName.assign("Friendly");
    ASSERT_STR("Label", present(ctl("none", "switch.x", "Label"), &e, ready()).title);
    ASSERT_STR("Friendly", present(ctl("none", "switch.x"), &e, ready()).title);
    core::ha::EntityState bare = ent("on");
    ASSERT_STR("switch.x", present(ctl("none", "switch.x"), &bare, ready()).title);
    ASSERT_STR("switch.x", present(ctl("none", "switch.x"), nullptr, ready()).title);
    e.attrs.friendlyName.assign("K\xC3\xB8kken");
    ASSERT_STR("K\xC3\xB8kken", present(ctl("none", "switch.x"), &e, ready()).title);
}

static void test_missing_entity() {
    PresentContext c;
    c.haReady = true;
    c.snapshotSeen = false;
    const uint32_t waiting[] = {0, 2999};
    for (uint32_t ms : waiting) {
        c.msSinceSubscribe = ms;
        ControlView v = present(ctl("brightness"), nullptr, c);
        ASSERT_STR("waiting...", v.stateText);
        ASSERT_STR("--", v.value);
        TEST_ASSERT_TRUE(v.dimmed);
        TEST_ASSERT_FALSE(waitOver(c));
    }
    c.msSinceSubscribe = 3000;
    ControlView v = present(ctl("brightness"), nullptr, c);
    ASSERT_STR("not found", v.stateText);
    TEST_ASSERT_TRUE(v.dimmed);
    ASSERT_STR("--", v.value);
    TEST_ASSERT_TRUE(waitOver(c));
    c.msSinceSubscribe = 0;
    c.snapshotSeen = true;
    v = present(ctl("none"), nullptr, c);
    ASSERT_STR("not found", v.stateText);
    ASSERT_STR("--", v.value);
    TEST_ASSERT_TRUE(v.dimmed);
    c.haReady = false;
    v = present(ctl("brightness"), nullptr, c);
    ASSERT_STR("HA offline", v.stateText);
    TEST_ASSERT_TRUE(v.dimmed);
    ASSERT_STR("--", v.value);
}

static void test_wait_over_boundaries() {
    PresentContext c;
    c.msSinceSubscribe = 2999;
    TEST_ASSERT_FALSE(waitOver(c));
    c.msSinceSubscribe = 3000;
    TEST_ASSERT_TRUE(waitOver(c));
    c.msSinceSubscribe = 0;
    c.snapshotSeen = true;
    TEST_ASSERT_TRUE(waitOver(c));
    c.snapshotSeen = false;
    c.msSinceSubscribe = UINT32_MAX;
    TEST_ASSERT_TRUE(waitOver(c));
}

static void test_not_ready_keeps_values_dimmed() {
    PresentContext c = ready();
    c.haReady = false;
    core::ha::EntityState e = ent("on");
    e.attrs.brightness = 128;
    ControlView v = present(ctl("brightness"), &e, c);
    ASSERT_STR("50", v.value);
    TEST_ASSERT_TRUE(v.dimmed);
    ASSERT_STR("HA offline", v.stateText);
    core::ha::EntityState t = ent("heat");
    t.attrs.temperature = 21;
    t.attrs.currentTemperature = 20;
    v = present(ctl("target_temp", "climate.x", "", 0.5), &t, c);
    ASSERT_STR("21.0", v.value);
    ASSERT_STR("HA offline", v.stateText);
    core::ha::EntityState m = ent("playing");
    m.attrs.volumeLevel = 0.5;
    m.attrs.isVolumeMuted = true;
    v = present(ctl("volume", "media_player.x"), &m, c);
    ASSERT_STR("50", v.value);
    ASSERT_STR("HA offline", v.stateText);
    core::ha::EntityState n = ent("on");
    v = present(ctl("none", "switch.x"), &n, c);
    ASSERT_STR("on", v.value);
    ASSERT_STR("HA offline", v.stateText);
    TEST_ASSERT_TRUE(v.dimmed);
}

static void test_view_equality_per_field() {
    core::ha::EntityState e = ent("on");
    e.attrs.brightness = 128;
    const ControlView a = present(ctl("brightness"), &e, ready());
    ControlView b = a;
    TEST_ASSERT_TRUE(a == b);
    TEST_ASSERT_FALSE(a != b);
    b = a; b.title += "x";        TEST_ASSERT_TRUE(a != b);
    b = a; b.value += "x";        TEST_ASSERT_TRUE(a != b);
    b = a; b.unit += "x";         TEST_ASSERT_TRUE(a != b);
    b = a; b.hasArc = !b.hasArc;  TEST_ASSERT_TRUE(a != b);
    b = a; b.arcMin += 1;         TEST_ASSERT_TRUE(a != b);
    b = a; b.arcMax += 1;         TEST_ASSERT_TRUE(a != b);
    b = a; b.arcValue += 1;       TEST_ASSERT_TRUE(a != b);
    b = a; b.stateText += "x";    TEST_ASSERT_TRUE(a != b);
    b = a; b.dimmed = !b.dimmed;  TEST_ASSERT_TRUE(a != b);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_brightness);
    RUN_TEST(test_brightness_off_and_absent);
    RUN_TEST(test_brightness_unavailable_dimmed);
    RUN_TEST(test_hue);
    RUN_TEST(test_hue_absent);
    RUN_TEST(test_color_temp_range);
    RUN_TEST(test_color_temp_default_and_bad_ranges);
    RUN_TEST(test_color_temp_clamped_to_range);
    RUN_TEST(test_target_temp_formatting);
    RUN_TEST(test_target_temp_step_fallbacks);
    RUN_TEST(test_target_temp_null_and_current);
    RUN_TEST(test_target_temp_range);
    RUN_TEST(test_position_percentage_volume);
    RUN_TEST(test_none_action);
    RUN_TEST(test_none_unavailable_unknown_empty);
    RUN_TEST(test_title_precedence);
    RUN_TEST(test_missing_entity);
    RUN_TEST(test_wait_over_boundaries);
    RUN_TEST(test_not_ready_keeps_values_dimmed);
    RUN_TEST(test_view_equality_per_field);
    RUN_TEST(test_stepped_climate_half_degree);
    RUN_TEST(test_stepped_integer_steps);
    RUN_TEST(test_stepped_other_fractional_steps);
    RUN_TEST(test_stepped_float_noise_is_rounded_away);
    RUN_TEST(test_stepped_negative_values);
    RUN_TEST(test_no_negative_zero);
    RUN_TEST(test_percent_is_integer);
    RUN_TEST(test_decimals_for_step);
    RUN_TEST(test_format_number_clamps_decimals);
    RUN_TEST(test_percent_to_byte_endpoints);
    RUN_TEST(test_percent_to_byte_rounding);
    RUN_TEST(test_percent_to_byte_monotonic);
    RUN_TEST(test_percent_to_byte_clamps_out_of_range);
    return UNITY_END();
}
