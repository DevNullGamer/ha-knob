// Native tests for HaSession (plan.md section 3.7): fake transport + fake clock.
#include <unity.h>

#include <ArduinoJson.h>

#include <cstring>
#include <optional>
#include <utility>
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

    // retainText (Step 6.4): copies the text; allocs / frees count the copies.
    int retainCalls = 0;
    int allocs = 0;
    int frees = 0;
    bool retainFails = false;
    static void releaseCopy(void *context, char *data) {
        ++static_cast<FakeTransport *>(context)->frees;
        delete[] data;
    }
    ha::RetainedText retainText(const char *data, size_t length) override {
        ++retainCalls;
        if (retainFails) return ha::RetainedText();
        char *copy = new char[length + 1];
        memcpy(copy, data, length);
        copy[length] = '\0';
        ++allocs;
        return ha::RetainedText(copy, length, &releaseCopy, this);
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

// ============================================================================================
// Step 6.4: resubscribe, events, get_states, liveness (plan-step6.md sections 3.1-3.3, 3.8)
// ============================================================================================

namespace {

struct Logs {
    std::vector<std::string> lines;
};

void attachLogs(ha::HaSession &s, Logs &logs) {
    s.setLogger([&logs](const std::string &line) { logs.lines.push_back(line); });
}

int countLog(const Logs &logs, const std::string &prefix) {
    int n = 0;
    for (const std::string &l : logs.lines) {
        if (l.compare(0, prefix.size(), prefix) == 0) ++n;
    }
    return n;
}

bool hasLog(const Logs &logs, const std::string &line) {
    for (const std::string &l : logs.lines) {
        if (l == line) return true;
    }
    return false;
}

// Connected, authenticated and (when ids are given) subscribed with the result of id 1.
void toReady(ha::HaSession &s, FakeTransport &t, uint32_t now, const std::vector<std::string> &ids = {}) {
    toAuthenticated(s, t, now, ids);
    if (!ids.empty()) {
        TEST_ASSERT_EQUAL(S::Subscribing, s.state());
        feed(s, resultOk(1), now);
    }
    TEST_ASSERT_EQUAL(S::Ready, s.state());
}

void reconfigure(ha::HaSession &s, const std::vector<std::string> &ids, uint32_t now) {
    s.configure(kUrl, "tok", ha::TlsSettings(), ids, now);
}

std::string subMsg(uint32_t id, const std::string &entityIdsJson) {
    return "{\"id\":" + std::to_string(id) + ",\"type\":\"subscribe_entities\",\"entity_ids\":[" + entityIdsJson + "]}";
}

std::string unsubMsg(uint32_t id, uint32_t subscription) {
    return "{\"id\":" + std::to_string(id) + ",\"type\":\"unsubscribe_events\",\"subscription\":" +
           std::to_string(subscription) + "}";
}

std::string getStatesMsg(uint32_t id) { return "{\"id\":" + std::to_string(id) + ",\"type\":\"get_states\"}"; }

std::string resultFail(uint32_t id, const std::string &message) {
    return "{\"id\":" + std::to_string(id) + ",\"type\":\"result\",\"success\":false,\"error\":{\"code\":\"x\",\"message\":\"" +
           message + "\"}}";
}

// Snapshot event of subscription `id` with the given (entity, state) pairs.
std::string eventA(uint32_t id, const std::vector<std::pair<std::string, std::string>> &entities) {
    std::string a;
    for (const auto &e : entities) {
        if (!a.empty()) a += ",";
        a += "\"" + e.first + "\":{\"s\":\"" + e.second + "\",\"a\":{}}";
    }
    return "{\"id\":" + std::to_string(id) + ",\"type\":\"event\",\"event\":{\"a\":{" + a + "}}}";
}

std::string entityJson(int i, size_t pad = 0) {
    std::string j = "{\"entity_id\":\"light.k" + std::to_string(i) + "\",\"state\":\"on\",\"attributes\":{";
    if (pad != 0) j += "\"junk\":\"" + std::string(pad, 'x') + "\",";
    j += "\"friendly_name\":\"K" + std::to_string(i) + "\"}}";
    return j;
}

// get_states result with n light entities; pad bytes of ignored attribute in the first one.
std::string statesResult(uint32_t id, int n, size_t pad = 0, bool resultFirst = false) {
    std::string arr = "[";
    for (int i = 0; i < n; ++i) {
        if (i != 0) arr += ",";
        arr += entityJson(i, i == 0 ? pad : 0);
    }
    arr += "]";
    const std::string sid = std::to_string(id);
    if (resultFirst) return "{\"result\":" + arr + ",\"id\":" + sid + ",\"type\":\"result\",\"success\":true}";
    return "{\"id\":" + sid + ",\"type\":\"result\",\"success\":true,\"result\":" + arr + "}";
}

const size_t kBig = 70000;  // > 64 KB (kStatesRetainMinBytes)

void balanced(const FakeTransport &t) {
    TEST_ASSERT_EQUAL_INT_MESSAGE(t.allocs, t.frees, "retained texts: allocs != frees");
}

void expectCatalog(ha::HaSession &s, int n) {
    TEST_ASSERT_EQUAL(ha::StatesState::Ready, s.statesState());
    std::optional<ha::EntityCatalog> c = s.takeCatalog();
    TEST_ASSERT_TRUE(c.has_value());
    TEST_ASSERT_EQUAL_UINT32((uint32_t)n, (uint32_t)c->size());
    for (int i = 0; i < n; ++i) {
        const ha::CatalogEntry e = c->entry((size_t)i);
        TEST_ASSERT_TRUE(std::string(e.id) == "light.k" + std::to_string(i));
        TEST_ASSERT_TRUE(std::string(e.name) == "K" + std::to_string(i));
        TEST_ASSERT_TRUE(std::string(e.state) == "on");
    }
}

// Runs tick() until the entity list build ends (statesState != Building).
void finishBuild(ha::HaSession &s, uint32_t &now) {
    for (int i = 0; i < 100000 && s.statesState() == ha::StatesState::Building; ++i) s.tick(now);
    TEST_ASSERT_NOT_EQUAL((int)ha::StatesState::Building, (int)s.statesState());
}

// Answers every ping sent since `seen` with its pong.
void answerPings(ha::HaSession &s, FakeTransport &t, size_t &seen, uint32_t now) {
    while (seen < t.sent.size()) {
        const std::string m = t.sent[seen++];
        if (m.find("\"type\":\"ping\"") != std::string::npos) {
            feed(s, "{\"id\":" + m.substr(6, m.find(',') - 6) + ",\"type\":\"pong\"}", now);
        }
    }
}

uint32_t lastId(const FakeTransport &t) {
    const std::string &m = t.sent.back();
    return (uint32_t)std::stoul(m.substr(6, m.find(',') - 6));
}

}  // namespace

// ---- Resubscribe ---------------------------------------------------------------------------

static void test_resub_ready_new_ids() {
    FakeTransport t;
    ha::HaSession s(t);
    Logs logs;
    attachLogs(s, logs);
    toReady(s, t, 0, {"light.a"});
    feed(s, eventA(1, {{"light.a", "on"}}), 1);
    TEST_ASSERT_EQUAL(1, (int)s.entities().size());
    const uint32_t cc = s.changeCount();
    const size_t sent = t.sent.size();
    reconfigure(s, {"light.b"}, 100);
    TEST_ASSERT_EQUAL((int)sent + 2, (int)t.sent.size());
    TEST_ASSERT_EQUAL_STRING(unsubMsg(2, 1).c_str(), t.sent[sent].c_str());
    TEST_ASSERT_EQUAL_STRING(subMsg(3, "\"light.b\"").c_str(), t.sent[sent + 1].c_str());
    TEST_ASSERT_EQUAL(S::Ready, s.state());
    TEST_ASSERT_EQUAL_UINT32(cc, s.changeCount());
    TEST_ASSERT_EQUAL(0, (int)s.entities().size());
    TEST_ASSERT_FALSE(s.entities().snapshotSeen());
    TEST_ASSERT_EQUAL(1, t.opens);
    TEST_ASSERT_EQUAL(0, t.closes);
    TEST_ASSERT_TRUE(hasLog(logs, "unsubscribe id=2 subscription=1"));
    TEST_ASSERT_TRUE(hasLog(logs, "subscribe id=3 ids=1"));
    // old subscription's events ignored, the new one's applied (before its result too)
    feed(s, eventA(1, {{"light.a", "off"}}), 101);
    TEST_ASSERT_EQUAL(0, (int)s.entities().size());
    feed(s, eventA(3, {{"light.b", "on"}}), 102);
    TEST_ASSERT_EQUAL(1, (int)s.entities().size());
    feed(s, resultOk(2), 103);
    feed(s, resultOk(3), 103);
    TEST_ASSERT_EQUAL(S::Ready, s.state());
    TEST_ASSERT_EQUAL(0, (int)s.pendingCount());
    TEST_ASSERT_EQUAL_UINT32(cc, s.changeCount());
    feed(s, eventA(3, {{"light.c", "on"}}), 104);  // still the active subscription
    TEST_ASSERT_EQUAL(2, (int)s.entities().size());
}

static void test_resub_ids_to_empty_from_ready() {
    FakeTransport t;
    ha::HaSession s(t);
    toReady(s, t, 0, {"light.a"});
    feed(s, eventA(1, {{"light.a", "on"}}), 1);
    const size_t sent = t.sent.size();
    reconfigure(s, {}, 100);
    TEST_ASSERT_EQUAL((int)sent + 1, (int)t.sent.size());  // unsubscribe only
    TEST_ASSERT_EQUAL_STRING(unsubMsg(2, 1).c_str(), t.sent.back().c_str());
    TEST_ASSERT_EQUAL(S::Ready, s.state());
    TEST_ASSERT_EQUAL(0, (int)s.entities().size());
    feed(s, eventA(1, {{"light.a", "on"}}), 101);  // ignored
    TEST_ASSERT_EQUAL(0, (int)s.entities().size());
}

static void test_resub_ids_to_empty_from_subscribing() {
    FakeTransport t;
    ha::HaSession s(t);
    toAuthenticated(s, t, 0, {"light.a"});
    TEST_ASSERT_EQUAL(S::Subscribing, s.state());
    const size_t sent = t.sent.size();
    reconfigure(s, {}, 100);
    TEST_ASSERT_EQUAL((int)sent + 1, (int)t.sent.size());
    TEST_ASSERT_EQUAL_STRING(unsubMsg(2, 1).c_str(), t.sent.back().c_str());  // the pending id
    TEST_ASSERT_EQUAL(S::Ready, s.state());
    feed(s, resultOk(1), 101);  // late result of the dropped subscribe: nothing
    feed(s, eventA(1, {{"light.a", "on"}}), 101);
    TEST_ASSERT_EQUAL(S::Ready, s.state());
    TEST_ASSERT_EQUAL(0, (int)s.entities().size());
}

static void test_resub_ids_to_empty_clears_subscribe_failed() {
    FakeTransport t;
    ha::HaSession s(t);
    toAuthenticated(s, t, 0, {"light.a"});
    feed(s, resultFail(1, "bad"), 10);
    TEST_ASSERT_EQUAL(S::Ready, s.state());
    TEST_ASSERT_EQUAL_STRING("subscribe_failed: bad", s.lastError().c_str());
    const size_t sent = t.sent.size();
    reconfigure(s, {}, 20);
    TEST_ASSERT_EQUAL((int)sent, (int)t.sent.size());  // no active subscription: nothing to send
    TEST_ASSERT_EQUAL(S::Ready, s.state());
    TEST_ASSERT_EQUAL_STRING("", s.lastError().c_str());
}

static void test_resub_same_set_other_order_does_nothing() {
    FakeTransport t;
    ha::HaSession s(t);
    toReady(s, t, 0, {"light.a", "light.b"});
    feed(s, eventA(1, {{"light.a", "on"}}), 1);
    const size_t sent = t.sent.size();
    const uint32_t cc = s.changeCount();
    reconfigure(s, {"light.b", "light.a"}, 100);
    reconfigure(s, {"light.b", "light.a", "light.b"}, 101);  // duplicate dropped, still the same set
    TEST_ASSERT_EQUAL((int)sent, (int)t.sent.size());
    TEST_ASSERT_EQUAL(1, (int)s.entities().size());
    TEST_ASSERT_TRUE(s.entities().snapshotSeen());
    TEST_ASSERT_EQUAL_UINT32(cc, s.changeCount());
    TEST_ASSERT_EQUAL_STRING("light.b", s.entityIds()[0].c_str());  // the new order is the configured one
}

static void test_resub_while_subscribing() {
    FakeTransport t;
    ha::HaSession s(t);
    toAuthenticated(s, t, 0, {"light.a"});
    feed(s, eventA(1, {{"light.a", "on"}}), 1);  // snapshot before the result
    TEST_ASSERT_EQUAL(1, (int)s.entities().size());
    const size_t sent = t.sent.size();
    reconfigure(s, {"light.b"}, 100);
    TEST_ASSERT_EQUAL((int)sent + 2, (int)t.sent.size());
    TEST_ASSERT_EQUAL_STRING(unsubMsg(2, 1).c_str(), t.sent[sent].c_str());
    TEST_ASSERT_EQUAL_STRING(subMsg(3, "\"light.b\"").c_str(), t.sent[sent + 1].c_str());
    TEST_ASSERT_EQUAL(S::Subscribing, s.state());
    TEST_ASSERT_EQUAL(0, (int)s.entities().size());
    TEST_ASSERT_EQUAL(2, (int)s.pendingCount());  // unsubscribe + new subscribe
    // the old subscribe's late result and events are ignored
    feed(s, resultOk(1), 101);
    TEST_ASSERT_EQUAL(S::Subscribing, s.state());
    feed(s, eventA(1, {{"light.a", "off"}}), 101);
    TEST_ASSERT_EQUAL(0, (int)s.entities().size());
    // the new id's snapshot before its result is applied
    feed(s, eventA(3, {{"light.b", "on"}}), 102);
    TEST_ASSERT_EQUAL(1, (int)s.entities().size());
    feed(s, resultOk(3), 103);
    TEST_ASSERT_EQUAL(S::Ready, s.state());
    TEST_ASSERT_EQUAL(1, (int)s.entities().size());
}

static void test_resub_in_connecting_authenticating_backoff() {
    FakeTransport t;
    ha::HaSession s(t);
    s.setNetworkUp(true, 0);
    s.configure(kUrl, "tok", ha::TlsSettings(), {"light.a"}, 0);
    TEST_ASSERT_EQUAL(S::Connecting, s.state());
    reconfigure(s, {"light.b"}, 1);  // Connecting: nothing sent
    TEST_ASSERT_EQUAL(0, (int)t.sent.size());
    feed(s, kAuthRequired, 2);
    TEST_ASSERT_EQUAL(S::Authenticating, s.state());
    const size_t sent = t.sent.size();
    reconfigure(s, {"light.c"}, 3);  // Authenticating: nothing sent
    TEST_ASSERT_EQUAL((int)sent, (int)t.sent.size());
    TEST_ASSERT_EQUAL(1, t.opens);
    feed(s, kAuthOk, 4);  // the new ids are used
    TEST_ASSERT_EQUAL_STRING(subMsg(1, "\"light.c\"").c_str(), t.sent.back().c_str());
    feed(s, resultOk(1), 5);
    feed(s, eventA(1, {{"light.c", "on"}}), 6);
    TEST_ASSERT_EQUAL(1, (int)s.entities().size());
    // Backoff keeps the last values; an id change then clears them and sends nothing
    s.onClosed("lost", 100);
    TEST_ASSERT_EQUAL(S::Backoff, s.state());
    TEST_ASSERT_EQUAL(1, (int)s.entities().size());
    const size_t sent2 = t.sent.size();
    const int closes = t.closes;
    reconfigure(s, {"light.d"}, 200);
    TEST_ASSERT_EQUAL((int)sent2, (int)t.sent.size());
    TEST_ASSERT_EQUAL(closes, t.closes);
    TEST_ASSERT_EQUAL(S::Backoff, s.state());
    TEST_ASSERT_EQUAL(0, (int)s.entities().size());
    s.tick(1100);
    feed(s, kAuthRequired, 1100);
    feed(s, kAuthOk, 1100);
    TEST_ASSERT_EQUAL_STRING(subMsg(1, "\"light.d\"").c_str(), t.sent.back().c_str());
}

static void test_resub_full_pending_map_evicts_oldest_unsubscribe() {
    FakeTransport t;
    ha::HaSession s(t);
    Logs logs;
    attachLogs(s, logs);
    toReady(s, t, 0, {"light.0"});
    // every change leaves one unanswered Unsubscribe entry in the map
    for (int i = 1; i <= 20; ++i) {
        const size_t sent = t.sent.size();
        reconfigure(s, {"light." + std::to_string(i)}, 1000);
        TEST_ASSERT_EQUAL((int)sent + 2, (int)t.sent.size());
        TEST_ASSERT_TRUE(t.sent[sent].find("\"type\":\"unsubscribe_events\"") != std::string::npos);
        TEST_ASSERT_TRUE(t.sent[sent + 1].find("\"type\":\"subscribe_entities\"") != std::string::npos);
        TEST_ASSERT_TRUE((int)s.pendingCount() <= (int)ha::HaSession::kMaxPending);
        if (i < 20) feed(s, resultOk(lastId(t)), 1000);  // the subscribe result
    }
    TEST_ASSERT_EQUAL(S::Ready, s.state());
    TEST_ASSERT_EQUAL((int)ha::HaSession::kMaxPending, (int)s.pendingCount());
    // the last subscribe is tracked: it expires and ends the connection
    s.tick(1000 + 14999);
    TEST_ASSERT_EQUAL(S::Ready, s.state());
    s.tick(1000 + 15000);
    TEST_ASSERT_EQUAL(S::Backoff, s.state());
    TEST_ASSERT_EQUAL_STRING("subscribe_timeout", s.lastError().c_str());
    TEST_ASSERT_TRUE(countLog(logs, "unsubscribe failed: timeout") >= 1);
}

// ---- Subscribe results ---------------------------------------------------------------------

static void test_resub_failure_in_ready() {
    FakeTransport t;
    ha::HaSession s(t);
    toReady(s, t, 0, {"light.a"});
    reconfigure(s, {"light.b"}, 100);
    feed(s, resultOk(2), 101);
    const uint32_t cc = s.changeCount();
    feed(s, resultFail(3, "nope"), 102);
    TEST_ASSERT_EQUAL(S::Ready, s.state());
    TEST_ASSERT_EQUAL_STRING("subscribe_failed: nope", s.lastError().c_str());
    TEST_ASSERT_TRUE(s.changeCount() > cc);
    TEST_ASSERT_EQUAL(0, (int)s.entities().size());
    feed(s, eventA(3, {{"light.b", "on"}}), 103);  // its events are ignored
    TEST_ASSERT_EQUAL(0, (int)s.entities().size());
    feed(s, eventA(1, {{"light.a", "on"}}), 103);
    TEST_ASSERT_EQUAL(0, (int)s.entities().size());
}

static void test_resub_subscribe_failure_clears_store() {
    FakeTransport t;
    ha::HaSession s(t);
    toAuthenticated(s, t, 0, {"light.a"});
    feed(s, eventA(1, {{"light.a", "on"}}), 1);
    TEST_ASSERT_EQUAL(1, (int)s.entities().size());
    feed(s, resultFail(1, "bad"), 2);
    TEST_ASSERT_EQUAL(0, (int)s.entities().size());
}

static void test_resub_expiry_in_ready() {
    FakeTransport t;
    ha::HaSession s(t);
    toReady(s, t, 0, {"light.a"});
    reconfigure(s, {"light.b"}, 1000);
    s.tick(1000 + 14999);
    TEST_ASSERT_EQUAL(S::Ready, s.state());
    s.tick(1000 + 15000);
    TEST_ASSERT_EQUAL(S::Backoff, s.state());
    TEST_ASSERT_EQUAL_STRING("subscribe_timeout", s.lastError().c_str());
}

static void test_unsubscribe_expiry_and_failure_only_log() {
    FakeTransport t;
    ha::HaSession s(t);
    Logs logs;
    attachLogs(s, logs);
    toReady(s, t, 0, {"light.a"});
    reconfigure(s, {}, 1000);  // unsubscribe id 2
    s.tick(1000 + 14999);
    TEST_ASSERT_EQUAL(0, countLog(logs, "unsubscribe failed"));
    s.tick(1000 + 15000);
    TEST_ASSERT_EQUAL(S::Ready, s.state());
    TEST_ASSERT_TRUE(hasLog(logs, "unsubscribe failed: timeout"));
    TEST_ASSERT_EQUAL(1, countLog(logs, "unsubscribe failed"));
    TEST_ASSERT_EQUAL(0, (int)s.pendingCount());
    // success:false logs the message, the state does not change
    reconfigure(s, {"light.b"}, 17000);  // unsubscribe: none active (ids were empty) -> subscribe only
    TEST_ASSERT_EQUAL_STRING(subMsg(3, "\"light.b\"").c_str(), t.sent.back().c_str());
    feed(s, resultOk(3), 17001);
    reconfigure(s, {"light.c"}, 17002);  // unsubscribe id 4, subscribe id 5
    feed(s, resultFail(4, "boom"), 17003);
    TEST_ASSERT_TRUE(hasLog(logs, "unsubscribe failed: boom"));
    TEST_ASSERT_EQUAL(S::Ready, s.state());
    TEST_ASSERT_EQUAL_STRING("", s.lastError().c_str());
}

static void test_send_failure_during_unsubscribe() {
    FakeTransport t;
    ha::HaSession s(t);
    toReady(s, t, 0, {"light.a"});
    const size_t sent = t.sent.size();
    t.sendResult = false;
    reconfigure(s, {"light.b"}, 100);
    TEST_ASSERT_EQUAL(S::Backoff, s.state());
    TEST_ASSERT_EQUAL_STRING("send_failed", s.lastError().c_str());
    TEST_ASSERT_EQUAL(1, (int)s.failures());
    TEST_ASSERT_EQUAL((int)sent + 1, (int)t.sent.size());  // no subscribe after the failed unsubscribe
    TEST_ASSERT_EQUAL(1, t.closes);
}

static void test_send_failure_during_subscribe_of_resubscribe() {
    FakeTransport t;
    ha::HaSession s(t);
    toReady(s, t, 0, {"light.a"});
    int n = 0;
    t.onSend = [&]() { t.sendResult = (++n != 2); };  // the unsubscribe goes, the subscribe fails
    reconfigure(s, {"light.b"}, 100);
    TEST_ASSERT_EQUAL(2, n);
    TEST_ASSERT_EQUAL(S::Backoff, s.state());
    TEST_ASSERT_EQUAL_STRING("send_failed", s.lastError().c_str());
    TEST_ASSERT_EQUAL(1, (int)s.failures());
    TEST_ASSERT_EQUAL(1, t.closes);
    TEST_ASSERT_EQUAL(0, (int)s.pendingCount());
}

static void test_send_failure_reporting_close_during_resubscribe_counts_once() {
    FakeTransport t;
    ha::HaSession s(t);
    toReady(s, t, 0, {"light.a"});
    t.sendResult = false;
    t.onSend = [&]() { s.onClosed("socket_closed", 100); };
    reconfigure(s, {"light.b"}, 100);
    TEST_ASSERT_EQUAL(S::Backoff, s.state());
    TEST_ASSERT_EQUAL_STRING("socket_closed", s.lastError().c_str());
    TEST_ASSERT_EQUAL(1, (int)s.failures());
}

// ---- Events --------------------------------------------------------------------------------

static void test_event_stale_and_zero_id_ignored() {
    FakeTransport t;
    ha::HaSession s(t);
    toReady(s, t, 0, {"light.a"});
    feed(s, eventA(99, {{"light.a", "on"}}), 1);
    TEST_ASSERT_EQUAL(0, (int)s.entities().size());
    feed(s, eventA(0, {{"light.a", "on"}}), 1);
    feed(s, "{\"type\":\"event\",\"event\":{\"a\":{\"light.a\":{\"s\":\"on\",\"a\":{}}}}}", 1);  // no id
    TEST_ASSERT_EQUAL(0, (int)s.entities().size());
    feed(s, eventA(1, {{"light.a", "on"}}), 2);
    TEST_ASSERT_EQUAL(1, (int)s.entities().size());
    const ha::EntityState *e = s.entities().find("light.a");
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_STRING("on", e->state.c_str());
}

static void test_event_too_large_logged_store_unchanged() {
    FakeTransport t;
    ha::HaSession s(t);
    Logs logs;
    attachLogs(s, logs);
    toReady(s, t, 0, {"light.a"});
    feed(s, eventA(1, {{"light.a", "on"}}), 1);
    const uint32_t v = s.entities().version();
    std::string big = "{\"id\":1,\"type\":\"event\",\"event\":{\"a\":{\"light.a\":{\"s\":\"off\",\"a\":{\"junk\":\"" +
                      std::string(300 * 1024, 'x') + "\"}}}}}";
    feed(s, big, 2);
    TEST_ASSERT_TRUE(hasLog(logs, "event dropped error=too_large"));
    TEST_ASSERT_EQUAL_UINT32(v, s.entities().version());
    TEST_ASSERT_EQUAL_STRING("on", s.entities().find("light.a")->state.c_str());
    // other subscription id: silent
    logs.lines.clear();
    std::string other = big;
    other[6] = '9';  // id 9
    feed(s, other, 3);
    TEST_ASSERT_EQUAL(0, (int)logs.lines.size());
    TEST_ASSERT_EQUAL(S::Ready, s.state());
}

static void test_event_unparsable_logged_only_for_own_event() {
    FakeTransport t;
    ha::HaSession s(t);
    Logs logs;
    attachLogs(s, logs);
    toReady(s, t, 0, {"light.a"});
    logs.lines.clear();
    feed(s, "{\"id\":1,\"type\":\"event\",\"event\":{\"a\":", 1);  // truncated
    TEST_ASSERT_EQUAL(1, (int)logs.lines.size());
    TEST_ASSERT_EQUAL_STRING("event dropped error=IncompleteInput", logs.lines[0].c_str());
    logs.lines.clear();
    feed(s, "{\"id\":7,\"type\":\"event\",\"event\":{\"a\":", 2);  // other id
    feed(s, "{\"id\":1,\"type\":\"result\",\"success\":tr", 3);    // not an event
    feed(s, "garbage", 4);
    feed(s, "{\"event\":{\"a\":", 5);
    TEST_ASSERT_EQUAL(0, (int)logs.lines.size());
    TEST_ASSERT_EQUAL(S::Ready, s.state());
}

static void test_snapshot_log_once_per_subscription() {
    FakeTransport t;
    ha::HaSession s(t);
    Logs logs;
    attachLogs(s, logs);
    toReady(s, t, 0, {"light.a", "light.b"});
    feed(s, eventA(1, {{"light.a", "on"}, {"light.b", "off"}}), 1);
    TEST_ASSERT_TRUE(hasLog(logs, "snapshot entities=2"));
    feed(s, eventA(1, {{"light.a", "off"}}), 2);
    feed(s, "{\"id\":1,\"type\":\"event\",\"event\":{\"c\":{\"light.a\":{\"+\":{\"s\":\"on\"}}}}}", 3);
    TEST_ASSERT_EQUAL(1, countLog(logs, "snapshot"));
    // a new subscription gets its own snapshot line, also an empty one
    reconfigure(s, {"light.c"}, 10);  // unsubscribe 2, subscribe 3
    feed(s, "{\"id\":3,\"type\":\"event\",\"event\":{\"a\":{}}}", 11);
    TEST_ASSERT_TRUE(hasLog(logs, "snapshot entities=0"));
    TEST_ASSERT_EQUAL(2, countLog(logs, "snapshot"));
    feed(s, eventA(3, {{"light.c", "on"}}), 12);
    TEST_ASSERT_EQUAL(2, countLog(logs, "snapshot"));
}

static void test_store_cleared_on_connect_kept_in_backoff() {
    FakeTransport t;
    ha::HaSession s(t);
    toReady(s, t, 0, {"light.a"});
    feed(s, eventA(1, {{"light.a", "on"}}), 1);
    s.onClosed("lost", 100);
    TEST_ASSERT_EQUAL(S::Backoff, s.state());
    TEST_ASSERT_EQUAL(1, (int)s.entities().size());
    s.tick(500);
    TEST_ASSERT_EQUAL(1, (int)s.entities().size());
    s.tick(1100);  // reconnect
    TEST_ASSERT_EQUAL(S::Connecting, s.state());
    TEST_ASSERT_EQUAL(0, (int)s.entities().size());
    TEST_ASSERT_FALSE(s.entities().snapshotSeen());
    // network loss: kept, too
    FakeTransport t2;
    ha::HaSession s2(t2);
    toReady(s2, t2, 0, {"light.a"});
    feed(s2, eventA(1, {{"light.a", "on"}}), 1);
    s2.setNetworkUp(false, 10);
    TEST_ASSERT_EQUAL(1, (int)s2.entities().size());
}

// ---- requestStates -------------------------------------------------------------------------

static void test_request_states_false_outside_ready() {
    FakeTransport t;
    ha::HaSession s(t);
    TEST_ASSERT_FALSE(s.requestStates(0));  // NoConfig
    s.setNetworkUp(true, 0);
    s.configure(kUrl, "tok", ha::TlsSettings(), {"light.a"}, 0);
    TEST_ASSERT_FALSE(s.requestStates(1));  // Connecting
    feed(s, kAuthRequired, 2);
    TEST_ASSERT_FALSE(s.requestStates(3));  // Authenticating
    feed(s, kAuthOk, 4);
    TEST_ASSERT_EQUAL(S::Subscribing, s.state());
    TEST_ASSERT_FALSE(s.requestStates(5));  // Subscribing
    s.onClosed("lost", 6);
    TEST_ASSERT_FALSE(s.requestStates(7));  // Backoff
    TEST_ASSERT_EQUAL(ha::StatesState::None, s.statesState());
    for (const std::string &m : t.sent) TEST_ASSERT_TRUE(m.find("get_states") == std::string::npos);
    FakeTransport t2;
    ha::HaSession s2(t2);
    s2.setNetworkUp(true, 0);
    s2.configure(kUrl, "bad", ha::TlsSettings(), {}, 0);
    feed(s2, kAuthRequired, 1);
    feed(s2, kAuthInvalid, 2);
    TEST_ASSERT_FALSE(s2.requestStates(3));  // AuthFailed
    TEST_ASSERT_EQUAL(0, t.retainCalls + t2.retainCalls);
}

static void test_request_states_coalesces_and_restarts() {
    FakeTransport t;
    ha::HaSession s(t);
    Logs logs;
    attachLogs(s, logs);
    toReady(s, t, 0);
    TEST_ASSERT_TRUE(s.requestStates(100));
    TEST_ASSERT_EQUAL_STRING(getStatesMsg(1).c_str(), t.sent.back().c_str());
    TEST_ASSERT_EQUAL(ha::StatesState::Fetching, s.statesState());
    TEST_ASSERT_TRUE(hasLog(logs, "states request id=1"));
    const size_t sent = t.sent.size();
    TEST_ASSERT_TRUE(s.requestStates(200));  // coalesced
    TEST_ASSERT_EQUAL((int)sent, (int)t.sent.size());
    feed(s, statesResult(1, 3), 300);
    TEST_ASSERT_EQUAL(ha::StatesState::Building, s.statesState());
    TEST_ASSERT_TRUE(s.requestStates(301));  // coalesced while Building
    TEST_ASSERT_EQUAL((int)sent, (int)t.sent.size());
    TEST_ASSERT_EQUAL(ha::StatesState::Building, s.statesState());
    uint32_t now = 302;
    finishBuild(s, now);
    TEST_ASSERT_EQUAL(ha::StatesState::Ready, s.statesState());
    // from Ready (catalog not taken): a new fetch drops the old catalog
    TEST_ASSERT_TRUE(s.requestStates(400));
    TEST_ASSERT_EQUAL_STRING(getStatesMsg(2).c_str(), t.sent.back().c_str());
    TEST_ASSERT_EQUAL(ha::StatesState::Fetching, s.statesState());
    TEST_ASSERT_FALSE(s.takeCatalog().has_value());
    // from Failed: the error is dropped
    s.onMessageDropped(5000000, "too_large", 401);
    TEST_ASSERT_EQUAL(ha::StatesState::Failed, s.statesState());
    TEST_ASSERT_EQUAL_STRING("states_too_large", s.statesError().c_str());
    TEST_ASSERT_TRUE(s.requestStates(500));
    TEST_ASSERT_EQUAL_STRING("", s.statesError().c_str());
    TEST_ASSERT_EQUAL_STRING(getStatesMsg(3).c_str(), t.sent.back().c_str());
    TEST_ASSERT_EQUAL(ha::StatesState::Fetching, s.statesState());
    balanced(t);
}

static void test_request_states_send_failure_inside_onclosed() {
    FakeTransport t;
    ha::HaSession s(t);
    Logs logs;
    attachLogs(s, logs);
    toReady(s, t, 0);
    t.sendResult = false;
    t.onSend = [&]() { s.onClosed("socket_closed", 2000); };
    TEST_ASSERT_FALSE(s.requestStates(2000));
    TEST_ASSERT_EQUAL(S::Backoff, s.state());
    TEST_ASSERT_EQUAL_STRING("socket_closed", s.lastError().c_str());
    TEST_ASSERT_EQUAL(1, (int)s.failures());
    TEST_ASSERT_EQUAL(ha::StatesState::Failed, s.statesState());
    TEST_ASSERT_EQUAL_STRING("connection_lost", s.statesError().c_str());
    TEST_ASSERT_EQUAL(1, countLog(logs, "catalog failed error=connection_lost"));
}

static void test_request_states_plain_send_failure() {
    FakeTransport t;
    ha::HaSession s(t);
    Logs logs;
    attachLogs(s, logs);
    toReady(s, t, 0);
    t.sendResult = false;
    TEST_ASSERT_FALSE(s.requestStates(2000));
    TEST_ASSERT_EQUAL(S::Backoff, s.state());
    TEST_ASSERT_EQUAL_STRING("send_failed", s.lastError().c_str());
    TEST_ASSERT_EQUAL(ha::StatesState::Failed, s.statesState());
    TEST_ASSERT_EQUAL_STRING("connection_lost", s.statesError().c_str());
    TEST_ASSERT_EQUAL(1, countLog(logs, "catalog failed error=connection_lost"));
}

// ---- Retain / build ------------------------------------------------------------------------

static void test_retain_id_first_result_builds_catalog() {
    FakeTransport t;
    ha::HaSession s(t);
    Logs logs;
    attachLogs(s, logs);
    toReady(s, t, 0);
    s.requestStates(100);
    const std::string r = statesResult(1, 5);
    feed(s, r, 200);
    TEST_ASSERT_EQUAL(1, t.retainCalls);
    TEST_ASSERT_EQUAL(1, t.allocs);
    TEST_ASSERT_EQUAL(0, t.frees);  // the text lives while Building
    TEST_ASSERT_EQUAL(ha::StatesState::Building, s.statesState());
    TEST_ASSERT_TRUE(hasLog(logs, "states retained len=" + std::to_string(r.size())));
    uint32_t now = 201;
    finishBuild(s, now);
    TEST_ASSERT_EQUAL(1, t.frees);
    TEST_ASSERT_EQUAL(0, countLog(logs, "catalog failed"));
    expectCatalog(s, 5);
    TEST_ASSERT_FALSE(s.takeCatalog().has_value());
    TEST_ASSERT_EQUAL(ha::StatesState::Ready, s.statesState());  // stays Ready after take
    balanced(t);
}

static void test_retain_small_result_first() {
    FakeTransport t;
    ha::HaSession s(t);
    toReady(s, t, 0);
    s.requestStates(100);
    const std::string r = statesResult(1, 4, 0, true);
    TEST_ASSERT_TRUE(r.size() < 64u * 1024u);
    feed(s, r, 200);
    TEST_ASSERT_EQUAL(ha::StatesState::Building, s.statesState());
    TEST_ASSERT_EQUAL(1, t.allocs);
    uint32_t now = 201;
    finishBuild(s, now);
    expectCatalog(s, 4);
    balanced(t);
}

static void test_retain_big_result_first_inconclusive_peek() {
    FakeTransport t;
    ha::HaSession s(t);
    toReady(s, t, 0);
    s.requestStates(100);
    const std::string r = statesResult(1, 4, kBig, true);
    TEST_ASSERT_TRUE(r.size() > 64u * 1024u);
    feed(s, r, 200);
    TEST_ASSERT_EQUAL(ha::StatesState::Building, s.statesState());
    TEST_ASSERT_EQUAL(1, t.allocs);
    uint32_t now = 201;
    finishBuild(s, now);
    expectCatalog(s, 4);
    balanced(t);
}

static void test_retain_big_id_first_result() {
    FakeTransport t;
    ha::HaSession s(t);
    toReady(s, t, 0);
    s.requestStates(100);
    feed(s, statesResult(1, 3, kBig), 200);
    TEST_ASSERT_EQUAL(ha::StatesState::Building, s.statesState());
    uint32_t now = 201;
    finishBuild(s, now);
    expectCatalog(s, 3);
    balanced(t);
}

static void test_fetching_other_message_handled_normally() {
    FakeTransport t;
    ha::HaSession s(t);
    toReady(s, t, 0, {"light.a"});
    s.tick(30000);  // ping (id 2)
    const uint32_t pingId = lastId(t);
    TEST_ASSERT_TRUE(t.sent.back().find("ping") != std::string::npos);
    s.requestStates(30001);
    TEST_ASSERT_EQUAL(ha::StatesState::Fetching, s.statesState());
    feed(s, "{\"id\":" + std::to_string(pingId) + ",\"type\":\"pong\"}", 30002);  // not retained
    feed(s, eventA(1, {{"light.a", "on"}}), 30003);                              // not retained
    TEST_ASSERT_EQUAL(0, t.retainCalls);
    TEST_ASSERT_EQUAL(ha::StatesState::Fetching, s.statesState());
    TEST_ASSERT_EQUAL(1, (int)s.entities().size());
    // the pong cleared the ping: no pong_timeout 10 s later
    s.onRxProgress(1, 40000);
    s.tick(40001);
    TEST_ASSERT_EQUAL(S::Ready, s.state());
}

static void test_retain_fails_gives_states_no_memory() {
    FakeTransport t;
    ha::HaSession s(t);
    Logs logs;
    attachLogs(s, logs);
    toReady(s, t, 0);
    s.requestStates(100);
    t.retainFails = true;
    feed(s, statesResult(1, 3), 200);
    TEST_ASSERT_EQUAL(1, t.retainCalls);
    TEST_ASSERT_EQUAL(ha::StatesState::Failed, s.statesState());
    TEST_ASSERT_EQUAL_STRING("states_no_memory", s.statesError().c_str());
    TEST_ASSERT_TRUE(hasLog(logs, "catalog failed error=states_no_memory"));
    TEST_ASSERT_EQUAL(S::Ready, s.state());
    balanced(t);
}

static void test_default_transport_retain_is_empty() {
    // The base Transport cannot retain: the session reports states_no_memory.
    struct Bare : ha::Transport {
        void open(const ha::HaUrl &, const ha::TlsSettings &) override {}
        void close() override {}
        bool sendText(const std::string &) override { return true; }
    } bare;
    ha::HaSession s(bare);
    s.setNetworkUp(true, 0);
    s.configure(kUrl, "tok", ha::TlsSettings(), {}, 0);
    feed(s, kAuthRequired, 0);
    feed(s, kAuthOk, 0);
    TEST_ASSERT_TRUE(s.requestStates(1));
    feed(s, statesResult(1, 2), 2);
    TEST_ASSERT_EQUAL_STRING("states_no_memory", s.statesError().c_str());
}

static std::string catalogDump(const ha::EntityCatalog &c) {
    std::string out = std::to_string(c.total()) + "/" + std::to_string(c.skipped()) + "/" + (c.truncated() ? "t" : "f") + ":";
    for (size_t i = 0; i < c.size(); ++i) {
        const ha::CatalogEntry e = c.entry(i);
        out += std::string(e.id) + "|" + std::string(e.name) + "|" + std::string(e.state) + "|" +
               std::to_string(e.actionMask) + ";";
    }
    return out;
}

static void test_build_budget_does_not_change_catalog() {
    std::string text = "{\"id\":1,\"type\":\"result\",\"success\":true,\"result\":[";
    for (int i = 0; i < 10; ++i) text += entityJson(i) + ",";
    text += "{\"entity_id\":\"sun.sun\",\"state\":\"above_horizon\",\"attributes\":{}},";
    text += "{\"entity_id\":\"switch.s\",\"state\":\"off\",\"attributes\":{\"friendly_name\":\"S\"}}]}";
    const size_t budgets[] = {1, 200, (size_t)-1};
    std::string first;
    for (size_t budget : budgets) {
        FakeTransport t;
        ha::SessionOptions opt;
        opt.statesBytesPerTick = budget;
        ha::HaSession s(t, nullptr, opt);
        toReady(s, t, 0);
        const uint32_t cc = s.changeCount();
        s.requestStates(100);
        const uint32_t sc = s.statesChangeCount();
        feed(s, text, 200);
        TEST_ASSERT_EQUAL(ha::StatesState::Building, s.statesState());
        const uint32_t sc2 = s.statesChangeCount();
        TEST_ASSERT_TRUE(sc2 > sc);
        int ticks = 0;
        while (s.statesState() == ha::StatesState::Building && ticks < 100000) {
            s.tick(300);
            ++ticks;
        }
        TEST_ASSERT_EQUAL(ha::StatesState::Ready, s.statesState());
        if (budget == 1) TEST_ASSERT_TRUE(ticks > 10);
        if (budget == (size_t)-1) TEST_ASSERT_EQUAL(1, ticks);
        TEST_ASSERT_TRUE(s.statesChangeCount() > sc2);  // a catalog became available
        TEST_ASSERT_EQUAL_UINT32(cc, s.changeCount());  // the connection status did not change
        std::optional<ha::EntityCatalog> c = s.takeCatalog();
        TEST_ASSERT_TRUE(c.has_value());
        TEST_ASSERT_FALSE(s.takeCatalog().has_value());
        const std::string dump = catalogDump(*c);
        if (first.empty()) {
            first = dump;
            TEST_ASSERT_TRUE(dump.find("light.k9|K9|on") != std::string::npos);
            TEST_ASSERT_TRUE(dump.find("switch.s|S|off") != std::string::npos);
        }
        TEST_ASSERT_EQUAL_STRING(first.c_str(), dump.c_str());
        TEST_ASSERT_EQUAL_INT(1, t.frees);
        balanced(t);
    }
}

static void test_states_result_failure_and_wrong_id() {
    {
        FakeTransport t;
        ha::HaSession s(t);
        toReady(s, t, 0);
        s.requestStates(100);
        feed(s, resultFail(1, "denied"), 200);
        uint32_t now = 201;
        finishBuild(s, now);
        TEST_ASSERT_EQUAL(ha::StatesState::Failed, s.statesState());
        TEST_ASSERT_EQUAL_STRING("states_failed: denied", s.statesError().c_str());
        TEST_ASSERT_FALSE(s.takeCatalog().has_value());
        balanced(t);
    }
    {
        // a large message without an id in front whose id is another one: retained, rejected
        FakeTransport t;
        ha::HaSession s(t);
        toReady(s, t, 0);
        s.requestStates(100);
        std::string wrong = statesResult(999, 3, kBig, true);
        feed(s, wrong, 200);
        TEST_ASSERT_EQUAL(1, t.allocs);
        uint32_t now = 201;
        finishBuild(s, now);
        TEST_ASSERT_EQUAL(ha::StatesState::Failed, s.statesState());
        TEST_ASSERT_EQUAL_STRING("states_unexpected", s.statesError().c_str());
        balanced(t);
        TEST_ASSERT_EQUAL(1, t.frees);
        TEST_ASSERT_EQUAL(S::Ready, s.state());
    }
}

static void test_build_survives_close_network_loss_reconnect() {
    ha::SessionOptions opt;
    opt.statesBytesPerTick = 1;
    {
        FakeTransport t;
        ha::HaSession s(t, nullptr, opt);
        toReady(s, t, 0);
        s.requestStates(100);
        feed(s, statesResult(1, 10), 200);
        s.tick(201);
        TEST_ASSERT_EQUAL(ha::StatesState::Building, s.statesState());
        s.onClosed("lost", 202);
        TEST_ASSERT_EQUAL(S::Backoff, s.state());
        s.tick(203);
        TEST_ASSERT_EQUAL(ha::StatesState::Building, s.statesState());
        s.tick(1202);  // reconnect
        TEST_ASSERT_EQUAL(S::Connecting, s.state());
        TEST_ASSERT_EQUAL(ha::StatesState::Building, s.statesState());
        TEST_ASSERT_EQUAL(0, t.frees);
        uint32_t now = 1203;
        finishBuild(s, now);
        expectCatalog(s, 10);
        balanced(t);
        TEST_ASSERT_EQUAL(1, t.frees);
    }
    {
        FakeTransport t;
        ha::HaSession s(t, nullptr, opt);
        toReady(s, t, 0);
        s.requestStates(100);
        feed(s, statesResult(1, 10), 200);
        s.tick(201);
        s.setNetworkUp(false, 202);
        TEST_ASSERT_EQUAL(S::Backoff, s.state());
        s.tick(203);
        TEST_ASSERT_EQUAL(ha::StatesState::Building, s.statesState());
        s.setNetworkUp(true, 5000);
        TEST_ASSERT_EQUAL(S::Connecting, s.state());
        TEST_ASSERT_EQUAL(ha::StatesState::Building, s.statesState());
        uint32_t now = 5001;
        finishBuild(s, now);
        expectCatalog(s, 10);
        balanced(t);
    }
}

static void test_config_change_mid_build_drops_everything() {
    struct Change {
        const char *url;
        const char *token;
        bool insecure;
    };
    const Change changes[] = {{"http://192.168.1.11:8123", "tok", false},
                              {"http://192.168.1.10:8123", "other", false},
                              {"http://192.168.1.10:8123", "tok", true}};
    ha::SessionOptions opt;
    opt.statesBytesPerTick = 1;
    for (const Change &c : changes) {
        FakeTransport t;
        ha::HaSession s(t, nullptr, opt);
        toReady(s, t, 0);
        s.requestStates(100);
        feed(s, statesResult(1, 6), 200);
        s.tick(201);
        TEST_ASSERT_EQUAL(ha::StatesState::Building, s.statesState());
        ha::TlsSettings tls;
        tls.insecure = c.insecure;
        s.configure(c.url, c.token, tls, {}, 300);
        TEST_ASSERT_EQUAL(ha::StatesState::None, s.statesState());
        TEST_ASSERT_EQUAL_STRING("", s.statesError().c_str());
        TEST_ASSERT_FALSE(s.takeCatalog().has_value());
        TEST_ASSERT_EQUAL(1, t.allocs);
        TEST_ASSERT_EQUAL(1, t.frees);
        s.tick(301);  // nothing left to build
        TEST_ASSERT_EQUAL(ha::StatesState::None, s.statesState());
    }
    // a finished, untaken catalog is dropped too
    FakeTransport t;
    ha::HaSession s(t);
    toReady(s, t, 0);
    s.requestStates(100);
    feed(s, statesResult(1, 6), 200);
    uint32_t now = 201;
    finishBuild(s, now);
    TEST_ASSERT_EQUAL(ha::StatesState::Ready, s.statesState());
    s.configure(kUrl, "newtoken", ha::TlsSettings(), {}, 300);
    TEST_ASSERT_EQUAL(ha::StatesState::None, s.statesState());
    TEST_ASSERT_FALSE(s.takeCatalog().has_value());
    balanced(t);
    // and a running fetch
    FakeTransport t2;
    ha::HaSession s2(t2);
    toReady(s2, t2, 0);
    s2.requestStates(100);
    s2.configure(kUrl, "newtoken", ha::TlsSettings(), {}, 300);
    TEST_ASSERT_EQUAL(ha::StatesState::None, s2.statesState());
    TEST_ASSERT_EQUAL(0, t2.allocs);
}

static void test_destroying_session_mid_build_frees_text_once() {
    ha::SessionOptions opt;
    opt.statesBytesPerTick = 1;
    FakeTransport t;
    {
        ha::HaSession s(t, nullptr, opt);
        toReady(s, t, 0);
        s.requestStates(100);
        feed(s, statesResult(1, 6), 200);
        s.tick(201);
        TEST_ASSERT_EQUAL(ha::StatesState::Building, s.statesState());
        TEST_ASSERT_EQUAL(1, t.allocs);
        TEST_ASSERT_EQUAL(0, t.frees);
    }
    TEST_ASSERT_EQUAL(1, t.allocs);
    TEST_ASSERT_EQUAL(1, t.frees);
}

static void test_failed_fetch_drops_untaken_catalog() {
    FakeTransport t;
    ha::HaSession s(t);
    toReady(s, t, 0);
    s.requestStates(100);
    feed(s, statesResult(1, 3), 200);
    uint32_t now = 201;
    finishBuild(s, now);
    TEST_ASSERT_EQUAL(ha::StatesState::Ready, s.statesState());
    TEST_ASSERT_TRUE(s.requestStates(300));
    s.onMessageDropped(10, "too_large", 301);
    TEST_ASSERT_EQUAL(ha::StatesState::Failed, s.statesState());
    TEST_ASSERT_FALSE(s.takeCatalog().has_value());
    balanced(t);
}

static void test_drops_while_fetching() {
    FakeTransport t;
    ha::HaSession s(t);
    Logs logs;
    attachLogs(s, logs);
    toReady(s, t, 0);
    s.requestStates(100);
    s.onMessageDropped(5u << 20, "binary", 200);  // not a result
    TEST_ASSERT_EQUAL(ha::StatesState::Fetching, s.statesState());
    s.onMessageDropped(5u << 20, "too_large", 201);
    TEST_ASSERT_EQUAL(ha::StatesState::Failed, s.statesState());
    TEST_ASSERT_EQUAL_STRING("states_too_large", s.statesError().c_str());
    TEST_ASSERT_TRUE(hasLog(logs, "catalog failed error=states_too_large"));
    s.requestStates(300);
    s.onMessageDropped(5u << 20, "no_memory", 301);
    TEST_ASSERT_EQUAL(ha::StatesState::Failed, s.statesState());
    TEST_ASSERT_EQUAL_STRING("states_no_memory", s.statesError().c_str());
    TEST_ASSERT_EQUAL_UINT32(3, s.droppedMessages());
    // not Fetching: only counted
    const uint32_t sc = s.statesChangeCount();
    s.onMessageDropped(5u << 20, "too_large", 400);
    TEST_ASSERT_EQUAL_UINT32(4, s.droppedMessages());
    TEST_ASSERT_EQUAL_UINT32(sc, s.statesChangeCount());
    TEST_ASSERT_EQUAL_STRING("states_no_memory", s.statesError().c_str());
    FakeTransport t2;
    ha::HaSession s2(t2);
    toReady(s2, t2, 0);
    s2.onMessageDropped(10, "too_large", 1);
    TEST_ASSERT_EQUAL(ha::StatesState::None, s2.statesState());
    TEST_ASSERT_EQUAL_UINT32(1, s2.droppedMessages());
    // a binary drop does not end the fetch: the real result still arrives
    s2.requestStates(2);
    s2.onMessageDropped(10, "binary", 3);
    feed(s2, statesResult(1, 2), 4);
    TEST_ASSERT_EQUAL(ha::StatesState::Building, s2.statesState());
}

static void test_connection_lost_while_fetching() {
    FakeTransport t;
    ha::HaSession s(t);
    Logs logs;
    attachLogs(s, logs);
    toReady(s, t, 0);
    s.requestStates(100);
    s.onClosed("lost", 200);
    TEST_ASSERT_EQUAL(ha::StatesState::Failed, s.statesState());
    TEST_ASSERT_EQUAL_STRING("connection_lost", s.statesError().c_str());
    TEST_ASSERT_EQUAL(1, countLog(logs, "catalog failed error=connection_lost"));
    // connect() while Fetching (retry from Backoff cannot be Fetching; network loss can)
    FakeTransport t2;
    ha::HaSession s2(t2);
    toReady(s2, t2, 0);
    s2.requestStates(100);
    s2.setNetworkUp(false, 200);
    TEST_ASSERT_EQUAL_STRING("connection_lost", s2.statesError().c_str());
}

// ---- Late results --------------------------------------------------------------------------

static void test_late_big_result_after_timeout_dropped() {
    FakeTransport t;
    ha::HaSession s(t);
    Logs logs;
    attachLogs(s, logs);
    toReady(s, t, 0);
    s.requestStates(1000);
    s.tick(1000 + 14999);
    TEST_ASSERT_EQUAL(ha::StatesState::Fetching, s.statesState());
    s.tick(1000 + 15000);
    TEST_ASSERT_EQUAL(ha::StatesState::Failed, s.statesState());
    TEST_ASSERT_EQUAL_STRING("states_timeout", s.statesError().c_str());
    TEST_ASSERT_EQUAL(S::Ready, s.state());
    const std::string big = statesResult(1, 3, kBig);
    feed(s, big, 17000);
    TEST_ASSERT_TRUE(hasLog(logs, "states late result dropped len=" + std::to_string(big.size())));
    TEST_ASSERT_EQUAL(ha::StatesState::Failed, s.statesState());
    TEST_ASSERT_EQUAL_STRING("states_timeout", s.statesError().c_str());
    TEST_ASSERT_EQUAL(0, t.retainCalls);
    // used once: a second copy takes the normal way (unknown result id)
    feed(s, big, 17001);
    TEST_ASSERT_EQUAL(1, countLog(logs, "states late result dropped"));
    TEST_ASSERT_EQUAL(ha::StatesState::Failed, s.statesState());
    TEST_ASSERT_EQUAL(S::Ready, s.state());
    TEST_ASSERT_EQUAL(0, t.retainCalls);
}

static void test_late_small_result_parsed_normally() {
    FakeTransport t;
    ha::HaSession s(t);
    Logs logs;
    attachLogs(s, logs);
    toReady(s, t, 0);
    s.requestStates(1000);
    s.tick(16000);
    TEST_ASSERT_EQUAL(ha::StatesState::Failed, s.statesState());
    const std::string small = statesResult(1, 3);
    feed(s, small, 17000);
    TEST_ASSERT_EQUAL(0, countLog(logs, "states late result"));
    TEST_ASSERT_EQUAL(ha::StatesState::Failed, s.statesState());
    TEST_ASSERT_EQUAL(S::Ready, s.state());
    TEST_ASSERT_EQUAL(0, t.retainCalls);
}

static void test_late_result_with_new_fetch() {
    FakeTransport t;
    ha::HaSession s(t);
    Logs logs;
    attachLogs(s, logs);
    toReady(s, t, 0);
    s.requestStates(1000);  // id 1
    s.tick(16000);          // timeout
    TEST_ASSERT_TRUE(s.requestStates(16100));
    TEST_ASSERT_EQUAL_STRING(getStatesMsg(2).c_str(), t.sent.back().c_str());
    feed(s, statesResult(1, 3, kBig), 16200);  // the late old one: dropped, the fetch goes on
    TEST_ASSERT_EQUAL(1, countLog(logs, "states late result dropped"));
    TEST_ASSERT_EQUAL(ha::StatesState::Fetching, s.statesState());
    TEST_ASSERT_EQUAL(0, t.retainCalls);
    feed(s, statesResult(2, 3, kBig), 16300);  // the new one: retained
    TEST_ASSERT_EQUAL(1, t.retainCalls);
    uint32_t now = 16301;
    finishBuild(s, now);
    expectCatalog(s, 3);
    balanced(t);
}

static void test_late_result_not_across_connections() {
    // via fail() / reconnect
    {
        FakeTransport t;
        ha::HaSession s(t);
        Logs logs;
        attachLogs(s, logs);
        toReady(s, t, 0);
        s.requestStates(1000);  // id 1 on connection A
        s.tick(16000);
        TEST_ASSERT_EQUAL(ha::StatesState::Failed, s.statesState());
        s.onClosed("lost", 16100);
        s.tick(17100);
        TEST_ASSERT_EQUAL(S::Connecting, s.state());
        feed(s, kAuthRequired, 17100);
        feed(s, kAuthOk, 17100);
        TEST_ASSERT_EQUAL(S::Ready, s.state());
        feed(s, statesResult(1, 3, kBig), 17200);  // same id on connection B, no fetch running
        TEST_ASSERT_EQUAL(0, countLog(logs, "states late result"));
        TEST_ASSERT_TRUE(s.requestStates(17300));  // id 1 again
        TEST_ASSERT_EQUAL_STRING(getStatesMsg(1).c_str(), t.sent.back().c_str());
        feed(s, statesResult(1, 3, kBig), 17400);
        TEST_ASSERT_EQUAL(0, countLog(logs, "states late result"));
        TEST_ASSERT_EQUAL(1, t.retainCalls);
        uint32_t now = 17401;
        finishBuild(s, now);
        expectCatalog(s, 3);
        balanced(t);
    }
    // via configure() with another URL
    {
        FakeTransport t;
        ha::HaSession s(t);
        Logs logs;
        attachLogs(s, logs);
        toReady(s, t, 0);
        s.requestStates(1000);
        s.tick(16000);
        s.configure("http://192.168.1.11:8123", "tok", ha::TlsSettings(), {}, 16100);
        TEST_ASSERT_EQUAL(S::Connecting, s.state());
        feed(s, kAuthRequired, 16100);
        feed(s, kAuthOk, 16100);
        TEST_ASSERT_EQUAL(S::Ready, s.state());
        feed(s, statesResult(1, 3, kBig), 16200);
        TEST_ASSERT_EQUAL(0, countLog(logs, "states late result"));
        TEST_ASSERT_TRUE(s.requestStates(16300));
        feed(s, statesResult(1, 3, kBig), 16400);
        TEST_ASSERT_EQUAL(1, t.retainCalls);
        TEST_ASSERT_EQUAL(ha::StatesState::Building, s.statesState());
    }
}

static void test_late_result_after_too_large_and_no_memory() {
    {
        FakeTransport t;
        ha::HaSession s(t);
        Logs logs;
        attachLogs(s, logs);
        toReady(s, t, 0);
        s.requestStates(1000);
        s.onMessageDropped(5u << 20, "too_large", 1100);
        TEST_ASSERT_EQUAL_STRING("states_too_large", s.statesError().c_str());
        feed(s, statesResult(1, 3, kBig), 1200);
        TEST_ASSERT_EQUAL(1, countLog(logs, "states late result dropped"));
        TEST_ASSERT_EQUAL(0, t.retainCalls);
    }
    {
        FakeTransport t;
        ha::HaSession s(t);
        Logs logs;
        attachLogs(s, logs);
        toReady(s, t, 0);
        s.requestStates(1000);
        t.retainFails = true;
        feed(s, statesResult(1, 3, kBig), 1100);
        TEST_ASSERT_EQUAL_STRING("states_no_memory", s.statesError().c_str());
        TEST_ASSERT_EQUAL(1, t.retainCalls);
        feed(s, statesResult(1, 3, kBig), 1200);  // the same id once more
        TEST_ASSERT_EQUAL(1, countLog(logs, "states late result dropped"));
        TEST_ASSERT_EQUAL(1, t.retainCalls);
    }
}

static void test_late_result_after_builder_failure() {
    FakeTransport t;
    ha::HaSession s(t);
    Logs logs;
    attachLogs(s, logs);
    toReady(s, t, 0);
    s.requestStates(1000);
    feed(s, resultFail(1, "denied"), 1100);
    uint32_t now = 1101;
    finishBuild(s, now);
    TEST_ASSERT_EQUAL_STRING("states_failed: denied", s.statesError().c_str());
    feed(s, statesResult(1, 3, kBig), 1200);
    TEST_ASSERT_EQUAL(1, countLog(logs, "states late result dropped"));
}

// ---- Timeouts ------------------------------------------------------------------------------

static void runStatesTimeoutNoRx(uint32_t base) {
    FakeTransport t;
    ha::HaSession s(t);
    toReady(s, t, base);
    s.requestStates(base + 1000);
    s.tick(base + 1000 + 14999);
    TEST_ASSERT_EQUAL(ha::StatesState::Fetching, s.statesState());
    s.tick(base + 1000 + 15000);
    TEST_ASSERT_EQUAL(ha::StatesState::Failed, s.statesState());
    TEST_ASSERT_EQUAL_STRING("states_timeout", s.statesError().c_str());
    TEST_ASSERT_EQUAL(S::Ready, s.state());
}

static void test_states_timeout_without_rx() {
    runStatesTimeoutNoRx(0);
    runStatesTimeoutNoRx(0xFFFFFFFFu - 5000u);  // across the uint32 wrap
}

static void test_pong_timeout_without_rx_as_step5() {
    FakeTransport t;
    ha::HaSession s(t);
    toReady(s, t, 0);
    s.tick(30000);
    s.tick(39999);
    TEST_ASSERT_EQUAL(S::Ready, s.state());
    s.tick(40000);
    TEST_ASSERT_EQUAL(S::Backoff, s.state());
    TEST_ASSERT_EQUAL_STRING("pong_timeout", s.lastError().c_str());
}

static void runStatesHardCap(uint32_t base) {
    FakeTransport t;
    ha::HaSession s(t);
    toReady(s, t, base);
    const uint32_t sent = base + 1000;
    s.requestStates(sent);
    size_t seen = t.sent.size();
    for (uint32_t d = 1000; d <= 89000; d += 1000) {
        const uint32_t now = sent + d;
        if (d % 5000 == 0) s.onRxProgress(100, now);  // a trickle every 5 s
        s.tick(now);
        answerPings(s, t, seen, now);
        TEST_ASSERT_EQUAL(ha::StatesState::Fetching, s.statesState());
        TEST_ASSERT_EQUAL(S::Ready, s.state());
    }
    s.onRxProgress(100, sent + 89500);
    s.tick(sent + 89999);
    TEST_ASSERT_EQUAL(ha::StatesState::Fetching, s.statesState());
    s.onRxProgress(100, sent + 90000);
    s.tick(sent + 90000);
    TEST_ASSERT_EQUAL(ha::StatesState::Failed, s.statesState());
    TEST_ASSERT_EQUAL_STRING("states_timeout", s.statesError().c_str());
    TEST_ASSERT_EQUAL(S::Ready, s.state());
}

static void test_states_hard_cap_with_trickle() {
    runStatesHardCap(0);
    runStatesHardCap(0xFFFFFFFFu - 40000u);
}

static void test_states_timeout_counts_from_later_of_sent_and_rx() {
    FakeTransport t;
    ha::HaSession s(t);
    toReady(s, t, 0);
    s.requestStates(1000);
    s.onRxProgress(10, 10000);
    s.tick(24999);
    TEST_ASSERT_EQUAL(ha::StatesState::Fetching, s.statesState());
    s.tick(25000);  // 15 s after the last byte
    TEST_ASSERT_EQUAL(ha::StatesState::Failed, s.statesState());
}

static void runPongWithRx(uint32_t base) {
    {
        // rx after the ping keeps it alive until 10 s after that rx
        FakeTransport t;
        ha::HaSession s(t);
        toReady(s, t, base);
        s.tick(base + 30000);  // ping
        s.onRxProgress(10, base + 35000);
        s.tick(base + 44999);
        TEST_ASSERT_EQUAL(S::Ready, s.state());
        s.tick(base + 45000);
        TEST_ASSERT_EQUAL(S::Backoff, s.state());
        TEST_ASSERT_EQUAL_STRING("pong_timeout", s.lastError().c_str());
    }
    {
        // rx before the ping does not extend: counts from the ping
        FakeTransport t;
        ha::HaSession s(t);
        toReady(s, t, base);
        s.onRxProgress(10, base + 25000);
        s.tick(base + 30000);
        s.tick(base + 39999);
        TEST_ASSERT_EQUAL(S::Ready, s.state());
        s.tick(base + 40000);
        TEST_ASSERT_EQUAL(S::Backoff, s.state());
    }
    {
        // repeated rx keeps it alive
        FakeTransport t;
        ha::HaSession s(t);
        toReady(s, t, base);
        s.tick(base + 30000);
        for (uint32_t d = 35000; d <= 80000; d += 5000) {
            s.onRxProgress(10, base + d);
            s.tick(base + d);
            TEST_ASSERT_EQUAL(S::Ready, s.state());
        }
        s.tick(base + 89999);
        TEST_ASSERT_EQUAL(S::Ready, s.state());
        s.tick(base + 90000);  // 10 s after the last rx
        TEST_ASSERT_EQUAL(S::Backoff, s.state());
    }
}

static void test_pong_timeout_counts_from_later_of_ping_and_rx() {
    runPongWithRx(0);
    runPongWithRx(0xFFFFFFFFu - 20000u);
}

static void runPongHardCap(uint32_t base) {
    {
        FakeTransport t;
        ha::HaSession s(t);
        toReady(s, t, base);
        for (uint32_t d = 1000; d <= 119000; d += 1000) {
            s.onRxProgress(10, base + d);
            s.tick(base + d);
            TEST_ASSERT_EQUAL(S::Ready, s.state());
        }
        s.onRxProgress(10, base + 119999);
        s.tick(base + 119999);  // ping at 30000 + 89999
        TEST_ASSERT_EQUAL(S::Ready, s.state());
        s.onRxProgress(10, base + 120000);
        s.tick(base + 120000);
        TEST_ASSERT_EQUAL(S::Backoff, s.state());
        TEST_ASSERT_EQUAL_STRING("pong_timeout", s.lastError().c_str());
    }
    {
        // a pong before the cap clears it (the next ping starts a new one)
        FakeTransport t;
        ha::HaSession s(t);
        toReady(s, t, base);
        for (uint32_t d = 1000; d <= 130000; d += 1000) {
            if (d == 100000) feed(s, "{\"id\":1,\"type\":\"pong\"}", base + d);
            s.onRxProgress(10, base + d);
            s.tick(base + d);
            TEST_ASSERT_EQUAL(S::Ready, s.state());
        }
        TEST_ASSERT_TRUE(t.sent.back().find("\"id\":2,\"type\":\"ping\"") != std::string::npos);
    }
}

static void test_pong_hard_cap() {
    runPongHardCap(0);
    runPongHardCap(0xFFFFFFFFu - 50000u);
}

static void test_rx_progress_ignored_cases() {
    FakeTransport t;
    ha::HaSession s(t);
    toReady(s, t, 0);
    s.tick(30000);
    s.onRxProgress(0, 39000);  // zero bytes: no liveness
    s.tick(40000);
    TEST_ASSERT_EQUAL(S::Backoff, s.state());
    // not active: ignored, and the next connection starts with its own last-rx time
    s.onRxProgress(5, 41000);
    s.tick(41000);  // reconnect (1 s backoff after 40000)
    TEST_ASSERT_EQUAL(S::Connecting, s.state());
    feed(s, kAuthRequired, 41000);
    feed(s, kAuthOk, 41000);
    s.tick(71000);  // ping
    s.tick(80999);
    TEST_ASSERT_EQUAL(S::Ready, s.state());
    s.tick(81000);
    TEST_ASSERT_EQUAL(S::Backoff, s.state());
    // NoConfig / AuthFailed ignore it as well
    FakeTransport t2;
    ha::HaSession s2(t2);
    s2.onRxProgress(5, 1);
    s2.setNetworkUp(true, 0);
    s2.configure(kUrl, "bad", ha::TlsSettings(), {}, 0);
    feed(s2, kAuthRequired, 1);
    feed(s2, kAuthInvalid, 2);
    s2.onRxProgress(5, 3);
    TEST_ASSERT_EQUAL(S::AuthFailed, s2.state());
}

static void test_rx_progress_during_connect_phase_does_not_extend_timeouts() {
    FakeTransport t;
    ha::HaSession s(t);
    s.setNetworkUp(true, 0);
    s.configure(kUrl, "tok", ha::TlsSettings(), {}, 0);
    s.onRxProgress(10, 15000);
    s.tick(20000);
    TEST_ASSERT_EQUAL(S::Backoff, s.state());
    TEST_ASSERT_EQUAL_STRING("connect_timeout", s.lastError().c_str());
}

// ---- Logs ----------------------------------------------------------------------------------

static void test_logs_never_contain_token() {
    FakeTransport t;
    ha::HaSession s(t);
    Logs logs;
    attachLogs(s, logs);
    const std::string token = "SECRETTOKEN123";
    s.setNetworkUp(true, 0);
    s.configure(kUrl, token, ha::TlsSettings(), {"light.a"}, 0);
    feed(s, kAuthRequired, 0);
    feed(s, kAuthOk, 0);
    feed(s, resultOk(1), 1);
    feed(s, eventA(1, {{"light.a", "on"}}), 2);
    feed(s, "{\"id\":1,\"type\":\"event\",\"event\":{\"a\":", 3);
    s.configure(kUrl, token, ha::TlsSettings(), {"light.b"}, 4);
    feed(s, resultFail(3, "denied"), 5);
    s.requestStates(6);
    s.onMessageDropped(5u << 20, "too_large", 7);
    s.requestStates(8);
    feed(s, statesResult(3, 2), 9);
    uint32_t now = 10;
    finishBuild(s, now);
    s.onClosed("lost", 11);
    s.tick(2000);
    feed(s, kAuthRequired, 2000);
    feed(s, kAuthInvalid, 2001);
    TEST_ASSERT_TRUE(logs.lines.size() > 5);
    for (const std::string &l : logs.lines) TEST_ASSERT_TRUE(l.find(token) == std::string::npos);
    TEST_ASSERT_TRUE(s.lastError().find(token) == std::string::npos);
    TEST_ASSERT_TRUE(s.statesError().find(token) == std::string::npos);
}

// ---- Step 6.5c: element limit per tick, envelope-first parse while Fetching -----------------

// Same events / pong with "id" not first (HA sends it first; other servers need not).
static std::string eventAIdLast(uint32_t id, const std::string &entity, const std::string &state) {
    return "{\"type\":\"event\",\"event\":{\"a\":{\"" + entity + "\":{\"s\":\"" + state +
           "\",\"a\":{}}}},\"id\":" + std::to_string(id) + "}";
}

static std::string eventCIdLast(uint32_t id, const std::string &entity, const std::string &state) {
    return "{\"event\":{\"c\":{\"" + entity + "\":{\"+\":{\"s\":\"" + state + "\"}}}},\"type\":\"event\",\"id\":" +
           std::to_string(id) + "}";
}

static int buildTicks(ha::HaSession &s, uint32_t now) {
    int ticks = 0;
    while (s.statesState() == ha::StatesState::Building && ticks < 100000) {
        s.tick(now);
        ++ticks;
    }
    return ticks;
}

static void test_default_element_limit_is_16_per_tick() {
    ha::SessionOptions def;
    TEST_ASSERT_EQUAL_UINT32(16, (uint32_t)def.statesElementsPerTick);
    struct Case { int n; int ticks; };
    const Case cases[] = {{40, 3}, {17, 2}, {100, 7}, {5, 1}};
    for (const Case &c : cases) {
        FakeTransport t;
        ha::HaSession s(t);  // default options
        toReady(s, t, 0);
        s.requestStates(100);
        const std::string r = statesResult(1, c.n);
        TEST_ASSERT_TRUE(r.size() < 32u * 1024u);  // the byte budget is not what limits the tick
        feed(s, r, 200);
        TEST_ASSERT_EQUAL(ha::StatesState::Building, s.statesState());
        TEST_ASSERT_EQUAL_INT(c.ticks, buildTicks(s, 300));
        expectCatalog(s, c.n);
        balanced(t);
    }
    // a larger limit builds the same catalog in fewer ticks
    ha::SessionOptions opt;
    opt.statesElementsPerTick = 1000;
    FakeTransport t;
    ha::HaSession s(t, nullptr, opt);
    toReady(s, t, 0);
    s.requestStates(100);
    feed(s, statesResult(1, 40), 200);
    TEST_ASSERT_EQUAL_INT(1, buildTicks(s, 300));
    expectCatalog(s, 40);
    // one element per tick: n + 1 ticks (the last one reaches the end), still complete
    ha::SessionOptions one;
    one.statesElementsPerTick = 1;
    FakeTransport t2;
    ha::HaSession s2(t2, nullptr, one);
    toReady(s2, t2, 0);
    s2.requestStates(100);
    feed(s2, statesResult(1, 6), 200);
    TEST_ASSERT_EQUAL_INT(7, buildTicks(s2, 300));
    expectCatalog(s2, 6);
}

static void test_fetching_own_event_id_not_first_applied() {
    FakeTransport t;
    ha::HaSession s(t);
    toReady(s, t, 0, {"light.a"});
    s.requestStates(10);
    TEST_ASSERT_EQUAL(ha::StatesState::Fetching, s.statesState());
    feed(s, eventAIdLast(1, "light.a", "on"), 11);
    TEST_ASSERT_EQUAL(1, (int)s.entities().size());
    TEST_ASSERT_EQUAL_STRING("on", s.entities().find("light.a")->state.c_str());
    feed(s, eventCIdLast(1, "light.a", "off"), 12);
    TEST_ASSERT_EQUAL_STRING("off", s.entities().find("light.a")->state.c_str());
    // another subscription's event with id last is still ignored
    feed(s, eventAIdLast(99, "light.z", "on"), 13);
    TEST_ASSERT_EQUAL(1, (int)s.entities().size());
    TEST_ASSERT_EQUAL(0, t.retainCalls);
    TEST_ASSERT_EQUAL(ha::StatesState::Fetching, s.statesState());
    TEST_ASSERT_EQUAL(S::Ready, s.state());
}

static void test_fetching_small_result_id_last_retained_and_built() {
    FakeTransport t;
    ha::HaSession s(t);
    Logs logs;
    attachLogs(s, logs);
    toReady(s, t, 0);
    s.requestStates(100);
    const std::string r = statesResult(1, 5, 0, true);  // "result" first, "id" last
    TEST_ASSERT_TRUE(r.size() < 64u * 1024u);
    TEST_ASSERT_TRUE(r.find("\"id\":1,") != std::string::npos);
    feed(s, r, 200);
    TEST_ASSERT_EQUAL(1, t.retainCalls);
    TEST_ASSERT_EQUAL(1, t.allocs);
    TEST_ASSERT_EQUAL(ha::StatesState::Building, s.statesState());
    TEST_ASSERT_TRUE(hasLog(logs, "states retained len=" + std::to_string(r.size())));
    uint32_t now = 201;
    finishBuild(s, now);
    expectCatalog(s, 5);
    balanced(t);
    // while a re-subscribe is pending too
    FakeTransport t2;
    ha::HaSession s2(t2);
    toReady(s2, t2, 0, {"light.a"});
    reconfigure(s2, {"light.b"}, 11);  // unsubscribe id 2, subscribe id 3
    s2.requestStates(12);              // get_states id 4
    feed(s2, statesResult(4, 3, 0, true), 13);
    TEST_ASSERT_EQUAL(1, t2.retainCalls);
    TEST_ASSERT_EQUAL(ha::StatesState::Building, s2.statesState());
    uint32_t now2 = 14;
    finishBuild(s2, now2);
    expectCatalog(s2, 3);
    balanced(t2);
}

static void test_fetching_other_or_stale_result_id_last_not_retained() {
    FakeTransport t;
    ha::HaSession s(t);
    toReady(s, t, 0);
    s.requestStates(100);
    feed(s, statesResult(999, 3, 0, true), 150);  // not our id, id last
    feed(s, statesResult(0, 3, 0, true), 151);
    TEST_ASSERT_EQUAL(0, t.retainCalls);
    TEST_ASSERT_EQUAL(ha::StatesState::Fetching, s.statesState());
    TEST_ASSERT_EQUAL(S::Ready, s.state());
    TEST_ASSERT_EQUAL_STRING("", s.statesError().c_str());
    // the real result still works afterwards
    feed(s, statesResult(1, 4, 0, true), 200);
    TEST_ASSERT_EQUAL(1, t.retainCalls);
    uint32_t now = 201;
    finishBuild(s, now);
    expectCatalog(s, 4);
    balanced(t);
}

static void test_fetching_failing_subscribe_result_id_last() {
    FakeTransport t;
    ha::HaSession s(t);
    toReady(s, t, 0, {"light.a"});
    reconfigure(s, {"light.b"}, 10);  // unsubscribe id 2, subscribe id 3
    feed(s, resultOk(2), 11);
    s.requestStates(12);              // get_states id 4
    TEST_ASSERT_EQUAL(ha::StatesState::Fetching, s.statesState());
    TEST_ASSERT_EQUAL_STRING(getStatesMsg(4).c_str(), t.sent.back().c_str());
    feed(s, "{\"type\":\"result\",\"success\":false,\"error\":{\"code\":\"x\",\"message\":\"nope\"},\"id\":3}", 13);
    TEST_ASSERT_EQUAL(0, t.retainCalls);
    TEST_ASSERT_EQUAL_STRING("subscribe_failed: nope", s.lastError().c_str());
    TEST_ASSERT_EQUAL(S::Ready, s.state());
    TEST_ASSERT_EQUAL(ha::StatesState::Fetching, s.statesState());  // fetch unaffected
    // a failing get_states result with id last fails the fetch (retained, builder reports it)
    feed(s, "{\"type\":\"result\",\"success\":false,\"error\":{\"code\":\"x\",\"message\":\"denied\"},\"id\":4}", 14);
    uint32_t now = 15;
    finishBuild(s, now);
    TEST_ASSERT_EQUAL(ha::StatesState::Failed, s.statesState());
    TEST_ASSERT_EQUAL_STRING("states_failed: denied", s.statesError().c_str());
    balanced(t);
}

static void test_fetching_pong_id_not_first_clears_ping() {
    FakeTransport t;
    ha::HaSession s(t);
    toReady(s, t, 0, {"light.a"});
    s.tick(30000);  // ping
    const uint32_t pingId = lastId(t);
    TEST_ASSERT_TRUE(t.sent.back().find("ping") != std::string::npos);
    s.requestStates(30001);
    feed(s, "{\"type\":\"pong\",\"id\":" + std::to_string(pingId) + "}", 30002);
    TEST_ASSERT_EQUAL(0, t.retainCalls);
    TEST_ASSERT_EQUAL(ha::StatesState::Fetching, s.statesState());
    s.onRxProgress(1, 40000);
    s.tick(40001);
    TEST_ASSERT_EQUAL(S::Ready, s.state());  // no pong_timeout: the pong was recognised
}

static void test_fetching_malformed_small_message_changes_nothing() {
    FakeTransport t;
    ha::HaSession s(t);
    Logs logs;
    attachLogs(s, logs);
    toReady(s, t, 0, {"light.a"});
    feed(s, eventA(1, {{"light.a", "on"}}), 1);
    s.requestStates(100);
    logs.lines.clear();
    const uint32_t cc = s.changeCount();
    const uint32_t sc = s.statesChangeCount();
    const size_t sent = t.sent.size();
    feed(s, "{\"type\":\"result\",\"success\":tr", 101);
    feed(s, "garbage", 102);
    feed(s, "{\"result\":[1,2,3", 103);
    feed(s, "[1,2]", 104);
    feed(s, "{\"type\":\"event\",\"event\":{\"a\":", 105);  // truncated event, no id
    TEST_ASSERT_EQUAL(0, t.retainCalls);
    TEST_ASSERT_EQUAL(ha::StatesState::Fetching, s.statesState());
    TEST_ASSERT_EQUAL(S::Ready, s.state());
    TEST_ASSERT_EQUAL_UINT32(cc, s.changeCount());
    TEST_ASSERT_EQUAL_UINT32(sc, s.statesChangeCount());
    TEST_ASSERT_EQUAL_STRING("", s.statesError().c_str());
    TEST_ASSERT_EQUAL((int)sent, (int)t.sent.size());
    TEST_ASSERT_EQUAL(0, (int)logs.lines.size());
    TEST_ASSERT_EQUAL(1, (int)s.entities().size());
    // and the fetch still completes
    feed(s, statesResult(2, 2), 200);  // subscribe id 1, get_states id 2
    uint32_t now = 201;
    finishBuild(s, now);
    expectCatalog(s, 2);
    balanced(t);
}

// ---- Step 6.8: msSinceSubscribe ------------------------------------------------------------

static void test_ms_since_subscribe_none_before_connect_and_authenticating() {
    FakeTransport t;
    ha::HaSession s(t);
    TEST_ASSERT_FALSE(s.msSinceSubscribe(0).has_value());
    s.setNetworkUp(true, 0);
    s.configure(kUrl, "tok", ha::TlsSettings(), {"light.a"}, 0);
    TEST_ASSERT_FALSE(s.msSinceSubscribe(10).has_value());  // Connecting
    feed(s, kAuthRequired, 20);
    TEST_ASSERT_EQUAL(S::Authenticating, s.state());
    TEST_ASSERT_FALSE(s.msSinceSubscribe(30).has_value());
}

static void test_ms_since_subscribe_pending_and_active() {
    FakeTransport t;
    ha::HaSession s(t);
    toAuthenticated(s, t, 1000, {"light.a"});  // subscribe sent at 1000, pending
    TEST_ASSERT_EQUAL(S::Subscribing, s.state());
    TEST_ASSERT_TRUE(s.msSinceSubscribe(1000).has_value());
    TEST_ASSERT_EQUAL_UINT32(0, *s.msSinceSubscribe(1000));
    TEST_ASSERT_EQUAL_UINT32(250, *s.msSinceSubscribe(1250));
    feed(s, resultOk(1), 1300);
    TEST_ASSERT_EQUAL(S::Ready, s.state());
    TEST_ASSERT_EQUAL_UINT32(2000, *s.msSinceSubscribe(3000));  // still counts from the send
}

static void test_ms_since_subscribe_restarts_on_resubscribe() {
    FakeTransport t;
    ha::HaSession s(t);
    toReady(s, t, 1000, {"light.a"});
    TEST_ASSERT_EQUAL_UINT32(4000, *s.msSinceSubscribe(5000));
    reconfigure(s, {"light.b"}, 5000);
    TEST_ASSERT_TRUE(s.msSinceSubscribe(5000).has_value());
    TEST_ASSERT_EQUAL_UINT32(0, *s.msSinceSubscribe(5000));
    TEST_ASSERT_EQUAL_UINT32(700, *s.msSinceSubscribe(5700));
}

static void test_ms_since_subscribe_none_after_failed_result() {
    FakeTransport t;
    ha::HaSession s(t);
    toAuthenticated(s, t, 0, {"light.a"});
    TEST_ASSERT_TRUE(s.msSinceSubscribe(5).has_value());
    feed(s, resultFail(1, "bad"), 10);
    TEST_ASSERT_EQUAL(S::Ready, s.state());
    TEST_ASSERT_FALSE(s.msSinceSubscribe(20).has_value());
}

static void test_ms_since_subscribe_none_after_disconnect_and_empty_ids() {
    FakeTransport t;
    ha::HaSession s(t);
    toReady(s, t, 0, {"light.a"});
    TEST_ASSERT_TRUE(s.msSinceSubscribe(10).has_value());
    reconfigure(s, {}, 20);
    TEST_ASSERT_FALSE(s.msSinceSubscribe(30).has_value());  // ids -> empty
    reconfigure(s, {"light.a"}, 40);
    TEST_ASSERT_TRUE(s.msSinceSubscribe(50).has_value());
    s.onClosed("gone", 60);
    TEST_ASSERT_FALSE(s.msSinceSubscribe(70).has_value());  // disconnected (Backoff)
}

static bool validUtf8(const std::string &s) {
    size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = (unsigned char)s[i];
        size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        if (n == 0 || i + n > s.size()) return false;
        for (size_t j = 1; j < n; ++j) {
            if (((unsigned char)s[i + j] & 0xC0) != 0x80) return false;
        }
        i += n;
    }
    return true;
}

