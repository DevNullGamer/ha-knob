// On-device test for ConfigStore (plan.md §4 Step 3, optional embedded test): save,
// reboot-less reload, equality, secret handling and the factory reset path.
// Uses the NVS namespace "haknob_test" so the real "haknob" config is never touched.
//
// Run: pio test -e t-encoder-pro -f embedded/test_configstore
#include <Arduino.h>
#include <Preferences.h>
#include <unity.h>

#include <string>

#include "config/ConfigStore.h"

// ConfigStore lives in src/, which test builds do not compile; build it into this test.
#include "../../../src/config/ConfigStore.cpp"

namespace cfg = core::config;
using Kind = cfg::SecretChange::Kind;

namespace {

constexpr const char *kNs = "haknob_test";

void wipeNamespace() {
    Preferences p;
    if (p.begin(kNs, false)) {
        p.clear();
        p.end();
    }
}

cfg::Config sampleConfig() {
    cfg::Config c;
    c.device.hostname = "test-knob";
    c.device.brightness = 42;
    c.device.buzzer = false;
    c.wifi.ssid = "TestNet";
    c.ha.url = "http://192.168.1.10:8123";
    cfg::Control a;
    a.entityId = "light.kitchen";
    a.label = "K\xC3\xB8kken";
    a.action = "brightness";
    a.step = 5;
    a.press = "toggle";
    cfg::Control b;
    b.entityId = "climate.living";
    b.action = "target_temp";
    b.step = 0.5;
    b.press = "none";
    c.controls = {a, b, a};
    return c;
}

cfg::SecretChanges allSecrets() {
    cfg::SecretChanges s;
    s.wifiPassword.kind = Kind::Set;
    s.wifiPassword.value = "wifi-pass-123";
    s.haToken.kind = Kind::Set;
    s.haToken.value = "token.abc.def";
    s.webPassword.kind = Kind::Set;
    s.webPassword.value = "web pass";
    return s;
}

std::string gApPw;

}  // namespace

void setUp() {}
void tearDown() {}

static void test_first_begin_defaults_and_ap_password() {
    wipeNamespace();
    ConfigStore store(kNs);
    TEST_ASSERT_TRUE(store.begin());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(cfg::LoadStatus::Missing), static_cast<int>(store.loadStatus()));
    TEST_ASSERT_FALSE(store.loadedFromNvs());
    TEST_ASSERT_TRUE(store.config() == cfg::Config());
    gApPw = store.apPassword();
    TEST_ASSERT_EQUAL_size_t(10, gApPw.size());
    const cfg::SecretsPresent p = store.secretsPresent();
    TEST_ASSERT_FALSE(p.wifiPassword);
    TEST_ASSERT_FALSE(p.haToken);
    TEST_ASSERT_FALSE(p.webPassword);
}

static void test_second_begin_loads_saved_defaults() {
    ConfigStore store(kNs);
    TEST_ASSERT_TRUE(store.begin());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(cfg::LoadStatus::Loaded), static_cast<int>(store.loadStatus()));
    TEST_ASSERT_TRUE(store.config() == cfg::Config());
    TEST_ASSERT_EQUAL_STRING(gApPw.c_str(), store.apPassword().c_str());  // not regenerated
}

static void test_save_and_reload_equal() {
    {
        ConfigStore store(kNs);
        TEST_ASSERT_TRUE(store.begin());
        const cfg::ValidationResult r = store.save(sampleConfig(), allSecrets());
        TEST_ASSERT_TRUE_MESSAGE(r.ok, r.message.c_str());
        TEST_ASSERT_TRUE(store.config() == sampleConfig());
        TEST_ASSERT_EQUAL_size_t(3, store.controlCount());
    }
    ConfigStore reloaded(kNs);
    TEST_ASSERT_TRUE(reloaded.begin());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(cfg::LoadStatus::Loaded), static_cast<int>(reloaded.loadStatus()));
    TEST_ASSERT_TRUE(reloaded.config() == sampleConfig());
    TEST_ASSERT_EQUAL_STRING("wifi-pass-123", reloaded.wifiPassword().c_str());
    TEST_ASSERT_EQUAL_STRING("token.abc.def", reloaded.haToken().c_str());
    TEST_ASSERT_EQUAL_STRING("web pass", reloaded.webPassword().c_str());
    const cfg::SecretsPresent p = reloaded.secretsPresent();
    TEST_ASSERT_TRUE(p.wifiPassword);
    TEST_ASSERT_TRUE(p.haToken);
    TEST_ASSERT_TRUE(p.webPassword);
}

