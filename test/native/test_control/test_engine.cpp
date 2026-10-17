// Native tests for ControlEngine (plan-step7.md section 3.4, Step 7.4, Amendment A1,
// DESIGN-NOTES "Step 7.4"; test points in chg001/testpoints-7.4.md).
// Called from test_main.cpp (run_engine_tests()).
#include <unity.h>

#include <ArduinoJson.h>

#include <cmath>
#include <set>
#include <string>
#include <vector>

#include "control/ControlEngine.h"
#include "ha/EntityAttrs.h"
#include "ha/EntityStore.h"
#include "ha/HaProtocol.h"
#include "ha/HaSession.h"

namespace ctl = core::control;
namespace da = core::domain_actions;
namespace ha = core::ha;
namespace cfg = core::config;
using NK = ctl::NoticeKind;
using CO = ha::CallOutcome;

namespace {

struct Sent {
    std::string domain, service, entity, data;
    uint32_t tag;
};

struct FakeSender : ctl::CallSender {
    std::vector<Sent> sent;
    bool accept = true;
    bool send(const da::ServiceCall &c, uint32_t tag) override {
        if (!accept) return false;
        Sent s{c.domain, c.service, c.entityId, "", tag};
        serializeJson(c.data, s.data);
        sent.push_back(s);
        return true;
    }
};

ha::EntityState ent(const char *state, const char *attrsJson = "{}") {
    ha::EntityState e;
    e.state = state;
    JsonDocument doc;
    TEST_ASSERT_TRUE(!deserializeJson(doc, attrsJson));
    ha::applyAttrs(doc.as<JsonObjectConst>(), e.attrs, ha::AttrMode::Replace);
    return e;
}

cfg::Control mkControl(const char *entity, const char *action, double step, const char *press) {
    cfg::Control c;
    c.entityId = entity;
    c.action = action;
    c.step = step;
    c.press = press;
    return c;
}

cfg::Control lightCtl(const char *entity = "light.a") {
    return mkControl(entity, "brightness", 5, "toggle");
}

ctl::EngineOptions opts(uint32_t interval = 250, uint32_t settle = 1500, size_t cap = 8,
                        uint32_t repeat = 2000) {
    ctl::EngineOptions o;
    o.sendIntervalMs = interval;
    o.settleMs = settle;
    o.maxInFlight = cap;
    o.noticeRepeatMs = repeat;
    return o;
}

ha::CallResult res(uint32_t tag, CO outcome, const std::string &code = "",
                   const std::string &message = "") {
    ha::CallResult r;
    r.tag = tag;
    r.outcome = outcome;
    r.code = code;
    r.message = message;
    return r;
}

struct Rig {
    FakeSender sender;
    ctl::ControlEngine eng;
    ha::EntityState on50 = ent("on", "{\"brightness\":128}");  // 50 %
    explicit Rig(const ctl::EngineOptions &o = opts(), bool oneLight = true) : eng(sender, o) {
        if (oneLight) eng.setControls({lightCtl()});
    }
    uint32_t tag(size_t i) const { return sender.sent.at(i).tag; }
    void ok(size_t i, uint32_t now) { eng.onResult(res(tag(i), CO::Ok), now); }
};

void expectNotice(const std::optional<ctl::Notice> &n, NK kind, const char *text, size_t control) {
    TEST_ASSERT_TRUE(n.has_value());
    TEST_ASSERT_EQUAL((int)kind, (int)n->kind);
    TEST_ASSERT_EQUAL_STRING(text, n->text.c_str());
    TEST_ASSERT_EQUAL((int)control, (int)n->control);
}

}  // namespace

// --- encoder: send, throttle, hold ---------------------------------------------------

static void test_eng_notice_kind_names() {
    TEST_ASSERT_EQUAL_STRING("ha_offline", ctl::noticeKindName(NK::HaOffline));
    TEST_ASSERT_EQUAL_STRING("not_found", ctl::noticeKindName(NK::NotFound));
    TEST_ASSERT_EQUAL_STRING("unavailable", ctl::noticeKindName(NK::Unavailable));
    TEST_ASSERT_EQUAL_STRING("no_value", ctl::noticeKindName(NK::NoValue));
    TEST_ASSERT_EQUAL_STRING("busy", ctl::noticeKindName(NK::Busy));
    TEST_ASSERT_EQUAL_STRING("failed", ctl::noticeKindName(NK::Failed));
    TEST_ASSERT_EQUAL_STRING("timeout", ctl::noticeKindName(NK::Timeout));
}

static void test_eng_rotate_sends_at_once() {
    Rig r;
    r.eng.rotate(0, 3, &r.on50, true, 1000);
    TEST_ASSERT_EQUAL(1, (int)r.sender.sent.size());
    TEST_ASSERT_EQUAL_STRING("light", r.sender.sent[0].domain.c_str());
    TEST_ASSERT_EQUAL_STRING("turn_on", r.sender.sent[0].service.c_str());
    TEST_ASSERT_EQUAL_STRING("light.a", r.sender.sent[0].entity.c_str());
    TEST_ASSERT_EQUAL_STRING("{\"brightness_pct\":65}", r.sender.sent[0].data.c_str());
    TEST_ASSERT_TRUE(r.sender.sent[0].tag != 0);
    TEST_ASSERT_EQUAL(1, (int)r.eng.inFlight());
    auto l = r.eng.local(0, 1000);
    TEST_ASSERT_TRUE(l.has_value());
    TEST_ASSERT_TRUE(std::fabs((65.0) - (l->value)) < 1e-9);
    TEST_ASSERT_TRUE(l->unconfirmed);
    TEST_ASSERT_FALSE(r.eng.takeNotice().has_value());
}

static void test_eng_off_light_starts_from_zero() {
    Rig r;
    ha::EntityState off = ent("off");
    r.eng.rotate(0, 3, &off, true, 1000);
    TEST_ASSERT_EQUAL_STRING("{\"brightness_pct\":15}", r.sender.sent.at(0).data.c_str());
}

static void test_eng_zero_brightness_turns_off() {
    Rig r;
    r.eng.rotate(0, -100, &r.on50, true, 1000);
    TEST_ASSERT_EQUAL_STRING("turn_off", r.sender.sent.at(0).service.c_str());
    TEST_ASSERT_EQUAL_STRING("null", r.sender.sent.at(0).data.c_str());
}

static void test_eng_trailing_value_after_interval() {
    Rig r;
    r.eng.rotate(0, 3, &r.on50, true, 1000);  // 65, leading edge
    r.eng.tick(true, 1000);
    const uint32_t at[] = {1050, 1100, 1150, 1200, 1249};
    for (uint32_t t : at) r.eng.rotate(0, 1, &r.on50, true, t);  // base = held target
    TEST_ASSERT_EQUAL(1, (int)r.sender.sent.size());
    r.eng.tick(true, 1249);
    TEST_ASSERT_EQUAL(1, (int)r.sender.sent.size());
    r.eng.tick(true, 1250);
    TEST_ASSERT_EQUAL(2, (int)r.sender.sent.size());
    TEST_ASSERT_EQUAL_STRING("{\"brightness_pct\":90}", r.sender.sent[1].data.c_str());
    r.eng.tick(true, 1260);
    r.eng.tick(true, 1600);
    TEST_ASSERT_EQUAL(2, (int)r.sender.sent.size());
    TEST_ASSERT_TRUE(std::fabs((90.0) - (r.eng.local(0, 1600)->value)) < 1e-9);
    TEST_ASSERT_TRUE(r.sender.sent[0].tag != r.sender.sent[1].tag);
}