static void test_last_error_cut_on_utf8_boundary() {
    FakeTransport t;
    ha::HaSession s(t);
    toAuthenticated(s, t, 0, {"light.a"});
    std::string message;
    for (int i = 0; i < 100; ++i) message += "\xC3\xB8";  // 200 bytes of 2-byte characters
    feed(s, "{\"id\":1,\"type\":\"result\",\"success\":false,\"error\":{\"code\":\"x\",\"message\":\"" + message + "\"}}", 10);
    TEST_ASSERT_TRUE(s.lastError().size() <= 120);
    TEST_ASSERT_TRUE(s.lastError().size() >= 100);
    TEST_ASSERT_TRUE(validUtf8(s.lastError()));
}

// ============================================================================================
// Step 7.3: call_service (plan-step7.md section 3.3)
// ============================================================================================

namespace {

using CO = ha::CallOutcome;

core::domain_actions::ServiceCall mkCall(const std::string &domain, const std::string &service,
                                         const std::string &entity, const std::string &dataJson = "") {
    core::domain_actions::ServiceCall c;
    c.domain = domain;
    c.service = service;
    c.entityId = entity;
    if (!dataJson.empty()) deserializeJson(c.data, dataJson);
    return c;
}

core::domain_actions::ServiceCall lightOn() {
    return mkCall("light", "turn_on", "light.a", "{\"brightness\":128}");
}

std::string callResultFail(uint32_t id, const std::string &code, const std::string &message) {
    return "{\"id\":" + std::to_string(id) + ",\"type\":\"result\",\"success\":false,\"error\":{\"code\":\"" + code +
           "\",\"message\":\"" + message + "\"}}";
}

bool anyLogContains(const Logs &logs, const std::string &needle) {
    for (const std::string &l : logs.lines) {
        if (l.find(needle) != std::string::npos) return true;
    }
    return false;
}

std::string lastLogWith(const Logs &logs, const std::string &prefix) {
    for (size_t i = logs.lines.size(); i > 0; --i) {
        if (logs.lines[i - 1].compare(0, prefix.size(), prefix) == 0) return logs.lines[i - 1];
    }
    return "";
}

}  // namespace

