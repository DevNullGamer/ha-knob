#include <unity.h>

#include <cstdint>
#include <limits>

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

static void test_clamp_boundary_values_are_inclusive() {
    TEST_ASSERT_EQUAL_INT32(0, core::clampInt(0, 0, 10));
    TEST_ASSERT_EQUAL_INT32(10, core::clampInt(10, 0, 10));
    TEST_ASSERT_EQUAL_INT32(0, core::clampInt(-1, 0, 10));
    TEST_ASSERT_EQUAL_INT32(10, core::clampInt(11, 0, 10));
    TEST_ASSERT_EQUAL_INT32(1, core::clampInt(1, 0, 10));
    TEST_ASSERT_EQUAL_INT32(9, core::clampInt(9, 0, 10));
}

static void test_clamp_equal_bounds_returns_bound() {
    TEST_ASSERT_EQUAL_INT32(7, core::clampInt(7, 7, 7));
    TEST_ASSERT_EQUAL_INT32(7, core::clampInt(-100, 7, 7));
    TEST_ASSERT_EQUAL_INT32(7, core::clampInt(100, 7, 7));
}

static void test_clamp_swapped_bounds_all_regions() {
    TEST_ASSERT_EQUAL_INT32(0, core::clampInt(-5, 10, 0));
    TEST_ASSERT_EQUAL_INT32(5, core::clampInt(5, 10, 0));
    TEST_ASSERT_EQUAL_INT32(0, core::clampInt(0, 10, 0));
    TEST_ASSERT_EQUAL_INT32(10, core::clampInt(10, 10, 0));
}

static void test_clamp_negative_range() {
    TEST_ASSERT_EQUAL_INT32(-10, core::clampInt(-20, -10, -5));
    TEST_ASSERT_EQUAL_INT32(-7, core::clampInt(-7, -10, -5));
    TEST_ASSERT_EQUAL_INT32(-5, core::clampInt(0, -10, -5));
}

static void test_clamp_int32_extremes() {
    const int32_t kMin = std::numeric_limits<int32_t>::min();
    const int32_t kMax = std::numeric_limits<int32_t>::max();
    TEST_ASSERT_EQUAL_INT32(kMin, core::clampInt(kMin, kMin, kMax));
    TEST_ASSERT_EQUAL_INT32(kMax, core::clampInt(kMax, kMin, kMax));
    TEST_ASSERT_EQUAL_INT32(0, core::clampInt(0, kMin, kMax));
    TEST_ASSERT_EQUAL_INT32(-1, core::clampInt(kMin, -1, 1));
    TEST_ASSERT_EQUAL_INT32(1, core::clampInt(kMax, -1, 1));
    TEST_ASSERT_EQUAL_INT32(kMin, core::clampInt(kMax, kMin, kMin));
    TEST_ASSERT_EQUAL_INT32(kMax, core::clampInt(kMin, kMax, kMax));
    // Swapped full-range bounds.
    TEST_ASSERT_EQUAL_INT32(123, core::clampInt(123, kMax, kMin));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_clamp_inside_range_is_unchanged);
    RUN_TEST(test_clamp_below_and_above);
    RUN_TEST(test_clamp_swapped_bounds);
    RUN_TEST(test_clamp_boundary_values_are_inclusive);
    RUN_TEST(test_clamp_equal_bounds_returns_bound);
    RUN_TEST(test_clamp_swapped_bounds_all_regions);
    RUN_TEST(test_clamp_negative_range);
    RUN_TEST(test_clamp_int32_extremes);
    return UNITY_END();
}
