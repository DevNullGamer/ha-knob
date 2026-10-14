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

int main() {
    UNITY_BEGIN();
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
