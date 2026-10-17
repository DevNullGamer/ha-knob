// Native tests for ui::IdleTimer (plan-step8.md section 3.1 / Step 8.1, testpoints-8.1-8.2.md).
#include <unity.h>

#include <cstdint>
#include <cstring>

#include "ui/IdleTimer.h"

using core::ButtonFsm;
using core::ui::IdleTimer;
using core::ui::panelSetting;
using core::ui::ScreenLevel;
using core::ui::screenLevelName;
using Ev = core::ButtonFsm::Event;

void setUp() {}
void tearDown() {}

static void test_default_timing() {
    IdleTimer t;
    t.configure(30, 300, 0);
    TEST_ASSERT_FALSE(t.tick(29999));
    TEST_ASSERT_TRUE(t.level() == ScreenLevel::On);
    TEST_ASSERT_TRUE(t.tick(30000));
    TEST_ASSERT_TRUE(t.level() == ScreenLevel::Dim);
    TEST_ASSERT_FALSE(t.tick(30001));
    TEST_ASSERT_FALSE(t.tick(299999));
    TEST_ASSERT_TRUE(t.level() == ScreenLevel::Dim);
    TEST_ASSERT_TRUE(t.tick(300000));
    TEST_ASSERT_TRUE(t.level() == ScreenLevel::Off);
    TEST_ASSERT_FALSE(t.tick(300001));
    TEST_ASSERT_FALSE(t.tick(900000));
}

static void test_dim_zero() {
    IdleTimer t;
    t.configure(0, 300, 0);
    TEST_ASSERT_FALSE(t.tick(299999));
    TEST_ASSERT_TRUE(t.level() == ScreenLevel::On);
    TEST_ASSERT_TRUE(t.tick(300000));
    TEST_ASSERT_TRUE(t.level() == ScreenLevel::Off);
}

static void test_off_zero() {
    IdleTimer t;
    t.configure(30, 0, 0);
    TEST_ASSERT_TRUE(t.tick(30000));
    TEST_ASSERT_TRUE(t.level() == ScreenLevel::Dim);
    TEST_ASSERT_FALSE(t.tick(1000000000u));
    TEST_ASSERT_TRUE(t.level() == ScreenLevel::Dim);
}

static void test_both_zero_never() {
    IdleTimer t;
    t.configure(0, 0, 0);
    for (uint32_t now = 0; now <= 1000000000u; now += 50000000u) TEST_ASSERT_FALSE(t.tick(now));
    TEST_ASSERT_FALSE(t.tick(1000000000u));
    TEST_ASSERT_TRUE(t.level() == ScreenLevel::On);
}

static void test_default_constructed_stays_on() {
    IdleTimer t;
    TEST_ASSERT_FALSE(t.tick(1000000000u));
    TEST_ASSERT_TRUE(t.level() == ScreenLevel::On);
}

static void test_off_before_dim() {
    IdleTimer t;
    t.configure(60, 20, 0);
    TEST_ASSERT_FALSE(t.tick(19999));
    TEST_ASSERT_TRUE(t.tick(20000));
    TEST_ASSERT_TRUE(t.level() == ScreenLevel::Off);
    TEST_ASSERT_FALSE(t.tick(60000));
    TEST_ASSERT_TRUE(t.level() == ScreenLevel::Off);
}

static void test_on_input_at_on() {
    IdleTimer t;
    t.configure(30, 300, 0);
    TEST_ASSERT_TRUE(t.onInput(10000));
    TEST_ASSERT_FALSE(t.tick(39999));
    TEST_ASSERT_TRUE(t.level() == ScreenLevel::On);
    TEST_ASSERT_TRUE(t.tick(40000));
    TEST_ASSERT_TRUE(t.level() == ScreenLevel::Dim);
}

static void test_on_input_at_dim() {
    IdleTimer t;
    t.configure(30, 300, 0);
    t.tick(30000);
    TEST_ASSERT_FALSE(t.onInput(31000));
    TEST_ASSERT_TRUE(t.level() == ScreenLevel::On);
    TEST_ASSERT_TRUE(t.onInput(32000));
}

static void test_on_input_at_off() {
    IdleTimer t;
    t.configure(30, 300, 0);
    t.tick(300000);
    TEST_ASSERT_FALSE(t.onInput(301000));
    TEST_ASSERT_TRUE(t.level() == ScreenLevel::On);
    TEST_ASSERT_TRUE(t.onInput(302000));
}

static void test_pressed_at_off_swallows_gesture() {
    IdleTimer t;
    t.configure(30, 300, 0);
    t.tick(300000);
    TEST_ASSERT_FALSE(t.onButton(Ev::Pressed, 300000));
    TEST_ASSERT_TRUE(t.level() == ScreenLevel::On);
    TEST_ASSERT_FALSE(t.onButton(Ev::LongPress, 300600));
    TEST_ASSERT_FALSE(t.onButton(Ev::VeryLongPress, 310000));
    TEST_ASSERT_FALSE(t.onButton(Ev::Released, 311000));
    TEST_ASSERT_TRUE(t.onButton(Ev::Pressed, 312000));
}