static void test_eng_base_is_held_target_then_ha_value() {
    Rig r;
    r.eng.rotate(0, 3, &r.on50, true, 1000);  // 65
    r.ok(0, 1100);
    ha::EntityState echo = ent("on", "{\"brightness\":166}");  // 65 %
    TEST_ASSERT_TRUE(r.eng.local(0, 1500).has_value());          // echo at +500: not shown
    TEST_ASSERT_TRUE(std::fabs((65.0) - (r.eng.local(0, 1500)->value)) < 1e-9);
    TEST_ASSERT_TRUE(r.eng.local(0, 2499).has_value());
    // while holding the next turn starts from the held target, not from (stale) HA
    r.eng.rotate(0, 1, &r.on50, true, 1600);
    TEST_ASSERT_EQUAL_STRING("{\"brightness_pct\":70}", r.sender.sent.at(1).data.c_str());

    Rig r2;
    r2.eng.rotate(0, 3, &r2.on50, true, 1000);
    r2.ok(0, 1100);
    r2.eng.tick(true, 2499);
    TEST_ASSERT_TRUE(r2.eng.local(0, 2499).has_value());
    r2.eng.tick(true, 2500);  // 1500 ms after the last turn: HA's value is shown again
    TEST_ASSERT_FALSE(r2.eng.local(0, 2500).has_value());
    r2.eng.rotate(0, 1, &echo, true, 2600);  // base = HA's 65
    TEST_ASSERT_EQUAL_STRING("{\"brightness_pct\":70}", r2.sender.sent.at(1).data.c_str());
}

static void test_eng_hold_lasts_while_call_unanswered() {
    Rig r;
    r.eng.rotate(0, 3, &r.on50, true, 1000);
    r.eng.tick(true, 5000);
    TEST_ASSERT_TRUE(r.eng.local(0, 5000).has_value());  // no answer yet
    r.ok(0, 5000);
    // settle time (counted from the last turn) is long over
    TEST_ASSERT_FALSE(r.eng.local(0, 5001).has_value());
}

static void test_eng_version_bumps() {
    Rig r;
    const uint32_t v0 = r.eng.version();
    r.eng.rotate(0, 3, &r.on50, true, 1000);
    r.eng.tick(true, 1000);
    TEST_ASSERT_EQUAL_UINT32(v0 + 1, r.eng.version());
    r.ok(0, 1100);
    const uint32_t v1 = r.eng.version();
    TEST_ASSERT_TRUE(v1 != v0 + 1);  // a result changes local() eligibility too
    r.eng.tick(true, 2499);
    TEST_ASSERT_EQUAL_UINT32(v1, r.eng.version());
    r.eng.tick(true, 2500);  // hold expiry seen
    TEST_ASSERT_EQUAL_UINT32(v1 + 1, r.eng.version());
    r.eng.tick(true, 2600);
    TEST_ASSERT_EQUAL_UINT32(v1 + 1, r.eng.version());
}

static void test_eng_two_controls_throttle_independently() {
    Rig r(opts(), false);
    r.eng.setControls({lightCtl(), mkControl("cover.c", "position", 10, "stop")});
    ha::EntityState cover = ent("open", "{\"current_position\":40}");
    r.eng.rotate(0, 1, &r.on50, true, 1000);
    r.eng.rotate(1, 1, &cover, true, 1000);
    TEST_ASSERT_EQUAL(2, (int)r.sender.sent.size());
    TEST_ASSERT_EQUAL_STRING("cover.c", r.sender.sent[1].entity.c_str());
    TEST_ASSERT_EQUAL_STRING("{\"position\":50}", r.sender.sent[1].data.c_str());
    r.eng.rotate(0, 1, &r.on50, true, 1100);  // trailing for control 0 only
    r.eng.tick(true, 1249);
    TEST_ASSERT_EQUAL(2, (int)r.sender.sent.size());
    r.eng.tick(true, 1250);
    TEST_ASSERT_EQUAL(3, (int)r.sender.sent.size());
    TEST_ASSERT_EQUAL_STRING("light.a", r.sender.sent[2].entity.c_str());
    TEST_ASSERT_EQUAL_STRING("{\"brightness_pct\":60}", r.sender.sent[2].data.c_str());
    r.eng.rotate(1, 1, &cover, true, 1260);  // its own interval has long passed
    TEST_ASSERT_EQUAL(4, (int)r.sender.sent.size());
    TEST_ASSERT_EQUAL_STRING("{\"position\":60}", r.sender.sent[3].data.c_str());
}

static void test_eng_cap_defers_and_sends_all_final_values() {
    Rig r(opts(), false);
    std::vector<cfg::Control> cs;
    std::vector<std::string> ids;
    for (int i = 0; i < 10; ++i) ids.push_back("light.l" + std::to_string(i));
    for (const std::string &id : ids) cs.push_back(lightCtl(id.c_str()));
    r.eng.setControls(cs);
    for (size_t i = 0; i < 10; ++i) r.eng.rotate(i, 3, &r.on50, true, 1000);
    TEST_ASSERT_EQUAL(8, (int)r.sender.sent.size());
    TEST_ASSERT_EQUAL(8, (int)r.eng.inFlight());
    TEST_ASSERT_TRUE(r.eng.local(8, 1000).has_value());  // deferred, not dropped
    TEST_ASSERT_TRUE(r.eng.local(9, 1000).has_value());
    r.eng.tick(true, 1300);
    TEST_ASSERT_EQUAL(8, (int)r.sender.sent.size());
    r.ok(0, 1400);
    r.eng.tick(true, 1400);
    TEST_ASSERT_EQUAL(9, (int)r.sender.sent.size());
    TEST_ASSERT_EQUAL_STRING("light.l8", r.sender.sent[8].entity.c_str());
    TEST_ASSERT_EQUAL_STRING("{\"brightness_pct\":65}", r.sender.sent[8].data.c_str());
    r.ok(1, 1450);
    r.eng.tick(true, 1450);
    TEST_ASSERT_EQUAL(10, (int)r.sender.sent.size());
    TEST_ASSERT_EQUAL_STRING("light.l9", r.sender.sent[9].entity.c_str());
    for (size_t i = 2; i < 10; ++i) r.ok(i, 1500);
    TEST_ASSERT_EQUAL(0, (int)r.eng.inFlight());
}

static void test_eng_cap_defers_trailing_value_not_dropped() {
    Rig r(opts(250, 1500, 1));
    r.eng.rotate(0, 1, &r.on50, true, 1000);  // 55 in flight (cap 1)
    r.eng.rotate(0, 1, &r.on50, true, 1100);  // 60 deferred
    r.eng.tick(true, 2000);
    TEST_ASSERT_EQUAL(1, (int)r.sender.sent.size());
    r.ok(0, 2100);
    r.eng.tick(true, 2100);
    TEST_ASSERT_EQUAL(2, (int)r.sender.sent.size());
    TEST_ASSERT_EQUAL_STRING("{\"brightness_pct\":60}", r.sender.sent[1].data.c_str());
}

// --- results ----------------------------------------------------------------------------

static void test_eng_failed_notice_and_hold_ends() {
    Rig r;
    r.eng.rotate(0, 3, &r.on50, true, 1000);
    r.eng.onResult(res(r.tag(0), CO::Failed, "home_assistant_error", "Light is gone"), 1100);
    expectNotice(r.eng.takeNotice(), NK::Failed, "Light is gone", 0);
    TEST_ASSERT_FALSE(r.eng.local(0, 1100).has_value());
    TEST_ASSERT_EQUAL(0, (int)r.eng.inFlight());
    TEST_ASSERT_FALSE(r.eng.takeNotice().has_value());
}

