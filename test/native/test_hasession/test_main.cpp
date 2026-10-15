// Native tests for HaSession (plan.md section 3.7): fake transport + fake clock.
#include <unity.h>

#include <ArduinoJson.h>

#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "ha/HaProtocol.h"
#include "ha/HaSession.h"

namespace ha = core::ha;
using S = ha::State;

void setUp() {}
void tearDown() {}

namespace {

struct FakeTransport : ha::Transport {
    int opens = 0;
    int closes = 0;
    std::vector<std::string> sent;
    ha::HaUrl lastEndpoint;
    ha::TlsSettings lastTls;
    bool sendResult = true;
    std::function<void()> onSend;  // runs inside sendText (before returning)

    void open(const ha::HaUrl &endpoint, const ha::TlsSettings &tls) override {
        ++opens;
        lastEndpoint = endpoint;
        lastTls = tls;
    }
    void close() override { ++closes; }
    bool sendText(const std::string &text) override {
        sent.push_back(text);
        if (onSend) onSend();
        return sendResult;
    }
};

const char *kAuthRequired = "{\"type\":\"auth_required\",\"ha_version\":\"2025.1.0\"}";
const char *kAuthOk = "{\"type\":\"auth_ok\",\"ha_version\":\"2025.1.0\"}";
const char *kAuthInvalid = "{\"type\":\"auth_invalid\",\"message\":\"Invalid access token or password\"}";
const char *kUrl = "http://192.168.1.10:8123";

void feed(ha::HaSession &s, const std::string &text, uint32_t now) { s.onText(text.c_str(), text.size(), now); }

// Connects (network up first) and answers auth_required / auth_ok. Returns the time used.
void toAuthenticated(ha::HaSession &s, FakeTransport &t, uint32_t now, const std::vector<std::string> &ids = {}) {
    s.setNetworkUp(true, now);
    s.configure(kUrl, "tok", ha::TlsSettings(), ids, now);
    TEST_ASSERT_EQUAL(S::Connecting, s.state());
    feed(s, kAuthRequired, now);
    TEST_ASSERT_EQUAL(S::Authenticating, s.state());
    TEST_ASSERT_EQUAL_STRING("{\"type\":\"auth\",\"access_token\":\"tok\"}", t.sent.back().c_str());
    feed(s, kAuthOk, now);
}

std::string resultOk(uint32_t id) {
    return "{\"id\":" + std::to_string(id) + ",\"type\":\"result\",\"success\":true,\"result\":null}";
}

}  // namespace

static void test_state_names() {
    TEST_ASSERT_EQUAL_STRING("no_config", ha::stateName(S::NoConfig));
    TEST_ASSERT_EQUAL_STRING("connecting", ha::stateName(S::Connecting));
    TEST_ASSERT_EQUAL_STRING("authenticating", ha::stateName(S::Authenticating));
    TEST_ASSERT_EQUAL_STRING("subscribing", ha::stateName(S::Subscribing));
    TEST_ASSERT_EQUAL_STRING("ready", ha::stateName(S::Ready));
    TEST_ASSERT_EQUAL_STRING("auth_failed", ha::stateName(S::AuthFailed));
    TEST_ASSERT_EQUAL_STRING("backoff", ha::stateName(S::Backoff));
}

static void test_initial_and_empty_config() {
    FakeTransport t;
    ha::HaSession s(t);
    TEST_ASSERT_EQUAL(S::NoConfig, s.state());
    s.setNetworkUp(true, 0);
    s.configure("", "", ha::TlsSettings(), {}, 0);
    TEST_ASSERT_EQUAL(S::NoConfig, s.state());
    s.configure(kUrl, "", ha::TlsSettings(), {}, 0);  // token missing
    TEST_ASSERT_EQUAL(S::NoConfig, s.state());
    s.configure("", "tok", ha::TlsSettings(), {}, 0);  // url missing
    TEST_ASSERT_EQUAL(S::NoConfig, s.state());
    s.tick(100000);
    TEST_ASSERT_EQUAL(0, t.opens);
}

static void test_auth_flow_empty_entities_ready() {
    FakeTransport t;
    ha::HaSession s(t);
    toAuthenticated(s, t, 1000);
    TEST_ASSERT_EQUAL(S::Ready, s.state());
    TEST_ASSERT_EQUAL_STRING("2025.1.0", s.haVersion().c_str());
    TEST_ASSERT_EQUAL_STRING("", s.lastError().c_str());
    TEST_ASSERT_EQUAL(1, t.opens);
    TEST_ASSERT_EQUAL(1, (int)t.sent.size());  // auth only, no subscribe
    TEST_ASSERT_EQUAL_UINT16(8123, t.lastEndpoint.port);
    TEST_ASSERT_EQUAL_STRING("/api/websocket", t.lastEndpoint.path.c_str());
}

static void test_auth_flow_with_entities_subscribes() {
    FakeTransport t;
    ha::HaSession s(t);
    toAuthenticated(s, t, 1000, {"light.a", "switch.b", "light.a"});  // duplicate dropped
    TEST_ASSERT_EQUAL(S::Subscribing, s.state());
    TEST_ASSERT_EQUAL(2, (int)t.sent.size());
    TEST_ASSERT_EQUAL_STRING("{\"id\":1,\"type\":\"subscribe_entities\",\"entity_ids\":[\"light.a\",\"switch.b\"]}",
                             t.sent[1].c_str());
    TEST_ASSERT_EQUAL(1, (int)s.pendingCount());
    feed(s, resultOk(1), 1100);
    TEST_ASSERT_EQUAL(S::Ready, s.state());
    TEST_ASSERT_EQUAL(0, (int)s.pendingCount());
}

static void test_subscribe_failed_result_still_ready() {
    FakeTransport t;
    ha::HaSession s(t);
    toAuthenticated(s, t, 0, {"light.a"});
    feed(s, "{\"id\":1,\"type\":\"result\",\"success\":false,\"error\":{\"code\":\"x\",\"message\":\"bad\"}}", 10);
    TEST_ASSERT_EQUAL(S::Ready, s.state());
    TEST_ASSERT_TRUE(s.lastError().find("bad") != std::string::npos);
}

