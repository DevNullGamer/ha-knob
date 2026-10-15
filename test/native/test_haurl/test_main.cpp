// Native tests for HaUrl (plan.md section 3.7) plus ConfigModel regression for ha.url behaviour.
#include <unity.h>

#include <string>

#include "config/ConfigModel.h"
#include "ha/HaUrl.h"

namespace ha = core::ha;
namespace cfg = core::config;

void setUp() {}
void tearDown() {}

static void test_http_with_port() {
    const ha::HaUrl u = ha::parseHaUrl("http://192.168.1.10:8123");
    TEST_ASSERT_TRUE(u.ok);
    TEST_ASSERT_FALSE(u.tls);
    TEST_ASSERT_EQUAL_STRING("192.168.1.10", u.host.c_str());
    TEST_ASSERT_EQUAL_UINT16(8123, u.port);
    TEST_ASSERT_EQUAL_STRING("/api/websocket", u.path.c_str());
    TEST_ASSERT_FALSE(u.hasQueryOrFragment);
    TEST_ASSERT_EQUAL_STRING("ws://192.168.1.10:8123/api/websocket", ha::webSocketUrl(u).c_str());
}

static void test_trailing_slash() {
    const ha::HaUrl u = ha::parseHaUrl("http://homeassistant.local:8123/");
    TEST_ASSERT_TRUE(u.ok);
    TEST_ASSERT_EQUAL_STRING("homeassistant.local", u.host.c_str());
    TEST_ASSERT_EQUAL_STRING("/api/websocket", u.path.c_str());
}

static void test_https_default_port() {
    const ha::HaUrl u = ha::parseHaUrl("https://x.ui.nabu.casa");
    TEST_ASSERT_TRUE(u.ok);
    TEST_ASSERT_TRUE(u.tls);
    TEST_ASSERT_EQUAL_UINT16(443, u.port);
    TEST_ASSERT_EQUAL_STRING("wss://x.ui.nabu.casa:443/api/websocket", ha::webSocketUrl(u).c_str());
}

static void test_http_default_port_and_case() {
    const ha::HaUrl u = ha::parseHaUrl("HTTP://HA.Local");
    TEST_ASSERT_TRUE(u.ok);
    TEST_ASSERT_FALSE(u.tls);
    TEST_ASSERT_EQUAL_UINT16(80, u.port);
    TEST_ASSERT_EQUAL_STRING("ha.local", u.host.c_str());
}

static void test_rejected() {
    const char *bad[] = {"ftp://ha.local", "ws://ha.local:8123", "http://",        "http://:8123",
                         "http://ha:70000", "http://ha:0",       "http://ha:",      "http://ha:80a",
                         "http://user@ha",  "ha.local",          "",                "http://ha local",
                         "http://ha:65536", "http://[::1]:8123"};
    for (const char *s : bad) {
        TEST_ASSERT_FALSE_MESSAGE(ha::parseHaUrl(s).ok, s);
    }
    TEST_ASSERT_EQUAL_STRING("", ha::webSocketUrl(ha::parseHaUrl("ftp://x")).c_str());
}

static void test_port_bounds() {
    TEST_ASSERT_TRUE(ha::parseHaUrl("http://ha:65535").ok);
    TEST_ASSERT_TRUE(ha::parseHaUrl("http://ha:1").ok);
    TEST_ASSERT_EQUAL_UINT16(65535, ha::parseHaUrl("http://ha:65535").port);
}

static void test_prefix_kept() {
    ha::HaUrl u = ha::parseHaUrl("http://ha:8123/prefix");
    TEST_ASSERT_TRUE(u.ok);
    TEST_ASSERT_EQUAL_STRING("/prefix/api/websocket", u.path.c_str());
    u = ha::parseHaUrl("https://x.ui.nabu.casa/ha/");
    TEST_ASSERT_EQUAL_STRING("/ha/api/websocket", u.path.c_str());
    u = ha::parseHaUrl("http://ha/a/b//");
    TEST_ASSERT_EQUAL_STRING("/a/b/api/websocket", u.path.c_str());
}

static void test_query_fragment_flag() {
    TEST_ASSERT_TRUE(ha::parseHaUrl("http://h/x?y").hasQueryOrFragment);
    TEST_ASSERT_TRUE(ha::parseHaUrl("http://h/#a").hasQueryOrFragment);
    TEST_ASSERT_TRUE(ha::parseHaUrl("http://h/x?y").ok);
    TEST_ASSERT_FALSE(ha::parseHaUrl("http://h/x").hasQueryOrFragment);
    TEST_ASSERT_FALSE(ha::parseHaUrl("http://h").hasQueryOrFragment);
    TEST_ASSERT_FALSE(ha::parseHaUrl("http://h:8123/").hasQueryOrFragment);
}

static void test_host_length() {
    const std::string h253(253, 'a');
    TEST_ASSERT_TRUE(ha::parseHaUrl("http://" + h253).ok);
    TEST_ASSERT_FALSE(ha::parseHaUrl("http://" + h253 + "a").ok);
}

// ---- ConfigModel regression (behaviour unchanged vs v0.4.0) ----

static void test_config_validate_unchanged() {
    cfg::Config c;
    const char *good[] = {"http://h/x?y", "http://h/#a", "https://h", "http://192.168.1.10:8123",
                          "https://ha.example.com/prefix/path", ""};
    for (const char *u : good) {
        c.ha.url = u;
        TEST_ASSERT_TRUE_MESSAGE(cfg::validate(c).ok, u);
    }
    const char *bad[] = {"ftp://x", "http://", "http://ha:0", "http://ha:65536", "http://ha local"};
    for (const char *u : bad) {
        c.ha.url = u;
        const cfg::ValidationResult r = cfg::validate(c);
        TEST_ASSERT_FALSE_MESSAGE(r.ok, u);
        TEST_ASSERT_EQUAL_STRING_MESSAGE("ha.url", r.field.c_str(), u);
    }
}

static void test_config_parse_url_endpoint() {
    cfg::UrlEndpoint e = cfg::parseUrlEndpoint("http://HA.Local:8123/x");
    TEST_ASSERT_TRUE(e.ok);
    e = cfg::parseUrlEndpoint("https://ha.local/");
    TEST_ASSERT_TRUE(e.ok);
    TEST_ASSERT_FALSE(cfg::parseUrlEndpoint("ftp://ha").ok);
    TEST_ASSERT_FALSE(cfg::parseUrlEndpoint("http://ha:0").ok);
    TEST_ASSERT_TRUE(cfg::parseUrlEndpoint("http://h/x?y").ok);
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_http_with_port);
    RUN_TEST(test_trailing_slash);
    RUN_TEST(test_https_default_port);
    RUN_TEST(test_http_default_port_and_case);
    RUN_TEST(test_rejected);
    RUN_TEST(test_port_bounds);
    RUN_TEST(test_prefix_kept);
    RUN_TEST(test_query_fragment_flag);
    RUN_TEST(test_host_length);
    RUN_TEST(test_config_validate_unchanged);
    RUN_TEST(test_config_parse_url_endpoint);
    return UNITY_END();
}