static void test_eng_failed_notices_never_suppressed() {
    Rig r;
    r.eng.rotate(0, 1, &r.on50, true, 1000);
    r.eng.onResult(res(r.tag(0), CO::Failed, "c", "first"), 1010);
    expectNotice(r.eng.takeNotice(), NK::Failed, "first", 0);
    r.eng.rotate(0, 1, &r.on50, true, 1300);
    r.eng.onResult(res(r.tag(1), CO::Failed, "c", "second"), 1310);
    expectNotice(r.eng.takeNotice(), NK::Failed, "second", 0);
}

static void test_eng_timeout_notice() {
    Rig r;
    r.eng.rotate(0, 3, &r.on50, true, 1000);
    r.eng.onResult(res(r.tag(0), CO::Timeout), 11000);
    expectNotice(r.eng.takeNotice(), NK::Timeout, "No answer from HA", 0);
    TEST_ASSERT_FALSE(r.eng.local(0, 11000).has_value());
    TEST_ASSERT_EQUAL(0, (int)r.eng.inFlight());
}

static void test_eng_connection_lost_no_notice_and_unsent_dropped() {
    Rig r;
    r.eng.rotate(0, 3, &r.on50, true, 1000);
    r.eng.rotate(0, 1, &r.on50, true, 1050);  // unsent trailing 70
    r.eng.onResult(res(r.tag(0), CO::ConnectionLost), 1100);
    TEST_ASSERT_FALSE(r.eng.takeNotice().has_value());
    TEST_ASSERT_FALSE(r.eng.local(0, 1100).has_value());
    r.eng.tick(true, 1300);
    r.eng.tick(true, 3000);
    TEST_ASSERT_EQUAL(1, (int)r.sender.sent.size());  // never sent after the loss
    TEST_ASSERT_EQUAL(0, (int)r.eng.inFlight());
}

static void test_eng_failed_empty_message_uses_code() {
    Rig r;
    r.eng.rotate(0, 3, &r.on50, true, 1000);
    r.eng.onResult(res(r.tag(0), CO::Failed, "not_found", ""), 1100);
    expectNotice(r.eng.takeNotice(), NK::Failed, "Failed: not_found", 0);
}

static void test_eng_failed_message_cut_at_60_bytes() {
    Rig r;
    r.eng.rotate(0, 1, &r.on50, true, 1000);
    r.eng.onResult(res(r.tag(0), CO::Failed, "c", std::string(100, 'x')), 1100);
    auto n = r.eng.takeNotice();
    TEST_ASSERT_TRUE(n.has_value());
    TEST_ASSERT_EQUAL(60, (int)n->text.size());

    // multi-byte character crossing byte 60 (bytes 59-60 are 0xC3 0xA9): cut before it
    r.eng.rotate(0, 1, &r.on50, true, 1300);
    std::string msg = std::string(59, 'a') + "\xC3\xA9" + "tail";
    r.eng.onResult(res(r.tag(1), CO::Failed, "c", msg), 1400);
    n = r.eng.takeNotice();
    TEST_ASSERT_TRUE(n.has_value());
    TEST_ASSERT_EQUAL(59, (int)n->text.size());
    TEST_ASSERT_EQUAL_STRING(std::string(59, 'a').c_str(), n->text.c_str());

    // a character that fits exactly (bytes 58-59) is kept
    r.eng.rotate(0, 1, &r.on50, true, 1700);
    msg = std::string(58, 'a') + "\xC3\xA9" + "tail";
    r.eng.onResult(res(r.tag(2), CO::Failed, "c", msg), 1800);
    n = r.eng.takeNotice();
    TEST_ASSERT_TRUE(n.has_value());
    TEST_ASSERT_EQUAL(60, (int)n->text.size());
    TEST_ASSERT_EQUAL_UINT8(0xA9, (uint8_t)n->text[59]);
}

static void test_eng_failed_empty_message_long_code_cut() {
    Rig r;
    r.eng.rotate(0, 1, &r.on50, true, 1000);
    r.eng.onResult(res(r.tag(0), CO::Failed, std::string(100, 'c'), ""), 1100);
    auto n = r.eng.takeNotice();
    TEST_ASSERT_TRUE(n.has_value());
    TEST_ASSERT_EQUAL(60, (int)n->text.size());
    TEST_ASSERT_EQUAL(0, n->text.compare(0, 8, "Failed: "));
}

static void test_eng_failed_older_call_newer_in_flight() {
    Rig r;
    r.eng.rotate(0, 1, &r.on50, true, 1000);
    r.eng.rotate(0, 1, &r.on50, true, 1100);
    r.eng.tick(true, 1250);
    TEST_ASSERT_EQUAL(2, (int)r.eng.inFlight());
    r.eng.onResult(res(r.tag(0), CO::Failed, "c", "older failed"), 1300);
    expectNotice(r.eng.takeNotice(), NK::Failed, "older failed", 0);
    TEST_ASSERT_FALSE(r.eng.local(0, 1300).has_value());
    TEST_ASSERT_EQUAL(1, (int)r.eng.inFlight());
    r.ok(1, 1400);  // the newer one only decrements
    TEST_ASSERT_FALSE(r.eng.takeNotice().has_value());
    TEST_ASSERT_FALSE(r.eng.local(0, 1400).has_value());
    TEST_ASSERT_EQUAL(0, (int)r.eng.inFlight());
}

static void test_eng_connection_lost_two_in_flight() {
    Rig r;
    r.eng.rotate(0, 3, &r.on50, true, 1000);
    r.eng.rotate(0, 1, &r.on50, true, 1100);
    r.eng.tick(true, 1250);
    TEST_ASSERT_EQUAL(2, (int)r.eng.inFlight());
    r.eng.onResult(res(r.tag(0), CO::ConnectionLost), 1300);
    r.eng.onResult(res(r.tag(1), CO::ConnectionLost), 1300);
    TEST_ASSERT_FALSE(r.eng.takeNotice().has_value());
    TEST_ASSERT_FALSE(r.eng.local(0, 1300).has_value());
    TEST_ASSERT_EQUAL(0, (int)r.eng.inFlight());
    r.eng.tick(true, 1500);  // idle
    r.eng.rotate(0, 3, &r.on50, true, 1500);  // same value as before: sent at once
    TEST_ASSERT_EQUAL(3, (int)r.sender.sent.size());
    TEST_ASSERT_EQUAL_STRING("{\"brightness_pct\":65}", r.sender.sent[2].data.c_str());
}

static void test_eng_unknown_and_finished_tags_ignored() {
    Rig r;
    r.eng.rotate(0, 1, &r.on50, true, 1000);
    const uint32_t v = r.eng.version();
    r.eng.onResult(res(7777, CO::Failed, "c", "x"), 1010);
    TEST_ASSERT_EQUAL(1, (int)r.eng.inFlight());
    TEST_ASSERT_EQUAL_UINT32(v, r.eng.version());
    TEST_ASSERT_FALSE(r.eng.takeNotice().has_value());
    r.ok(0, 1020);
    TEST_ASSERT_EQUAL(0, (int)r.eng.inFlight());
    r.eng.onResult(res(r.tag(0), CO::Failed, "c", "late"), 1030);  // already finished
    TEST_ASSERT_FALSE(r.eng.takeNotice().has_value());
    TEST_ASSERT_TRUE(r.eng.local(0, 1030).has_value());  // settle hold unaffected
    TEST_ASSERT_EQUAL(0, (int)r.eng.inFlight());
}

