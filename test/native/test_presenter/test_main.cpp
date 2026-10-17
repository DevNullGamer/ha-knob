// Native tests for Presenter number formatting and MathUtil::percentToByte
// (plan.md §4 Step 2).
#include <unity.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>

#include "control/DomainActions.h"
#include "ha/HaSession.h"
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


// ---------------------------------------------------------------------------
// Step 7.5: local value, hue colour, titleFor, percentage helper
// ---------------------------------------------------------------------------

using core::control::LocalValue;

static PresentContext readyLocal(double v) {
    PresentContext c = ready();
    c.local = LocalValue{v, true};
    return c;
}

static void test_local_brightness() {
    core::ha::EntityState e = ent("on");
    e.attrs.brightness = 255;
    ControlView v = present(ctl("brightness"), &e, readyLocal(65));
    ASSERT_STR("65", v.value);
    ASSERT_STR("%", v.unit);
    TEST_ASSERT_EQUAL_FLOAT(65, v.arcValue);
    TEST_ASSERT_TRUE(v.unconfirmed);
    TEST_ASSERT_FALSE(v.dimmed);
    ASSERT_STR("on", v.stateText);
    v = present(ctl("brightness"), &e, ready());
    TEST_ASSERT_FALSE(v.unconfirmed);
}

static void test_local_climate_keeps_state_line() {
    core::ha::EntityState e = ent("heat");
    e.attrs.temperature = 20;
    e.attrs.currentTemperature = 21;
    ControlView v = present(ctl("target_temp", "climate.c", "", 0.5), &e, readyLocal(22));
    ASSERT_STR("22.0", v.value);
    ASSERT_STR(DEG, v.unit);
    ASSERT_STR("heat, now 21.0" DEG, v.stateText);
    TEST_ASSERT_TRUE(v.unconfirmed);
    TEST_ASSERT_FALSE(v.dimmed);
}

static void test_local_volume_keeps_muted() {
    core::ha::EntityState e = ent("playing");
    e.attrs.volumeLevel = 0.1;
    e.attrs.isVolumeMuted = true;
    ControlView v = present(ctl("volume", "media_player.m"), &e, readyLocal(42));
    ASSERT_STR("42", v.value);
    ASSERT_STR("%", v.unit);
    ASSERT_STR("playing, muted", v.stateText);
    TEST_ASSERT_TRUE(v.unconfirmed);
}

static void test_local_kelvin() {
    core::ha::EntityState e = ent("on");
    e.attrs.colorTempKelvin = 4000;
    ControlView v = present(ctl("color_temp"), &e, readyLocal(2800));
    ASSERT_STR("2800", v.value);
    ASSERT_STR("K", v.unit);
    TEST_ASSERT_EQUAL_FLOAT(2800, v.arcValue);
}

static void test_local_position() {
    core::ha::EntityState e = ent("open");
    e.attrs.currentPosition = 10;
    ControlView v = present(ctl("position", "cover.c"), &e, readyLocal(80));
    ASSERT_STR("80", v.value);
    ASSERT_STR("%", v.unit);
}

static void test_local_fan_floored() {
    core::ha::EntityState e = ent("on");
    e.attrs.percentage = 10;
    ControlView v = present(ctl("percentage", "fan.f"), &e, readyLocal(66.6666667));
    ASSERT_STR("66", v.value);
    TEST_ASSERT_EQUAL_FLOAT(66, v.arcValue);
    v = present(ctl("percentage", "fan.f"), &e, readyLocal(99.9999999));
    ASSERT_STR("100", v.value);
    TEST_ASSERT_EQUAL_FLOAT(100, v.arcValue);
    v = present(ctl("percentage", "fan.f"), &e, readyLocal(33.3333333));
    ASSERT_STR("33", v.value);
    v = present(ctl("percentage", "fan.f"), &e, readyLocal(0));
    ASSERT_STR("0", v.value);
}