static void test_ignores_irrelevant_messages() {
    FakeTransport t;
    ha::HaSession s(t);
    s.setNetworkUp(true, 0);
    s.configure(kUrl, "tok", ha::TlsSettings(), {}, 0);
    feed(s, "garbage", 1);
    feed(s, kAuthOk, 1);  // not valid before auth_required
    TEST_ASSERT_EQUAL(S::Connecting, s.state());
    feed(s, kAuthRequired, 2);
    feed(s, "garbage", 3);
    TEST_ASSERT_EQUAL(S::Authenticating, s.state());
}

static void test_auth_invalid_terminal() {
    FakeTransport t;
    ha::HaSession s(t);
    s.setNetworkUp(true, 0);
    s.configure(kUrl, "bad", ha::TlsSettings(), {"light.a"}, 0);
    feed(s, kAuthRequired, 0);
    feed(s, kAuthInvalid, 5);
    TEST_ASSERT_EQUAL(S::AuthFailed, s.state());
    TEST_ASSERT_TRUE(s.lastError().find("auth_invalid") != std::string::npos);
    TEST_ASSERT_TRUE(s.lastError().find("Invalid access token") != std::string::npos);
    TEST_ASSERT_EQUAL(1, t.opens);

    // simulated disconnect
    s.onClosed("closed", 10);
    s.tick(20);
    TEST_ASSERT_EQUAL(S::AuthFailed, s.state());
    // network down / up
    s.setNetworkUp(false, 30);
    s.setNetworkUp(true, 40);
    s.tick(50);
    TEST_ASSERT_EQUAL(S::AuthFailed, s.state());
    // unchanged configure
    s.configure(kUrl, "bad", ha::TlsSettings(), {"light.a"}, 60);
    s.configure(kUrl, "bad", ha::TlsSettings(), {"light.a", "light.b"}, 61);  // entity change only
    TEST_ASSERT_EQUAL(S::AuthFailed, s.state());
    // long time
    for (uint32_t now = 1000; now < 10 * 60 * 1000; now += 997) s.tick(now);
    s.tick(0xFFFFFFF0u);
    TEST_ASSERT_EQUAL(S::AuthFailed, s.state());
    TEST_ASSERT_EQUAL(1, t.opens);
}

static void test_retry_leaves_auth_failed() {
    FakeTransport t;
    ha::HaSession s(t);
    s.setNetworkUp(true, 0);
    s.configure(kUrl, "bad", ha::TlsSettings(), {}, 0);
    feed(s, kAuthRequired, 0);
    feed(s, kAuthInvalid, 0);
    TEST_ASSERT_EQUAL(S::AuthFailed, s.state());
    TEST_ASSERT_TRUE(s.retry(100));
    TEST_ASSERT_EQUAL(S::Connecting, s.state());
    TEST_ASSERT_EQUAL(2, t.opens);
    TEST_ASSERT_EQUAL_STRING("", s.lastError().c_str());
}

static void test_retry_noop_in_other_states() {
    FakeTransport t;
    ha::HaSession s(t);
    TEST_ASSERT_FALSE(s.retry(0));  // NoConfig
    toAuthenticated(s, t, 0);
    TEST_ASSERT_EQUAL(S::Ready, s.state());
    TEST_ASSERT_FALSE(s.retry(10));
    TEST_ASSERT_EQUAL(S::Ready, s.state());
}

static void test_retry_ends_backoff_early() {
    FakeTransport t;
    ha::HaSession s(t);
    s.setNetworkUp(true, 0);
    s.configure(kUrl, "tok", ha::TlsSettings(), {}, 0);
    s.onClosed("x", 10);
    s.onClosed("x", 10);  // ignored while in Backoff
    TEST_ASSERT_EQUAL(S::Backoff, s.state());
    TEST_ASSERT_TRUE(s.retry(20));
    TEST_ASSERT_EQUAL(S::Connecting, s.state());
    TEST_ASSERT_EQUAL(2, t.opens);
}

static void test_new_token_leaves_auth_failed() {
    FakeTransport t;
    ha::HaSession s(t);
    s.setNetworkUp(true, 0);
    s.configure(kUrl, "bad", ha::TlsSettings(), {}, 0);
    feed(s, kAuthRequired, 0);
    feed(s, kAuthInvalid, 0);
    TEST_ASSERT_EQUAL(S::AuthFailed, s.state());
    s.configure(kUrl, "good", ha::TlsSettings(), {}, 50);
    TEST_ASSERT_EQUAL(S::Connecting, s.state());
    TEST_ASSERT_EQUAL(2, t.opens);
    feed(s, kAuthRequired, 51);
    TEST_ASSERT_EQUAL_STRING("{\"type\":\"auth\",\"access_token\":\"good\"}", t.sent.back().c_str());
}

static void test_new_url_leaves_auth_failed() {
    FakeTransport t;
    ha::HaSession s(t);
    s.setNetworkUp(true, 0);
    s.configure(kUrl, "bad", ha::TlsSettings(), {}, 0);
    feed(s, kAuthRequired, 0);
    feed(s, kAuthInvalid, 0);
    s.configure("http://192.168.1.11:8123", "bad", ha::TlsSettings(), {}, 50);
    TEST_ASSERT_EQUAL(S::Connecting, s.state());
    TEST_ASSERT_EQUAL(2, t.opens);
}

static void test_ping_and_pong() {
    FakeTransport t;
    ha::HaSession s(t);
    toAuthenticated(s, t, 1000);
    const size_t before = t.sent.size();
    s.tick(1000 + 29999);
    TEST_ASSERT_EQUAL((int)before, (int)t.sent.size());
    s.tick(1000 + 30000);
    TEST_ASSERT_EQUAL((int)before + 1, (int)t.sent.size());
    TEST_ASSERT_EQUAL_STRING("{\"id\":1,\"type\":\"ping\"}", t.sent.back().c_str());
    feed(s, "{\"id\":1,\"type\":\"pong\"}", 31100);
    s.tick(1000 + 30000 + 15000);
    TEST_ASSERT_EQUAL(S::Ready, s.state());
    // next ping 30 s after the previous one
    s.tick(1000 + 60000);
    TEST_ASSERT_EQUAL_STRING("{\"id\":2,\"type\":\"ping\"}", t.sent.back().c_str());
}