// 1
static void test_call_only_in_ready() {
    FakeTransport t;
    ha::HaSession s(t);
    // NoConfig
    TEST_ASSERT_EQUAL_UINT32(0, s.callService(lightOn(), 1, 0));
    s.setNetworkUp(true, 0);
    s.configure(kUrl, "tok", ha::TlsSettings(), {"light.a"}, 0);  // Connecting
    TEST_ASSERT_EQUAL(S::Connecting, s.state());
    TEST_ASSERT_EQUAL_UINT32(0, s.callService(lightOn(), 1, 1));
    feed(s, kAuthRequired, 2);  // Authenticating
    TEST_ASSERT_EQUAL(S::Authenticating, s.state());
    TEST_ASSERT_EQUAL_UINT32(0, s.callService(lightOn(), 1, 3));
    feed(s, kAuthOk, 4);  // Subscribing
    TEST_ASSERT_EQUAL(S::Subscribing, s.state());
    const size_t sent = t.sent.size();
    TEST_ASSERT_EQUAL_UINT32(0, s.callService(lightOn(), 1, 5));
    TEST_ASSERT_EQUAL((int)sent, (int)t.sent.size());
    feed(s, resultOk(1), 6);
    TEST_ASSERT_EQUAL(S::Ready, s.state());
    s.onClosed("x", 7);  // Backoff
    TEST_ASSERT_EQUAL(S::Backoff, s.state());
    const size_t sent2 = t.sent.size();
    TEST_ASSERT_EQUAL_UINT32(0, s.callService(lightOn(), 1, 8));
    TEST_ASSERT_EQUAL((int)sent2, (int)t.sent.size());
    TEST_ASSERT_EQUAL(0, (int)s.callsPending());
    TEST_ASSERT_TRUE(s.takeCallResults().empty());

    // AuthFailed
    FakeTransport t2;
    ha::HaSession s2(t2);
    s2.setNetworkUp(true, 0);
    s2.configure(kUrl, "bad", ha::TlsSettings(), {}, 0);
    feed(s2, kAuthRequired, 0);
    feed(s2, kAuthInvalid, 0);
    TEST_ASSERT_EQUAL(S::AuthFailed, s2.state());
    const size_t sent3 = t2.sent.size();
    TEST_ASSERT_EQUAL_UINT32(0, s2.callService(lightOn(), 1, 1));
    TEST_ASSERT_EQUAL((int)sent3, (int)t2.sent.size());
    TEST_ASSERT_TRUE(s2.takeCallResults().empty());
}

