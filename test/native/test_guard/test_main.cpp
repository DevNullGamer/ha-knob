// Native tests for the web request guards (plan.md §3.8, §4 Step 4).
#include <unity.h>

#include <string>

#include "web/RequestGuard.h"

namespace web = core::web;
using web::Method;
using web::Verdict;

void setUp() {}
void tearDown() {}

namespace {

web::GuardContext staCtx() {
    web::GuardContext c;
    c.hostname = "ha-knob";
    c.deviceIp = "192.168.1.50";
    c.apActive = false;
    return c;
}

web::GuardContext apCtx() {
    web::GuardContext c;
    c.hostname = "ha-knob";
    c.deviceIp = "";
    c.apActive = true;
    return c;
}

web::RequestInfo get(const char *host, const char *path = "/api/status") {
    web::RequestInfo r;
    r.method = Method::Get;
    r.host = host;
    r.path = path;
    return r;
}

// A POST that passes every guard.
web::RequestInfo goodPost(const char *host = "192.168.1.50") {
    web::RequestInfo r;
    r.method = Method::Post;
    r.host = host;
    r.path = "/api/config";
    r.contentType = "application/json";
    r.hasCsrfHeader = true;
    r.csrfHeader = "1";
    r.contentLength = 100;
    return r;
}

void expect(const web::RequestInfo &r, const web::GuardContext &c, Verdict v, int status,
            const char *what) {
    const web::Decision d = web::checkRequest(r, c);
    TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(v), static_cast<int>(d.verdict), what);
    TEST_ASSERT_EQUAL_INT_MESSAGE(status, d.status, what);
    TEST_ASSERT_EQUAL_MESSAGE(v == Verdict::Allow, d.allowed(), what);
}

void allow(const web::RequestInfo &r, const web::GuardContext &c, const char *what) {
    expect(r, c, Verdict::Allow, 200, what);
}

void forbid(const web::RequestInfo &r, const web::GuardContext &c, const char *what) {
    expect(r, c, Verdict::Forbidden, 403, what);
}

// "Basic " + base64(s)
std::string basic(const char *b64) { return std::string("Basic ") + b64; }

}  // namespace

// ---------------------------------------------------------------------------
// Host allow-list (spec cases)
// ---------------------------------------------------------------------------

static void test_host_device_ip_allowed() {
    allow(get("192.168.1.50"), staCtx(), "device ip");
    allow(goodPost("192.168.1.50"), staCtx(), "device ip post");
}

static void test_host_mdns_name_allowed() {
    allow(get("ha-knob.local"), staCtx(), "ha-knob.local");
    allow(goodPost("ha-knob.local"), staCtx(), "ha-knob.local post");
}

static void test_host_bare_hostname_allowed() {
    allow(get("ha-knob"), staCtx(), "ha-knob");
    allow(get("ha-knob:80"), staCtx(), "ha-knob:80");
}

static void test_host_ap_ip_allowed() {
    allow(get("192.168.4.1"), apCtx(), "ap ip in ap mode");
    allow(get("192.168.4.1"), staCtx(), "ap ip in sta mode");
    allow(goodPost("192.168.4.1"), apCtx(), "post to ap ip");
}

static void test_host_foreign_rejected() {
    forbid(get("evil.com"), staCtx(), "evil.com");
    forbid(goodPost("evil.com"), staCtx(), "evil.com post");
    forbid(goodPost("evil.example"), staCtx(), "evil.example post");
    forbid(get("192.168.1.51"), staCtx(), "other ip");
    forbid(get("ha-knob.local.evil.com"), staCtx(), "suffix attack");
    forbid(get("evil-ha-knob.local"), staCtx(), "prefix attack");
    forbid(get("ha-knob.lan"), staCtx(), "other tld");
    forbid(get("ha-knob.localhost"), staCtx(), "localhost tld");
    forbid(get("localhost"), staCtx(), "localhost");
    forbid(get("127.0.0.1"), staCtx(), "loopback");
    forbid(get("192.168.1.500"), staCtx(), "ip prefix match");
    forbid(get("2192.168.1.50"), staCtx(), "ip suffix match");
    forbid(get("ha-knob.local.."), staCtx(), "two trailing dots");
}