static void test_missing_pong_reconnects() {
    FakeTransport t;
    ha::HaSession s(t);
    toAuthenticated(s, t, 0);
    s.tick(30000);  // ping sent
    s.tick(39999);
    TEST_ASSERT_EQUAL(S::Ready, s.state());
    s.tick(40000);
    TEST_ASSERT_EQUAL(S::Backoff, s.state());
    TEST_ASSERT_EQUAL_STRING("pong_timeout", s.lastError().c_str());
    TEST_ASSERT_EQUAL(1, t.closes);
    TEST_ASSERT_EQUAL(1, (int)s.failures());
    s.tick(40999);
    TEST_ASSERT_EQUAL(1, t.opens);
    s.tick(41000);  // 1 s backoff
    TEST_ASSERT_EQUAL(2, t.opens);
    TEST_ASSERT_EQUAL(S::Connecting, s.state());
}

static void test_wrong_pong_id_does_not_count() {
    FakeTransport t;
    ha::HaSession s(t);
    toAuthenticated(s, t, 0);
    s.tick(30000);
    feed(s, "{\"id\":99,\"type\":\"pong\"}", 31000);
    s.tick(40000);
    TEST_ASSERT_EQUAL(S::Backoff, s.state());
}

static void test_backoff_sequence() {
    FakeTransport t;
    ha::HaSession s(t);
    s.setNetworkUp(true, 0);
    s.configure(kUrl, "tok", ha::TlsSettings(), {}, 0);
    const uint32_t expected[] = {1000, 2000, 4000, 8000, 16000, 30000, 30000};
    uint32_t now = 0;
    for (size_t i = 0; i < 7; ++i) {
        TEST_ASSERT_EQUAL(S::Connecting, s.state());
        s.onClosed("connect_failed", now);
        TEST_ASSERT_EQUAL(S::Backoff, s.state());
        TEST_ASSERT_EQUAL_UINT32(expected[i], ha::HaSession::backoffDelayMs((uint32_t)i + 1));
        TEST_ASSERT_EQUAL_UINT32(now + expected[i], s.nextAttemptMs());
        const int opensBefore = t.opens;
        s.tick(now + expected[i] - 1);
        TEST_ASSERT_EQUAL(opensBefore, t.opens);
        now += expected[i];
        s.tick(now);
        TEST_ASSERT_EQUAL(opensBefore + 1, t.opens);
    }
    TEST_ASSERT_EQUAL_UINT32(30000, ha::HaSession::backoffDelayMs(100));
    TEST_ASSERT_EQUAL_UINT32(0, ha::HaSession::backoffDelayMs(0));
}

static void test_backoff_resets_after_ready() {
    FakeTransport t;
    ha::HaSession s(t);
    s.setNetworkUp(true, 0);
    s.configure(kUrl, "tok", ha::TlsSettings(), {}, 0);
    s.onClosed("x", 0);
    s.tick(1000);
    s.onClosed("x", 1000);
    s.tick(3000);
    feed(s, kAuthRequired, 3000);
    feed(s, kAuthOk, 3000);
    TEST_ASSERT_EQUAL(S::Ready, s.state());
    TEST_ASSERT_EQUAL(0, (int)s.failures());
    s.onClosed("x", 4000);
    TEST_ASSERT_EQUAL_UINT32(5000, s.nextAttemptMs());  // 1 s again
}

static void test_ids_restart_after_reconnect() {
    FakeTransport t;
    ha::HaSession s(t);
    toAuthenticated(s, t, 0, {"light.a"});
    feed(s, resultOk(1), 10);
    s.tick(30000);  // ping id 2
    TEST_ASSERT_EQUAL_STRING("{\"id\":2,\"type\":\"ping\"}", t.sent.back().c_str());
    s.onClosed("lost", 31000);
    s.tick(32000);
    TEST_ASSERT_EQUAL(S::Connecting, s.state());
    feed(s, kAuthRequired, 32000);
    feed(s, kAuthOk, 32000);
    TEST_ASSERT_EQUAL_STRING("{\"id\":1,\"type\":\"subscribe_entities\",\"entity_ids\":[\"light.a\"]}",
                             t.sent.back().c_str());
}

static void test_connect_and_auth_timeouts() {
    FakeTransport t;
    ha::HaSession s(t);
    s.setNetworkUp(true, 0);
    s.configure(kUrl, "tok", ha::TlsSettings(), {}, 0);
    s.tick(19999);
    TEST_ASSERT_EQUAL(S::Connecting, s.state());
    s.tick(20000);
    TEST_ASSERT_EQUAL(S::Backoff, s.state());
    TEST_ASSERT_EQUAL_STRING("connect_timeout", s.lastError().c_str());
    s.tick(21000);
    feed(s, kAuthRequired, 21000);
    s.tick(30999);
    TEST_ASSERT_EQUAL(S::Authenticating, s.state());
    s.tick(31000);
    TEST_ASSERT_EQUAL(S::Backoff, s.state());
    TEST_ASSERT_EQUAL_STRING("auth_timeout", s.lastError().c_str());
}

static void test_subscribe_expiry() {
    FakeTransport t;
    ha::HaSession s(t);
    toAuthenticated(s, t, 0, {"light.a"});
    TEST_ASSERT_EQUAL(S::Subscribing, s.state());
    s.tick(14999);
    TEST_ASSERT_EQUAL(S::Subscribing, s.state());
    s.tick(15000);
    TEST_ASSERT_EQUAL(S::Backoff, s.state());
    TEST_ASSERT_EQUAL(0, (int)s.pendingCount());
}

static void test_unknown_result_id_ignored() {
    FakeTransport t;
    ha::HaSession s(t);
    toAuthenticated(s, t, 0, {"light.a"});
    feed(s, resultOk(77), 5);
    TEST_ASSERT_EQUAL(S::Subscribing, s.state());
}