// 1
static void test_call_frames_in_ready() {
    FakeTransport t;
    ha::HaSession s(t);
    toReady(s, t, 0, {"light.a"});  // subscribe used id 1
    TEST_ASSERT_EQUAL_UINT32(2, s.callService(lightOn(), 7, 10));
    TEST_ASSERT_EQUAL_STRING(
        "{\"id\":2,\"type\":\"call_service\",\"domain\":\"light\",\"service\":\"turn_on\","
        "\"service_data\":{\"brightness\":128},\"target\":{\"entity_id\":\"light.a\"}}",
        t.sent.back().c_str());
    TEST_ASSERT_EQUAL(1, (int)s.callsPending());
    // without service_data
    TEST_ASSERT_EQUAL_UINT32(3, s.callService(mkCall("light", "turn_off", "light.a"), 8, 11));
    TEST_ASSERT_EQUAL_STRING(
        "{\"id\":3,\"type\":\"call_service\",\"domain\":\"light\",\"service\":\"turn_off\","
        "\"target\":{\"entity_id\":\"light.a\"}}",
        t.sent.back().c_str());
    // empty entity: no target
    TEST_ASSERT_EQUAL_UINT32(4, s.callService(mkCall("script", "reload", "", "{\"x\":1}"), 9, 12));
    TEST_ASSERT_TRUE(t.sent.back().find("target") == std::string::npos);
    TEST_ASSERT_TRUE(t.sent.back().find("\"service_data\":{\"x\":1}") != std::string::npos);
    TEST_ASSERT_EQUAL(3, (int)s.callsPending());
}