static void test_save_invalid_config_rejected_and_nothing_written() {
    ConfigStore store(kNs);
    TEST_ASSERT_TRUE(store.begin());
    cfg::Config bad = sampleConfig();
    bad.device.brightness = 1;
    const cfg::ValidationResult r = store.save(bad, cfg::SecretChanges());
    TEST_ASSERT_FALSE(r.ok);
    TEST_ASSERT_EQUAL_STRING("device.brightness", r.field.c_str());
    TEST_ASSERT_TRUE(store.config() == sampleConfig());
    ConfigStore reloaded(kNs);
    TEST_ASSERT_TRUE(reloaded.begin());
    TEST_ASSERT_TRUE(reloaded.config() == sampleConfig());
}

static void test_apply_patch_token_guard_keeps_store() {
    ConfigStore store(kNs);
    TEST_ASSERT_TRUE(store.begin());
    const char *patch = "{\"ha\":{\"url\":\"http://other.example.com:8123\"}}";
    const cfg::MergeResult r = store.applyPatch(patch, strlen(patch));
    TEST_ASSERT_FALSE(r.ok);
    TEST_ASSERT_EQUAL_STRING("token_required", r.error.message.c_str());
    TEST_ASSERT_FALSE(r.cfgWritten);
    TEST_ASSERT_EQUAL_STRING("http://192.168.1.10:8123", store.config().ha.url.c_str());
    TEST_ASSERT_EQUAL_STRING("token.abc.def", store.haToken().c_str());
}

static void test_apply_patch_new_url_with_token_replaces_token() {
    ConfigStore store(kNs);
    TEST_ASSERT_TRUE(store.begin());
    const char *patch = "{\"ha\":{\"url\":\"https://ha.example.com\",\"token\":\"new.token\"}}";
    const cfg::MergeResult r = store.applyPatch(patch, strlen(patch));
    TEST_ASSERT_TRUE_MESSAGE(r.ok, r.error.message.c_str());
    TEST_ASSERT_TRUE(r.cfgWritten);
    ConfigStore reloaded(kNs);
    TEST_ASSERT_TRUE(reloaded.begin());
    TEST_ASSERT_EQUAL_STRING("https://ha.example.com", reloaded.config().ha.url.c_str());
    TEST_ASSERT_EQUAL_STRING("new.token", reloaded.haToken().c_str());
    TEST_ASSERT_EQUAL_STRING("wifi-pass-123", reloaded.wifiPassword().c_str());  // untouched
}

static void test_apply_patch_url_cleared_removes_token_and_ssid_cleared_removes_password() {
    ConfigStore store(kNs);
    TEST_ASSERT_TRUE(store.begin());
    const char *patch = "{\"ha\":{\"url\":\"\"},\"wifi\":{\"ssid\":\"\"},\"web\":{\"password\":null}}";
    const cfg::MergeResult r = store.applyPatch(patch, strlen(patch));
    TEST_ASSERT_TRUE_MESSAGE(r.ok, r.error.message.c_str());
    ConfigStore reloaded(kNs);
    TEST_ASSERT_TRUE(reloaded.begin());
    const cfg::SecretsPresent p = reloaded.secretsPresent();
    TEST_ASSERT_FALSE(p.haToken);
    TEST_ASSERT_FALSE(p.wifiPassword);
    TEST_ASSERT_FALSE(p.webPassword);
    TEST_ASSERT_TRUE(reloaded.haToken().empty());
    TEST_ASSERT_TRUE(reloaded.config().ha.url.empty());
    TEST_ASSERT_TRUE(reloaded.config().wifi.ssid.empty());
}

static void test_factory_reset_keeps_ap_password() {
    {
        ConfigStore store(kNs);
        TEST_ASSERT_TRUE(store.begin());
        TEST_ASSERT_TRUE(store.save(sampleConfig(), allSecrets()).ok);
        TEST_ASSERT_TRUE(store.factoryReset());
        TEST_ASSERT_TRUE(store.config() == cfg::Config());
        const cfg::SecretsPresent p = store.secretsPresent();
        TEST_ASSERT_FALSE(p.wifiPassword);
        TEST_ASSERT_FALSE(p.haToken);
        TEST_ASSERT_FALSE(p.webPassword);
        TEST_ASSERT_EQUAL_STRING(gApPw.c_str(), store.apPassword().c_str());
    }
    ConfigStore reloaded(kNs);
    TEST_ASSERT_TRUE(reloaded.begin());
    // cfg was removed -> defaults on this "boot".
    TEST_ASSERT_EQUAL_INT(static_cast<int>(cfg::LoadStatus::Missing), static_cast<int>(reloaded.loadStatus()));
    TEST_ASSERT_TRUE(reloaded.config() == cfg::Config());
    TEST_ASSERT_EQUAL_STRING(gApPw.c_str(), reloaded.apPassword().c_str());
    TEST_ASSERT_TRUE(reloaded.wifiPassword().empty());
}

