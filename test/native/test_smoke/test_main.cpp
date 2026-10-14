#include <unity.h>

#include "util/MathUtil.h"

void setUp() {}
void tearDown() {}

static void test_clamp_inside_range_is_unchanged() {
    TEST_ASSERT_EQUAL_INT32(5, core::clampInt(5, 0, 10));
}

static void test_clamp_below_and_above() {
    TEST_ASSERT_EQUAL_INT32(0, core::clampInt(-3, 0, 10));
    TEST_ASSERT_EQUAL_INT32(10, core::clampInt(42, 0, 10));
}

static void test_clamp_swapped_bounds() {
    TEST_ASSERT_EQUAL_INT32(10, core::clampInt(42, 10, 0));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_clamp_inside_range_is_unchanged);
    RUN_TEST(test_clamp_below_and_above);
    RUN_TEST(test_clamp_swapped_bounds);
    return UNITY_END();
}
