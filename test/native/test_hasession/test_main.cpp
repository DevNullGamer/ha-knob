// Native tests for HaSession (plan.md section 3.7): fake transport + fake clock.
#include <unity.h>

#include <ArduinoJson.h>

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
    bool sendResult = true;
    std::function<void()> onSend;  // runs inside sendText (before returning)

    void open(const ha::HaUrl &endpoint) override {
        ++opens;
        lastEndpoint = endpoint;
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
    s.configure(kUrl, "tok", ids, now);
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
    s.configure("", "", {}, 0);
    TEST_ASSERT_EQUAL(S::NoConfig, s.state());
    s.configure(kUrl, "", {}, 0);  // token missing
    TEST_ASSERT_EQUAL(S::NoConfig, s.state());
    s.configure("", "tok", {}, 0);  // url missing
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
    s.configure(kUrl, "tok", {}, 0);
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
    s.configure(kUrl, "bad", {"light.a"}, 0);
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
    s.configure(kUrl, "bad", {"light.a"}, 60);
    s.configure(kUrl, "bad", {"light.a", "light.b"}, 61);  // entity change only
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
    s.configure(kUrl, "bad", {}, 0);
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
    s.configure(kUrl, "tok", {}, 0);
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
    s.configure(kUrl, "bad", {}, 0);
    feed(s, kAuthRequired, 0);
    feed(s, kAuthInvalid, 0);
    TEST_ASSERT_EQUAL(S::AuthFailed, s.state());
    s.configure(kUrl, "good", {}, 50);
    TEST_ASSERT_EQUAL(S::Connecting, s.state());
    TEST_ASSERT_EQUAL(2, t.opens);
    feed(s, kAuthRequired, 51);
    TEST_ASSERT_EQUAL_STRING("{\"type\":\"auth\",\"access_token\":\"good\"}", t.sent.back().c_str());
}

static void test_new_url_leaves_auth_failed() {
    FakeTransport t;
    ha::HaSession s(t);
    s.setNetworkUp(true, 0);
    s.configure(kUrl, "bad", {}, 0);
    feed(s, kAuthRequired, 0);
    feed(s, kAuthInvalid, 0);
    s.configure("http://192.168.1.11:8123", "bad", {}, 50);
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
    s.configure(kUrl, "tok", {}, 0);
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
    s.configure(kUrl, "tok", {}, 0);
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
    s.configure(kUrl, "tok", {}, 0);
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

static void test_https_not_supported() {
    FakeTransport t;
    ha::HaSession s(t);
    s.setNetworkUp(true, 0);
    s.configure("https://x.ui.nabu.casa", "tok", {}, 0);
    TEST_ASSERT_EQUAL(S::NoConfig, s.state());
    TEST_ASSERT_EQUAL_STRING("tls_not_supported", s.lastError().c_str());
    s.setNetworkUp(false, 10);
    s.setNetworkUp(true, 20);
    s.tick(100000);
    TEST_ASSERT_FALSE(s.retry(100001));
    s.tick(200000);
    TEST_ASSERT_EQUAL(0, t.opens);
    TEST_ASSERT_EQUAL(0, (int)t.sent.size());
    TEST_ASSERT_EQUAL(S::NoConfig, s.state());
    // switching to http connects
    s.configure(kUrl, "tok", {}, 300000);
    TEST_ASSERT_EQUAL(S::Connecting, s.state());
    TEST_ASSERT_EQUAL(1, t.opens);
    TEST_ASSERT_EQUAL_STRING("", s.lastError().c_str());
}

static void test_https_while_network_down_no_open() {
    FakeTransport t;
    ha::HaSession s(t);
    s.configure("https://h", "tok", {}, 0);
    s.setNetworkUp(true, 5);
    s.tick(10);
    TEST_ASSERT_EQUAL(0, t.opens);
    TEST_ASSERT_EQUAL_STRING("tls_not_supported", s.lastError().c_str());
}

static void test_query_fragment_url_invalid() {
    FakeTransport t;
    ha::HaSession s(t);
    s.setNetworkUp(true, 0);
    s.configure("http://h/x?y", "tok", {}, 0);
    TEST_ASSERT_EQUAL(S::NoConfig, s.state());
    TEST_ASSERT_EQUAL_STRING("invalid_url", s.lastError().c_str());
    s.configure("http://h/#a", "tok", {}, 1);
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
    s.configure("ftp://h", "tok", {}, 0);
    TEST_ASSERT_EQUAL(S::NoConfig, s.state());
    TEST_ASSERT_EQUAL_STRING("invalid_url", s.lastError().c_str());
    TEST_ASSERT_EQUAL(0, t.opens);
}

static void test_send_failure_reporting_close() {
    FakeTransport t;
    ha::HaSession s(t);
    s.setNetworkUp(true, 0);
    s.configure(kUrl, "tok", {}, 0);
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
    s.configure(kUrl, "tok", {}, 0);
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
    s.configure(kUrl, "tok", {}, 0);
    s.setNetworkUp(false, 10);
    TEST_ASSERT_EQUAL(S::Backoff, s.state());
}

static void test_configure_while_network_down_waits() {
    FakeTransport t;
    ha::HaSession s(t);
    s.configure(kUrl, "tok", {}, 0);
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
    s.configure(kUrl, "other", {}, 100);
    TEST_ASSERT_EQUAL(S::Connecting, s.state());
    TEST_ASSERT_EQUAL(2, t.opens);
    TEST_ASSERT_EQUAL_STRING("", s.haVersion().c_str());
    s.configure("", "", {}, 200);
    TEST_ASSERT_EQUAL(S::NoConfig, s.state());
    TEST_ASSERT_EQUAL(2, t.opens);
}

static void test_close_event_after_local_close_ignored() {
    FakeTransport t;
    ha::HaSession s(t);
    toAuthenticated(s, t, 0);
    s.configure("", "", {}, 10);
    s.onClosed("late", 11);
    TEST_ASSERT_EQUAL(S::NoConfig, s.state());
    TEST_ASSERT_EQUAL(0, (int)s.failures());
}

static void test_error_cut_to_max_len() {
    FakeTransport t;
    ha::HaSession s(t);
    s.setNetworkUp(true, 0);
    s.configure(kUrl, "tok", {}, 0);
    s.onClosed(std::string(500, 'x'), 1);
    TEST_ASSERT_EQUAL((int)ha::HaSession::kMaxErrorLen, (int)s.lastError().size());
}

static void test_token_not_in_status() {
    FakeTransport t;
    ha::HaSession s(t);
    s.setNetworkUp(true, 0);
    s.configure(kUrl, "SECRETTOKEN123", {}, 0);
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
    s.configure(kUrl, "tok", {}, 0);
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
    RUN_TEST(test_https_not_supported);
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
    return UNITY_END();
}
