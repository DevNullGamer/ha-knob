// Native tests for core::ui::PageRing (plan-step6.md §3.7, Step 6.8).
#include <unity.h>

#include "ui/PageRing.h"

using core::ui::PageRing;

void setUp() {}
void tearDown() {}

// Page of the ring as a string: "c<i>", "none" or "status".
static const char *pageName(const PageRing &r, char *buf) {
    if (r.onStatus()) return "status";
    if (r.onNoControls()) return "none";
    buf[0] = 'c';
    buf[1] = static_cast<char>('0' + r.controlPage());
    buf[2] = 0;
    return buf;
}

static void test_initial_state() {
    PageRing r;
    TEST_ASSERT_EQUAL(0, (int)r.controlCount());
    TEST_ASSERT_FALSE(r.onStatus());
    TEST_ASSERT_TRUE(r.onNoControls());
    TEST_ASSERT_EQUAL_INT(-1, r.controlPage());
}

static void test_zero_controls_ring_is_nocontrols_then_status() {
    PageRing r;
    r.setControlCount(0);
    r.next();
    TEST_ASSERT_TRUE(r.onStatus());
    TEST_ASSERT_FALSE(r.onNoControls());
    TEST_ASSERT_EQUAL_INT(-1, r.controlPage());
    r.next();
    TEST_ASSERT_TRUE(r.onNoControls());
    r.previous();
    TEST_ASSERT_TRUE(r.onStatus());
    r.previous();
    TEST_ASSERT_TRUE(r.onNoControls());
}

static void test_one_control_ring() {
    PageRing r;
    r.setControlCount(1);
    TEST_ASSERT_EQUAL_INT(0, r.controlPage());
    TEST_ASSERT_FALSE(r.onNoControls());
    r.next();
    TEST_ASSERT_TRUE(r.onStatus());
    r.next();
    TEST_ASSERT_EQUAL_INT(0, r.controlPage());
    r.previous();
    TEST_ASSERT_TRUE(r.onStatus());
    r.previous();
    TEST_ASSERT_EQUAL_INT(0, r.controlPage());
}

static void test_three_controls_next_wraps_through_status() {
    PageRing r;
    r.setControlCount(3);
    char b[3];
    const char *expect[] = {"c1", "c2", "status", "c0", "c1"};
    for (const char *e : expect) {
        r.next();
        TEST_ASSERT_EQUAL_STRING(e, pageName(r, b));
    }
}

static void test_three_controls_previous_wraps_through_status() {
    PageRing r;
    r.setControlCount(3);
    char b[3];
    const char *expect[] = {"status", "c2", "c1", "c0", "status"};
    for (const char *e : expect) {
        r.previous();
        TEST_ASSERT_EQUAL_STRING(e, pageName(r, b));
    }
}

static void test_full_cycle_returns_to_start() {
    for (size_t n = 0; n <= 5; ++n) {
        PageRing r;
        r.setControlCount(n);
        for (size_t i = 0; i < n + 1; ++i) r.next();  // n pages (or 1) + status
        if (n == 0) {
            TEST_ASSERT_TRUE(r.onNoControls() || r.onStatus());
        }
        PageRing q;
        q.setControlCount(n);
        const size_t pages = (n == 0 ? 1 : n) + 1;
        for (size_t i = 0; i < pages; ++i) q.next();
        TEST_ASSERT_EQUAL_INT(n == 0 ? -1 : 0, q.controlPage());
        TEST_ASSERT_FALSE(q.onStatus());
        for (size_t i = 0; i < pages; ++i) q.previous();
        TEST_ASSERT_EQUAL_INT(n == 0 ? -1 : 0, q.controlPage());
        TEST_ASSERT_FALSE(q.onStatus());
    }
}

static void test_control_page_minus_one_on_status_and_nocontrols() {
    PageRing r;
    r.setControlCount(2);
    r.previous();
    TEST_ASSERT_TRUE(r.onStatus());
    TEST_ASSERT_EQUAL_INT(-1, r.controlPage());
    r.setControlCount(0);
    TEST_ASSERT_TRUE(r.onStatus());
    r.next();
    TEST_ASSERT_TRUE(r.onNoControls());
    TEST_ASSERT_EQUAL_INT(-1, r.controlPage());
}

static void test_shrink_below_index_goes_to_first_page() {
    PageRing r;
    r.setControlCount(5);
    r.next(); r.next(); r.next();  // c3
    TEST_ASSERT_EQUAL_INT(3, r.controlPage());
    r.setControlCount(2);
    TEST_ASSERT_EQUAL_INT(0, r.controlPage());
    TEST_ASSERT_EQUAL(2, (int)r.controlCount());
}

static void test_shrink_to_exactly_index_goes_to_first_page() {
    PageRing r;
    r.setControlCount(4);
    r.next(); r.next();  // c2
    r.setControlCount(2);  // index 2 out of range
    TEST_ASSERT_EQUAL_INT(0, r.controlPage());
}

static void test_in_range_index_is_kept() {
    PageRing r;
    r.setControlCount(5);
    r.next(); r.next();  // c2
    r.setControlCount(3);
    TEST_ASSERT_EQUAL_INT(2, r.controlPage());
    r.setControlCount(9);
    TEST_ASSERT_EQUAL_INT(2, r.controlPage());
    r.setControlCount(9);
    TEST_ASSERT_EQUAL_INT(2, r.controlPage());
}

static void test_status_is_kept_across_count_changes() {
    PageRing r;
    r.setControlCount(3);
    r.previous();
    TEST_ASSERT_TRUE(r.onStatus());
    r.setControlCount(1);
    TEST_ASSERT_TRUE(r.onStatus());
    r.setControlCount(0);
    TEST_ASSERT_TRUE(r.onStatus());
    r.setControlCount(4);
    TEST_ASSERT_TRUE(r.onStatus());
    r.next();
    TEST_ASSERT_EQUAL_INT(0, r.controlPage());
}

static void test_zero_to_n_to_zero() {
    PageRing r;
    r.setControlCount(0);
    TEST_ASSERT_TRUE(r.onNoControls());
    r.setControlCount(3);
    TEST_ASSERT_FALSE(r.onNoControls());
    TEST_ASSERT_EQUAL_INT(0, r.controlPage());
    r.next();
    TEST_ASSERT_EQUAL_INT(1, r.controlPage());
    r.setControlCount(0);
    TEST_ASSERT_TRUE(r.onNoControls());
    TEST_ASSERT_EQUAL_INT(-1, r.controlPage());
    r.setControlCount(2);
    TEST_ASSERT_EQUAL_INT(0, r.controlPage());
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_initial_state);
    RUN_TEST(test_zero_controls_ring_is_nocontrols_then_status);
    RUN_TEST(test_one_control_ring);
    RUN_TEST(test_three_controls_next_wraps_through_status);
    RUN_TEST(test_three_controls_previous_wraps_through_status);
    RUN_TEST(test_full_cycle_returns_to_start);
    RUN_TEST(test_control_page_minus_one_on_status_and_nocontrols);
    RUN_TEST(test_shrink_below_index_goes_to_first_page);
    RUN_TEST(test_shrink_to_exactly_index_goes_to_first_page);
    RUN_TEST(test_in_range_index_is_kept);
    RUN_TEST(test_status_is_kept_across_count_changes);
    RUN_TEST(test_zero_to_n_to_zero);
    return UNITY_END();
}
