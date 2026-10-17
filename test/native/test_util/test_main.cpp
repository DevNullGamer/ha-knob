// Native tests for util/Sha1 and util/Base64 (plan-step5b.md section 3.2, Step 5b.1).
#include <unity.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "util/Base64.h"
#include "util/MathUtil.h"
#include "util/Sha1.h"
#include "util/Text.h"

namespace u = core::util;

void setUp() {}
void tearDown() {}

static std::string hex(const std::array<uint8_t, 20> &d) {
    std::string s;
    char b[3];
    for (uint8_t v : d) {
        std::snprintf(b, sizeof(b), "%02x", v);
        s += b;
    }
    return s;
}

static std::string sha(const std::string &in) {
    return hex(u::sha1(reinterpret_cast<const uint8_t *>(in.data()), in.size()));
}

static void test_sha1_empty() {
    TEST_ASSERT_EQUAL_STRING("da39a3ee5e6b4b0d3255bfef95601890afd80709", sha("").c_str());
    // nullptr with len 0 must also work
    TEST_ASSERT_EQUAL_STRING("da39a3ee5e6b4b0d3255bfef95601890afd80709", hex(u::sha1(nullptr, 0)).c_str());
}

static void test_sha1_abc() {
    TEST_ASSERT_EQUAL_STRING("a9993e364706816aba3e25717850c26c9cd0d89d", sha("abc").c_str());
}

static void test_sha1_56_bytes() {
    TEST_ASSERT_EQUAL_STRING(
        "84983e441c3bd26ebaae4aa1f95129e5e54670f1",
        sha("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq").c_str());
}

static void test_sha1_million_a() {
    const std::string in(1000000, 'a');
    TEST_ASSERT_EQUAL_STRING("34aa973cd4c4daa4f61eeb2bdbad27316534016f", sha(in).c_str());
}

static void test_sha1_padding_boundaries() {
    // Expected values computed with Python hashlib.
    struct V { size_t n; const char *h; };
    const V vs[] = {
        {55, "c1c8bbdc22796e28c0e15163d20899b65621d65a"},
        {56, "c2db330f6083854c99d4b5bfb6e8f29f201be699"},
        {63, "03f09f5b158a7a8cdad920bddc29b81c18a551f5"},
        {64, "0098ba824b5c16427bd7a1122a5a442a25ec644d"},
        {65, "11655326c708d70319be2610e8a57d9a5b959d3b"},
        {119, "ee971065aaa017e0632a8ca6c77bb3bf8b1dfc56"},
        {120, "f34c1488385346a55709ba056ddd08280dd4c6d6"},
        {128, "ad5b3fdbcb526778c2839d2f151ea753995e26a0"},
    };
    for (const V &v : vs) {
        char msg[32];
        std::snprintf(msg, sizeof(msg), "len %u", unsigned(v.n));
        TEST_ASSERT_EQUAL_STRING_MESSAGE(v.h, sha(std::string(v.n, 'a')).c_str(), msg);
    }
}

// ---- Base64 ----

static std::string enc(const std::string &s) {
    return u::base64Encode(reinterpret_cast<const uint8_t *>(s.data()), s.size());
}

static void test_b64_encode_rfc4648() {
    TEST_ASSERT_EQUAL_STRING("", enc("").c_str());
    TEST_ASSERT_EQUAL_STRING("Zg==", enc("f").c_str());
    TEST_ASSERT_EQUAL_STRING("Zm8=", enc("fo").c_str());
    TEST_ASSERT_EQUAL_STRING("Zm9v", enc("foo").c_str());
    TEST_ASSERT_EQUAL_STRING("Zm9vYg==", enc("foob").c_str());
    TEST_ASSERT_EQUAL_STRING("Zm9vYmE=", enc("fooba").c_str());
    TEST_ASSERT_EQUAL_STRING("Zm9vYmFy", enc("foobar").c_str());
}

static void test_b64_encode_binary() {
    const uint8_t all[] = {0x00, 0xFF, 0xFE, 0xFB, 0xEF, 0x3E, 0x3F};
    // Python: base64.b64encode(bytes([0,255,254,251,239,62,63]))
    TEST_ASSERT_EQUAL_STRING("AP/+++8+Pw==", u::base64Encode(all, sizeof(all)).c_str());
}

static std::string dec(const std::string &t, bool &ok) {
    std::vector<uint8_t> out;
    ok = u::base64Decode(t, out);
    return std::string(out.begin(), out.end());
}

