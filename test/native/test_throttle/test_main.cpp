// Native tests for control::Throttle (plan-step7.md section 3.2, Q7-1, Q7-3, Amendment A1).
#include <unity.h>

#include <cstdint>
#include <optional>

#include "control/Throttle.h"

using core::control::Throttle;

void setUp() {}
void tearDown() {}

static void assertDue(double expected, const std::optional<double> &due) {
    TEST_ASSERT_TRUE(due.has_value());
    TEST_ASSERT_EQUAL_FLOAT(expected, *due);
}

static void assertNotDue(const std::optional<double> &due) { TEST_ASSERT_FALSE(due.has_value()); }

static void test_initial_state() {
    Throttle t(250, 1500);
    assertNotDue(t.due(0, true));
    TEST_ASSERT_FALSE(t.holding(0));
    TEST_ASSERT_FALSE(t.unconfirmed(0));
    TEST_ASSERT_EQUAL_FLOAT(0, t.target());
    TEST_ASSERT_EQUAL_UINT32(0, t.inFlight());
}

static void test_first_change_due_at_once() {
    Throttle t(250, 1500);
    t.change(10, 5000);
    assertDue(10, t.due(5000, true));
    TEST_ASSERT_TRUE(t.holding(5000));
    TEST_ASSERT_EQUAL_FLOAT(10, t.target());
}

static void test_leading_then_trailing() {
    Throttle t(250, 1500);
    t.change(1, 0);
    assertDue(1, t.due(0, true));
    t.sent(1, 0);
    for (int i = 2; i <= 10; ++i) {
        t.change(i, (uint32_t)((i - 1) * 240 / 9));
    }
    assertNotDue(t.due(240, true));
    assertNotDue(t.due(249, true));
    assertDue(10, t.due(250, true));
    t.sent(10, 250);
    assertNotDue(t.due(260, true));
    TEST_ASSERT_EQUAL_UINT32(2, t.inFlight());
}

static void test_can_send_false_defers_final_value() {
    Throttle t(250, 1500);
    t.change(5, 0);
    assertNotDue(t.due(0, false));
    assertDue(5, t.due(0, true));
    t.sent(5, 0);
    t.change(6, 10);
    assertNotDue(t.due(300, false));
    assertNotDue(t.due(100000, false));
    assertDue(6, t.due(100000, true));
}

static void test_sent_target_not_due_again() {
    Throttle t(250, 1500);
    t.change(5, 0);
    t.sent(5, 0);
    assertNotDue(t.due(10000, true));
}

static void test_settle_boundary() {
    Throttle t(250, 1500);
    t.change(5, 0);
    t.sent(5, 0);
    t.answered(true);
    TEST_ASSERT_TRUE(t.holding(1499));
    TEST_ASSERT_FALSE(t.holding(1500));
}

static void test_settle_counts_from_last_change() {
    Throttle t(250, 1500);
    t.change(5, 0);
    t.sent(5, 0);
    t.answered(true);
    t.change(6, 1000);
    t.sent(6, 1250);
    t.answered(true);
    TEST_ASSERT_TRUE(t.holding(2499));
    TEST_ASSERT_FALSE(t.holding(2500));
}

static void test_holding_while_in_flight_past_settle() {
    Throttle t(250, 1500);
    t.change(5, 0);
    t.sent(5, 0);
    TEST_ASSERT_TRUE(t.holding(1500));
    TEST_ASSERT_TRUE(t.holding(60000));
    t.answered(true);
    TEST_ASSERT_FALSE(t.holding(60000));
}

static void test_holding_while_unsent_past_settle() {
    Throttle t(250, 1500);
    t.change(5, 0);
    TEST_ASSERT_TRUE(t.holding(100000));  // unsent (e.g. canSend false)
}

static void test_failed_ends_hold_and_drops_unsent() {
    Throttle t(250, 1500);
    t.change(5, 0);
    t.sent(5, 0);
    t.change(6, 10);
    t.failed();
    assertNotDue(t.due(1000, true));
    TEST_ASSERT_FALSE(t.holding(10));
    TEST_ASSERT_EQUAL_UINT32(1, t.inFlight());  // the older call is still counted
}

static void test_failed_keeps_interval() {
    Throttle t(250, 1500);
    t.change(5, 0);
    t.sent(5, 0);
    t.failed();
    t.change(7, 100);
    assertNotDue(t.due(100, true));
    assertNotDue(t.due(249, true));
    assertDue(7, t.due(250, true));
}

static void test_change_after_failed_holds_while_older_in_flight() {
    Throttle t(250, 1500);
    t.change(5, 0);
    t.sent(5, 0);
    t.failed();
    t.change(7, 100);
    t.sent(7, 250);
    TEST_ASSERT_EQUAL_UINT32(2, t.inFlight());
    TEST_ASSERT_TRUE(t.holding(100000));
    t.answered(true);
    t.answered(true);
    TEST_ASSERT_FALSE(t.holding(100000));
}

