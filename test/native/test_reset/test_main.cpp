// Native tests for ui::ResetConfirm (plan-step8.md section 3.2 / Step 8.2, testpoints-8.1-8.2.md).
#include <unity.h>

#include <cstdint>

#include "input/ButtonFsm.h"
#include "ui/ResetConfirm.h"

using core::ButtonFsm;
using core::ui::cancelReasonName;
using core::ui::ResetConfirm;
using Ev = core::ButtonFsm::Event;
using R = ResetConfirm::Result;
using CR = ResetConfirm::CancelReason;

void setUp() {}
void tearDown() {}

static ResetConfirm armedObj() {
    ResetConfirm r;
    r.observeButton(false);
    return r;
}

static ResetConfirm opened(uint32_t now) {
    ResetConfirm r = armedObj();
    TEST_ASSERT_TRUE(r.onButton(Ev::VeryLongPress, now) == R::Opened);
    return r;
}

static void test_not_armed() {
    ResetConfirm r;
    TEST_ASSERT_FALSE(r.armed());
    TEST_ASSERT_TRUE(r.onButton(Ev::VeryLongPress, 0) == R::IgnoredNotArmed);
    TEST_ASSERT_FALSE(r.confirming());
    TEST_ASSERT_FALSE(r.captures());
}

static void test_held_since_boot_not_armed() {
    ResetConfirm r;
    r.observeButton(true);
    r.observeButton(true);
    TEST_ASSERT_FALSE(r.armed());
    TEST_ASSERT_TRUE(r.onButton(Ev::VeryLongPress, 10000) == R::IgnoredNotArmed);
    TEST_ASSERT_FALSE(r.confirming());
}

static void test_armed_opens() {
    ResetConfirm r;
    r.observeButton(false);
    TEST_ASSERT_TRUE(r.armed());
    TEST_ASSERT_TRUE(r.onButton(Ev::VeryLongPress, 1000) == R::Opened);
    TEST_ASSERT_TRUE(r.confirming());
    TEST_ASSERT_TRUE(r.captures());
    TEST_ASSERT_FALSE(r.confirmed());
}

static void test_idle_other_inputs_none() {
    ResetConfirm r = armedObj();
    TEST_ASSERT_TRUE(r.onButton(Ev::Pressed, 0) == R::None);
    TEST_ASSERT_TRUE(r.onButton(Ev::ShortPress, 0) == R::None);
    TEST_ASSERT_TRUE(r.onButton(Ev::LongPress, 0) == R::None);
    TEST_ASSERT_TRUE(r.onButton(Ev::Released, 0) == R::None);
    TEST_ASSERT_TRUE(r.onButton(Ev::None, 0) == R::None);
    TEST_ASSERT_TRUE(r.onRotate(1, 0) == R::None);
    TEST_ASSERT_TRUE(r.onSwipe(0) == R::None);
    TEST_ASSERT_TRUE(r.tick(100000) == R::None);
    TEST_ASSERT_FALSE(r.captures());
    TEST_ASSERT_EQUAL_UINT32(0, r.secondsLeft(0));
    TEST_ASSERT_TRUE(r.lastCancel() == CR::None);
}

static void test_countdown() {
    ResetConfirm r = opened(1000);
    TEST_ASSERT_EQUAL_UINT32(10, r.secondsLeft(1000));
    TEST_ASSERT_EQUAL_UINT32(10, r.secondsLeft(1001));
    TEST_ASSERT_EQUAL_UINT32(2, r.secondsLeft(1000 + 8999));
    TEST_ASSERT_EQUAL_UINT32(1, r.secondsLeft(1000 + 9000));
    TEST_ASSERT_EQUAL_UINT32(1, r.secondsLeft(1000 + 9001));
    TEST_ASSERT_EQUAL_UINT32(0, r.secondsLeft(1000 + 10000));  // before tick
    TEST_ASSERT_EQUAL_UINT32(10, r.secondsLeft(500));          // clock before open: capped
}