static void test_eng_tags_unique_and_nonzero() {
    Rig r;
    std::set<uint32_t> tags;
    for (int i = 0; i < 60; ++i) {
        r.eng.press(0, &r.on50, true, 1000 + i);
        const uint32_t t = r.sender.sent.back().tag;
        TEST_ASSERT_TRUE(t != 0);
        TEST_ASSERT_TRUE(tags.insert(t).second);
        r.eng.onResult(res(t, CO::Ok), 1000 + i);
    }
    // tags of calls still in flight are distinct as well
    Rig r2;
    for (int i = 0; i < 8; ++i) r2.eng.press(0, &r2.on50, true, 1000);
    std::set<uint32_t> inflight;
    for (const Sent &s : r2.sender.sent) {
        TEST_ASSERT_TRUE(s.tag != 0);
        inflight.insert(s.tag);
    }
    TEST_ASSERT_EQUAL(8, (int)inflight.size());
}

// --- idle reset, A1 ----------------------------------------------------------------------

static void test_eng_idle_reset_sends_equal_value() {
    Rig r;
    r.eng.rotate(0, 3, &r.on50, true, 1000);  // 65
    r.ok(0, 1100);
    r.eng.tick(true, 2750);  // settle + interval over, nothing in flight: idle
    r.eng.rotate(0, 3, &r.on50, true, 2800);  // HA still reports 50: 65 again, sent (A1)
    TEST_ASSERT_EQUAL(2, (int)r.sender.sent.size());
    TEST_ASSERT_EQUAL_STRING("{\"brightness_pct\":65}", r.sender.sent[1].data.c_str());
}

static void test_eng_same_value_inside_hold_not_resent() {
    Rig r;
    r.eng.rotate(0, 100, &r.on50, true, 1000);  // clamped to 100
    TEST_ASSERT_EQUAL_STRING("{\"brightness_pct\":100}", r.sender.sent.at(0).data.c_str());
    r.ok(0, 1100);
    r.eng.rotate(0, 1, &r.on50, true, 1400);  // still 100
    r.eng.tick(true, 1700);
    r.eng.tick(true, 2000);
    TEST_ASSERT_EQUAL(1, (int)r.sender.sent.size());
}

// --- setOptions --------------------------------------------------------------------------

static void test_eng_set_options_interval_applies_when_idle() {
    Rig r;
    r.eng.rotate(0, 1, &r.on50, true, 1000);  // 55
    r.eng.setOptions(opts(1000));
    r.eng.rotate(0, 1, &r.on50, true, 1100);  // 60 unsent
    r.eng.tick(true, 1250);                   // old interval (250) still in force mid-hold
    TEST_ASSERT_EQUAL(2, (int)r.sender.sent.size());
    r.ok(0, 1300);
    r.ok(1, 1300);
    r.eng.tick(true, 4000);  // idle -> new interval
    r.eng.rotate(0, 1, &r.on50, true, 5000);
    TEST_ASSERT_EQUAL(3, (int)r.sender.sent.size());
    r.eng.rotate(0, 1, &r.on50, true, 5100);
    r.eng.tick(true, 5250);
    TEST_ASSERT_EQUAL(3, (int)r.sender.sent.size());
    r.eng.tick(true, 5999);
    TEST_ASSERT_EQUAL(3, (int)r.sender.sent.size());
    r.eng.tick(true, 6000);
    TEST_ASSERT_EQUAL(4, (int)r.sender.sent.size());
}

static void test_eng_set_options_idle_control_gets_new_settle() {
    Rig r;
    r.eng.setOptions(opts(250, 500));
    r.eng.rotate(0, 1, &r.on50, true, 1000);
    r.ok(0, 1010);
    TEST_ASSERT_TRUE(r.eng.local(0, 1499).has_value());
    TEST_ASSERT_FALSE(r.eng.local(0, 1500).has_value());
}

static void test_eng_set_options_cap_applies_at_once() {
    Rig r(opts(250, 1500, 1));
    r.eng.rotate(0, 1, &r.on50, true, 1000);
    r.eng.press(0, &r.on50, true, 1100);
    TEST_ASSERT_EQUAL(1, (int)r.sender.sent.size());  // Busy
    expectNotice(r.eng.takeNotice(), NK::Busy, "Busy, try again", 0);
    r.eng.setOptions(opts(250, 1500, 2));
    r.eng.press(0, &r.on50, true, 1200);
    TEST_ASSERT_EQUAL(2, (int)r.sender.sent.size());
    TEST_ASSERT_EQUAL_STRING("toggle", r.sender.sent[1].service.c_str());
}

static void test_eng_cap_zero_is_one() {
    Rig r(opts(250, 1500, 0), false);
    r.eng.setControls({lightCtl("light.a"), lightCtl("light.b")});
    r.eng.rotate(0, 1, &r.on50, true, 1000);
    r.eng.rotate(1, 1, &r.on50, true, 1000);
    TEST_ASSERT_EQUAL(1, (int)r.sender.sent.size());
    r.eng.press(0, &r.on50, true, 1010);
    TEST_ASSERT_EQUAL(1, (int)r.sender.sent.size());
    expectNotice(r.eng.takeNotice(), NK::Busy, "Busy, try again", 0);

    Rig r2;
    r2.eng.setControls({lightCtl("light.a"), lightCtl("light.b")});
    r2.eng.setOptions(opts(250, 1500, 0));
    r2.eng.rotate(0, 1, &r2.on50, true, 1000);
    r2.eng.rotate(1, 1, &r2.on50, true, 1000);
    TEST_ASSERT_EQUAL(1, (int)r2.sender.sent.size());
    r2.ok(0, 1100);
    r2.eng.tick(true, 1100);
    TEST_ASSERT_EQUAL(2, (int)r2.sender.sent.size());
}

// --- notices -----------------------------------------------------------------------------

static void test_eng_offline_notice_and_suppression() {
    Rig r;
    r.eng.rotate(0, 1, &r.on50, false, 1000);
    expectNotice(r.eng.takeNotice(), NK::HaOffline, "HA offline", 0);
    TEST_ASSERT_EQUAL(0, (int)r.sender.sent.size());
    TEST_ASSERT_FALSE(r.eng.local(0, 1000).has_value());
    r.eng.rotate(0, 1, &r.on50, false, 2999);
    TEST_ASSERT_FALSE(r.eng.takeNotice().has_value());
    r.eng.press(0, &r.on50, false, 2999);  // same kind, same control
    TEST_ASSERT_FALSE(r.eng.takeNotice().has_value());
    r.eng.rotate(0, 1, &r.on50, false, 3000);
    expectNotice(r.eng.takeNotice(), NK::HaOffline, "HA offline", 0);
    TEST_ASSERT_EQUAL(0, (int)r.sender.sent.size());
}

static void test_eng_notice_suppression_is_per_control_and_kind() {
    Rig r(opts(), false);
    r.eng.setControls({lightCtl("light.a"), lightCtl("light.b")});
    r.eng.rotate(0, 1, &r.on50, false, 1000);
    TEST_ASSERT_TRUE(r.eng.takeNotice().has_value());
    r.eng.rotate(1, 1, &r.on50, false, 1010);  // other control: reported
    auto n = r.eng.takeNotice();
    TEST_ASSERT_TRUE(n.has_value());
    TEST_ASSERT_EQUAL(1, (int)n->control);
    r.eng.rotate(0, 1, nullptr, true, 1020);  // other kind for control 0: reported
    expectNotice(r.eng.takeNotice(), NK::NotFound, "Entity not found", 0);
    r.eng.rotate(0, 1, nullptr, true, 1030);  // repeat: suppressed
    TEST_ASSERT_FALSE(r.eng.takeNotice().has_value());
}