// Step 4 round 2: no-op patches must not write NVS; cfgWritten / secretsWritten report it.
namespace {
cfg::MergeResult patch(ConfigStore &store, const char *json) {
    return store.applyPatch(json, strlen(json));
}

void resetToSample(ConfigStore &store) {
    TEST_ASSERT_TRUE(store.factoryReset());
    TEST_ASSERT_TRUE(store.save(sampleConfig(), allSecrets()).ok);
}
}  // namespace

static void test_noop_empty_patch_writes_nothing() {
    ConfigStore store(kNs);
    TEST_ASSERT_TRUE(store.begin());
    resetToSample(store);
    cfg::MergeResult r = patch(store, "{}");
    TEST_ASSERT_TRUE(r.ok);
    TEST_ASSERT_FALSE(r.cfgWritten);
    TEST_ASSERT_FALSE(r.secretsWritten);
    TEST_ASSERT_FALSE(r.restartWifi);
    // An identical full section is also a no-op.
    r = patch(store, "{\"device\":{\"hostname\":\"test-knob\",\"brightness\":42,\"buzzer\":false}}");
    TEST_ASSERT_TRUE(r.ok);
    TEST_ASSERT_FALSE(r.cfgWritten);
    TEST_ASSERT_FALSE(r.secretsWritten);
    // save() of the identical config + identical secrets: stored state unchanged.
    TEST_ASSERT_TRUE(store.save(sampleConfig(), allSecrets()).ok);
    ConfigStore reloaded(kNs);
    TEST_ASSERT_TRUE(reloaded.begin());
    TEST_ASSERT_TRUE(reloaded.config() == sampleConfig());
    TEST_ASSERT_EQUAL_STRING("token.abc.def", reloaded.haToken().c_str());
    TEST_ASSERT_EQUAL_STRING("wifi-pass-123", reloaded.wifiPassword().c_str());
    TEST_ASSERT_EQUAL_STRING("web pass", reloaded.webPassword().c_str());
}

static void test_noop_same_value_secret_set() {
    ConfigStore store(kNs);
    TEST_ASSERT_TRUE(store.begin());
    resetToSample(store);
    cfg::MergeResult r = patch(store, "{\"web\":{\"password\":\"web pass\"}}");
    TEST_ASSERT_TRUE(r.ok);
    TEST_ASSERT_FALSE(r.cfgWritten);
    TEST_ASSERT_FALSE(r.secretsWritten);
    r = patch(store, "{\"wifi\":{\"ssid\":\"TestNet\",\"password\":\"wifi-pass-123\"}}");
    TEST_ASSERT_TRUE(r.ok);
    TEST_ASSERT_FALSE(r.cfgWritten);
    TEST_ASSERT_FALSE(r.secretsWritten);
    r = patch(store, "{\"ha\":{\"url\":\"http://192.168.1.10:8123\",\"token\":\"token.abc.def\"}}");
    TEST_ASSERT_TRUE(r.ok);
    TEST_ASSERT_FALSE(r.cfgWritten);
    TEST_ASSERT_FALSE(r.secretsWritten);
    TEST_ASSERT_EQUAL_STRING("token.abc.def", store.haToken().c_str());
    TEST_ASSERT_EQUAL_STRING("wifi-pass-123", store.wifiPassword().c_str());
}

static void test_noop_clear_of_absent_secret() {
    ConfigStore store(kNs);
    TEST_ASSERT_TRUE(store.begin());
    resetToSample(store);
    cfg::MergeResult r = patch(store, "{\"web\":{\"password\":null}}");
    TEST_ASSERT_TRUE(r.ok);
    TEST_ASSERT_TRUE(r.secretsWritten);  // removed the stored web_pw
    TEST_ASSERT_FALSE(r.cfgWritten);
    TEST_ASSERT_FALSE(store.secretsPresent().webPassword);
    r = patch(store, "{\"web\":{\"password\":null}}");  // again: nothing to remove
    TEST_ASSERT_TRUE(r.ok);
    TEST_ASSERT_FALSE(r.secretsWritten);
    TEST_ASSERT_FALSE(r.cfgWritten);
    TEST_ASSERT_FALSE(store.secretsPresent().webPassword);
}