static void test_seconds_left_zero_when_not_confirming() {
    ResetConfirm r = opened(0);
    r.onSwipe(10);
    TEST_ASSERT_EQUAL_UINT32(0, r.secondsLeft(10));
}

static void test_confirming_ignores_non_confirm_events() {
    ResetConfirm r = opened(0);
    TEST_ASSERT_TRUE(r.onButton(Ev::Released, 10500) == R::None);
    TEST_ASSERT_TRUE(r.confirming());
    TEST_ASSERT_TRUE(r.onButton(Ev::VeryLongPress, 100) == R::None);
    TEST_ASSERT_TRUE(r.onButton(Ev::None, 100) == R::None);
    TEST_ASSERT_TRUE(r.confirming());
}

static void test_pressed_then_short_confirms() {
    ResetConfirm r = opened(0);
    TEST_ASSERT_TRUE(r.onButton(Ev::Pressed, 1000) == R::None);
    TEST_ASSERT_TRUE(r.confirming());
    TEST_ASSERT_TRUE(r.onButton(Ev::ShortPress, 1100) == R::Confirmed);
    TEST_ASSERT_TRUE(r.confirmed());
    TEST_ASSERT_TRUE(r.captures());
    TEST_ASSERT_FALSE(r.confirming());
}

static void test_confirmed_is_terminal() {
    ResetConfirm r = opened(0);
    r.onButton(Ev::ShortPress, 100);
    TEST_ASSERT_TRUE(r.onRotate(1, 200) == R::None);
    TEST_ASSERT_TRUE(r.onSwipe(200) == R::None);
    TEST_ASSERT_TRUE(r.tick(0xFFFF0000u) == R::None);
    TEST_ASSERT_TRUE(r.onButton(Ev::VeryLongPress, 300) == R::None);
    TEST_ASSERT_TRUE(r.onButton(Ev::ShortPress, 300) == R::None);
    TEST_ASSERT_EQUAL_UINT32(0, r.secondsLeft(300));
    TEST_ASSERT_TRUE(r.captures());
    TEST_ASSERT_TRUE(r.confirmed());
}

static void test_long_press_released_none() {
    ResetConfirm r = opened(0);
    TEST_ASSERT_TRUE(r.onButton(Ev::LongPress, 1000) == R::None);
    TEST_ASSERT_TRUE(r.onButton(Ev::Released, 2000) == R::None);
    TEST_ASSERT_TRUE(r.confirming());
}

static void test_rotate_cancels() {
    ResetConfirm a = opened(0);
    TEST_ASSERT_TRUE(a.onRotate(1, 100) == R::Cancelled);
    TEST_ASSERT_TRUE(a.lastCancel() == CR::Rotate);
    TEST_ASSERT_FALSE(a.captures());
    ResetConfirm b = opened(0);
    TEST_ASSERT_TRUE(b.onRotate(-3, 100) == R::Cancelled);
    TEST_ASSERT_TRUE(b.lastCancel() == CR::Rotate);
    ResetConfirm c = opened(0);
    TEST_ASSERT_TRUE(c.onRotate(0, 100) == R::None);
    TEST_ASSERT_TRUE(c.confirming());
}

static void test_swipe_cancels() {
    ResetConfirm r = opened(0);
    TEST_ASSERT_TRUE(r.onSwipe(100) == R::Cancelled);
    TEST_ASSERT_TRUE(r.lastCancel() == CR::Swipe);
    TEST_ASSERT_FALSE(r.captures());
}

static void test_timeout() {
    ResetConfirm r = opened(5000);
    TEST_ASSERT_TRUE(r.tick(5000 + 9999) == R::None);
    TEST_ASSERT_TRUE(r.confirming());
    TEST_ASSERT_TRUE(r.tick(5000 + 10000) == R::Cancelled);
    TEST_ASSERT_TRUE(r.lastCancel() == CR::Timeout);
    TEST_ASSERT_FALSE(r.captures());
    TEST_ASSERT_TRUE(r.tick(5000 + 10001) == R::None);
}