static void test_host_with_port_suffix() {
    allow(get("192.168.1.50:80"), staCtx(), "ip:80");
    allow(get("ha-knob.local:80"), staCtx(), "mdns:80");
    allow(get("ha-knob.local:8080"), staCtx(), "mdns:8080");
    allow(get("192.168.4.1:80"), apCtx(), "ap:80");
    allow(get("ha-knob.local:1"), staCtx(), "port 1");
    allow(get("ha-knob.local:65535"), staCtx(), "port 65535");
    forbid(get("evil.com:80"), staCtx(), "evil:80");
}

static void test_host_case_insensitive_and_trailing_dot() {
    allow(get("HA-KNOB.LOCAL"), staCtx(), "upper");
    allow(get("Ha-Knob.Local:80"), staCtx(), "mixed with port");
    allow(get("ha-knob.local."), staCtx(), "trailing dot");
    allow(get("HA-KNOB.local.:80"), staCtx(), "trailing dot with port");
    allow(get("ha-knob."), staCtx(), "bare hostname trailing dot");
    web::GuardContext c = staCtx();
    c.hostname = "Kitchen-Knob";  // context case does not matter either
    allow(get("kitchen-knob.local"), c, "context upper");
    forbid(get("ha-knob.local"), c, "old hostname not allowed");
}

static void test_host_bad_port_rejected() {
    const char *bad[] = {"ha-knob.local:0",      "ha-knob.local:65536", "ha-knob.local:99999",
                         "ha-knob.local:abc",    "ha-knob.local:",      "ha-knob.local:80:80",
                         "ha-knob.local:8a",     "ha-knob.local:-80",   "ha-knob.local:123456",
                         "192.168.1.50:80:",     "ha-knob.local: 80"};
    for (const char *h : bad) {
        forbid(get(h), staCtx(), h);
        std::string name = "x";
        TEST_ASSERT_FALSE_MESSAGE(web::normaliseHost(h, name), h);
    }
}

static void test_host_ipv6_literal_rejected() {
    forbid(get("[::1]"), staCtx(), "[::1]");
    forbid(get("[::1]:80"), staCtx(), "[::1]:80");
    forbid(get("[fe80::1]"), staCtx(), "[fe80::1]");
    forbid(get("::1"), staCtx(), "bare ::1");
    std::string name;
    TEST_ASSERT_FALSE(web::normaliseHost("[::1]", name));
}

static void test_host_bad_characters_rejected() {
    const char *bad[] = {"ha_knob.local", "ha-knob.local/x", "user@ha-knob.local", "ha knob.local",
                         "ha-knob.l\xC3\xB8""cal", "ha-knob.local\r\n", "*.local"};
    for (const char *h : bad) forbid(get(h), staCtx(), h);
}

static void test_host_empty_rejected() {
    forbid(get(""), staCtx(), "empty host, sta");
    forbid(get("   "), staCtx(), "blank host, sta");
    forbid(get(":80"), staCtx(), "port only");
    // Not connected (deviceIp ""): an empty Host must not match the empty IP.
    web::GuardContext c = staCtx();
    c.deviceIp = "";
    forbid(get(""), c, "empty host, empty ip");
    forbid(goodPost(""), c, "empty host post, empty ip");
    TEST_ASSERT_FALSE(web::isAllowedHost("", c));
    // Empty hostname in the context must not allow ".local".
    c.hostname = "";
    forbid(get(".local"), c, ".local with empty hostname");
    TEST_ASSERT_FALSE(web::isAllowedHost(".local", c));
}

static void test_host_device_ip_not_connected() {
    web::GuardContext c = staCtx();
    c.deviceIp = "";
    forbid(get("192.168.1.50"), c, "old ip once disconnected");
    allow(get("ha-knob.local"), c, "mdns still ok");
    allow(get("192.168.4.1"), c, "ap ip still ok");
}