static void test_fan_off_without_percentage() {
    core::ha::EntityState off = ent("off");
    ControlView v = present(ctl("percentage", "fan.f"), &off, ready());
    ASSERT_STR("0", v.value);
    ASSERT_STR("%", v.unit);
    core::ha::EntityState on = ent("on");
    v = present(ctl("percentage", "fan.f"), &on, ready());
    ASSERT_STR("--", v.value);
    ASSERT_STR("", v.unit);
}

static void test_local_unavailable_ready_not_dimmed() {
    core::ha::EntityState e = ent("unavailable");
    ControlView v = present(ctl("brightness"), &e, readyLocal(30));
    TEST_ASSERT_FALSE(v.dimmed);
    ASSERT_STR("unavailable", v.stateText);
    ASSERT_STR("30", v.value);
    TEST_ASSERT_TRUE(v.unconfirmed);
}

static void test_local_not_ready_dimmed_offline() {
    core::ha::EntityState e = ent("on");
    PresentContext c = readyLocal(40);
    c.haReady = false;
    ControlView v = present(ctl("brightness"), &e, c);
    TEST_ASSERT_TRUE(v.dimmed);
    ASSERT_STR("HA offline", v.stateText);
    ASSERT_STR("40", v.value);
    TEST_ASSERT_TRUE(v.unconfirmed);
}

static void test_local_ignored_for_none_and_missing_entity() {
    core::ha::EntityState e = ent("on");
    ControlView v = present(ctl("none", "switch.s"), &e, readyLocal(50));
    TEST_ASSERT_FALSE(v.unconfirmed);
    ASSERT_STR("on", v.value);
    TEST_ASSERT_FALSE(v.hasArc);
    v = present(ctl("brightness"), nullptr, readyLocal(50));
    TEST_ASSERT_FALSE(v.unconfirmed);
    ASSERT_STR("--", v.value);
    ASSERT_STR("not found", v.stateText);
    PresentContext c = readyLocal(50);
    c.snapshotSeen = false;
    c.msSinceSubscribe = 0;
    v = present(ctl("brightness"), nullptr, c);
    ASSERT_STR("waiting...", v.stateText);
    TEST_ASSERT_FALSE(v.unconfirmed);
}

static void test_local_nan_shows_dashes() {
    core::ha::EntityState e = ent("on");
    e.attrs.brightness = 100;
    ControlView v = present(ctl("brightness"), &e, readyLocal(std::nan("")));
    ASSERT_STR("--", v.value);
    ASSERT_STR("", v.unit);
}

static void test_hue_to_rgb() {
    using core::presenter::hueToRgb;
    TEST_ASSERT_EQUAL_HEX32(0xFF0000, hueToRgb(0));
    TEST_ASSERT_EQUAL_HEX32(0x00FF00, hueToRgb(120));
    TEST_ASSERT_EQUAL_HEX32(0x0000FF, hueToRgb(240));
    TEST_ASSERT_EQUAL_HEX32(0xFFFF00, hueToRgb(60));
    TEST_ASSERT_EQUAL_HEX32(0x00FFFF, hueToRgb(180));
    TEST_ASSERT_EQUAL_HEX32(0xFF00FF, hueToRgb(300));
    TEST_ASSERT_EQUAL_HEX32(0xFF8000, hueToRgb(30));
    const uint32_t near360 = hueToRgb(359.6);
    TEST_ASSERT_EQUAL_HEX32(0xFF0000, near360 & 0xFF0000);
    TEST_ASSERT_EQUAL_HEX32(0, near360 & 0x00FF00);
    TEST_ASSERT_TRUE((near360 & 0xFF) > 0 && (near360 & 0xFF) < 0x10);
    TEST_ASSERT_EQUAL_HEX32(0x0000FF, hueToRgb(-120));
    TEST_ASSERT_EQUAL_HEX32(0xFF0000, hueToRgb(720));
    TEST_ASSERT_EQUAL_HEX32(0xFF0000, hueToRgb(-1e-20));
    TEST_ASSERT_EQUAL_HEX32(0xFF0000, hueToRgb(std::nan("")));
    TEST_ASSERT_EQUAL_HEX32(0xFF0000, hueToRgb(INFINITY));
    TEST_ASSERT_EQUAL_HEX32(0xFF0000, hueToRgb(-INFINITY));
}