static void test_short_press_alone_at_dim() {
    IdleTimer t;
    t.configure(30, 300, 0);
    t.tick(30000);
    TEST_ASSERT_FALSE(t.onButton(Ev::ShortPress, 31000));
    TEST_ASSERT_TRUE(t.level() == ScreenLevel::On);
    TEST_ASSERT_TRUE(t.onButton(Ev::ShortPress, 32000));
}

static void test_long_press_alone_at_off_swallows_until_released() {
    IdleTimer t;
    t.configure(30, 300, 0);
    t.tick(300000);
    TEST_ASSERT_FALSE(t.onButton(Ev::LongPress, 301000));
    TEST_ASSERT_TRUE(t.level() == ScreenLevel::On);
    TEST_ASSERT_FALSE(t.onButton(Ev::VeryLongPress, 311000));
    TEST_ASSERT_FALSE(t.onButton(Ev::Released, 312000));
    TEST_ASSERT_TRUE(t.onButton(Ev::Pressed, 313000));
}

static void test_stale_swallow_does_not_leak() {
    IdleTimer t;
    t.configure(30, 300, 0);
    t.tick(300000);
    TEST_ASSERT_FALSE(t.onButton(Ev::LongPress, 301000));  // swallowing, no Released seen
    TEST_ASSERT_TRUE(t.onButton(Ev::Pressed, 302000));     // level On now
    TEST_ASSERT_TRUE(t.onButton(Ev::ShortPress, 302100));
}

static void test_held_press_keeps_screen_on() {
    IdleTimer t;
    t.configure(30, 300, 0);
    TEST_ASSERT_TRUE(t.onButton(Ev::Pressed, 0));
    TEST_ASSERT_FALSE(t.tick(30000));
    TEST_ASSERT_FALSE(t.tick(60000));
    TEST_ASSERT_TRUE(t.level() == ScreenLevel::On);
    TEST_ASSERT_TRUE(t.onButton(Ev::Released, 60000));
    TEST_ASSERT_FALSE(t.tick(89999));
    TEST_ASSERT_TRUE(t.tick(90000));
    TEST_ASSERT_TRUE(t.level() == ScreenLevel::Dim);
}

static void test_swallowed_pressed_at_off_holds() {
    IdleTimer t;
    t.configure(30, 300, 0);
    t.tick(300000);
    TEST_ASSERT_FALSE(t.onButton(Ev::Pressed, 300000));
    TEST_ASSERT_FALSE(t.tick(400000));
    TEST_ASSERT_TRUE(t.level() == ScreenLevel::On);
    TEST_ASSERT_FALSE(t.onButton(Ev::Released, 400000));
    TEST_ASSERT_FALSE(t.tick(429999));
    TEST_ASSERT_TRUE(t.tick(430000));
}

static void test_configure_mid_gesture() {
    IdleTimer t;
    t.configure(30, 300, 0);
    TEST_ASSERT_TRUE(t.onButton(Ev::Pressed, 1000));
    t.configure(30, 300, 2000);
    TEST_ASSERT_TRUE(t.onButton(Ev::Released, 3000));
    TEST_ASSERT_FALSE(t.tick(32999));
    TEST_ASSERT_TRUE(t.tick(33000));  // held cleared
}

static void test_keep_awake_mid_gesture() {
    IdleTimer t;
    t.configure(30, 300, 0);
    TEST_ASSERT_TRUE(t.onButton(Ev::Pressed, 1000));
    t.keepAwake(2000);
    TEST_ASSERT_TRUE(t.onButton(Ev::Released, 3000));
    TEST_ASSERT_TRUE(t.tick(33000));
}

static void test_short_release_at_on_without_pressed() {
    IdleTimer t;
    t.configure(30, 300, 0);
    TEST_ASSERT_TRUE(t.onButton(Ev::ShortPress, 100));
    TEST_ASSERT_TRUE(t.onButton(Ev::Released, 200));
    TEST_ASSERT_TRUE(t.tick(30200));  // not held
}

static void test_button_none() {
    IdleTimer t;
    t.configure(0, 10, 0);
    t.tick(10000);
    TEST_ASSERT_TRUE(t.level() == ScreenLevel::Off);
    TEST_ASSERT_TRUE(t.onButton(Ev::None, 20000));
    TEST_ASSERT_TRUE(t.level() == ScreenLevel::Off);

    IdleTimer u;
    u.configure(30, 0, 0);
    TEST_ASSERT_TRUE(u.onButton(Ev::None, 20000));
    TEST_ASSERT_TRUE(u.tick(30000));  // timer not reset by None
}

static void test_keep_awake_at_off() {
    IdleTimer t;
    t.configure(30, 300, 0);
    t.tick(300000);
    t.keepAwake(301000);
    TEST_ASSERT_TRUE(t.level() == ScreenLevel::On);
    TEST_ASSERT_TRUE(t.onInput(302000));
    TEST_ASSERT_TRUE(t.onButton(Ev::Pressed, 303000));
}