static void test_normalise_host() {
    std::string name;
    TEST_ASSERT_TRUE(web::normaliseHost("HA-Knob.Local.:8080", name));
    TEST_ASSERT_EQUAL_STRING("ha-knob.local", name.c_str());
    TEST_ASSERT_TRUE(web::normaliseHost("192.168.1.50", name));
    TEST_ASSERT_EQUAL_STRING("192.168.1.50", name.c_str());
    TEST_ASSERT_TRUE(web::normaliseHost(" ha-knob ", name));
    TEST_ASSERT_EQUAL_STRING("ha-knob", name.c_str());
    TEST_ASSERT_FALSE(web::normaliseHost("", name));
    TEST_ASSERT_TRUE(name.empty());
    TEST_ASSERT_FALSE(web::normaliseHost(".", name));
    TEST_ASSERT_FALSE(web::normaliseHost("a:0", name));
}

static void test_is_allowed_host_helper() {
    const web::GuardContext c = staCtx();
    TEST_ASSERT_TRUE(web::isAllowedHost("192.168.1.50", c));
    TEST_ASSERT_TRUE(web::isAllowedHost("ha-knob.local", c));
    TEST_ASSERT_TRUE(web::isAllowedHost("ha-knob", c));
    TEST_ASSERT_TRUE(web::isAllowedHost("192.168.4.1", c));
    TEST_ASSERT_FALSE(web::isAllowedHost("evil.com", c));
    TEST_ASSERT_FALSE(web::isAllowedHost("[::1]", c));
}

// ---------------------------------------------------------------------------
// Captive portal
// ---------------------------------------------------------------------------

static void test_captive_redirect_only_when_ap_active() {
    const web::GuardContext ap = apCtx();
    expect(get("connectivitycheck.gstatic.com", "/generate_204"), ap, Verdict::Redirect, 302, "android");
    expect(get("captive.apple.com", "/hotspot-detect.html"), ap, Verdict::Redirect, 302, "apple");
    expect(get("evil.com", "/"), ap, Verdict::Redirect, 302, "root");
    web::RequestInfo head = get("www.msftconnecttest.com", "/connecttest.txt");
    head.method = Method::Head;
    expect(head, ap, Verdict::Redirect, 302, "head");
    expect(get("", "/"), ap, Verdict::Redirect, 302, "empty host in ap mode");
    // AP down: no redirect, plain 403.
    forbid(get("connectivitycheck.gstatic.com", "/generate_204"), staCtx(), "sta mode");
    TEST_ASSERT_EQUAL_STRING("http://192.168.4.1/", web::kCaptivePortalUrl);
}

static void test_captive_never_for_api_or_unsafe_methods() {
    const web::GuardContext ap = apCtx();
    forbid(get("evil.com", "/api/status"), ap, "api get foreign host");
    forbid(get("evil.com", "/api"), ap, "/api foreign host");
    forbid(get("evil.com", "/api/config"), ap, "api config");
    web::RequestInfo p = goodPost("evil.com");
    forbid(p, ap, "post foreign host");
    p.path = "/";
    forbid(p, ap, "post to / foreign host");
    web::RequestInfo o = get("evil.com", "/");
    o.method = Method::Options;
    forbid(o, ap, "options foreign host");
    o.method = Method::Other;
    forbid(o, ap, "put foreign host");
}

static void test_ap_mode_allowed_host_not_redirected() {
    const web::GuardContext ap = apCtx();
    allow(get("192.168.4.1", "/"), ap, "ap root");
    allow(get("192.168.4.1", "/api/status"), ap, "ap api");
    allow(get("ha-knob.local", "/"), ap, "mdns in ap mode");
}

static void test_is_api_path() {
    TEST_ASSERT_TRUE(web::isApiPath("/api"));
    TEST_ASSERT_TRUE(web::isApiPath("/api/"));
    TEST_ASSERT_TRUE(web::isApiPath("/api/status"));
    TEST_ASSERT_TRUE(web::isApiPath("/api/ha/entities/refresh"));
    TEST_ASSERT_FALSE(web::isApiPath("/apix"));
    TEST_ASSERT_FALSE(web::isApiPath("/apis/x"));
    TEST_ASSERT_FALSE(web::isApiPath("/"));
    TEST_ASSERT_FALSE(web::isApiPath(""));
    TEST_ASSERT_FALSE(web::isApiPath("api/status"));
    TEST_ASSERT_FALSE(web::isApiPath("/index.html"));
}