static void test_reopen_without_rearm() {
    ResetConfirm r = opened(0);
    r.onSwipe(10);
    TEST_ASSERT_TRUE(r.armed());
    r.observeButton(true);
    TEST_ASSERT_TRUE(r.armed());
    TEST_ASSERT_TRUE(r.lastCancel() == CR::Swipe);
    TEST_ASSERT_TRUE(r.onButton(Ev::VeryLongPress, 20000) == R::Opened);
    TEST_ASSERT_TRUE(r.lastCancel() == CR::None);
    TEST_ASSERT_TRUE(r.confirming());
    TEST_ASSERT_EQUAL_UINT32(10, r.secondsLeft(20000));
}

static void test_clock_wrap() {
    const uint32_t open = 0xFFFFF000u;
    ResetConfirm r = opened(open);
    TEST_ASSERT_EQUAL_UINT32(10, r.secondsLeft(open));
    TEST_ASSERT_EQUAL_UINT32(1, r.secondsLeft(open + 9500u));  // wrapped
    TEST_ASSERT_TRUE(r.tick(open + 9999u) == R::None);
    TEST_ASSERT_TRUE(r.confirming());
    TEST_ASSERT_TRUE(r.tick(open + 10000u) == R::Cancelled);
    TEST_ASSERT_TRUE(r.lastCancel() == CR::Timeout);
}

static void test_real_button_fsm_integration() {
    ButtonFsm fsm;
    ResetConfirm r;
    int opens = 0;
    int others = 0;
    bool confirmedNow = false;
    auto step = [&](bool raw, uint32_t now) {
        Ev e = fsm.update(raw, now);
        r.observeButton(fsm.isDown());
        R res = r.onButton(e, now);
        if (res == R::Opened) ++opens;
        else if (res == R::Confirmed) confirmedNow = true;
        else if (res != R::None) ++others;
    };
    uint32_t now = 0;
    for (; now < 100; now += 10) step(false, now);  // released since boot: arms
    TEST_ASSERT_TRUE(r.armed());
    const uint32_t start = now;
    for (; now < start + 10500; now += 10) step(true, now);
    TEST_ASSERT_EQUAL_INT(1, opens);
    for (; now < start + 10700; now += 10) step(false, now);  // release
    TEST_ASSERT_EQUAL_INT(1, opens);
    TEST_ASSERT_EQUAL_INT(0, others);
    TEST_ASSERT_TRUE(r.confirming());
    TEST_ASSERT_FALSE(confirmedNow);
    const uint32_t tap = now;
    for (; now < tap + 100; now += 10) step(true, now);
    for (; now < tap + 300; now += 10) step(false, now);
    TEST_ASSERT_TRUE(confirmedNow);
    TEST_ASSERT_TRUE(r.confirmed());
    TEST_ASSERT_EQUAL_INT(1, opens);
}

static void test_names() {
    TEST_ASSERT_EQUAL_STRING("none", cancelReasonName(CR::None));
    TEST_ASSERT_EQUAL_STRING("rotate", cancelReasonName(CR::Rotate));
    TEST_ASSERT_EQUAL_STRING("swipe", cancelReasonName(CR::Swipe));
    TEST_ASSERT_EQUAL_STRING("timeout", cancelReasonName(CR::Timeout));
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_not_armed);
    RUN_TEST(test_held_since_boot_not_armed);
    RUN_TEST(test_armed_opens);
    RUN_TEST(test_idle_other_inputs_none);
    RUN_TEST(test_countdown);
    RUN_TEST(test_seconds_left_zero_when_not_confirming);
    RUN_TEST(test_confirming_ignores_non_confirm_events);
    RUN_TEST(test_pressed_then_short_confirms);
    RUN_TEST(test_confirmed_is_terminal);
    RUN_TEST(test_long_press_released_none);
    RUN_TEST(test_rotate_cancels);
    RUN_TEST(test_swipe_cancels);
    RUN_TEST(test_timeout);
    RUN_TEST(test_reopen_without_rearm);
    RUN_TEST(test_clock_wrap);
    RUN_TEST(test_real_button_fsm_integration);
    RUN_TEST(test_names);
    return UNITY_END();
}