static void test_eng_not_found_unavailable_no_value() {
    Rig r;
    r.eng.rotate(0, 1, nullptr, true, 1000);
    expectNotice(r.eng.takeNotice(), NK::NotFound, "Entity not found", 0);
    ha::EntityState un = ent("unavailable");
    r.eng.rotate(0, 1, &un, true, 1010);
    expectNotice(r.eng.takeNotice(), NK::Unavailable, "Unavailable", 0);
    ha::EntityState unk = ent("unknown");
    r.eng.rotate(0, 1, &unk, true, 3100);
    expectNotice(r.eng.takeNotice(), NK::Unavailable, "Unavailable", 0);
    TEST_ASSERT_EQUAL(0, (int)r.sender.sent.size());
    TEST_ASSERT_FALSE(r.eng.local(0, 3100).has_value());

    Rig c(opts(), false);
    c.eng.setControls({mkControl("climate.k", "target_temp", 0.5, "toggle")});
    ha::EntityState noTemp = ent("heat", "{\"temperature\":null}");
    c.eng.rotate(0, 1, &noTemp, true, 1000);
    expectNotice(c.eng.takeNotice(), NK::NoValue, "No value to change", 0);
    TEST_ASSERT_EQUAL(0, (int)c.sender.sent.size());
    TEST_ASSERT_FALSE(c.eng.local(0, 1000).has_value());
}

static void test_eng_only_newest_notice_kept() {
    Rig r;
    r.eng.rotate(0, 1, &r.on50, false, 1000);  // HaOffline
    r.eng.rotate(0, 1, nullptr, true, 1010);   // NotFound
    expectNotice(r.eng.takeNotice(), NK::NotFound, "Entity not found", 0);
    TEST_ASSERT_FALSE(r.eng.takeNotice().has_value());
}

static void test_eng_action_none_is_silent() {
    Rig r(opts(), false);
    r.eng.setControls({mkControl("light.a", "none", 0, "toggle"),
                       mkControl("switch.s", "brightness", 5, "none")});
    const uint32_t v = r.eng.version();
    r.eng.rotate(0, 1, &r.on50, false, 1000);  // offline too
    r.eng.rotate(0, 1, &r.on50, true, 1010);
    r.eng.rotate(0, 1, nullptr, true, 1020);
    r.eng.rotate(1, 1, &r.on50, false, 1030);  // action the domain lacks
    r.eng.rotate(1, 1, &r.on50, true, 1040);
    r.eng.press(1, &r.on50, false, 1050);      // press none, offline
    r.eng.press(1, &r.on50, true, 1060);
    TEST_ASSERT_FALSE(r.eng.takeNotice().has_value());
    TEST_ASSERT_EQUAL(0, (int)r.sender.sent.size());
    TEST_ASSERT_EQUAL_UINT32(v, r.eng.version());
    TEST_ASSERT_FALSE(r.eng.local(0, 1010).has_value());
}

static void test_eng_out_of_range_and_zero_steps() {
    Rig r;
    const uint32_t v = r.eng.version();
    r.eng.rotate(5, 1, &r.on50, true, 1000);
    r.eng.press(5, &r.on50, true, 1000);
    r.eng.rotate(0, 0, &r.on50, true, 1000);
    r.eng.rotate(0, 0, &r.on50, false, 1000);
    TEST_ASSERT_FALSE(r.eng.local(5, 1000).has_value());
    TEST_ASSERT_FALSE(r.eng.takeNotice().has_value());
    TEST_ASSERT_EQUAL(0, (int)r.sender.sent.size());
    TEST_ASSERT_EQUAL_UINT32(v, r.eng.version());
}

// --- tick without HA -----------------------------------------------------------------

static void test_eng_tick_not_ready_drops_unsent_and_hold() {
    Rig r;
    r.eng.rotate(0, 1, &r.on50, true, 1000);
    r.eng.rotate(0, 1, &r.on50, true, 1050);  // unsent
    const uint32_t v = r.eng.version();
    r.eng.tick(false, 1100);
    TEST_ASSERT_FALSE(r.eng.local(0, 1100).has_value());
    TEST_ASSERT_TRUE(v != r.eng.version());
    r.eng.tick(true, 1400);
    r.eng.tick(true, 3000);
    TEST_ASSERT_EQUAL(1, (int)r.sender.sent.size());  // never sent after HA is back
}

// --- press -----------------------------------------------------------------------------

static void test_eng_press_sends_call_at_once() {
    Rig r;
    r.eng.press(0, &r.on50, true, 1000);
    TEST_ASSERT_EQUAL(1, (int)r.sender.sent.size());
    TEST_ASSERT_EQUAL_STRING("light", r.sender.sent[0].domain.c_str());
    TEST_ASSERT_EQUAL_STRING("toggle", r.sender.sent[0].service.c_str());
    TEST_ASSERT_EQUAL_STRING("light.a", r.sender.sent[0].entity.c_str());
    TEST_ASSERT_EQUAL_STRING("null", r.sender.sent[0].data.c_str());
    TEST_ASSERT_EQUAL(1, (int)r.eng.inFlight());
    TEST_ASSERT_FALSE(r.eng.local(0, 1000).has_value());  // not optimistic
    r.ok(0, 1100);
    TEST_ASSERT_FALSE(r.eng.takeNotice().has_value());
    TEST_ASSERT_EQUAL(0, (int)r.eng.inFlight());
}

static void test_eng_press_drops_hold_and_unsent_value() {
    Rig r;
    r.eng.rotate(0, 1, &r.on50, true, 1000);
    r.eng.rotate(0, 1, &r.on50, true, 1050);  // unsent trailing
    r.eng.press(0, &r.on50, true, 1100);
    TEST_ASSERT_FALSE(r.eng.local(0, 1100).has_value());
    TEST_ASSERT_EQUAL(2, (int)r.sender.sent.size());
    TEST_ASSERT_EQUAL_STRING("toggle", r.sender.sent[1].service.c_str());
    r.eng.tick(true, 1300);
    r.eng.tick(true, 3000);
    TEST_ASSERT_EQUAL(2, (int)r.sender.sent.size());  // trailing never sent
}

static void test_eng_press_climate_and_mute_use_state() {
    Rig r(opts(), false);
    r.eng.setControls({mkControl("climate.k", "target_temp", 0.5, "toggle"),
                       mkControl("media_player.m", "volume", 2, "mute")});
    ha::EntityState heat = ent("heat");
    ha::EntityState off = ent("off");
    ha::EntityState mp = ent("playing", "{\"is_volume_muted\":true}");
    r.eng.press(0, &heat, true, 1000);
    TEST_ASSERT_EQUAL_STRING("climate.turn_off",
                             (r.sender.sent[0].domain + "." + r.sender.sent[0].service).c_str());
    r.eng.press(0, &off, true, 1010);
    TEST_ASSERT_EQUAL_STRING("turn_on", r.sender.sent[1].service.c_str());
    r.eng.press(1, &mp, true, 1020);
    TEST_ASSERT_EQUAL_STRING("volume_mute", r.sender.sent[2].service.c_str());
    TEST_ASSERT_EQUAL_STRING("{\"is_volume_muted\":false}", r.sender.sent[2].data.c_str());
}