// ---------------------------------------------------------------------------
// Methods
// ---------------------------------------------------------------------------

static void test_methods() {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Method::Get), static_cast<int>(web::methodFromString("GET")));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Method::Head), static_cast<int>(web::methodFromString("HEAD")));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Method::Post), static_cast<int>(web::methodFromString("POST")));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Method::Options),
                          static_cast<int>(web::methodFromString("OPTIONS")));
    const char *other[] = {"PUT", "DELETE", "PATCH", "TRACE", "CONNECT", "get", "", "POSTX"};
    for (const char *m : other) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(Method::Other),
                                      static_cast<int>(web::methodFromString(m)), m);
    }
}

static void test_options_put_delete_405() {
    web::RequestInfo r = get("192.168.1.50", "/api/config");
    r.method = Method::Options;
    expect(r, staCtx(), Verdict::MethodNotAllowed, 405, "options");
    r.method = web::methodFromString("PUT");
    expect(r, staCtx(), Verdict::MethodNotAllowed, 405, "put");
    r.method = web::methodFromString("DELETE");
    expect(r, staCtx(), Verdict::MethodNotAllowed, 405, "delete");
    // A preflight-looking OPTIONS with JSON and the CSRF header is still 405.
    r.method = Method::Options;
    r.contentType = "application/json";
    r.hasCsrfHeader = true;
    r.csrfHeader = "1";
    expect(r, staCtx(), Verdict::MethodNotAllowed, 405, "options with headers");
}

static void test_get_head_need_no_csrf() {
    web::RequestInfo r = get("192.168.1.50", "/api/config");
    allow(r, staCtx(), "get without csrf");
    r.method = Method::Head;
    allow(r, staCtx(), "head without csrf");
    r.contentType = "text/plain";
    allow(r, staCtx(), "head with text/plain");
}

// ---------------------------------------------------------------------------
// POST: content type and CSRF header
// ---------------------------------------------------------------------------

static void test_post_good_allowed() { allow(goodPost(), staCtx(), "good post"); }

static void test_post_without_csrf_header_403() {
    web::RequestInfo r = goodPost();
    r.hasCsrfHeader = false;
    r.csrfHeader = "";
    forbid(r, staCtx(), "no X-HAKnob");
}

static void test_post_csrf_value_must_be_1() {
    const char *bad[] = {"", "0", "2", "01", "true", "yes", "1 1", "11", "l"};
    for (const char *v : bad) {
        web::RequestInfo r = goodPost();
        r.csrfHeader = v;
        forbid(r, staCtx(), v);
    }
}

static void test_post_text_plain_403() {
    web::RequestInfo r = goodPost();
    r.contentType = "text/plain";
    forbid(r, staCtx(), "text/plain");
    r.hasCsrfHeader = false;
    forbid(r, staCtx(), "text/plain without csrf");
}

static void test_post_other_simple_content_types_403() {
    const char *bad[] = {"", "application/x-www-form-urlencoded", "multipart/form-data; boundary=x",
                         "text/plain; charset=utf-8", "application/json-evil", "application/jsonp",
                         "application/json, text/plain", "text/json", "application/ld+json",
                         "application/json5", "xapplication/json"};
    for (const char *ct : bad) {
        web::RequestInfo r = goodPost();
        r.contentType = ct;
        forbid(r, staCtx(), ct);
        TEST_ASSERT_FALSE_MESSAGE(web::isJsonContentType(ct), ct);
    }
}