static void test_changed_values_write() {
    ConfigStore store(kNs);
    TEST_ASSERT_TRUE(store.begin());
    resetToSample(store);
    cfg::MergeResult r = patch(store, "{\"web\":{\"password\":\"other pass\"}}");
    TEST_ASSERT_TRUE(r.ok);
    TEST_ASSERT_TRUE(r.secretsWritten);
    TEST_ASSERT_FALSE(r.cfgWritten);
    r = patch(store, "{\"device\":{\"hostname\":\"test-knob\",\"brightness\":43,\"buzzer\":false}}");
    TEST_ASSERT_TRUE(r.ok);
    TEST_ASSERT_TRUE(r.cfgWritten);
    TEST_ASSERT_FALSE(r.secretsWritten);
    r = patch(store, "{\"ha\":{\"url\":\"http://192.168.1.10:8123\",\"token\":\"token.changed\"}}");
    TEST_ASSERT_TRUE(r.ok);
    TEST_ASSERT_TRUE(r.secretsWritten);
    TEST_ASSERT_FALSE(r.cfgWritten);
    ConfigStore reloaded(kNs);
    TEST_ASSERT_TRUE(reloaded.begin());
    TEST_ASSERT_EQUAL_STRING("other pass", reloaded.webPassword().c_str());
    TEST_ASSERT_EQUAL_STRING("token.changed", reloaded.haToken().c_str());
    TEST_ASSERT_EQUAL_INT32(43, reloaded.config().device.brightness);
}

static void test_url_cleared_still_removes_stored_token() {
    ConfigStore store(kNs);
    TEST_ASSERT_TRUE(store.begin());
    resetToSample(store);
    cfg::MergeResult r = patch(store, "{\"ha\":{\"url\":\"\"}}");
    TEST_ASSERT_TRUE(r.ok);
    TEST_ASSERT_TRUE(r.cfgWritten);
    TEST_ASSERT_TRUE(r.secretsWritten);
    ConfigStore reloaded(kNs);
    TEST_ASSERT_TRUE(reloaded.begin());
    TEST_ASSERT_FALSE(reloaded.secretsPresent().haToken);
    TEST_ASSERT_TRUE(reloaded.haToken().empty());
    TEST_ASSERT_TRUE(reloaded.config().ha.url.empty());
    // Again with a supplied token while the URL stays empty: forced Clear of an absent
    // token is a no-op, and the token is not stored.
    r = patch(store, "{\"ha\":{\"url\":\"\",\"token\":\"abc\"}}");
    TEST_ASSERT_TRUE(r.ok);
    TEST_ASSERT_FALSE(r.cfgWritten);
    TEST_ASSERT_FALSE(r.secretsWritten);
    TEST_ASSERT_FALSE(store.secretsPresent().haToken);
}

static void test_corrupt_stored_cfg_gives_defaults_and_is_kept() {
    {
        Preferences p;
        TEST_ASSERT_TRUE(p.begin(kNs, false));
        const char junk[] = "{not json";
        p.putBytes("cfg", junk, sizeof(junk) - 1);
        p.end();
    }
    ConfigStore store(kNs);
    TEST_ASSERT_TRUE(store.begin());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(cfg::LoadStatus::Corrupt), static_cast<int>(store.loadStatus()));
    TEST_ASSERT_TRUE(store.config() == cfg::Config());
    Preferences p;
    TEST_ASSERT_TRUE(p.begin(kNs, true));
    TEST_ASSERT_EQUAL_size_t(9, p.getBytesLength("cfg"));  // left in place
    p.end();
}

void setup() {
    Serial.begin(115200);
    delay(3000);  // USB-CDC enumeration
    UNITY_BEGIN();
    RUN_TEST(test_first_begin_defaults_and_ap_password);
    RUN_TEST(test_second_begin_loads_saved_defaults);
    RUN_TEST(test_save_and_reload_equal);
    RUN_TEST(test_save_invalid_config_rejected_and_nothing_written);
    RUN_TEST(test_apply_patch_token_guard_keeps_store);
    RUN_TEST(test_apply_patch_new_url_with_token_replaces_token);
    RUN_TEST(test_apply_patch_url_cleared_removes_token_and_ssid_cleared_removes_password);
    RUN_TEST(test_factory_reset_keeps_ap_password);
    RUN_TEST(test_noop_empty_patch_writes_nothing);
    RUN_TEST(test_noop_same_value_secret_set);
    RUN_TEST(test_noop_clear_of_absent_secret);
    RUN_TEST(test_changed_values_write);
    RUN_TEST(test_url_cleared_still_removes_stored_token);
    RUN_TEST(test_corrupt_stored_cfg_gives_defaults_and_is_kept);
    wipeNamespace();
    UNITY_END();
}

void loop() {}