static void test_eng_press_problems_drop_hold_but_send_nothing() {
    Rig r;
    ha::EntityState un = ent("unavailable");
    r.eng.rotate(0, 1, &r.on50, true, 1000);
    r.eng.rotate(0, 1, &r.on50, true, 1050);
    r.eng.press(0, &un, true, 1100);
    expectNotice(r.eng.takeNotice(), NK::Unavailable, "Unavailable", 0);
    TEST_ASSERT_FALSE(r.eng.local(0, 1100).has_value());
    TEST_ASSERT_EQUAL(1, (int)r.sender.sent.size());
    r.eng.rotate(0, 1, &r.on50, true, 1300);
    r.eng.press(0, nullptr, true, 1310);
    expectNotice(r.eng.takeNotice(), NK::NotFound, "Entity not found", 0);
    TEST_ASSERT_FALSE(r.eng.local(0, 1310).has_value());
    TEST_ASSERT_EQUAL(2, (int)r.sender.sent.size());
}

static void test_eng_press_offline_notice_keeps_hold() {
    Rig r;
    r.eng.rotate(0, 1, &r.on50, true, 1000);
    r.eng.press(0, &r.on50, false, 1100);
    expectNotice(r.eng.takeNotice(), NK::HaOffline, "HA offline", 0);
    TEST_ASSERT_EQUAL(1, (int)r.sender.sent.size());
    TEST_ASSERT_TRUE(r.eng.local(0, 1100).has_value());
}

static void test_eng_press_busy_at_cap() {
    Rig r(opts(250, 1500, 1));
    r.eng.rotate(0, 1, &r.on50, true, 1000);
    r.eng.rotate(0, 1, &r.on50, true, 1050);  // unsent
    r.eng.press(0, &r.on50, true, 1100);
    expectNotice(r.eng.takeNotice(), NK::Busy, "Busy, try again", 0);
    TEST_ASSERT_EQUAL(1, (int)r.sender.sent.size());
    TEST_ASSERT_EQUAL(1, (int)r.eng.inFlight());
    TEST_ASSERT_FALSE(r.eng.local(0, 1100).has_value());  // hold still dropped
    r.eng.press(0, &r.on50, true, 1500);  // Busy again within 2 s: suppressed
    TEST_ASSERT_FALSE(r.eng.takeNotice().has_value());
}

static void test_eng_press_results() {
    Rig r;
    r.eng.press(0, &r.on50, true, 1000);
    r.eng.onResult(res(r.tag(0), CO::Failed, "c", "nope"), 1100);
    expectNotice(r.eng.takeNotice(), NK::Failed, "nope", 0);
    r.eng.press(0, &r.on50, true, 1200);
    r.eng.onResult(res(r.tag(1), CO::Timeout), 1300);
    expectNotice(r.eng.takeNotice(), NK::Timeout, "No answer from HA", 0);
    r.eng.press(0, &r.on50, true, 1400);
    r.eng.onResult(res(r.tag(2), CO::ConnectionLost), 1500);
    TEST_ASSERT_FALSE(r.eng.takeNotice().has_value());
    r.eng.press(0, &r.on50, true, 1600);
    r.eng.onResult(res(r.tag(3), CO::Failed, "bad_code", ""), 1700);
    expectNotice(r.eng.takeNotice(), NK::Failed, "Failed: bad_code", 0);
    TEST_ASSERT_EQUAL(0, (int)r.eng.inFlight());
}

static void test_eng_press_result_does_not_end_encoder_hold() {
    Rig r;
    r.eng.press(0, &r.on50, true, 1000);       // tag 0 (press)
    r.eng.rotate(0, 1, &r.on50, true, 1010);   // tag 1 (encoder), hold
    r.eng.onResult(res(r.tag(0), CO::Failed, "c", "press failed"), 1100);
    expectNotice(r.eng.takeNotice(), NK::Failed, "press failed", 0);
    TEST_ASSERT_TRUE(r.eng.local(0, 1100).has_value());
    r.eng.onResult(res(r.tag(0), CO::Timeout), 1110);  // finished tag: ignored
    TEST_ASSERT_TRUE(r.eng.local(0, 1110).has_value());
}

// --- sender refusal ------------------------------------------------------------------

static void test_eng_sender_refusal_rotate() {
    Rig r;
    r.sender.accept = false;
    r.eng.rotate(0, 1, &r.on50, true, 1000);
    expectNotice(r.eng.takeNotice(), NK::Busy, "Busy, try again", 0);
    TEST_ASSERT_FALSE(r.eng.local(0, 1000).has_value());
    TEST_ASSERT_EQUAL(0, (int)r.eng.inFlight());
    TEST_ASSERT_EQUAL(0, (int)r.sender.sent.size());
    r.eng.rotate(0, 1, &r.on50, true, 2999);  // suppressed 2 s
    TEST_ASSERT_FALSE(r.eng.takeNotice().has_value());
    r.eng.rotate(0, 1, &r.on50, true, 3000);
    expectNotice(r.eng.takeNotice(), NK::Busy, "Busy, try again", 0);
    r.sender.accept = true;
    r.eng.tick(true, 5000);
    TEST_ASSERT_EQUAL(0, (int)r.sender.sent.size());  // dropped, not retried
}

static void test_eng_sender_refusal_press() {
    Rig r;
    r.sender.accept = false;
    r.eng.press(0, &r.on50, true, 1000);
    expectNotice(r.eng.takeNotice(), NK::Busy, "Busy, try again", 0);
    TEST_ASSERT_EQUAL(0, (int)r.eng.inFlight());
    r.eng.press(0, &r.on50, true, 1500);
    TEST_ASSERT_FALSE(r.eng.takeNotice().has_value());
    r.sender.accept = true;
    r.eng.press(0, &r.on50, true, 1600);
    TEST_ASSERT_EQUAL(1, (int)r.sender.sent.size());
    TEST_ASSERT_EQUAL(1, (int)r.eng.inFlight());
}

// --- setControls -----------------------------------------------------------------------

static void test_eng_set_controls_equal_keeps_state() {
    Rig r;
    r.eng.rotate(0, 1, &r.on50, true, 1000);
    const uint32_t v = r.eng.version();
    r.eng.setControls({lightCtl()});
    TEST_ASSERT_EQUAL_UINT32(v, r.eng.version());
    TEST_ASSERT_TRUE(r.eng.local(0, 1100).has_value());
    TEST_ASSERT_EQUAL(1, (int)r.eng.inFlight());
}

static void test_eng_set_controls_change_resets_all() {
    Rig r;
    r.eng.rotate(0, 1, &r.on50, false, 1000);  // pending notice HaOffline (+ suppression stamp)
    r.eng.rotate(0, 1, &r.on50, true, 1010);
    r.eng.rotate(0, 1, &r.on50, true, 1020);   // unsent
    const uint32_t v = r.eng.version();
    r.eng.setControls({lightCtl("light.other")});
    TEST_ASSERT_TRUE(v != r.eng.version());
    TEST_ASSERT_FALSE(r.eng.local(0, 1030).has_value());
    TEST_ASSERT_FALSE(r.eng.takeNotice().has_value());  // pending notice cleared
    r.eng.tick(true, 1500);
    TEST_ASSERT_EQUAL(1, (int)r.sender.sent.size());  // unsent value gone
    r.eng.rotate(0, 1, &r.on50, false, 1100);          // stamp reset: reported again
    expectNotice(r.eng.takeNotice(), NK::HaOffline, "HA offline", 0);
    r.eng.setControls({});
    TEST_ASSERT_FALSE(r.eng.local(0, 1100).has_value());
    r.eng.rotate(0, 1, &r.on50, true, 1200);  // no control: nothing
    TEST_ASSERT_EQUAL(1, (int)r.sender.sent.size());
}