static void test_drop_ends_hold() {
    Throttle t(250, 1500);
    t.change(5, 0);
    t.drop();
    TEST_ASSERT_FALSE(t.holding(0));
    assertNotDue(t.due(1000, true));
    t.change(6, 10);
    TEST_ASSERT_TRUE(t.holding(10));
    assertDue(6, t.due(10, true));
}

static void test_answered_false_calls_failed() {
    Throttle t(250, 1500);
    t.change(5, 0);
    t.sent(5, 0);
    t.change(6, 10);
    t.answered(false);
    TEST_ASSERT_EQUAL_UINT32(0, t.inFlight());
    TEST_ASSERT_FALSE(t.holding(20));
    assertNotDue(t.due(1000, true));
}

static void test_same_value_after_settle_is_sent_again() {
    Throttle t(250, 1500);
    t.change(60, 0);
    t.sent(60, 0);
    t.answered(true);
    t.change(60, 2000);  // HA's value may have moved: new hold forgets the last sent value
    assertDue(60, t.due(2000, true));
}

static void test_same_value_after_drop_is_sent_again() {
    Throttle t(250, 1500);
    t.change(60, 0);
    t.sent(60, 0);
    t.answered(true);
    t.drop();
    t.change(60, 500);
    assertDue(60, t.due(500, true));
}

static void test_back_to_last_sent_within_hold_not_due() {
    Throttle t(250, 1500);
    t.change(60, 0);
    t.sent(60, 0);
    t.answered(true);
    t.change(70, 100);
    assertDue(70, t.due(300, true));
    t.change(60, 200);
    assertNotDue(t.due(300, true));
    assertNotDue(t.due(10000, true));
}

static void test_same_value_after_answered_false_waits_for_interval() {
    Throttle t(250, 1500);
    t.change(60, 0);
    t.sent(60, 0);
    t.answered(false);
    t.change(60, 100);
    assertNotDue(t.due(100, true));
    assertNotDue(t.due(249, true));
    assertDue(60, t.due(250, true));
}

static void test_dedupe_while_holding_due_to_in_flight() {
    Throttle t(250, 1500);
    t.change(60, 0);
    t.sent(60, 0);
    t.change(60, 5000);  // still in flight: same hold
    assertNotDue(t.due(5000, true));
}

static void test_answered_without_in_flight_does_not_underflow() {
    Throttle t(250, 1500);
    t.answered(true);
    TEST_ASSERT_EQUAL_UINT32(0, t.inFlight());
    t.answered(false);
    TEST_ASSERT_EQUAL_UINT32(0, t.inFlight());
    TEST_ASSERT_FALSE(t.holding(0));
}

static void test_other_intervals() {
    Throttle t(100, 2000);
    t.change(1, 0);
    t.sent(1, 0);
    t.answered(true);
    t.change(2, 10);
    assertNotDue(t.due(99, true));
    assertDue(2, t.due(100, true));
    t.sent(2, 100);
    t.answered(true);
    TEST_ASSERT_TRUE(t.holding(2009));
    TEST_ASSERT_FALSE(t.holding(2010));
}

static void test_millis_wraparound() {
    Throttle t(250, 1500);
    t.change(1, 0xFFFFFF00u);
    t.sent(1, 0xFFFFFF00u);
    t.answered(true);
    t.change(2, 0xFFFFFF10u);
    assertNotDue(t.due(0xFFFFFFF9u, true));  // 249 ms
    assertDue(2, t.due(0xFFFFFFFAu, true));  // 250 ms after the send
    assertDue(2, t.due(0u, true));
    TEST_ASSERT_TRUE(t.holding(0x10u));
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_initial_state);
    RUN_TEST(test_first_change_due_at_once);
    RUN_TEST(test_leading_then_trailing);
    RUN_TEST(test_can_send_false_defers_final_value);
    RUN_TEST(test_sent_target_not_due_again);
    RUN_TEST(test_settle_boundary);
    RUN_TEST(test_settle_counts_from_last_change);
    RUN_TEST(test_holding_while_in_flight_past_settle);
    RUN_TEST(test_holding_while_unsent_past_settle);
    RUN_TEST(test_failed_ends_hold_and_drops_unsent);
    RUN_TEST(test_failed_keeps_interval);
    RUN_TEST(test_change_after_failed_holds_while_older_in_flight);
    RUN_TEST(test_drop_ends_hold);
    RUN_TEST(test_answered_false_calls_failed);
    RUN_TEST(test_same_value_after_settle_is_sent_again);
    RUN_TEST(test_same_value_after_drop_is_sent_again);
    RUN_TEST(test_back_to_last_sent_within_hold_not_due);
    RUN_TEST(test_same_value_after_answered_false_waits_for_interval);
    RUN_TEST(test_dedupe_while_holding_due_to_in_flight);
    RUN_TEST(test_answered_without_in_flight_does_not_underflow);
    RUN_TEST(test_other_intervals);
    RUN_TEST(test_millis_wraparound);
    return UNITY_END();
}