static void test_https_opens_with_tls() {
    FakeTransport t;
    ha::HaSession s(t);
    s.setNetworkUp(true, 0);
    s.configure("https://x.ui.nabu.casa", "tok", ha::TlsSettings(), {}, 0);
    TEST_ASSERT_EQUAL(S::Connecting, s.state());
    TEST_ASSERT_EQUAL_STRING("", s.lastError().c_str());
    TEST_ASSERT_EQUAL(1, t.opens);
    TEST_ASSERT_TRUE(t.lastEndpoint.ok);
    TEST_ASSERT_TRUE(t.lastEndpoint.tls);
    TEST_ASSERT_EQUAL_STRING("x.ui.nabu.casa", t.lastEndpoint.host.c_str());
    TEST_ASSERT_EQUAL_UINT16(443, t.lastEndpoint.port);
    TEST_ASSERT_FALSE(t.lastTls.insecure);
    TEST_ASSERT_EQUAL_STRING("", t.lastTls.caPem.c_str());
    TEST_ASSERT_EQUAL(ha::TlsMode::Bundle, s.tlsMode());
    // the normal auth flow works over a TLS endpoint, token only after auth_required
    TEST_ASSERT_EQUAL(0, (int)t.sent.size());
    feed(s, kAuthRequired, 1);
    TEST_ASSERT_EQUAL(S::Authenticating, s.state());
    feed(s, kAuthOk, 2);
    TEST_ASSERT_EQUAL(S::Ready, s.state());
    // switching to http reconnects in plain mode
    s.configure(kUrl, "tok", ha::TlsSettings(), {}, 300000);
    TEST_ASSERT_EQUAL(S::Connecting, s.state());
    TEST_ASSERT_EQUAL(2, t.opens);
    TEST_ASSERT_FALSE(t.lastEndpoint.tls);
    TEST_ASSERT_EQUAL(ha::TlsMode::None, s.tlsMode());
}

static void test_tls_settings_reach_transport_open() {
    FakeTransport t;
    ha::HaSession s(t);
    s.setNetworkUp(true, 0);
    ha::TlsSettings tls;
    tls.insecure = true;
    s.configure(kUrl, "tok", tls, {}, 0);
    TEST_ASSERT_EQUAL(1, t.opens);
    TEST_ASSERT_TRUE(t.lastTls.insecure);
    TEST_ASSERT_TRUE(s.tlsSettings() == tls);
    ha::TlsSettings ca;
    ca.caPem = "-----BEGIN CERTIFICATE-----\nAAAA\n-----END CERTIFICATE-----\n";
    s.configure(kUrl, "tok", ca, {}, 10);
    TEST_ASSERT_EQUAL(2, t.opens);
    TEST_ASSERT_FALSE(t.lastTls.insecure);
    TEST_ASSERT_EQUAL_STRING(ca.caPem.c_str(), t.lastTls.caPem.c_str());
}

static void test_tls_settings_change_alone_restarts() {
    FakeTransport t;
    ha::HaSession s(t);
    toAuthenticated(s, t, 0);
    TEST_ASSERT_EQUAL(S::Ready, s.state());
    TEST_ASSERT_EQUAL(1, t.opens);
    const int closes = t.closes;
    const uint32_t before = s.changeCount();
    // unchanged TLS settings keep the connection
    s.configure(kUrl, "tok", ha::TlsSettings(), {}, 5);
    TEST_ASSERT_EQUAL(S::Ready, s.state());
    TEST_ASSERT_EQUAL(1, t.opens);
    TEST_ASSERT_EQUAL(closes, t.closes);
    TEST_ASSERT_EQUAL_UINT32(before, s.changeCount());
    // insecure flag alone restarts: close + open
    ha::TlsSettings ins;
    ins.insecure = true;
    s.configure(kUrl, "tok", ins, {}, 6);
    TEST_ASSERT_EQUAL(closes + 1, t.closes);
    TEST_ASSERT_EQUAL(2, t.opens);
    TEST_ASSERT_EQUAL(S::Connecting, s.state());
    TEST_ASSERT_TRUE(t.lastTls.insecure);
    // CA text alone restarts
    ha::TlsSettings ca;
    ca.caPem = "pem";
    s.configure(kUrl, "tok", ca, {}, 7);
    TEST_ASSERT_EQUAL(closes + 2, t.closes);
    TEST_ASSERT_EQUAL(3, t.opens);
    // same again keeps
    s.configure(kUrl, "tok", ca, {}, 8);
    TEST_ASSERT_EQUAL(3, t.opens);
    TEST_ASSERT_EQUAL(closes + 2, t.closes);
}

static void test_tls_settings_in_auth_failed() {
    FakeTransport t;
    ha::HaSession s(t);
    s.setNetworkUp(true, 0);
    s.configure(kUrl, "bad", ha::TlsSettings(), {}, 0);
    feed(s, kAuthRequired, 1);
    feed(s, kAuthInvalid, 2);
    TEST_ASSERT_EQUAL(S::AuthFailed, s.state());
    TEST_ASSERT_EQUAL(1, t.opens);
    // unchanged TLS settings: stay AuthFailed, no new connection
    s.configure(kUrl, "bad", ha::TlsSettings(), {}, 50);
    s.tick(100000);
    TEST_ASSERT_EQUAL(S::AuthFailed, s.state());
    TEST_ASSERT_EQUAL(1, t.opens);
    // a TLS change alone leaves AuthFailed
    ha::TlsSettings ins;
    ins.insecure = true;
    s.configure(kUrl, "bad", ins, {}, 60);
    TEST_ASSERT_EQUAL(S::Connecting, s.state());
    TEST_ASSERT_EQUAL(2, t.opens);
}