static void test_eng_orphans_hold_cap_and_do_not_touch_new_slot() {
    Rig r(opts(250, 1500, 2));
    r.eng.rotate(0, 1, &r.on50, true, 1000);  // tag 0
    const uint32_t orphan = r.tag(0);
    r.eng.setControls({lightCtl("light.b")});
    TEST_ASSERT_EQUAL(1, (int)r.eng.inFlight());
    r.eng.rotate(0, 1, &r.on50, true, 1010);  // new slot, tag 1
    TEST_ASSERT_EQUAL(2, (int)r.eng.inFlight());
    r.eng.press(0, &r.on50, true, 1020);       // press drops the hold, cap reached
    expectNotice(r.eng.takeNotice(), NK::Busy, "Busy, try again", 0);
    r.eng.rotate(0, 1, &r.on50, true, 1030);  // hold again (deferred by cap)
    TEST_ASSERT_TRUE(r.eng.local(0, 1030).has_value());
    r.eng.onResult(res(orphan, CO::Failed, "c", "old failure"), 1100);
    TEST_ASSERT_FALSE(r.eng.takeNotice().has_value());  // orphan: slot freed only
    TEST_ASSERT_EQUAL(1, (int)r.eng.inFlight());
    TEST_ASSERT_TRUE(r.eng.local(0, 1100).has_value());  // new slot untouched
    r.eng.onResult(res(orphan, CO::Failed, "c", "again"), 1110);  // finished
    TEST_ASSERT_EQUAL(1, (int)r.eng.inFlight());
    r.eng.tick(true, 1300);  // room again: the deferred value goes out
    TEST_ASSERT_EQUAL(3, (int)r.sender.sent.size());
    TEST_ASSERT_EQUAL_STRING("{\"brightness_pct\":55}", r.sender.sent[2].data.c_str());
}

// --- millis() wrap --------------------------------------------------------------------

static void test_eng_clock_wrap_interval_and_settle() {
    const uint32_t t0 = 0xFFFFFF00u;
    Rig r;
    r.eng.rotate(0, 3, &r.on50, true, t0);
    r.eng.rotate(0, 1, &r.on50, true, t0 + 100);
    r.eng.tick(true, t0 + 249);
    TEST_ASSERT_EQUAL(1, (int)r.sender.sent.size());
    r.eng.tick(true, t0 + 250);
    TEST_ASSERT_EQUAL(2, (int)r.sender.sent.size());
    TEST_ASSERT_EQUAL_STRING("{\"brightness_pct\":70}", r.sender.sent[1].data.c_str());
    r.eng.rotate(0, 1, &r.on50, true, t0 + 260);  // clock wrapped
    r.eng.tick(true, t0 + 499);
    TEST_ASSERT_EQUAL(2, (int)r.sender.sent.size());
    r.eng.tick(true, t0 + 500);
    TEST_ASSERT_EQUAL(3, (int)r.sender.sent.size());
    TEST_ASSERT_EQUAL_STRING("{\"brightness_pct\":75}", r.sender.sent[2].data.c_str());
    r.ok(0, t0 + 600);
    r.ok(1, t0 + 600);
    r.ok(2, t0 + 600);
    TEST_ASSERT_TRUE(r.eng.local(0, t0 + 260 + 1499).has_value());
    r.eng.tick(true, t0 + 260 + 1499);
    TEST_ASSERT_TRUE(r.eng.local(0, t0 + 260 + 1499).has_value());
    r.eng.tick(true, t0 + 260 + 1500);
    TEST_ASSERT_FALSE(r.eng.local(0, t0 + 260 + 1500).has_value());
}

static void test_eng_clock_wrap_notice_suppression() {
    const uint32_t t0 = 0xFFFFFF00u;
    Rig r;
    r.eng.rotate(0, 1, &r.on50, false, t0 + 200);
    TEST_ASSERT_TRUE(r.eng.takeNotice().has_value());
    r.eng.rotate(0, 1, &r.on50, false, t0 + 200 + 1999);
    TEST_ASSERT_FALSE(r.eng.takeNotice().has_value());
    r.eng.rotate(0, 1, &r.on50, false, t0 + 200 + 2000);
    TEST_ASSERT_TRUE(r.eng.takeNotice().has_value());
}

static void test_eng_long_idle_local_empty_and_fresh_turn_sends() {
    Rig r;
    r.eng.rotate(0, 3, &r.on50, true, 1000);
    r.ok(0, 1100);
    r.eng.tick(true, 5000);  // idle
    r.eng.tick(true, 0x80000000u);
    r.eng.tick(true, 0xFFFFFF00u);
    r.eng.tick(true, 1100u);  // about 2^32 ms after the turn
    TEST_ASSERT_FALSE(r.eng.local(0, 1100u).has_value());
    TEST_ASSERT_FALSE(r.eng.local(0, 1500u).has_value());
    r.eng.rotate(0, 3, &r.on50, true, 1100u);
    TEST_ASSERT_EQUAL(2, (int)r.sender.sent.size());
    TEST_ASSERT_EQUAL_STRING("{\"brightness_pct\":65}", r.sender.sent[1].data.c_str());
}

// --- integration with the real HaSession on a fake transport ------------------------------

namespace {

struct FakeTransport : ha::Transport {
    std::vector<std::string> sent;
    void open(const ha::HaUrl &, const ha::TlsSettings &) override {}
    void close() override {}
    bool sendText(const std::string &text) override {
        sent.push_back(text);
        return true;
    }
};

struct SessionSender : ctl::CallSender {
    ha::HaSession *session = nullptr;
    uint32_t now = 0;
    bool send(const da::ServiceCall &c, uint32_t tag) override {
        return session->callService(c, tag, now) != 0;
    }
};

struct IntRig {
    FakeTransport t;
    ha::HaSession s;
    SessionSender sender;
    ctl::ControlEngine eng;
    ha::EntityState on50 = ent("on", "{\"brightness\":128}");
    IntRig() : s(t), eng(sender) {
        sender.session = &s;
        s.setNetworkUp(true, 0);
        s.configure("http://192.168.1.10:8123", "tok", ha::TlsSettings(), {"light.a"}, 0);
        feedText("{\"type\":\"auth_required\",\"ha_version\":\"2025.1.0\"}", 0);
        feedText("{\"type\":\"auth_ok\",\"ha_version\":\"2025.1.0\"}", 0);
        feedText("{\"id\":1,\"type\":\"result\",\"success\":true,\"result\":null}", 0);
        TEST_ASSERT_EQUAL((int)ha::State::Ready, (int)s.state());
        eng.setControls({lightCtl("light.a")});
    }
    void feedText(const std::string &text, uint32_t now) { s.onText(text.c_str(), text.size(), now); }
    void rotate(int steps, uint32_t now) {
        sender.now = now;
        eng.rotate(0, steps, &on50, s.state() == ha::State::Ready, now);
    }
    void tick(uint32_t now) {
        sender.now = now;
        s.tick(now);
        drain(now);
        eng.tick(s.state() == ha::State::Ready, now);
    }
    void drain(uint32_t now) {
        for (const ha::CallResult &r : s.takeCallResults()) eng.onResult(r, now);
    }
};

}  // namespace