static void test_json_content_type_variants_ok() {
    const char *good[] = {"application/json", "application/json; charset=utf-8", "Application/JSON",
                          "application/json;charset=UTF-8", " application/json ",
                          "application/json ; charset=utf-8"};
    for (const char *ct : good) {
        TEST_ASSERT_TRUE_MESSAGE(web::isJsonContentType(ct), ct);
        web::RequestInfo r = goodPost();
        r.contentType = ct;
        allow(r, staCtx(), ct);
    }
}

// ---------------------------------------------------------------------------
// Chunked and body size
// ---------------------------------------------------------------------------

static void test_chunked_post_411() {
    web::RequestInfo r = goodPost();
    r.chunked = true;
    r.contentLength = 0;
    expect(r, staCtx(), Verdict::LengthRequired, 411, "chunked");
}

static void test_body_size_limit() {
    TEST_ASSERT_EQUAL_size_t(16384, web::kMaxBodyBytes);
    web::RequestInfo r = goodPost();
    r.contentLength = 16384;
    allow(r, staCtx(), "exactly 16 KB");
    r.contentLength = 16385;
    expect(r, staCtx(), Verdict::PayloadTooLarge, 413, "16 KB + 1");
    r.contentLength = 10u * 1024u * 1024u;
    expect(r, staCtx(), Verdict::PayloadTooLarge, 413, "10 MB");
    r.contentLength = 0;
    allow(r, staCtx(), "empty body");
    // Any method.
    web::RequestInfo g = get("192.168.1.50", "/api/status");
    g.contentLength = 16385;
    expect(g, staCtx(), Verdict::PayloadTooLarge, 413, "get with big body");
}

// ---------------------------------------------------------------------------
// Check order: host -> method -> content type + csrf -> chunked -> length
// ---------------------------------------------------------------------------

static void test_check_order() {
    // Foreign host beats method.
    web::RequestInfo r = get("evil.com", "/api/config");
    r.method = Method::Options;
    forbid(r, staCtx(), "host before method");
    // Method beats content type.
    r = get("192.168.1.50", "/api/config");
    r.method = Method::Other;
    r.contentType = "text/plain";
    expect(r, staCtx(), Verdict::MethodNotAllowed, 405, "method before content type");
    // Method beats size.
    r.contentLength = 99999;
    expect(r, staCtx(), Verdict::MethodNotAllowed, 405, "method before size");
    // Content type / CSRF beat chunked.
    r = goodPost();
    r.chunked = true;
    r.hasCsrfHeader = false;
    forbid(r, staCtx(), "csrf before chunked");
    r = goodPost();
    r.chunked = true;
    r.contentType = "text/plain";
    forbid(r, staCtx(), "content type before chunked");
    // CSRF beats size.
    r = goodPost();
    r.contentLength = 20000;
    r.hasCsrfHeader = false;
    forbid(r, staCtx(), "csrf before size");
    // Chunked beats size.
    r = goodPost();
    r.chunked = true;
    r.contentLength = 20000;
    expect(r, staCtx(), Verdict::LengthRequired, 411, "chunked before size");
    // Foreign host beats everything for POST too.
    r = goodPost("evil.com");
    r.contentLength = 20000;
    r.chunked = true;
    forbid(r, staCtx(), "host first for post");
}

static void test_decision_reasons_set() {
    const web::Decision d = web::checkRequest(get("evil.com"), staCtx());
    TEST_ASSERT_NOT_NULL(d.reason);
    TEST_ASSERT_TRUE(std::string(d.reason).size() > 0);
    const web::Decision ok = web::checkRequest(get("ha-knob.local"), staCtx());
    TEST_ASSERT_TRUE(ok.allowed());
    TEST_ASSERT_EQUAL_INT(200, ok.status);
}

// ---------------------------------------------------------------------------
// base64, constant-time compare, Basic auth
// ---------------------------------------------------------------------------