static void test_arc_rgb_in_present() {
    core::ha::EntityState e = ent("on");
    e.attrs.hue = 120;
    ControlView v = present(ctl("hue"), &e, ready());
    TEST_ASSERT_TRUE(v.arcRgb.has_value());
    TEST_ASSERT_EQUAL_HEX32(0x00FF00, *v.arcRgb);
    v = present(ctl("hue"), &e, readyLocal(240));
    TEST_ASSERT_TRUE(v.arcRgb.has_value());
    TEST_ASSERT_EQUAL_HEX32(0x0000FF, *v.arcRgb);
    ASSERT_STR("240", v.value);
    TEST_ASSERT_TRUE(v.unconfirmed);
    core::ha::EntityState none = ent("on");  // absent hue
    v = present(ctl("hue"), &none, ready());
    TEST_ASSERT_FALSE(v.arcRgb.has_value());
    e.attrs.brightness = 100;  // other actions
    v = present(ctl("brightness"), &e, ready());
    TEST_ASSERT_FALSE(v.arcRgb.has_value());
    v = present(ctl("none"), &e, ready());
    TEST_ASSERT_FALSE(v.arcRgb.has_value());
    v = present(ctl("hue"), nullptr, ready());
    TEST_ASSERT_FALSE(v.arcRgb.has_value());
}

static void test_hue_359_6_text_stays_360() {
    core::ha::EntityState e = ent("on");
    e.attrs.hue = 359.6;
    ControlView v = present(ctl("hue"), &e, ready());
    ASSERT_STR("360", v.value);
    TEST_ASSERT_TRUE(v.arcRgb.has_value());
}

static void test_view_equality_sees_new_fields() {
    core::ha::EntityState e = ent("on");
    e.attrs.hue = 120;
    ControlView a = present(ctl("hue"), &e, ready());
    ControlView b = a;
    TEST_ASSERT_TRUE(a == b);
    b.unconfirmed = !a.unconfirmed;
    TEST_ASSERT_TRUE(a != b);
    b = a;
    b.arcRgb = 0x123456;
    TEST_ASSERT_TRUE(a != b);
    b.arcRgb.reset();
    TEST_ASSERT_TRUE(a != b);
}

static void test_title_for_fallbacks() {
    using core::presenter::titleFor;
    core::ha::EntityState e = ent("on");
    e.attrs.friendlyName.assign("Lamp");
    ASSERT_STR("Mine", titleFor(ctl("brightness", "light.x", "Mine"), &e));
    ASSERT_STR("Lamp", titleFor(ctl("brightness", "light.x"), &e));
    core::ha::EntityState bare = ent("on");
    ASSERT_STR("light.x", titleFor(ctl("brightness", "light.x"), &bare));
    ASSERT_STR("light.x", titleFor(ctl("brightness", "light.x"), nullptr));
    ASSERT_STR("Mine", titleFor(ctl("brightness", "light.x", "Mine"), nullptr));
}

static void test_percentage_helper_same_in_call_and_view() {
    namespace da = core::domain_actions;
    const da::DomainSpec *fan = da::findDomain("fan");
    TEST_ASSERT_NOT_NULL(fan);
    const da::ActionSpec *act = da::findAction(*fan, "percentage");
    TEST_ASSERT_NOT_NULL(act);
    core::ha::EntityState e = ent("on");
    e.attrs.percentage = 10;
    const double values[] = {0, 33.3333333, 50, 66.6666667, 99.9999999, 100};
    for (double val : values) {
        da::CallBuild b = da::encoderCall(*fan, *act, "fan.f", val, &e);
        TEST_ASSERT_TRUE(b.call.has_value());
        const int sent = b.call->data["percentage"].as<int>();
        ControlView v = present(ctl("percentage", "fan.f"), &e, readyLocal(val));
        TEST_ASSERT_EQUAL_STRING(std::to_string(sent).c_str(), v.value.c_str());
    }
}