// 2, 17
static void test_call_success_rtt_and_log() {
    FakeTransport t;
    ha::HaSession s(t);
    Logs logs;
    attachLogs(s, logs);
    toReady(s, t, 0);
    const uint32_t id = s.callService(lightOn(), 42, 1000);
    TEST_ASSERT_EQUAL_UINT32(1, id);
    TEST_ASSERT_TRUE(hasLog(logs, "call id=1 light.turn_on entity=light.a data={\"brightness\":128}"));
    feed(s, resultOk(id), 1237);
    TEST_ASSERT_EQUAL(0, (int)s.callsPending());
    std::vector<ha::CallResult> r = s.takeCallResults();
    TEST_ASSERT_EQUAL(1, (int)r.size());
    TEST_ASSERT_EQUAL_UINT32(42, r[0].tag);
    TEST_ASSERT_EQUAL((int)CO::Ok, (int)r[0].outcome);
    TEST_ASSERT_EQUAL_UINT32(237, r[0].rttMs);
    TEST_ASSERT_EQUAL_STRING("", r[0].code.c_str());
    TEST_ASSERT_EQUAL_STRING("", r[0].message.c_str());
    TEST_ASSERT_TRUE(hasLog(logs, "call ok id=1 ms=237"));
    TEST_ASSERT_TRUE(s.takeCallResults().empty());
}