static void test_https_with_tls_settings_opens_with_them() {
    FakeTransport t;
    ha::HaSession s(t);
    s.setNetworkUp(true, 0);
    ha::TlsSettings ins;
    ins.insecure = true;
    s.configure("https://ha.example.com", "tok", ins, {}, 0);
    TEST_ASSERT_EQUAL(S::Connecting, s.state());
    TEST_ASSERT_EQUAL_STRING("", s.lastError().c_str());
    TEST_ASSERT_EQUAL(1, t.opens);
    TEST_ASSERT_TRUE(t.lastEndpoint.tls);
    TEST_ASSERT_TRUE(t.lastTls.insecure);
    TEST_ASSERT_EQUAL(ha::TlsMode::Insecure, s.tlsMode());
    ha::TlsSettings ca;
    ca.caPem = "pem";
    s.configure("https://ha.example.com", "tok", ca, {}, 5);
    TEST_ASSERT_EQUAL(2, t.opens);
    TEST_ASSERT_TRUE(t.lastEndpoint.tls);
    TEST_ASSERT_FALSE(t.lastTls.insecure);
    TEST_ASSERT_EQUAL_STRING("pem", t.lastTls.caPem.c_str());
    TEST_ASSERT_EQUAL(S::Connecting, s.state());
    TEST_ASSERT_EQUAL(ha::TlsMode::CustomCa, s.tlsMode());
}

static void test_tls_mode_for_and_name() {
    const ha::HaUrl plain = ha::parseHaUrl("http://192.168.1.10:8123");
    const ha::HaUrl secure = ha::parseHaUrl("https://ha.example.com");
    const ha::HaUrl bad = ha::parseHaUrl("ftp://h");
    ha::TlsSettings none, ins, ca, both;
    ins.insecure = true;
    ca.caPem = "pem";
    both.insecure = true;
    both.caPem = "pem";
    TEST_ASSERT_TRUE(plain.ok);
    TEST_ASSERT_TRUE(secure.ok);
    TEST_ASSERT_FALSE(bad.ok);
    // ws: settings are ignored
    TEST_ASSERT_EQUAL(ha::TlsMode::None, ha::tlsModeFor(plain, none));
    TEST_ASSERT_EQUAL(ha::TlsMode::None, ha::tlsModeFor(plain, ins));
    TEST_ASSERT_EQUAL(ha::TlsMode::None, ha::tlsModeFor(plain, ca));
    TEST_ASSERT_EQUAL(ha::TlsMode::None, ha::tlsModeFor(bad, ins));
    // https
    TEST_ASSERT_EQUAL(ha::TlsMode::Insecure, ha::tlsModeFor(secure, ins));
    TEST_ASSERT_EQUAL(ha::TlsMode::CustomCa, ha::tlsModeFor(secure, ca));
    TEST_ASSERT_EQUAL(ha::TlsMode::Bundle, ha::tlsModeFor(secure, none));
    TEST_ASSERT_EQUAL(ha::TlsMode::CustomCa, ha::tlsModeFor(secure, both));  // CA wins
    TEST_ASSERT_EQUAL_STRING("none", ha::tlsModeName(ha::TlsMode::None));
    TEST_ASSERT_EQUAL_STRING("bundle", ha::tlsModeName(ha::TlsMode::Bundle));
    TEST_ASSERT_EQUAL_STRING("custom_ca", ha::tlsModeName(ha::TlsMode::CustomCa));
    TEST_ASSERT_EQUAL_STRING("insecure", ha::tlsModeName(ha::TlsMode::Insecure));
}

static void test_tls_mode_getter() {
    FakeTransport t;
    ha::HaSession s(t);
    TEST_ASSERT_EQUAL(ha::TlsMode::None, s.tlsMode());  // nothing configured
    s.setNetworkUp(true, 0);
    s.configure(kUrl, "tok", ha::TlsSettings(), {}, 0);
    TEST_ASSERT_EQUAL(ha::TlsMode::None, s.tlsMode());
    ha::TlsSettings ins;
    ins.insecure = true;
    s.configure("https://h.example", "tok", ha::TlsSettings(), {}, 1);
    TEST_ASSERT_EQUAL(ha::TlsMode::Bundle, s.tlsMode());
    s.configure("https://h.example", "tok", ins, {}, 2);
    TEST_ASSERT_EQUAL(ha::TlsMode::Insecure, s.tlsMode());
    ha::TlsSettings ca;
    ca.caPem = "pem";
    s.configure("https://h.example", "tok", ca, {}, 3);
    TEST_ASSERT_EQUAL(ha::TlsMode::CustomCa, s.tlsMode());
    s.configure("https://h.example", "tok", ins, {}, 4);
    TEST_ASSERT_EQUAL(ha::TlsMode::Insecure, s.tlsMode());
    s.configure("http://h.example", "tok", ins, {}, 5);  // http: settings ignored
    TEST_ASSERT_EQUAL(ha::TlsMode::None, s.tlsMode());
    s.configure("", "", ha::TlsSettings(), {}, 6);  // NoConfig
    TEST_ASSERT_EQUAL(ha::TlsMode::None, s.tlsMode());
}

static void test_tls_only_ca_or_insecure_change_restarts_https() {
    FakeTransport t;
    ha::HaSession s(t);
    s.setNetworkUp(true, 0);
    const char *url = "https://ha.example.com";
    s.configure(url, "tok", ha::TlsSettings(), {}, 0);
    feed(s, kAuthRequired, 1);
    feed(s, kAuthOk, 2);
    TEST_ASSERT_EQUAL(S::Ready, s.state());
    int closes = t.closes;
    // unchanged -> nothing happens
    s.configure(url, "tok", ha::TlsSettings(), {}, 3);
    TEST_ASSERT_EQUAL(S::Ready, s.state());
    TEST_ASSERT_EQUAL(closes, t.closes);
    TEST_ASSERT_EQUAL(1, t.opens);
    // only insecure changes
    ha::TlsSettings ins;
    ins.insecure = true;
    uint32_t cc = s.changeCount();
    s.configure(url, "tok", ins, {}, 4);
    TEST_ASSERT_EQUAL(closes + 1, t.closes);
    TEST_ASSERT_EQUAL(2, t.opens);
    TEST_ASSERT_TRUE(t.lastTls.insecure);
    TEST_ASSERT_TRUE(s.changeCount() > cc);
    TEST_ASSERT_EQUAL(S::Connecting, s.state());
    // only the CA changes (insecure off again)
    ha::TlsSettings ca;
    ca.caPem = "pem-A";
    s.configure(url, "tok", ca, {}, 5);
    TEST_ASSERT_EQUAL(closes + 2, t.closes);
    TEST_ASSERT_EQUAL(3, t.opens);
    TEST_ASSERT_EQUAL_STRING("pem-A", t.lastTls.caPem.c_str());
    // other CA text
    ca.caPem = "pem-B";
    s.configure(url, "tok", ca, {}, 6);
    TEST_ASSERT_EQUAL(closes + 3, t.closes);
    TEST_ASSERT_EQUAL(4, t.opens);
    // same again keeps
    s.configure(url, "tok", ca, {}, 7);
    TEST_ASSERT_EQUAL(closes + 3, t.closes);
    TEST_ASSERT_EQUAL(4, t.opens);
    // CA removed -> back to bundle, restarts
    s.configure(url, "tok", ha::TlsSettings(), {}, 8);
    TEST_ASSERT_EQUAL(5, t.opens);
    TEST_ASSERT_EQUAL(ha::TlsMode::Bundle, s.tlsMode());
}