static void test_eng_integration_success_and_failure() {
    IntRig r;
    const size_t before = r.t.sent.size();
    r.rotate(3, 1000);
    TEST_ASSERT_EQUAL((int)before + 1, (int)r.t.sent.size());
    TEST_ASSERT_EQUAL_STRING(
        "{\"id\":2,\"type\":\"call_service\",\"domain\":\"light\",\"service\":\"turn_on\","
        "\"service_data\":{\"brightness_pct\":65},\"target\":{\"entity_id\":\"light.a\"}}",
        r.t.sent.back().c_str());
    TEST_ASSERT_EQUAL(1, (int)r.eng.inFlight());
    r.feedText("{\"id\":2,\"type\":\"result\",\"success\":true,\"result\":{\"context\":{}}}", 1100);
    r.tick(1100);
    TEST_ASSERT_EQUAL(0, (int)r.eng.inFlight());
    TEST_ASSERT_FALSE(r.eng.takeNotice().has_value());
    TEST_ASSERT_TRUE(r.eng.local(0, 1200).has_value());

    r.rotate(1, 3000);  // later hold: HA reports 50 -> 55
    TEST_ASSERT_TRUE(r.t.sent.back().find("\"id\":3") != std::string::npos);
    TEST_ASSERT_TRUE(r.t.sent.back().find("\"brightness_pct\":55") != std::string::npos);
    r.feedText("{\"id\":3,\"type\":\"result\",\"success\":false,\"error\":{\"code\":\"home_assistant_error\","
               "\"message\":\"Boom\"}}",
               3100);
    r.tick(3100);
    expectNotice(r.eng.takeNotice(), NK::Failed, "Boom", 0);
    TEST_ASSERT_FALSE(r.eng.local(0, 3100).has_value());
    TEST_ASSERT_EQUAL(0, (int)r.eng.inFlight());
}

static void test_eng_integration_timeout() {
    IntRig r;
    r.rotate(3, 1000);
    r.tick(10999);
    TEST_ASSERT_EQUAL(1, (int)r.eng.inFlight());
    r.tick(11000);  // kCallTimeoutMs
    expectNotice(r.eng.takeNotice(), NK::Timeout, "No answer from HA", 0);
    TEST_ASSERT_EQUAL(0, (int)r.eng.inFlight());
    TEST_ASSERT_FALSE(r.eng.local(0, 11000).has_value());
}

static void test_eng_integration_disconnect_with_pending_calls() {
    IntRig r;
    r.rotate(3, 1000);
    r.rotate(1, 1100);
    r.tick(1250);  // trailing call
    TEST_ASSERT_EQUAL(2, (int)r.eng.inFlight());
    r.rotate(1, 1260);  // unsent value
    r.s.onClosed("lost", 1300);
    std::vector<ha::CallResult> results = r.s.takeCallResults();
    TEST_ASSERT_EQUAL(2, (int)results.size());
    for (const ha::CallResult &c : results) {
        TEST_ASSERT_EQUAL((int)CO::ConnectionLost, (int)c.outcome);
        r.eng.onResult(c, 1300);
    }
    TEST_ASSERT_EQUAL(0, (int)r.eng.inFlight());
    TEST_ASSERT_FALSE(r.eng.takeNotice().has_value());
    TEST_ASSERT_FALSE(r.eng.local(0, 1300).has_value());
    r.tick(1600);  // session no longer Ready: nothing is sent
    TEST_ASSERT_FALSE(r.eng.takeNotice().has_value());
    r.rotate(1, 1700);  // HA offline notice
    expectNotice(r.eng.takeNotice(), NK::HaOffline, "HA offline", 0);
}

void run_engine_tests() {
    RUN_TEST(test_eng_notice_kind_names);
    RUN_TEST(test_eng_rotate_sends_at_once);
    RUN_TEST(test_eng_off_light_starts_from_zero);
    RUN_TEST(test_eng_zero_brightness_turns_off);
    RUN_TEST(test_eng_trailing_value_after_interval);
    RUN_TEST(test_eng_base_is_held_target_then_ha_value);
    RUN_TEST(test_eng_hold_lasts_while_call_unanswered);
    RUN_TEST(test_eng_version_bumps);
    RUN_TEST(test_eng_two_controls_throttle_independently);
    RUN_TEST(test_eng_cap_defers_and_sends_all_final_values);
    RUN_TEST(test_eng_cap_defers_trailing_value_not_dropped);
    RUN_TEST(test_eng_failed_notice_and_hold_ends);
    RUN_TEST(test_eng_failed_notices_never_suppressed);
    RUN_TEST(test_eng_timeout_notice);
    RUN_TEST(test_eng_connection_lost_no_notice_and_unsent_dropped);
    RUN_TEST(test_eng_failed_empty_message_uses_code);
    RUN_TEST(test_eng_failed_message_cut_at_60_bytes);
    RUN_TEST(test_eng_failed_empty_message_long_code_cut);
    RUN_TEST(test_eng_failed_older_call_newer_in_flight);
    RUN_TEST(test_eng_connection_lost_two_in_flight);
    RUN_TEST(test_eng_unknown_and_finished_tags_ignored);
    RUN_TEST(test_eng_tags_unique_and_nonzero);
    RUN_TEST(test_eng_idle_reset_sends_equal_value);
    RUN_TEST(test_eng_same_value_inside_hold_not_resent);
    RUN_TEST(test_eng_set_options_interval_applies_when_idle);
    RUN_TEST(test_eng_set_options_idle_control_gets_new_settle);
    RUN_TEST(test_eng_set_options_cap_applies_at_once);
    RUN_TEST(test_eng_cap_zero_is_one);
    RUN_TEST(test_eng_offline_notice_and_suppression);
    RUN_TEST(test_eng_notice_suppression_is_per_control_and_kind);
    RUN_TEST(test_eng_not_found_unavailable_no_value);
    RUN_TEST(test_eng_only_newest_notice_kept);
    RUN_TEST(test_eng_action_none_is_silent);
    RUN_TEST(test_eng_out_of_range_and_zero_steps);
    RUN_TEST(test_eng_tick_not_ready_drops_unsent_and_hold);
    RUN_TEST(test_eng_press_sends_call_at_once);
    RUN_TEST(test_eng_press_drops_hold_and_unsent_value);
    RUN_TEST(test_eng_press_climate_and_mute_use_state);
    RUN_TEST(test_eng_press_problems_drop_hold_but_send_nothing);
    RUN_TEST(test_eng_press_offline_notice_keeps_hold);
    RUN_TEST(test_eng_press_busy_at_cap);
    RUN_TEST(test_eng_press_results);
    RUN_TEST(test_eng_press_result_does_not_end_encoder_hold);
    RUN_TEST(test_eng_sender_refusal_rotate);
    RUN_TEST(test_eng_sender_refusal_press);
    RUN_TEST(test_eng_set_controls_equal_keeps_state);
    RUN_TEST(test_eng_set_controls_change_resets_all);
    RUN_TEST(test_eng_orphans_hold_cap_and_do_not_touch_new_slot);
    RUN_TEST(test_eng_clock_wrap_interval_and_settle);
    RUN_TEST(test_eng_clock_wrap_notice_suppression);
    RUN_TEST(test_eng_long_idle_local_empty_and_fresh_turn_sends);
    RUN_TEST(test_eng_integration_success_and_failure);
    RUN_TEST(test_eng_integration_timeout);
    RUN_TEST(test_eng_integration_disconnect_with_pending_calls);
}