// 3
static void test_call_failure_variants() {
    FakeTransport t;
    ha::HaSession s(t);
    Logs logs;
    attachLogs(s, logs);
    toReady(s, t, 0);
    const uint32_t a = s.callService(lightOn(), 1, 100);
    const uint32_t b = s.callService(lightOn(), 2, 100);
    const uint32_t c = s.callService(lightOn(), 3, 100);
    const uint32_t d = s.callService(lightOn(), 4, 100);
    feed(s, callResultFail(a, "not_found", "Service not found."), 150);
    feed(s, "{\"id\":" + std::to_string(b) + ",\"type\":\"result\",\"success\":false}", 160);
    feed(s, "{\"id\":" + std::to_string(c) + ",\"type\":\"result\",\"result\":null}", 170);       // no success
    feed(s, "{\"id\":" + std::to_string(d) + ",\"type\":\"result\",\"success\":\"yes\"}", 180);  // not a bool
    std::vector<ha::CallResult> r = s.takeCallResults();
    TEST_ASSERT_EQUAL(4, (int)r.size());
    TEST_ASSERT_EQUAL((int)CO::Failed, (int)r[0].outcome);
    TEST_ASSERT_EQUAL_STRING("not_found", r[0].code.c_str());
    TEST_ASSERT_EQUAL_STRING("Service not found.", r[0].message.c_str());
    TEST_ASSERT_EQUAL_UINT32(50, r[0].rttMs);
    TEST_ASSERT_EQUAL((int)CO::Failed, (int)r[1].outcome);
    TEST_ASSERT_EQUAL_STRING("unknown", r[1].code.c_str());
    TEST_ASSERT_EQUAL_STRING("", r[1].message.c_str());
    TEST_ASSERT_EQUAL((int)CO::Failed, (int)r[2].outcome);
    TEST_ASSERT_EQUAL((int)CO::Failed, (int)r[3].outcome);
    TEST_ASSERT_TRUE(hasLog(logs, "call failed id=1 error=not_found: Service not found."));
    TEST_ASSERT_TRUE(hasLog(logs, "call failed id=2 error=unknown"));  // no ": "
    TEST_ASSERT_EQUAL(0, (int)s.callsPending());
}