static void test_b64_decode_rfc4648() {
    const char *enc_[] = {"", "Zg==", "Zm8=", "Zm9v", "Zm9vYg==", "Zm9vYmE=", "Zm9vYmFy"};
    const char *plain[] = {"", "f", "fo", "foo", "foob", "fooba", "foobar"};
    for (int i = 0; i < 7; ++i) {
        bool ok = false;
        std::string d = dec(enc_[i], ok);
        TEST_ASSERT_TRUE_MESSAGE(ok, enc_[i]);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(plain[i], d.c_str(), enc_[i]);
    }
}

static void test_b64_decode_empty() {
    std::vector<uint8_t> out{1, 2, 3};
    TEST_ASSERT_TRUE(u::base64Decode("", out));
    TEST_ASSERT_EQUAL_UINT32(0, out.size());
}

static void test_b64_decode_ignores_crlf() {
    bool ok = false;
    TEST_ASSERT_EQUAL_STRING("foobar", dec("Zm9v\nYmFy", ok).c_str());
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_STRING("foobar", dec("Zm9v\r\nYmFy\r\n", ok).c_str());
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_STRING("fooba", dec("Zm9v\r\nYmE=\r\n", ok).c_str());
    TEST_ASSERT_TRUE(ok);
    // newline inside a group and before padding
    TEST_ASSERT_EQUAL_STRING("f", dec("Z\ng\r\n=\n=", ok).c_str());
    TEST_ASSERT_TRUE(ok);
}

static void test_b64_decode_rejects() {
    const char *bad[] = {
        "Zm9!",      // bad char
        "Zm9v YmFy", // space
        "Zm9-",      // url-safe alphabet
        "Zm9_",      // url-safe alphabet
        "Zg",        // missing padding
        "Zg=",       // short padding
        "Zm8",       // missing padding
        "Zm9vY",     // incomplete group
        "Q===",      // too much padding
        "QQ=",       // padding incomplete
        "====",      // only padding
        "Zg==Zg==",  // data after padding
        "Zg==Z",     // data after padding
        "Zm=v",      // '=' in the middle
        "Z=g=",      // '=' in the middle
        "=Zg=",      // leading '='
        "Zm9v=",     // stray '=' after a full group
    };
    for (const char *b : bad) {
        std::vector<uint8_t> out{9, 9};
        TEST_ASSERT_FALSE_MESSAGE(u::base64Decode(b, out), b);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, out.size(), b);  // out left empty
    }
}

static void test_b64_roundtrip_all_lengths() {
    std::vector<uint8_t> data;
    for (int n = 0; n < 40; ++n) {
        std::string e = u::base64Encode(data.data(), data.size());
        std::vector<uint8_t> back;
        TEST_ASSERT_TRUE(u::base64Decode(e, back));
        TEST_ASSERT_TRUE(back == data);
        data.push_back(uint8_t(n * 37 + 11));
    }
}

// ---- Text: truncateUtf8 / logSafe ----

static void test_logsafe_control_chars() {
    TEST_ASSERT_EQUAL_STRING("a?b?c", u::logSafe("a\nb\x7f" "c", 10).c_str());
    TEST_ASSERT_EQUAL_STRING("?", u::logSafe("\x1f", 10).c_str());
    TEST_ASSERT_EQUAL_STRING("?x", u::logSafe("\x01x", 10).c_str());
    TEST_ASSERT_EQUAL_STRING("a?b", u::logSafe("a\rb", 10).c_str());
    TEST_ASSERT_EQUAL_UINT32(3, (uint32_t)u::logSafe(std::string("a\0b", 3), 10).size());
    TEST_ASSERT_EQUAL_STRING("a?b", u::logSafe(std::string("a\0b", 3), 10).c_str());
}

static void test_logsafe_keeps_utf8() {
    TEST_ASSERT_EQUAL_STRING("K\xC3\xB8kken", u::logSafe("K\xC3\xB8kken", 20).c_str());
    TEST_ASSERT_EQUAL_STRING("a~", u::logSafe("a~", 5).c_str());
}