static void test_configure_at_dim() {
    IdleTimer t;
    t.configure(30, 300, 0);
    t.tick(30000);
    t.configure(30, 300, 31000);
    TEST_ASSERT_TRUE(t.level() == ScreenLevel::On);
}

static void test_sticky_off() {
    IdleTimer t;
    t.configure(30, 300, 0);
    t.tick(300000);
    TEST_ASSERT_FALSE(t.tick(UINT32_MAX - 999u));
    TEST_ASSERT_TRUE(t.level() == ScreenLevel::Off);
    TEST_ASSERT_FALSE(t.tick(2147483648u + 5u));
    TEST_ASSERT_TRUE(t.level() == ScreenLevel::Off);
}

static void test_sticky_dim_off_zero() {
    IdleTimer t;
    t.configure(30, 0, 0);
    t.tick(30000);
    TEST_ASSERT_FALSE(t.tick(UINT32_MAX));
    TEST_ASSERT_TRUE(t.level() == ScreenLevel::Dim);
    TEST_ASSERT_FALSE(t.tick(100));  // wrapped past 2^32
    TEST_ASSERT_TRUE(t.level() == ScreenLevel::Dim);
}

static void test_seconds_conversion() {
    IdleTimer t;
    t.configure(86400, 0, 5);
    TEST_ASSERT_FALSE(t.tick(5u + 86399999u));
    TEST_ASSERT_TRUE(t.tick(5u + 86400000u));
    TEST_ASSERT_TRUE(t.level() == ScreenLevel::Dim);
}

static void test_seconds_overflow_clamped() {
    IdleTimer t;
    t.configure(UINT32_MAX, 0, 0);
    TEST_ASSERT_FALSE(t.tick(1000000000u));
    TEST_ASSERT_FALSE(t.tick(UINT32_MAX - 1u));
    TEST_ASSERT_TRUE(t.level() == ScreenLevel::On);
    TEST_ASSERT_TRUE(t.tick(UINT32_MAX));
}

static void test_panel_setting() {
    auto p = panelSetting(ScreenLevel::On, 80, 15);
    TEST_ASSERT_TRUE(p.on);
    TEST_ASSERT_EQUAL_INT32(80, p.percent);
    p = panelSetting(ScreenLevel::Dim, 80, 15);
    TEST_ASSERT_TRUE(p.on);
    TEST_ASSERT_EQUAL_INT32(15, p.percent);
    p = panelSetting(ScreenLevel::Dim, 10, 50);
    TEST_ASSERT_TRUE(p.on);
    TEST_ASSERT_EQUAL_INT32(10, p.percent);
    p = panelSetting(ScreenLevel::Dim, 80, 0);
    TEST_ASSERT_TRUE(p.on);
    TEST_ASSERT_EQUAL_INT32(0, p.percent);
    p = panelSetting(ScreenLevel::Dim, 80, -5);
    TEST_ASSERT_TRUE(p.on);
    TEST_ASSERT_EQUAL_INT32(-5, p.percent);
    p = panelSetting(ScreenLevel::Off, 80, 15);
    TEST_ASSERT_FALSE(p.on);
    TEST_ASSERT_EQUAL_INT32(0, p.percent);
}

static void test_names() {
    TEST_ASSERT_EQUAL_STRING("on", screenLevelName(ScreenLevel::On));
    TEST_ASSERT_EQUAL_STRING("dim", screenLevelName(ScreenLevel::Dim));
    TEST_ASSERT_EQUAL_STRING("off", screenLevelName(ScreenLevel::Off));
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_default_timing);
    RUN_TEST(test_dim_zero);
    RUN_TEST(test_off_zero);
    RUN_TEST(test_both_zero_never);
    RUN_TEST(test_default_constructed_stays_on);
    RUN_TEST(test_off_before_dim);
    RUN_TEST(test_on_input_at_on);
    RUN_TEST(test_on_input_at_dim);
    RUN_TEST(test_on_input_at_off);
    RUN_TEST(test_pressed_at_off_swallows_gesture);
    RUN_TEST(test_short_press_alone_at_dim);
    RUN_TEST(test_long_press_alone_at_off_swallows_until_released);
    RUN_TEST(test_stale_swallow_does_not_leak);
    RUN_TEST(test_held_press_keeps_screen_on);
    RUN_TEST(test_swallowed_pressed_at_off_holds);
    RUN_TEST(test_configure_mid_gesture);
    RUN_TEST(test_keep_awake_mid_gesture);
    RUN_TEST(test_short_release_at_on_without_pressed);
    RUN_TEST(test_button_none);
    RUN_TEST(test_keep_awake_at_off);
    RUN_TEST(test_configure_at_dim);
    RUN_TEST(test_sticky_off);
    RUN_TEST(test_sticky_dim_off_zero);
    RUN_TEST(test_seconds_conversion);
    RUN_TEST(test_seconds_overflow_clamped);
    RUN_TEST(test_panel_setting);
    RUN_TEST(test_names);
    return UNITY_END();
}
