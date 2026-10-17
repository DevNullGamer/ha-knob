// Native tests for core::ui::SelectModel (plan-step7.md §3.5, Step 7.5).
#include <unity.h>

#include <climits>
#include <cstdint>

#include "ui/SelectModel.h"

using core::ui::SelectModel;

void setUp() {}
void tearDown() {}

static void test_initial_closed() {
    SelectModel m;
    TEST_ASSERT_FALSE(m.isOpen());
    TEST_ASSERT_FALSE(m.choose().has_value());
    TEST_ASSERT_FALSE(m.tick(100000));
}

static void test_open_zero_stays_closed() {
    SelectModel m;
    m.open(0, 0, 0);
    TEST_ASSERT_FALSE(m.isOpen());
    m.open(3, 1, 0);
    m.close();
    m.open(0, 0, 0);
    TEST_ASSERT_FALSE(m.isOpen());
}

static void test_open_selects_and_clamps() {
    SelectModel m;
    m.open(3, 1, 0);
    TEST_ASSERT_TRUE(m.isOpen());
    TEST_ASSERT_EQUAL(3, (int)m.count());
    TEST_ASSERT_EQUAL(1, (int)m.selected());
    m.open(3, 7, 0);
    TEST_ASSERT_EQUAL(2, (int)m.selected());
}

static void test_move_clamps_both_ends() {
    SelectModel m;
    m.open(5, 2, 0);
    m.move(-5, 1);
    TEST_ASSERT_EQUAL(0, (int)m.selected());
    m.move(1, 2);
    TEST_ASSERT_EQUAL(1, (int)m.selected());
    m.move(99, 3);
    TEST_ASSERT_EQUAL(4, (int)m.selected());
    m.move(-1, 4);
    TEST_ASSERT_EQUAL(3, (int)m.selected());
}

static void test_move_extreme_deltas_no_overflow() {
    SelectModel m;
    m.open(4, 2, 0);
    m.move(INT_MAX, 1);
    TEST_ASSERT_EQUAL(3, (int)m.selected());
    m.move(INT_MIN, 2);
    TEST_ASSERT_EQUAL(0, (int)m.selected());
    m.move(INT_MAX, 3);
    TEST_ASSERT_EQUAL(3, (int)m.selected());
}

static void test_closed_move_and_choose_noop() {
    SelectModel m;
    m.open(4, 1, 0);
    m.close();
    m.move(2, 5);
    TEST_ASSERT_EQUAL(1, (int)m.selected());
    TEST_ASSERT_FALSE(m.choose().has_value());
    TEST_ASSERT_FALSE(m.isOpen());
}

static void test_choose_closes() {
    SelectModel m;
    m.open(4, 1, 0);
    m.move(1, 10);
    auto c = m.choose();
    TEST_ASSERT_TRUE(c.has_value());
    TEST_ASSERT_EQUAL(2, (int)*c);
    TEST_ASSERT_FALSE(m.isOpen());
    TEST_ASSERT_FALSE(m.choose().has_value());
}

static void test_tick_idle_boundary_once() {
    SelectModel m;
    m.open(3, 0, 1000);
    TEST_ASSERT_FALSE(m.tick(1000));
    TEST_ASSERT_FALSE(m.tick(1000 + 7999));
    TEST_ASSERT_TRUE(m.isOpen());
    TEST_ASSERT_TRUE(m.tick(1000 + 8000));
    TEST_ASSERT_FALSE(m.isOpen());
    TEST_ASSERT_FALSE(m.tick(1000 + 8001));
    TEST_ASSERT_FALSE(m.tick(1000 + 90000));
}

static void test_move_restarts_idle_also_when_clamped() {
    SelectModel m;
    m.open(3, 0, 0);
    m.move(1, 5000);
    TEST_ASSERT_FALSE(m.tick(12999));
    TEST_ASSERT_TRUE(m.tick(13000));

    m.open(3, 0, 0);
    m.move(-1, 5000);  // clamped at the first item, still input
    TEST_ASSERT_EQUAL(0, (int)m.selected());
    TEST_ASSERT_FALSE(m.tick(12999));
    TEST_ASSERT_TRUE(m.tick(13000));
}

static void test_clock_wrap() {
    SelectModel m;
    m.open(3, 0, 0xFFFFF000u);
    TEST_ASSERT_FALSE(m.tick(0xFFFFFFFFu));
    TEST_ASSERT_TRUE(m.tick(0x00001000u));  // 8192 ms later
    m.open(3, 0, 0xFFFFF000u);
    TEST_ASSERT_FALSE(m.tick(0x00000F3Fu));  // 7999 ms
    TEST_ASSERT_TRUE(m.tick(0x00001000u));
}

static void test_tick_after_wrap_uses_4096_window() {
    SelectModel m;
    m.open(3, 0, 0xFFFFF000u);
    TEST_ASSERT_FALSE(m.tick(0x00000000u + 0x00000000u));  // 4096 ms
    TEST_ASSERT_TRUE(m.isOpen());
}

static void test_reopen_restarts_idle() {
    SelectModel m;
    m.open(3, 0, 0);
    m.open(3, 1, 6000);
    TEST_ASSERT_EQUAL(1, (int)m.selected());
    TEST_ASSERT_FALSE(m.tick(13999));
    TEST_ASSERT_TRUE(m.tick(14000));
    m.open(3, 0, 20000);  // after a timeout
    TEST_ASSERT_TRUE(m.isOpen());
    TEST_ASSERT_FALSE(m.tick(20001));
}

static void test_close_is_noop_when_closed() {
    SelectModel m;
    m.close();
    TEST_ASSERT_FALSE(m.isOpen());
    m.open(2, 0, 0);
    m.close();
    TEST_ASSERT_FALSE(m.isOpen());
    TEST_ASSERT_FALSE(m.tick(100000));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_initial_closed);
    RUN_TEST(test_open_zero_stays_closed);
    RUN_TEST(test_open_selects_and_clamps);
    RUN_TEST(test_move_clamps_both_ends);
    RUN_TEST(test_move_extreme_deltas_no_overflow);
    RUN_TEST(test_closed_move_and_choose_noop);
    RUN_TEST(test_choose_closes);
    RUN_TEST(test_tick_idle_boundary_once);
    RUN_TEST(test_move_restarts_idle_also_when_clamped);
    RUN_TEST(test_clock_wrap);
    RUN_TEST(test_tick_after_wrap_uses_4096_window);
    RUN_TEST(test_reopen_restarts_idle);
    RUN_TEST(test_close_is_noop_when_closed);
    return UNITY_END();
}