static void test_tls_mode_change_bumps_change_count_from_backoff() {
    FakeTransport t;
    ha::HaSession s(t);
    s.setNetworkUp(true, 0);
    const char *url = "https://ha.example.com";
    s.configure(url, "tok", ha::TlsSettings(), {}, 0);
    s.onClosed("tls_cert_rejected", 1);
    TEST_ASSERT_EQUAL(S::Backoff, s.state());
    const uint32_t cc = s.changeCount();
    ha::TlsSettings ins;
    ins.insecure = true;
    s.configure(url, "tok", ins, {}, 2);
    TEST_ASSERT_TRUE(s.changeCount() > cc);
    TEST_ASSERT_EQUAL(S::Connecting, s.state());
    TEST_ASSERT_EQUAL_STRING("", s.lastError().c_str());
}

static void test_tls_unchanged_keeps_auth_failed_https() {
    FakeTransport t;
    ha::HaSession s(t);
    s.setNetworkUp(true, 0);
    const char *url = "https://ha.example.com";
    ha::TlsSettings ca;
    ca.caPem = "pem";
    s.configure(url, "bad", ca, {}, 0);
    feed(s, kAuthRequired, 1);
    feed(s, kAuthInvalid, 2);
    TEST_ASSERT_EQUAL(S::AuthFailed, s.state());
    const uint32_t cc = s.changeCount();
    s.configure(url, "bad", ca, {}, 50);
    s.tick(200000);
    TEST_ASSERT_EQUAL(S::AuthFailed, s.state());
    TEST_ASSERT_EQUAL(1, t.opens);
    TEST_ASSERT_EQUAL_UINT32(cc, s.changeCount());
    // a CA change alone leaves AuthFailed and reconnects
    ha::TlsSettings ca2;
    ca2.caPem = "pem2";
    s.configure(url, "bad", ca2, {}, 60);
    TEST_ASSERT_EQUAL(S::Connecting, s.state());
    TEST_ASSERT_EQUAL(2, t.opens);
}

static void test_tls_close_reasons_reach_last_error_and_backoff() {
    const char *reasons[] = {"tls_cert_rejected", "tls_ca_invalid", "tls_handshake_failed", "tls_timeout"};
    for (const char *reason : reasons) {
        FakeTransport t;
        ha::HaSession s(t);
        s.setNetworkUp(true, 0);
        ha::TlsSettings ca;
        ca.caPem = "pem";
        s.configure("https://ha.example.com", "tok", ca, {}, 0);
        TEST_ASSERT_EQUAL(S::Connecting, s.state());
        s.onClosed(reason, 100);
        TEST_ASSERT_EQUAL_MESSAGE(S::Backoff, s.state(), reason);
        TEST_ASSERT_EQUAL_STRING(reason, s.lastError().c_str());
        TEST_ASSERT_EQUAL_UINT32(1, s.failures());
        TEST_ASSERT_EQUAL_UINT32(100 + 1000, s.nextAttemptMs());
        // not terminal: the session retries by itself after the backoff
        s.tick(1099);
        TEST_ASSERT_EQUAL(1, t.opens);
        s.tick(1100);
        TEST_ASSERT_EQUAL_MESSAGE(S::Connecting, s.state(), reason);
        TEST_ASSERT_EQUAL(2, t.opens);
        TEST_ASSERT_TRUE(t.lastEndpoint.tls);
        TEST_ASSERT_EQUAL_STRING("pem", t.lastTls.caPem.c_str());
        // second failure -> longer backoff (2 s), Backoff and not AuthFailed
        s.onClosed(reason, 1200);
        TEST_ASSERT_EQUAL(S::Backoff, s.state());
        TEST_ASSERT_EQUAL_UINT32(1200 + 2000, s.nextAttemptMs());
    }
}

static void test_dropped_counted_only_while_open_and_active() {
    FakeTransport t;
    ha::HaSession s(t);
    TEST_ASSERT_EQUAL_UINT32(0, s.droppedMessages());
    s.onMessageDropped(10, "too_large", 0);  // NoConfig
    TEST_ASSERT_EQUAL_UINT32(0, s.droppedMessages());
    s.setNetworkUp(true, 0);
    s.configure(kUrl, "tok", ha::TlsSettings(), {}, 0);
    s.onMessageDropped(10, "too_large", 1);  // Connecting, transport open
    TEST_ASSERT_EQUAL_UINT32(1, s.droppedMessages());
    feed(s, kAuthRequired, 2);
    s.onMessageDropped(5u * 1024u * 1024u, "too_large", 3);  // Authenticating
    TEST_ASSERT_EQUAL_UINT32(2, s.droppedMessages());
    feed(s, kAuthOk, 4);
    TEST_ASSERT_EQUAL(S::Ready, s.state());
    s.onMessageDropped(1, "no_memory", 5);
    s.onMessageDropped(1, "binary", 6);
    TEST_ASSERT_EQUAL_UINT32(4, s.droppedMessages());
    TEST_ASSERT_EQUAL(S::Ready, s.state());  // the connection stays up
    TEST_ASSERT_EQUAL_STRING("", s.lastError().c_str());
    // Backoff: not counted
    s.onClosed("closed_by_server", 10);
    TEST_ASSERT_EQUAL(S::Backoff, s.state());
    s.onMessageDropped(10, "too_large", 11);
    TEST_ASSERT_EQUAL_UINT32(4, s.droppedMessages());
    // AuthFailed: not counted
    FakeTransport t2;
    ha::HaSession s2(t2);
    s2.setNetworkUp(true, 0);
    s2.configure(kUrl, "bad", ha::TlsSettings(), {}, 0);
    feed(s2, kAuthRequired, 1);
    feed(s2, kAuthInvalid, 2);
    s2.onMessageDropped(10, "too_large", 3);
    TEST_ASSERT_EQUAL_UINT32(0, s2.droppedMessages());
    // Network down closes the connection: not counted
    FakeTransport t3;
    ha::HaSession s3(t3);
    s3.setNetworkUp(true, 0);
    s3.configure(kUrl, "tok", ha::TlsSettings(), {}, 0);
    s3.setNetworkUp(false, 5);
    s3.onMessageDropped(10, "too_large", 6);
    TEST_ASSERT_EQUAL_UINT32(0, s3.droppedMessages());
}