static void test_base64_decode_valid() {
    std::string out;
    TEST_ASSERT_TRUE(web::base64Decode("", out));
    TEST_ASSERT_EQUAL_STRING("", out.c_str());
    TEST_ASSERT_TRUE(web::base64Decode("Zg==", out));
    TEST_ASSERT_EQUAL_STRING("f", out.c_str());
    TEST_ASSERT_TRUE(web::base64Decode("Zm8=", out));
    TEST_ASSERT_EQUAL_STRING("fo", out.c_str());
    TEST_ASSERT_TRUE(web::base64Decode("Zm9v", out));
    TEST_ASSERT_EQUAL_STRING("foo", out.c_str());
    TEST_ASSERT_TRUE(web::base64Decode("Zm9vYmFy", out));
    TEST_ASSERT_EQUAL_STRING("foobar", out.c_str());
    TEST_ASSERT_TRUE(web::base64Decode("YWRtaW46c2VjcmV0", out));  // admin:secret
    TEST_ASSERT_EQUAL_STRING("admin:secret", out.c_str());
    TEST_ASSERT_TRUE(web::base64Decode("+/+/", out));
    TEST_ASSERT_EQUAL_size_t(3, out.size());
    TEST_ASSERT_EQUAL_HEX8(0xFB, static_cast<unsigned char>(out[0]));
    TEST_ASSERT_EQUAL_HEX8(0xFF, static_cast<unsigned char>(out[1]));
    TEST_ASSERT_EQUAL_HEX8(0xBF, static_cast<unsigned char>(out[2]));
    TEST_ASSERT_TRUE(web::base64Decode("AA==", out));  // NUL byte
    TEST_ASSERT_EQUAL_size_t(1, out.size());
    TEST_ASSERT_EQUAL_HEX8(0, static_cast<unsigned char>(out[0]));
}

static void test_base64_decode_strict_rejects() {
    const char *bad[] = {"Zg",      "Zg=",      "Zm9",      "Z",        "Zg===",   "=Zg=",
                         "Z===",    "====",     "Zg==Zg==", "Zm=v",     "Zm9v=",   "Zm9v!",
                         "Zm9 v",   "Zm9v\n",   "Zm-v",     "Zm_v",     "Zm9\xC3", "Z\0g="};
    for (const char *b : bad) {
        std::string out = "junk";
        TEST_ASSERT_FALSE_MESSAGE(web::base64Decode(b, out), b);
    }
    std::string out;
    TEST_ASSERT_FALSE(web::base64Decode(std::string("Zg=\0", 4), out));
}

static void test_constant_time_equals() {
    TEST_ASSERT_TRUE(web::constantTimeEquals("", ""));
    TEST_ASSERT_TRUE(web::constantTimeEquals("abc", "abc"));
    TEST_ASSERT_FALSE(web::constantTimeEquals("abc", "abd"));
    TEST_ASSERT_FALSE(web::constantTimeEquals("abc", "xbc"));
    TEST_ASSERT_FALSE(web::constantTimeEquals("abc", "ab"));
    TEST_ASSERT_FALSE(web::constantTimeEquals("ab", "abc"));
    TEST_ASSERT_FALSE(web::constantTimeEquals("", "a"));
    TEST_ASSERT_FALSE(web::constantTimeEquals("a", ""));
    // A shorter string that is a prefix padded with NULs must not match.
    TEST_ASSERT_FALSE(web::constantTimeEquals(std::string("ab\0", 3), "ab"));
    TEST_ASSERT_TRUE(web::constantTimeEquals(std::string("a\0b", 3), std::string("a\0b", 3)));
}