// 4
static void test_call_log_cuts_and_control_chars() {
    FakeTransport t;
    ha::HaSession s(t);
    Logs logs;
    attachLogs(s, logs);
    toReady(s, t, 0);
    const uint32_t id = s.callService(lightOn(), 1, 0);
    std::string msg = "ab\\ncd\\te";  // JSON escapes -> control chars in the message
    msg += std::string(100, 'z');
    feed(s, callResultFail(id, "boom", msg), 5);
    const std::string expected = "ab?cd?e" + std::string(53, 'z');  // 60 bytes
    TEST_ASSERT_EQUAL_STRING(("call failed id=1 error=boom: " + expected).c_str(),
                             lastLogWith(logs, "call failed").c_str());
    std::vector<ha::CallResult> r = s.takeCallResults();
    TEST_ASSERT_EQUAL(1, (int)r.size());
    TEST_ASSERT_EQUAL(107, (int)r[0].message.size());  // full message
    TEST_ASSERT_EQUAL('\n', r[0].message[2]);

    // call line cuts: domain 32 / service 48 / entity 96 / data 96, no control chars
    logs.lines.clear();
    const std::string dom(40, 'd'), svc(60, 's'), ent(120, 'e');
    const std::string big = "{\"k\":\"" + std::string(200, 'v') + "\"}";
    const uint32_t id2 = s.callService(mkCall(dom, svc, ent, big), 2, 10);
    TEST_ASSERT_TRUE(id2 != 0);
    const std::string line = lastLogWith(logs, "call id=");
    const std::string expect = "call id=" + std::to_string(id2) + " " + std::string(32, 'd') + "." +
                               std::string(48, 's') + " entity=" + std::string(96, 'e') + " data=" +
                               big.substr(0, 96);
    TEST_ASSERT_EQUAL_STRING(expect.c_str(), line.c_str());
    // error code cut at 48
    feed(s, callResultFail(id2, std::string(70, 'c'), "m"), 11);
    TEST_ASSERT_EQUAL_STRING(("call failed id=" + std::to_string(id2) + " error=" + std::string(48, 'c') + ": m").c_str(),
                             lastLogWith(logs, "call failed").c_str());
    // no service_data -> data=null
    const uint32_t id3 = s.callService(mkCall("light", "turn_off", "light.a"), 3, 12);
    TEST_ASSERT_EQUAL_STRING(("call id=" + std::to_string(id3) + " light.turn_off entity=light.a data=null").c_str(),
                             lastLogWith(logs, "call id=").c_str());
}

// 5
static void test_call_limit_8_and_subscribe_still_works() {
    FakeTransport t;
    ha::HaSession s(t);
    toReady(s, t, 0, {"light.a"});
    for (int i = 0; i < 8; ++i) TEST_ASSERT_TRUE(s.callService(lightOn(), (uint32_t)i, 100) != 0);
    TEST_ASSERT_EQUAL(8, (int)s.callsPending());
    const size_t sent = t.sent.size();
    TEST_ASSERT_EQUAL_UINT32(0, s.callService(lightOn(), 99, 100));
    TEST_ASSERT_EQUAL((int)sent, (int)t.sent.size());
    TEST_ASSERT_EQUAL(8, (int)s.callsPending());
    // id-set change: unsubscribe + subscribe still go out and the session stays Ready
    reconfigure(s, {"light.b"}, 200);
    TEST_ASSERT_EQUAL((int)sent + 2, (int)t.sent.size());
    TEST_ASSERT_TRUE(t.sent[sent].find("unsubscribe_events") != std::string::npos);
    TEST_ASSERT_TRUE(t.sent[sent + 1].find("subscribe_entities") != std::string::npos);
    TEST_ASSERT_EQUAL(S::Ready, s.state());
    TEST_ASSERT_EQUAL(8, (int)s.callsPending());
    // the subscribe still expires at 15 s (the calls timed out at 10 s before)
    s.tick(200 + 9000);  // calls at 100 expire at 10100
    s.tick(10100);
    TEST_ASSERT_EQUAL(8, (int)s.takeCallResults().size());
    TEST_ASSERT_EQUAL(S::Ready, s.state());
    s.tick(200 + 14999);
    TEST_ASSERT_EQUAL(S::Ready, s.state());
    s.tick(200 + 15000);
    TEST_ASSERT_EQUAL(S::Backoff, s.state());
    TEST_ASSERT_EQUAL_STRING("subscribe_timeout", s.lastError().c_str());
}

// 6
static void test_call_refused_when_map_full_of_unsubscribes_calls_never_evicted() {
    FakeTransport t;
    ha::HaSession s(t);
    toReady(s, t, 0, {"light.0"});
    for (int i = 1; i <= 20; ++i) {
        reconfigure(s, {"light." + std::to_string(i)}, 1000);
        if (i < 20) feed(s, resultOk(lastId(t)), 1000);
    }
    TEST_ASSERT_EQUAL((int)ha::HaSession::kMaxPending, (int)s.pendingCount());
    const size_t sent = t.sent.size();
    TEST_ASSERT_EQUAL_UINT32(0, s.callService(lightOn(), 1, 1000));
    TEST_ASSERT_EQUAL((int)sent, (int)t.sent.size());
    TEST_ASSERT_EQUAL(0, (int)s.callsPending());
    TEST_ASSERT_TRUE(s.takeCallResults().empty());

    // with calls pending, subscribes evict unsubscribes and never a call
    FakeTransport t2;
    ha::HaSession s2(t2);
    toReady(s2, t2, 0, {"light.0"});
    for (int i = 0; i < 3; ++i) TEST_ASSERT_TRUE(s2.callService(lightOn(), (uint32_t)i, 10) != 0);
    for (int i = 1; i <= 20; ++i) {
        const size_t before = t2.sent.size();
        reconfigure(s2, {"light." + std::to_string(i)}, 20);
        TEST_ASSERT_EQUAL((int)before + 2, (int)t2.sent.size());
        TEST_ASSERT_EQUAL(3, (int)s2.callsPending());
        TEST_ASSERT_TRUE((int)s2.pendingCount() <= (int)ha::HaSession::kMaxPending);
        feed(s2, resultOk(lastId(t2)), 20);
    }
    TEST_ASSERT_TRUE(s2.takeCallResults().empty());
    s2.tick(10 + 10000);
    std::vector<ha::CallResult> r = s2.takeCallResults();
    TEST_ASSERT_EQUAL(3, (int)r.size());
    for (const ha::CallResult &x : r) TEST_ASSERT_EQUAL((int)CO::Timeout, (int)x.outcome);
}

// 7
static void test_call_timeout_exact_and_late_result_ignored() {
    FakeTransport t;
    ha::HaSession s(t);
    Logs logs;
    attachLogs(s, logs);
    toReady(s, t, 0);
    const uint32_t id = s.callService(lightOn(), 5, 1000);
    s.tick(1000 + 9999);
    TEST_ASSERT_TRUE(s.takeCallResults().empty());
    TEST_ASSERT_EQUAL(1, (int)s.callsPending());
    s.tick(1000 + 10000);
    std::vector<ha::CallResult> r = s.takeCallResults();
    TEST_ASSERT_EQUAL(1, (int)r.size());
    TEST_ASSERT_EQUAL_UINT32(5, r[0].tag);
    TEST_ASSERT_EQUAL((int)CO::Timeout, (int)r[0].outcome);
    TEST_ASSERT_EQUAL_STRING("timeout", r[0].code.c_str());
    TEST_ASSERT_EQUAL_UINT32(10000, r[0].rttMs);
    TEST_ASSERT_TRUE(hasLog(logs, "call timeout id=" + std::to_string(id)));
    TEST_ASSERT_EQUAL(0, (int)s.callsPending());
    s.tick(1000 + 10001);
    TEST_ASSERT_TRUE(s.takeCallResults().empty());
    // later result ignored
    feed(s, resultOk(id), 1000 + 10500);
    TEST_ASSERT_TRUE(s.takeCallResults().empty());
    TEST_ASSERT_EQUAL(S::Ready, s.state());
    TEST_ASSERT_EQUAL(1, countLog(logs, "call timeout"));
}

// 8
static void test_call_timeout_and_pong_timeout_same_tick() {
    FakeTransport t;
    ha::HaSession s(t);
    Logs logs;
    attachLogs(s, logs);
    toReady(s, t, 0);
    s.tick(30000);  // ping id 1
    const uint32_t a = s.callService(lightOn(), 1, 30000);
    const uint32_t b = s.callService(lightOn(), 2, 31000);
    TEST_ASSERT_TRUE(a != 0 && b != 0);
    s.tick(40000);  // call 1 expires and the pong is missing
    TEST_ASSERT_EQUAL(S::Backoff, s.state());
    TEST_ASSERT_EQUAL_STRING("pong_timeout", s.lastError().c_str());
    std::vector<ha::CallResult> r = s.takeCallResults();
    TEST_ASSERT_EQUAL(2, (int)r.size());
    TEST_ASSERT_EQUAL_UINT32(1, r[0].tag);
    TEST_ASSERT_EQUAL((int)CO::Timeout, (int)r[0].outcome);
    TEST_ASSERT_EQUAL_UINT32(2, r[1].tag);
    TEST_ASSERT_EQUAL((int)CO::ConnectionLost, (int)r[1].outcome);
    TEST_ASSERT_EQUAL(1, countLog(logs, "call timeout"));
    TEST_ASSERT_EQUAL(1, countLog(logs, "calls dropped"));
    TEST_ASSERT_TRUE(hasLog(logs, "calls dropped n=1 reason=connection_lost"));
}

static void test_call_timeout_and_subscribe_timeout_same_tick() {
    FakeTransport t;
    ha::HaSession s(t);
    Logs logs;
    attachLogs(s, logs);
    toReady(s, t, 0, {"light.a"});
    s.callService(lightOn(), 1, 100);            // id 2, expires 10100
    reconfigure(s, {"light.b"}, 101);            // unsub 3, sub 4 (expires 15101)
    s.callService(lightOn(), 2, 6000);           // id 5, not expired at 15101
    s.tick(15101);
    TEST_ASSERT_EQUAL(S::Backoff, s.state());
    TEST_ASSERT_EQUAL_STRING("subscribe_timeout", s.lastError().c_str());
    std::vector<ha::CallResult> r = s.takeCallResults();
    TEST_ASSERT_EQUAL(2, (int)r.size());
    TEST_ASSERT_EQUAL((int)CO::Timeout, (int)r[0].outcome);
    TEST_ASSERT_EQUAL((int)CO::ConnectionLost, (int)r[1].outcome);
    TEST_ASSERT_EQUAL(1, countLog(logs, "calls dropped n=1"));
    TEST_ASSERT_TRUE(s.takeCallResults().empty());
}

// 9
static void expectTwoLostOnce(ha::HaSession &s, uint32_t firstTag = 1) {
    std::vector<ha::CallResult> r = s.takeCallResults();
    TEST_ASSERT_EQUAL(2, (int)r.size());
    TEST_ASSERT_EQUAL_UINT32(firstTag, r[0].tag);
    TEST_ASSERT_EQUAL_UINT32(firstTag + 1, r[1].tag);
    for (const ha::CallResult &x : r) {
        TEST_ASSERT_EQUAL((int)CO::ConnectionLost, (int)x.outcome);
        TEST_ASSERT_EQUAL_STRING("connection_lost", x.code.c_str());
    }
    TEST_ASSERT_EQUAL(0, (int)s.callsPending());
    TEST_ASSERT_TRUE(s.takeCallResults().empty());
}

static void test_call_connection_lost_exactly_once_per_path() {
    const uint32_t kBase = 100;
    // onClosed, then the reconnect
    {
        FakeTransport t;
        ha::HaSession s(t);
        toReady(s, t, kBase);
        s.callService(lightOn(), 1, kBase + 1);
        s.callService(lightOn(), 2, kBase + 2);
        s.onClosed("x", kBase + 3);
        expectTwoLostOnce(s);
        s.tick(kBase + 3 + 1000);  // reconnect: connect() clears nothing more
        TEST_ASSERT_TRUE(s.takeCallResults().empty());
        s.onClosed("y", kBase + 5000);
        TEST_ASSERT_TRUE(s.takeCallResults().empty());
    }
    // pong timeout
    {
        FakeTransport t;
        ha::HaSession s(t);
        toReady(s, t, 0);
        s.tick(30000);
        s.callService(lightOn(), 1, 30500);
        s.callService(lightOn(), 2, 31000);
        s.tick(40000);
        TEST_ASSERT_EQUAL(S::Backoff, s.state());
        expectTwoLostOnce(s);
        s.tick(41000);
        TEST_ASSERT_TRUE(s.takeCallResults().empty());
    }
    // network down
    {
        FakeTransport t;
        ha::HaSession s(t);
        toReady(s, t, kBase);
        s.callService(lightOn(), 1, kBase + 1);
        s.callService(lightOn(), 2, kBase + 2);
        s.setNetworkUp(false, kBase + 3);
        expectTwoLostOnce(s);
        s.setNetworkUp(true, kBase + 10000);
        s.tick(kBase + 20000);
        TEST_ASSERT_TRUE(s.takeCallResults().empty());
    }
    // configure: new URL / token / TLS / empty URL
    for (int variant = 0; variant < 4; ++variant) {
        FakeTransport t;
        ha::HaSession s(t);
        Logs logs;
        attachLogs(s, logs);
        toReady(s, t, kBase);
        s.callService(lightOn(), 1, kBase + 1);
        s.callService(lightOn(), 2, kBase + 2);
        if (variant == 0) s.configure("http://192.168.1.11:8123", "tok", ha::TlsSettings(), {}, kBase + 3);
        if (variant == 1) s.configure(kUrl, "tok2", ha::TlsSettings(), {}, kBase + 3);
        if (variant == 2) {
            ha::TlsSettings tls;
            tls.insecure = true;
            s.configure(kUrl, "tok", tls, {}, kBase + 3);
        }
        if (variant == 3) s.configure("", "tok", ha::TlsSettings(), {}, kBase + 3);
        expectTwoLostOnce(s);
        TEST_ASSERT_EQUAL(1, countLog(logs, "calls dropped n=2"));
        feed(s, kAuthRequired, kBase + 4);  // harmless in NoConfig
        s.tick(kBase + 5000);
        TEST_ASSERT_TRUE(s.takeCallResults().empty());
    }
}

// 10
static void test_call_kept_when_only_entity_ids_change() {
    FakeTransport t;
    ha::HaSession s(t);
    toReady(s, t, 0, {"light.a"});
    const uint32_t id = s.callService(lightOn(), 3, 10);
    reconfigure(s, {"light.a", "light.b"}, 20);
    TEST_ASSERT_EQUAL(S::Ready, s.state());
    TEST_ASSERT_EQUAL(1, (int)s.callsPending());
    TEST_ASSERT_TRUE(s.takeCallResults().empty());
    feed(s, resultOk(id), 30);
    std::vector<ha::CallResult> r = s.takeCallResults();
    TEST_ASSERT_EQUAL(1, (int)r.size());
    TEST_ASSERT_EQUAL((int)CO::Ok, (int)r[0].outcome);
    TEST_ASSERT_EQUAL_UINT32(20, r[0].rttMs);
}

// 11
static void test_call_send_failure() {
    FakeTransport t;
    ha::HaSession s(t);
    toReady(s, t, 0);
    s.callService(lightOn(), 1, 10);
    s.callService(lightOn(), 2, 11);
    t.sendResult = false;
    TEST_ASSERT_EQUAL_UINT32(0, s.callService(lightOn(), 3, 12));
    TEST_ASSERT_EQUAL(S::Backoff, s.state());
    TEST_ASSERT_EQUAL_STRING("send_failed", s.lastError().c_str());
    std::vector<ha::CallResult> r = s.takeCallResults();
    TEST_ASSERT_EQUAL(2, (int)r.size());
    TEST_ASSERT_EQUAL_UINT32(1, r[0].tag);
    TEST_ASSERT_EQUAL_UINT32(2, r[1].tag);
    for (const ha::CallResult &x : r) TEST_ASSERT_EQUAL((int)CO::ConnectionLost, (int)x.outcome);
    TEST_ASSERT_EQUAL(0, (int)s.callsPending());
}