// ---- Step 8.6: disconnected banner (offlineReason / offlineText / ControlView::banner) ----
// Presenter.h only forward-declares ha::State, so the full enum comes from HaSession.h.
using core::presenter::OfflineReason;
using core::presenter::offlineReason;
using core::presenter::offlineText;
using HaState = core::ha::State;

static void test_offline_reason_every_state_wifi_up() {
    TEST_ASSERT_EQUAL((int)OfflineReason::HaNotSetUp, (int)offlineReason(HaState::NoConfig, true));
    TEST_ASSERT_EQUAL((int)OfflineReason::Connecting, (int)offlineReason(HaState::Connecting, true));
    TEST_ASSERT_EQUAL((int)OfflineReason::Connecting, (int)offlineReason(HaState::Authenticating, true));
    TEST_ASSERT_EQUAL((int)OfflineReason::Connecting, (int)offlineReason(HaState::Subscribing, true));
    TEST_ASSERT_EQUAL((int)OfflineReason::None, (int)offlineReason(HaState::Ready, true));
    TEST_ASSERT_EQUAL((int)OfflineReason::TokenRejected, (int)offlineReason(HaState::AuthFailed, true));
    TEST_ASSERT_EQUAL((int)OfflineReason::Offline, (int)offlineReason(HaState::Backoff, true));
}

static void test_offline_reason_every_state_wifi_down() {
    // Ready wins over a missing Wi-Fi; every other state says "No Wi-Fi"
    TEST_ASSERT_EQUAL((int)OfflineReason::None, (int)offlineReason(HaState::Ready, false));
    const HaState others[] = {HaState::NoConfig, HaState::Connecting, HaState::Authenticating,
                              HaState::Subscribing, HaState::AuthFailed, HaState::Backoff};
    for (HaState s : others) {
        TEST_ASSERT_EQUAL((int)OfflineReason::NoWifi, (int)offlineReason(s, false));
    }
}

static void test_offline_text_each_reason() {
    TEST_ASSERT_EQUAL_STRING("", offlineText(OfflineReason::None));
    TEST_ASSERT_EQUAL_STRING("No Wi-Fi", offlineText(OfflineReason::NoWifi));
    TEST_ASSERT_EQUAL_STRING("HA not set up", offlineText(OfflineReason::HaNotSetUp));
    TEST_ASSERT_EQUAL_STRING("Connecting to HA...", offlineText(OfflineReason::Connecting));
    TEST_ASSERT_EQUAL_STRING("HA token rejected", offlineText(OfflineReason::TokenRejected));
    TEST_ASSERT_EQUAL_STRING("HA offline", offlineText(OfflineReason::Offline));
}

static void test_present_banner_per_reason() {
    const OfflineReason reasons[] = {OfflineReason::NoWifi, OfflineReason::HaNotSetUp,
                                     OfflineReason::Connecting, OfflineReason::TokenRejected,
                                     OfflineReason::Offline};
    for (OfflineReason r : reasons) {
        core::ha::EntityState e = ent("on");
        e.attrs.brightness = 255;
        PresentContext c = ready();
        c.haReady = false;
        c.offline = r;
        ControlView v = present(ctl("brightness"), &e, c);
        ASSERT_STR(offlineText(r), v.stateText);
        TEST_ASSERT_TRUE(v.banner);
        TEST_ASSERT_TRUE(v.dimmed);
        ASSERT_STR("100", v.value);  // the last values stay, only dimmed
    }
}

static void test_present_ready_has_no_banner() {
    core::ha::EntityState e = ent("on");
    e.attrs.brightness = 255;
    ControlView v = present(ctl("brightness"), &e, ready());
    TEST_ASSERT_FALSE(v.banner);
    TEST_ASSERT_FALSE(v.dimmed);
    ASSERT_STR("on", v.stateText);
    // an ignored offline reason while Ready
    PresentContext c = ready();
    c.offline = OfflineReason::NoWifi;
    v = present(ctl("brightness"), &e, c);
    TEST_ASSERT_FALSE(v.banner);
    ASSERT_STR("on", v.stateText);
    // Ready with an unavailable entity: dimmed, but no banner
    core::ha::EntityState u = ent("unavailable");
    v = present(ctl("brightness"), &u, ready());
    TEST_ASSERT_TRUE(v.dimmed);
    TEST_ASSERT_FALSE(v.banner);
    // Ready, entity missing after the grace time: "not found", no banner
    PresentContext m = ready();
    m.msSinceSubscribe = core::presenter::kNotFoundAfterMs;
    v = present(ctl("brightness"), nullptr, m);
    ASSERT_STR("not found", v.stateText);
    TEST_ASSERT_FALSE(v.banner);
}