static void test_https_while_network_down_no_open() {
    FakeTransport t;
    ha::HaSession s(t);
    ha::TlsSettings ca;
    ca.caPem = "pem";
    s.configure("https://h", "tok", ca, {}, 0);
    TEST_ASSERT_EQUAL(0, t.opens);
    TEST_ASSERT_EQUAL(S::Backoff, s.state());
    s.tick(10);
    TEST_ASSERT_EQUAL(0, t.opens);
    s.setNetworkUp(true, 5);
    TEST_ASSERT_EQUAL(1, t.opens);  // opens once the network is up, with the TLS settings
    TEST_ASSERT_TRUE(t.lastEndpoint.tls);
    TEST_ASSERT_EQUAL_STRING("pem", t.lastTls.caPem.c_str());
    TEST_ASSERT_EQUAL(S::Connecting, s.state());
    TEST_ASSERT_NOT_EQUAL(0, strcmp("tls_not_supported", s.lastError().c_str()));
}

static void test_query_fragment_url_invalid() {
    FakeTransport t;
    ha::HaSession s(t);
    s.setNetworkUp(true, 0);
    s.configure("http://h/x?y", "tok", ha::TlsSettings(), {}, 0);
    TEST_ASSERT_EQUAL(S::NoConfig, s.state());
    TEST_ASSERT_EQUAL_STRING("invalid_url", s.lastError().c_str());
    s.configure("http://h/#a", "tok", ha::TlsSettings(), {}, 1);
    TEST_ASSERT_EQUAL(S::NoConfig, s.state());
    TEST_ASSERT_EQUAL_STRING("invalid_url", s.lastError().c_str());
    s.tick(100000);
    s.retry(100001);
    TEST_ASSERT_EQUAL(0, t.opens);
}

static void test_unparsable_url_invalid() {
    FakeTransport t;
    ha::HaSession s(t);
    s.setNetworkUp(true, 0);
    s.configure("ftp://h", "tok", ha::TlsSettings(), {}, 0);
    TEST_ASSERT_EQUAL(S::NoConfig, s.state());
    TEST_ASSERT_EQUAL_STRING("invalid_url", s.lastError().c_str());
    TEST_ASSERT_EQUAL(0, t.opens);
}

static void test_send_failure_reporting_close() {
    FakeTransport t;
    ha::HaSession s(t);
    s.setNetworkUp(true, 0);
    s.configure(kUrl, "tok", ha::TlsSettings(), {}, 0);
    t.sendResult = false;
    t.onSend = [&]() { s.onClosed("socket_closed", 100); };
    feed(s, kAuthRequired, 100);
    TEST_ASSERT_EQUAL(S::Backoff, s.state());
    TEST_ASSERT_EQUAL(1, (int)s.failures());
    TEST_ASSERT_EQUAL_STRING("socket_closed", s.lastError().c_str());
    TEST_ASSERT_EQUAL_UINT32(1100, s.nextAttemptMs());
}

static void test_send_failure_plain() {
    FakeTransport t;
    ha::HaSession s(t);
    s.setNetworkUp(true, 0);
    s.configure(kUrl, "tok", ha::TlsSettings(), {}, 0);
    t.sendResult = false;
    feed(s, kAuthRequired, 100);
    TEST_ASSERT_EQUAL(S::Backoff, s.state());
    TEST_ASSERT_EQUAL(1, (int)s.failures());
    TEST_ASSERT_EQUAL_STRING("send_failed", s.lastError().c_str());
}

static void test_wifi_loss_goes_to_backoff() {
    FakeTransport t;
    ha::HaSession s(t);
    toAuthenticated(s, t, 0);
    TEST_ASSERT_EQUAL(S::Ready, s.state());
    s.setNetworkUp(false, 1000);
    TEST_ASSERT_EQUAL(S::Backoff, s.state());
    TEST_ASSERT_EQUAL(1, t.closes);
    s.tick(100000);
    TEST_ASSERT_EQUAL(1, t.opens);  // waits for the network
    s.setNetworkUp(true, 100000);
    TEST_ASSERT_EQUAL(S::Connecting, s.state());
    TEST_ASSERT_EQUAL(2, t.opens);
}

static void test_wifi_loss_during_connecting() {
    FakeTransport t;
    ha::HaSession s(t);
    s.setNetworkUp(true, 0);
    s.configure(kUrl, "tok", ha::TlsSettings(), {}, 0);
    s.setNetworkUp(false, 10);
    TEST_ASSERT_EQUAL(S::Backoff, s.state());
}

static void test_configure_while_network_down_waits() {
    FakeTransport t;
    ha::HaSession s(t);
    s.configure(kUrl, "tok", ha::TlsSettings(), {}, 0);
    TEST_ASSERT_EQUAL(S::Backoff, s.state());
    TEST_ASSERT_EQUAL(0, t.opens);
    s.tick(5000);
    TEST_ASSERT_EQUAL(0, t.opens);
    s.setNetworkUp(true, 6000);
    TEST_ASSERT_EQUAL(1, t.opens);
}