// 12
static void test_call_send_reports_close_then_true() {
    FakeTransport t;
    ha::HaSession s(t);
    toReady(s, t, 0);
    s.callService(lightOn(), 1, 10);
    t.onSend = [&]() {
        t.onSend = nullptr;
        s.onClosed("peer_closed", 12);
    };
    TEST_ASSERT_EQUAL_UINT32(0, s.callService(lightOn(), 2, 12));
    TEST_ASSERT_EQUAL(S::Backoff, s.state());
    std::vector<ha::CallResult> r = s.takeCallResults();
    TEST_ASSERT_EQUAL(1, (int)r.size());  // only the earlier call
    TEST_ASSERT_EQUAL_UINT32(1, r[0].tag);
    TEST_ASSERT_EQUAL((int)CO::ConnectionLost, (int)r[0].outcome);
    TEST_ASSERT_EQUAL(0, (int)s.callsPending());
    TEST_ASSERT_TRUE(s.takeCallResults().empty());
}

// 13
static void test_call_result_while_fetching() {
    FakeTransport t;
    ha::HaSession s(t);
    Logs logs;
    attachLogs(s, logs);
    toReady(s, t, 0);
    const uint32_t a = s.callService(lightOn(), 1, 10);  // id 1
    const uint32_t b = s.callService(lightOn(), 2, 10);  // id 2
    const uint32_t c = s.callService(lightOn(), 3, 10);  // id 3
    TEST_ASSERT_TRUE(s.requestStates(11));
    TEST_ASSERT_EQUAL(ha::StatesState::Fetching, s.statesState());
    feed(s, callResultFail(a, "not_found", "nope"), 20);  // id first
    feed(s, "{\"type\":\"result\",\"success\":false,\"error\":{\"code\":\"bad\",\"message\":\"worse\"},\"id\":" +
                std::to_string(b) + "}",
         21);  // id not first
    feed(s, "{\"success\":true,\"result\":null,\"type\":\"result\",\"id\":" + std::to_string(c) + "}", 22);
    TEST_ASSERT_EQUAL(0, t.retainCalls);
    TEST_ASSERT_EQUAL(ha::StatesState::Fetching, s.statesState());
    std::vector<ha::CallResult> r = s.takeCallResults();
    TEST_ASSERT_EQUAL(3, (int)r.size());
    TEST_ASSERT_EQUAL((int)CO::Failed, (int)r[0].outcome);
    TEST_ASSERT_EQUAL_STRING("not_found", r[0].code.c_str());
    TEST_ASSERT_EQUAL_STRING("nope", r[0].message.c_str());
    TEST_ASSERT_EQUAL((int)CO::Failed, (int)r[1].outcome);
    TEST_ASSERT_EQUAL_STRING("bad", r[1].code.c_str());
    TEST_ASSERT_EQUAL_STRING("worse", r[1].message.c_str());
    TEST_ASSERT_EQUAL((int)CO::Ok, (int)r[2].outcome);
    TEST_ASSERT_EQUAL(S::Ready, s.state());
    balanced(t);
}

// 14
static void test_call_results_not_confused_with_other_ids() {
    FakeTransport t;
    ha::HaSession s(t);
    toReady(s, t, 0, {"light.a"});       // subscribe id 1
    const uint32_t call1 = s.callService(lightOn(), 10, 5);  // 2
    reconfigure(s, {"light.b"}, 6);      // unsubscribe 3, subscribe 4
    s.requestStates(7);                  // get_states 5
    const uint32_t call2 = s.callService(lightOn(), 11, 8);  // 6
    TEST_ASSERT_EQUAL_UINT32(2, call1);
    TEST_ASSERT_EQUAL_UINT32(6, call2);
    feed(s, resultOk(3), 9);   // unsubscribe
    feed(s, resultOk(4), 10);  // subscribe
    TEST_ASSERT_TRUE(s.takeCallResults().empty());
    TEST_ASSERT_EQUAL(2, (int)s.callsPending());
    TEST_ASSERT_EQUAL(ha::StatesState::Fetching, s.statesState());
    feed(s, callResultFail(6, "x", "y"), 11);
    std::vector<ha::CallResult> r = s.takeCallResults();
    TEST_ASSERT_EQUAL(1, (int)r.size());
    TEST_ASSERT_EQUAL_UINT32(11, r[0].tag);
    feed(s, statesResult(5, 2), 12);  // get_states result
    TEST_ASSERT_TRUE(s.takeCallResults().empty());
    TEST_ASSERT_EQUAL(1, (int)s.callsPending());
    feed(s, resultOk(2), 13);
    r = s.takeCallResults();
    TEST_ASSERT_EQUAL(1, (int)r.size());
    TEST_ASSERT_EQUAL_UINT32(10, r[0].tag);
    TEST_ASSERT_EQUAL((int)CO::Ok, (int)r[0].outcome);
    // a failed call result does not touch lastError (subscribe_failed only for subscribes)
    TEST_ASSERT_TRUE(s.lastError().find("subscribe_failed") == std::string::npos);
    TEST_ASSERT_EQUAL(S::Ready, s.state());
}

// 15
static void test_call_millis_wrap() {
    FakeTransport t;
    ha::HaSession s(t);
    toReady(s, t, 0xFFFFF000u - 10);
    const uint32_t id = s.callService(lightOn(), 1, 0xFFFFF000u);
    s.tick(0x00000800u);  // 6144 ms later: no timeout
    TEST_ASSERT_TRUE(s.takeCallResults().empty());
    feed(s, resultOk(id), 0x00000800u);
    std::vector<ha::CallResult> r = s.takeCallResults();
    TEST_ASSERT_EQUAL(1, (int)r.size());
    TEST_ASSERT_EQUAL((int)CO::Ok, (int)r[0].outcome);
    TEST_ASSERT_EQUAL_UINT32(0x1800u, r[0].rttMs);
    // timeout across the wrap
    const uint32_t id2 = s.callService(lightOn(), 2, 0xFFFFF000u + 100);
    TEST_ASSERT_TRUE(id2 != 0);
    s.tick(0xFFFFF000u + 100 + 9999);
    TEST_ASSERT_TRUE(s.takeCallResults().empty());
    s.tick(0xFFFFF000u + 100 + 10000);
    r = s.takeCallResults();
    TEST_ASSERT_EQUAL(1, (int)r.size());
    TEST_ASSERT_EQUAL((int)CO::Timeout, (int)r[0].outcome);
    TEST_ASSERT_EQUAL_UINT32(10000, r[0].rttMs);
}

// 16
static void test_call_clock_going_backwards() {
    FakeTransport t;
    ha::HaSession s(t);
    toReady(s, t, 1000);
    const uint32_t id = s.callService(lightOn(), 1, 1000);
    s.tick(500);
    TEST_ASSERT_TRUE(s.takeCallResults().empty());
    TEST_ASSERT_EQUAL(1, (int)s.callsPending());
    feed(s, resultOk(id), 900);
    std::vector<ha::CallResult> r = s.takeCallResults();
    TEST_ASSERT_EQUAL(1, (int)r.size());
    TEST_ASSERT_EQUAL((int)CO::Ok, (int)r[0].outcome);
    TEST_ASSERT_EQUAL_UINT32(0, r[0].rttMs);

    // subscribe expiry unchanged otherwise
    FakeTransport t2;
    ha::HaSession s2(t2);
    toReady(s2, t2, 1000, {"light.a"});
    reconfigure(s2, {"light.b"}, 2000);
    s2.tick(2000 + 14999);
    TEST_ASSERT_EQUAL(S::Ready, s2.state());
    s2.tick(2000 + 15000);
    TEST_ASSERT_EQUAL(S::Backoff, s2.state());
    TEST_ASSERT_EQUAL_STRING("subscribe_timeout", s2.lastError().c_str());
}

// 17
static void test_call_results_oldest_first() {
    FakeTransport t;
    ha::HaSession s(t);
    toReady(s, t, 0);
    const uint32_t a = s.callService(lightOn(), 10, 0);
    const uint32_t b = s.callService(lightOn(), 20, 0);
    const uint32_t c = s.callService(lightOn(), 30, 0);
    feed(s, resultOk(b), 5);
    feed(s, resultOk(c), 6);
    feed(s, resultOk(a), 7);
    std::vector<ha::CallResult> r = s.takeCallResults();
    TEST_ASSERT_EQUAL(3, (int)r.size());
    TEST_ASSERT_EQUAL_UINT32(20, r[0].tag);
    TEST_ASSERT_EQUAL_UINT32(30, r[1].tag);
    TEST_ASSERT_EQUAL_UINT32(10, r[2].tag);
    TEST_ASSERT_TRUE(s.takeCallResults().empty());
}

// 18
static void test_call_logs_never_contain_token() {
    FakeTransport t;
    ha::HaSession s(t);
    Logs logs;
    attachLogs(s, logs);
    s.setNetworkUp(true, 0);
    s.configure(kUrl, "SECRETTOKEN123", ha::TlsSettings(), {}, 0);
    feed(s, kAuthRequired, 0);
    feed(s, kAuthOk, 0);
    const uint32_t a = s.callService(lightOn(), 1, 1);
    const uint32_t b = s.callService(lightOn(), 2, 1);
    const uint32_t c = s.callService(lightOn(), 3, 1);
    feed(s, resultOk(a), 2);
    feed(s, callResultFail(b, "bad", "msg"), 3);
    s.tick(20000);  // c times out
    s.callService(lightOn(), 4, 20001);
    s.onClosed("x", 20002);
    TEST_ASSERT_TRUE(c != 0);
    TEST_ASSERT_TRUE(logs.lines.size() >= 6);
    TEST_ASSERT_FALSE(anyLogContains(logs, "SECRETTOKEN123"));
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
    RUN_TEST(test_resub_ready_new_ids);
    RUN_TEST(test_resub_ids_to_empty_from_ready);
    RUN_TEST(test_resub_ids_to_empty_from_subscribing);
    RUN_TEST(test_resub_ids_to_empty_clears_subscribe_failed);
    RUN_TEST(test_resub_same_set_other_order_does_nothing);
    RUN_TEST(test_resub_while_subscribing);
    RUN_TEST(test_resub_in_connecting_authenticating_backoff);
    RUN_TEST(test_resub_full_pending_map_evicts_oldest_unsubscribe);
    RUN_TEST(test_resub_failure_in_ready);
    RUN_TEST(test_resub_subscribe_failure_clears_store);
    RUN_TEST(test_resub_expiry_in_ready);
    RUN_TEST(test_unsubscribe_expiry_and_failure_only_log);
    RUN_TEST(test_send_failure_during_unsubscribe);
    RUN_TEST(test_send_failure_during_subscribe_of_resubscribe);
    RUN_TEST(test_send_failure_reporting_close_during_resubscribe_counts_once);
    RUN_TEST(test_event_stale_and_zero_id_ignored);
    RUN_TEST(test_event_too_large_logged_store_unchanged);
    RUN_TEST(test_event_unparsable_logged_only_for_own_event);
    RUN_TEST(test_snapshot_log_once_per_subscription);
    RUN_TEST(test_store_cleared_on_connect_kept_in_backoff);
    RUN_TEST(test_request_states_false_outside_ready);
    RUN_TEST(test_request_states_coalesces_and_restarts);
    RUN_TEST(test_request_states_send_failure_inside_onclosed);
    RUN_TEST(test_request_states_plain_send_failure);
    RUN_TEST(test_retain_id_first_result_builds_catalog);
    RUN_TEST(test_retain_small_result_first);
    RUN_TEST(test_retain_big_result_first_inconclusive_peek);
    RUN_TEST(test_retain_big_id_first_result);
    RUN_TEST(test_fetching_other_message_handled_normally);
    RUN_TEST(test_retain_fails_gives_states_no_memory);
    RUN_TEST(test_default_transport_retain_is_empty);
    RUN_TEST(test_build_budget_does_not_change_catalog);
    RUN_TEST(test_states_result_failure_and_wrong_id);
    RUN_TEST(test_build_survives_close_network_loss_reconnect);
    RUN_TEST(test_config_change_mid_build_drops_everything);
    RUN_TEST(test_destroying_session_mid_build_frees_text_once);
    RUN_TEST(test_failed_fetch_drops_untaken_catalog);
    RUN_TEST(test_drops_while_fetching);
    RUN_TEST(test_connection_lost_while_fetching);
    RUN_TEST(test_late_big_result_after_timeout_dropped);
    RUN_TEST(test_late_small_result_parsed_normally);
    RUN_TEST(test_late_result_with_new_fetch);
    RUN_TEST(test_late_result_not_across_connections);
    RUN_TEST(test_late_result_after_too_large_and_no_memory);
    RUN_TEST(test_late_result_after_builder_failure);
    RUN_TEST(test_states_timeout_without_rx);
    RUN_TEST(test_pong_timeout_without_rx_as_step5);
    RUN_TEST(test_states_hard_cap_with_trickle);
    RUN_TEST(test_states_timeout_counts_from_later_of_sent_and_rx);
    RUN_TEST(test_pong_timeout_counts_from_later_of_ping_and_rx);
    RUN_TEST(test_pong_hard_cap);
    RUN_TEST(test_rx_progress_ignored_cases);
    RUN_TEST(test_rx_progress_during_connect_phase_does_not_extend_timeouts);
    RUN_TEST(test_logs_never_contain_token);
    RUN_TEST(test_default_element_limit_is_16_per_tick);
    RUN_TEST(test_fetching_own_event_id_not_first_applied);
    RUN_TEST(test_fetching_small_result_id_last_retained_and_built);
    RUN_TEST(test_fetching_other_or_stale_result_id_last_not_retained);
    RUN_TEST(test_fetching_failing_subscribe_result_id_last);
    RUN_TEST(test_fetching_pong_id_not_first_clears_ping);
    RUN_TEST(test_fetching_malformed_small_message_changes_nothing);
    RUN_TEST(test_ms_since_subscribe_none_before_connect_and_authenticating);
    RUN_TEST(test_ms_since_subscribe_pending_and_active);
    RUN_TEST(test_ms_since_subscribe_restarts_on_resubscribe);
    RUN_TEST(test_ms_since_subscribe_none_after_failed_result);
    RUN_TEST(test_ms_since_subscribe_none_after_disconnect_and_empty_ids);
    RUN_TEST(test_last_error_cut_on_utf8_boundary);
    RUN_TEST(test_call_only_in_ready);
    RUN_TEST(test_call_frames_in_ready);
    RUN_TEST(test_call_success_rtt_and_log);
    RUN_TEST(test_call_failure_variants);
    RUN_TEST(test_call_log_cuts_and_control_chars);
    RUN_TEST(test_call_limit_8_and_subscribe_still_works);
    RUN_TEST(test_call_refused_when_map_full_of_unsubscribes_calls_never_evicted);
    RUN_TEST(test_call_timeout_exact_and_late_result_ignored);
    RUN_TEST(test_call_timeout_and_pong_timeout_same_tick);
    RUN_TEST(test_call_timeout_and_subscribe_timeout_same_tick);
    RUN_TEST(test_call_connection_lost_exactly_once_per_path);
    RUN_TEST(test_call_kept_when_only_entity_ids_change);
    RUN_TEST(test_call_send_failure);
    RUN_TEST(test_call_send_reports_close_then_true);
    RUN_TEST(test_call_result_while_fetching);
    RUN_TEST(test_call_results_not_confused_with_other_ids);
    RUN_TEST(test_call_millis_wrap);
    RUN_TEST(test_call_clock_going_backwards);
    RUN_TEST(test_call_results_oldest_first);
    RUN_TEST(test_call_logs_never_contain_token);
    return UNITY_END();
}