static void test_present_local_not_ready_has_banner() {
    core::ha::EntityState e = ent("on");
    PresentContext c = readyLocal(40);
    c.haReady = false;
    c.offline = OfflineReason::Connecting;
    ControlView v = present(ctl("brightness"), &e, c);
    TEST_ASSERT_TRUE(v.banner);
    TEST_ASSERT_TRUE(v.dimmed);
    ASSERT_STR("Connecting to HA...", v.stateText);
    ASSERT_STR("40", v.value);
}

static void test_present_not_ready_missing_entity_banner() {
    PresentContext c;
    c.haReady = false;
    c.offline = OfflineReason::NoWifi;
    ControlView v = present(ctl("brightness"), nullptr, c);
    TEST_ASSERT_TRUE(v.banner);
    TEST_ASSERT_TRUE(v.dimmed);
    ASSERT_STR("No Wi-Fi", v.stateText);
}

static void test_present_not_ready_reason_none_falls_back_to_offline() {
    core::ha::EntityState e = ent("on");
    PresentContext c;
    c.haReady = false;
    c.offline = OfflineReason::None;  // caller inconsistency
    ControlView v = present(ctl("brightness"), &e, c);
    ASSERT_STR("HA offline", v.stateText);
    TEST_ASSERT_TRUE(v.banner);
    // the default context still gives "HA offline"
    v = present(ctl("brightness"), &e, PresentContext());
    ASSERT_STR("HA offline", v.stateText);
    TEST_ASSERT_TRUE(v.banner);
}

static void test_view_equality_includes_banner() {
    core::ha::EntityState e = ent("on");
    ControlView a = present(ctl("brightness"), &e, ready());
    ControlView b = a;
    TEST_ASSERT_TRUE(a == b);
    b.banner = !b.banner;
    TEST_ASSERT_FALSE(a == b);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_offline_reason_every_state_wifi_up);
    RUN_TEST(test_offline_reason_every_state_wifi_down);
    RUN_TEST(test_offline_text_each_reason);
    RUN_TEST(test_present_banner_per_reason);
    RUN_TEST(test_present_ready_has_no_banner);
    RUN_TEST(test_present_local_not_ready_has_banner);
    RUN_TEST(test_present_not_ready_missing_entity_banner);
    RUN_TEST(test_present_not_ready_reason_none_falls_back_to_offline);
    RUN_TEST(test_view_equality_includes_banner);
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
    RUN_TEST(test_local_brightness);
    RUN_TEST(test_local_climate_keeps_state_line);
    RUN_TEST(test_local_volume_keeps_muted);
    RUN_TEST(test_local_kelvin);
    RUN_TEST(test_local_position);
    RUN_TEST(test_local_fan_floored);
    RUN_TEST(test_fan_off_without_percentage);
    RUN_TEST(test_local_unavailable_ready_not_dimmed);
    RUN_TEST(test_local_not_ready_dimmed_offline);
    RUN_TEST(test_local_ignored_for_none_and_missing_entity);
    RUN_TEST(test_local_nan_shows_dashes);
    RUN_TEST(test_hue_to_rgb);
    RUN_TEST(test_arc_rgb_in_present);
    RUN_TEST(test_hue_359_6_text_stays_360);
    RUN_TEST(test_view_equality_sees_new_fields);
    RUN_TEST(test_title_for_fallbacks);
    RUN_TEST(test_percentage_helper_same_in_call_and_view);
    return UNITY_END();
}