static void test_truncate_utf8_boundaries() {
    TEST_ASSERT_EQUAL_STRING("ab", u::logSafe("ab\xC2\xB0", 3).c_str());  // cut inside 2-byte
    TEST_ASSERT_EQUAL_STRING("ab\xC2\xB0", u::logSafe("ab\xC2\xB0", 4).c_str());
    const char *three = "a\xE2\x82\xAC";  // euro sign
    TEST_ASSERT_EQUAL_STRING("a", u::truncateUtf8(three, 2).c_str());
    TEST_ASSERT_EQUAL_STRING("a", u::truncateUtf8(three, 3).c_str());
    TEST_ASSERT_EQUAL_STRING(three, u::truncateUtf8(three, 4).c_str());
    const char *four = "a\xF0\x9F\x98\x80";  // emoji
    TEST_ASSERT_EQUAL_STRING("a", u::truncateUtf8(four, 2).c_str());
    TEST_ASSERT_EQUAL_STRING("a", u::truncateUtf8(four, 3).c_str());
    TEST_ASSERT_EQUAL_STRING("a", u::truncateUtf8(four, 4).c_str());
    TEST_ASSERT_EQUAL_STRING(four, u::truncateUtf8(four, 5).c_str());
    TEST_ASSERT_EQUAL_STRING("", u::truncateUtf8("\xF0\x9F\x98\x80", 3).c_str());
    TEST_ASSERT_EQUAL_STRING("abc", u::truncateUtf8("abcdef", 3).c_str());
    TEST_ASSERT_EQUAL_STRING("abc", u::truncateUtf8("abc", 3).c_str());
    TEST_ASSERT_EQUAL_STRING("abc", u::truncateUtf8("abc", 100).c_str());
}

static void test_text_zero_and_empty() {
    TEST_ASSERT_EQUAL_STRING("", u::logSafe("abc", 0).c_str());
    TEST_ASSERT_EQUAL_STRING("", u::truncateUtf8("abc", 0).c_str());
    TEST_ASSERT_EQUAL_STRING("", u::logSafe("", 10).c_str());
    TEST_ASSERT_EQUAL_STRING("", u::truncateUtf8("", 0).c_str());
}

// ---- MathUtil: decimalsForStep / roundToDecimals ----

static void test_math_decimals_for_step() {
    TEST_ASSERT_EQUAL_INT(0, core::decimalsForStep(1));
    TEST_ASSERT_EQUAL_INT(0, core::decimalsForStep(100));
    TEST_ASSERT_EQUAL_INT(1, core::decimalsForStep(0.5));
    TEST_ASSERT_EQUAL_INT(1, core::decimalsForStep(0.1));
    TEST_ASSERT_EQUAL_INT(1, core::decimalsForStep(0.3));
    TEST_ASSERT_EQUAL_INT(2, core::decimalsForStep(0.25));
    TEST_ASSERT_EQUAL_INT(3, core::decimalsForStep(0.125));
    TEST_ASSERT_EQUAL_INT(3, core::decimalsForStep(33.333333));
    TEST_ASSERT_EQUAL_INT(0, core::decimalsForStep(0));
    TEST_ASSERT_EQUAL_INT(0, core::decimalsForStep(-0.5));
    TEST_ASSERT_EQUAL_INT(0, core::decimalsForStep(std::nan("")));
}

static void test_math_round_to_decimals() {
    TEST_ASSERT_EQUAL_FLOAT(3, core::roundToDecimals(2.5, 0));
    TEST_ASSERT_EQUAL_FLOAT(-3, core::roundToDecimals(-2.5, 0));
    TEST_ASSERT_EQUAL_FLOAT(21.25, core::roundToDecimals(21.2500001, 2));
    TEST_ASSERT_EQUAL_FLOAT(1.234568, core::roundToDecimals(1.23456789, 9));  // clamped to 6
    TEST_ASSERT_EQUAL_FLOAT(2, core::roundToDecimals(1.6, -3));               // clamped to 0
    TEST_ASSERT_FALSE(std::signbit(core::roundToDecimals(-0.0001, 1)));
    TEST_ASSERT_FALSE(std::signbit(core::roundToDecimals(-0.0, 1)));
    TEST_ASSERT_TRUE(core::roundToDecimals(30.000000000000004, 1) == 30.0);
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_sha1_empty);
    RUN_TEST(test_sha1_abc);
    RUN_TEST(test_sha1_56_bytes);
    RUN_TEST(test_sha1_million_a);
    RUN_TEST(test_sha1_padding_boundaries);
    RUN_TEST(test_b64_encode_rfc4648);
    RUN_TEST(test_b64_encode_binary);
    RUN_TEST(test_b64_decode_rfc4648);
    RUN_TEST(test_b64_decode_empty);
    RUN_TEST(test_b64_decode_ignores_crlf);
    RUN_TEST(test_b64_decode_rejects);
    RUN_TEST(test_b64_roundtrip_all_lengths);
    RUN_TEST(test_logsafe_control_chars);
    RUN_TEST(test_logsafe_keeps_utf8);
    RUN_TEST(test_truncate_utf8_boundaries);
    RUN_TEST(test_text_zero_and_empty);
    RUN_TEST(test_math_decimals_for_step);
    RUN_TEST(test_math_round_to_decimals);
    return UNITY_END();
}