static void test_basic_auth() {
    // admin:secret = YWRtaW46c2VjcmV0
    TEST_ASSERT_EQUAL_STRING("admin", web::kAuthUser);
    TEST_ASSERT_TRUE(web::basicAuthMatches(basic("YWRtaW46c2VjcmV0"), "admin", "secret"));
    TEST_ASSERT_TRUE(web::basicAuthMatches("basic YWRtaW46c2VjcmV0", "admin", "secret"));  // scheme case
    TEST_ASSERT_TRUE(web::basicAuthMatches("  Basic   YWRtaW46c2VjcmV0  ", "admin", "secret"));
    // Wrong password: admin:secreT
    TEST_ASSERT_FALSE(web::basicAuthMatches(basic("YWRtaW46c2VjcmVU"), "admin", "secret"));
    // Wrong user: root:secret
    TEST_ASSERT_FALSE(web::basicAuthMatches(basic("cm9vdDpzZWNyZXQ="), "admin", "secret"));
    // Password prefix / extension: admin:secre and admin:secrets
    TEST_ASSERT_FALSE(web::basicAuthMatches(basic("YWRtaW46c2VjcmU="), "admin", "secret"));
    TEST_ASSERT_FALSE(web::basicAuthMatches(basic("YWRtaW46c2VjcmV0cw=="), "admin", "secret"));
    // Empty password supplied (admin:) against a real password.
    TEST_ASSERT_FALSE(web::basicAuthMatches(basic("YWRtaW46"), "admin", "secret"));
    // No colon at all: "adminsecret"
    TEST_ASSERT_FALSE(web::basicAuthMatches(basic("YWRtaW5zZWNyZXQ="), "admin", "secret"));
    // Password containing ':' (admin:se:cret).
    TEST_ASSERT_TRUE(web::basicAuthMatches(basic("YWRtaW46c2U6Y3JldA=="), "admin", "se:cret"));
    // UTF-8 password (admin:pæss).
    TEST_ASSERT_TRUE(web::basicAuthMatches(basic("YWRtaW46cMOmc3M="), "admin", "p\xC3\xA6ss"));
}

static void test_basic_auth_malformed() {
    const char *bad[] = {"",
                         "Basic",
                         "Basic ",
                         "YWRtaW46c2VjcmV0",
                         "Bearer YWRtaW46c2VjcmV0",
                         "Digest YWRtaW46c2VjcmV0",
                         "BasicYWRtaW46c2VjcmV0",
                         "Basic YWRtaW46c2VjcmV0!",
                         "Basic YWRtaW46c2VjcmV",
                         "Basic admin:secret",
                         "Basic YWRtaW46 c2VjcmV0"};
    for (const char *h : bad) {
        TEST_ASSERT_FALSE_MESSAGE(web::basicAuthMatches(h, "admin", "secret"), h);
    }
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_host_device_ip_allowed);
    RUN_TEST(test_host_mdns_name_allowed);
    RUN_TEST(test_host_bare_hostname_allowed);
    RUN_TEST(test_host_ap_ip_allowed);
    RUN_TEST(test_host_foreign_rejected);
    RUN_TEST(test_host_with_port_suffix);
    RUN_TEST(test_host_case_insensitive_and_trailing_dot);
    RUN_TEST(test_host_bad_port_rejected);
    RUN_TEST(test_host_ipv6_literal_rejected);
    RUN_TEST(test_host_bad_characters_rejected);
    RUN_TEST(test_host_empty_rejected);
    RUN_TEST(test_host_device_ip_not_connected);
    RUN_TEST(test_normalise_host);
    RUN_TEST(test_is_allowed_host_helper);
    RUN_TEST(test_captive_redirect_only_when_ap_active);
    RUN_TEST(test_captive_never_for_api_or_unsafe_methods);
    RUN_TEST(test_ap_mode_allowed_host_not_redirected);
    RUN_TEST(test_is_api_path);
    RUN_TEST(test_methods);
    RUN_TEST(test_options_put_delete_405);
    RUN_TEST(test_get_head_need_no_csrf);
    RUN_TEST(test_post_good_allowed);
    RUN_TEST(test_post_without_csrf_header_403);
    RUN_TEST(test_post_csrf_value_must_be_1);
    RUN_TEST(test_post_text_plain_403);
    RUN_TEST(test_post_other_simple_content_types_403);
    RUN_TEST(test_json_content_type_variants_ok);
    RUN_TEST(test_chunked_post_411);
    RUN_TEST(test_body_size_limit);
    RUN_TEST(test_check_order);
    RUN_TEST(test_decision_reasons_set);
    RUN_TEST(test_base64_decode_valid);
    RUN_TEST(test_base64_decode_strict_rejects);
    RUN_TEST(test_constant_time_equals);
    RUN_TEST(test_basic_auth);
    RUN_TEST(test_basic_auth_malformed);
    return UNITY_END();
}