static void test_config_change_reconnects_and_clears() {
    FakeTransport t;
    ha::HaSession s(t);
    toAuthenticated(s, t, 0);
    s.configure(kUrl, "other", ha::TlsSettings(), {}, 100);
    TEST_ASSERT_EQUAL(S::Connecting, s.state());
    TEST_ASSERT_EQUAL(2, t.opens);
    TEST_ASSERT_EQUAL_STRING("", s.haVersion().c_str());
    s.configure("", "", ha::TlsSettings(), {}, 200);
    TEST_ASSERT_EQUAL(S::NoConfig, s.state());
    TEST_ASSERT_EQUAL(2, t.opens);
}

static void test_close_event_after_local_close_ignored() {
    FakeTransport t;
    ha::HaSession s(t);
    toAuthenticated(s, t, 0);
    s.configure("", "", ha::TlsSettings(), {}, 10);
    s.onClosed("late", 11);
    TEST_ASSERT_EQUAL(S::NoConfig, s.state());
    TEST_ASSERT_EQUAL(0, (int)s.failures());
}

static void test_error_cut_to_max_len() {
    FakeTransport t;
    ha::HaSession s(t);
    s.setNetworkUp(true, 0);
    s.configure(kUrl, "tok", ha::TlsSettings(), {}, 0);
    s.onClosed(std::string(500, 'x'), 1);
    TEST_ASSERT_EQUAL((int)ha::HaSession::kMaxErrorLen, (int)s.lastError().size());
}

static void test_token_not_in_status() {
    FakeTransport t;
    ha::HaSession s(t);
    s.setNetworkUp(true, 0);
    s.configure(kUrl, "SECRETTOKEN123", ha::TlsSettings(), {}, 0);
    feed(s, kAuthRequired, 0);
    feed(s, kAuthInvalid, 0);
    TEST_ASSERT_TRUE(s.lastError().find("SECRETTOKEN123") == std::string::npos);
    TEST_ASSERT_TRUE(s.haVersion().find("SECRETTOKEN123") == std::string::npos);
}

static void test_change_count_increments() {
    FakeTransport t;
    ha::HaSession s(t);
    const uint32_t c0 = s.changeCount();
    s.setNetworkUp(true, 0);
    s.configure(kUrl, "tok", ha::TlsSettings(), {}, 0);
    TEST_ASSERT_TRUE(s.changeCount() > c0);
    const uint32_t c1 = s.changeCount();
    s.tick(1);
    TEST_ASSERT_EQUAL_UINT32(c1, s.changeCount());
}

static void test_tick_wraparound() {
    FakeTransport t;
    ha::HaSession s(t);
    const uint32_t start = 0xFFFFFFFFu - 5000u;
    toAuthenticated(s, t, start);
    s.tick(start + 30000u);  // wraps past 0
    TEST_ASSERT_EQUAL_STRING("{\"id\":1,\"type\":\"ping\"}", t.sent.back().c_str());
    s.tick(start + 40000u);
    TEST_ASSERT_EQUAL(S::Backoff, s.state());
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_state_names);
    RUN_TEST(test_initial_and_empty_config);
    RUN_TEST(test_auth_flow_empty_entities_ready);
    RUN_TEST(test_auth_flow_with_entities_subscribes);
    RUN_TEST(test_subscribe_failed_result_still_ready);
    RUN_TEST(test_ignores_irrelevant_messages);
    RUN_TEST(test_auth_invalid_terminal);
    RUN_TEST(test_retry_leaves_auth_failed);
    RUN_TEST(test_retry_noop_in_other_states);
    RUN_TEST(test_retry_ends_backoff_early);
    RUN_TEST(test_new_token_leaves_auth_failed);
    RUN_TEST(test_new_url_leaves_auth_failed);
    RUN_TEST(test_ping_and_pong);
    RUN_TEST(test_missing_pong_reconnects);
    RUN_TEST(test_wrong_pong_id_does_not_count);
    RUN_TEST(test_backoff_sequence);
    RUN_TEST(test_backoff_resets_after_ready);
    RUN_TEST(test_ids_restart_after_reconnect);
    RUN_TEST(test_connect_and_auth_timeouts);
    RUN_TEST(test_subscribe_expiry);
    RUN_TEST(test_unknown_result_id_ignored);
    RUN_TEST(test_https_opens_with_tls);
    RUN_TEST(test_https_while_network_down_no_open);
    RUN_TEST(test_query_fragment_url_invalid);
    RUN_TEST(test_unparsable_url_invalid);
    RUN_TEST(test_send_failure_reporting_close);
    RUN_TEST(test_send_failure_plain);
    RUN_TEST(test_wifi_loss_goes_to_backoff);
    RUN_TEST(test_wifi_loss_during_connecting);
    RUN_TEST(test_configure_while_network_down_waits);
    RUN_TEST(test_config_change_reconnects_and_clears);
    RUN_TEST(test_close_event_after_local_close_ignored);
    RUN_TEST(test_error_cut_to_max_len);
    RUN_TEST(test_token_not_in_status);
    RUN_TEST(test_change_count_increments);
    RUN_TEST(test_tick_wraparound);
    RUN_TEST(test_tls_settings_reach_transport_open);
    RUN_TEST(test_tls_settings_change_alone_restarts);
    RUN_TEST(test_tls_settings_in_auth_failed);
    RUN_TEST(test_https_with_tls_settings_opens_with_them);
    RUN_TEST(test_tls_mode_for_and_name);
    RUN_TEST(test_tls_mode_getter);
    RUN_TEST(test_tls_only_ca_or_insecure_change_restarts_https);
    RUN_TEST(test_tls_mode_change_bumps_change_count_from_backoff);
    RUN_TEST(test_tls_unchanged_keeps_auth_failed_https);
    RUN_TEST(test_tls_close_reasons_reach_last_error_and_backoff);
    RUN_TEST(test_dropped_counted_only_while_open_and_active);
    return UNITY_END();
}
