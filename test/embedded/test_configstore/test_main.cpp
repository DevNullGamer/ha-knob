// On-device test for ConfigStore (plan.md §4 Step 3, optional embedded test): save,
// reboot-less reload, equality, secret handling and the factory reset path.
// Uses the NVS namespace "haknob_test" so the real "haknob" config is never touched.
//
// Run: pio test -e t-encoder-pro -f embedded/test_configstore
#include <Arduino.h>
#include <ArduinoJson.h>
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

// Step 5b.3: TLS settings, the 12 KB cfg blob and skip-unchanged with a normalised CA.
namespace {
// Public throwaway test certificate (no key), used only for structure checks.
const char *kCaPem =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIDETCCAfmgAwIBAgIUDpzdr37XTTj3C6/ahUAwRlXcM48wDQYJKoZIhvcNAQEL\n"
    "BQAwGDEWMBQGA1UEAwwNVGVzdCBSU0EgUm9vdDAeFw0yNjEwMTUxODI1MTZaFw0z\n"
    "NjEwMTIxODI1MTZaMBgxFjAUBgNVBAMMDVRlc3QgUlNBIFJvb3QwggEiMA0GCSqG\n"
    "SIb3DQEBAQUAA4IBDwAwggEKAoIBAQC2mzSXgaopTusx389VWeJiuv5E1EKnALu3\n"
    "u7VUr/Tp2FYRyV449gxohLiprmD6hKBuJEsNAOZAefiPi2Y7kXIEFjep6XdzkhXp\n"
    "uzMbAr8WG0Sm7P+58U1vFgOR5DZd3fvf6uxiExQHRPUcS67YxxTSn0d5np0WRpf0\n"
    "eXS0unovQAYfC58cKpCXIqt/NXfcqngEL4GCb4oCl+uiXmRcO2V3Ei1MpFzRP1ik\n"
    "4IcdRjvxdyjy78g3YT9oG3VSXR3CGvD0b8Jayy4pBhpEUinXfjzOukZ6mth9tPMa\n"
    "3aW+h/wye5VWia5/iPON1jbHmH7ObU5UCZjUnIrT3r4UgP0wKvf9AgMBAAGjUzBR\n"
    "MB0GA1UdDgQWBBT+8kBFBdF7OQ3qL6w4LKpHdvGx0jAfBgNVHSMEGDAWgBT+8kBF\n"
    "BdF7OQ3qL6w4LKpHdvGx0jAPBgNVHRMBAf8EBTADAQH/MA0GCSqGSIb3DQEBCwUA\n"
    "A4IBAQAAFM2gAU/uq7Hxnyb/xXtBg034VYIsW8mZuW0sHjgdMQVmKKo9vu8zEhMv\n"
    "Bp5SqwlaYtSmhmjZquRzT8uD2PH1vDqZYrhihMmPmneC0DmoYa7lAnfh5oq6DTyM\n"
    "qXgplPD/ATfRjQNUn2iqe0QGbot84v6VDzaEV6GDtc6MTEV6fAlrJQ958CrbOFH8\n"
    "ahgAucdGzAoTAQzKoIV8St46rzrUJyciKx4WA8WXRo8g1vXyyU4a6q/plsn34CfZ\n"
    "NNmjD5OwsO0oMzWYz06ZtU9mwH5VjNHplT3qSZNpSXlcKHdqMr5P5HA9rrX+nwoh\n"
    "arP8Z30Fpkin39mlKbxmze8tgtL3\n"
    "-----END CERTIFICATE-----\n";

// A structurally valid (DER shape only) CERTIFICATE block of exactly 4096 bytes: the
// maximum ha.ca_pem. Base64 of SEQUENCE { SEQUENCE { zeros } }, 62 lines.
std::string maxCaPem() {
    const size_t bodyChars = 3980;
    const size_t lines = 4096 - 28 - 26 - bodyChars;
    std::string der(bodyChars / 4 * 3, '\0');
    const size_t n = der.size();
    der[0] = 0x30;
    der[1] = static_cast<char>(0x82);
    der[2] = static_cast<char>((n - 4) >> 8);
    der[3] = static_cast<char>((n - 4) & 0xFF);
    der[4] = 0x30;
    der[5] = static_cast<char>(0x82);
    der[6] = static_cast<char>((n - 8) >> 8);
    der[7] = static_cast<char>((n - 8) & 0xFF);
    static const char *tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string b64;
    for (size_t i = 0; i < n; i += 3) {
        const uint32_t v = (static_cast<uint8_t>(der[i]) << 16) | (static_cast<uint8_t>(der[i + 1]) << 8) |
                           static_cast<uint8_t>(der[i + 2]);
        b64 += tbl[(v >> 18) & 63];
        b64 += tbl[(v >> 12) & 63];
        b64 += tbl[(v >> 6) & 63];
        b64 += tbl[v & 63];
    }
    std::string out = "-----BEGIN CERTIFICATE-----\n";
    const size_t width = (b64.size() + lines - 1) / lines;
    for (size_t pos = 0; pos < b64.size(); pos += width) out += b64.substr(pos, width) + "\n";
    return out + "-----END CERTIFICATE-----\n";
}

// 24 controls with long entity ids and a 4096-byte CA: the serialised cfg is just under
// kMaxConfigJsonBytes (12288), so the blob is about 12 KB.
cfg::Config bigConfig(char labelFill = 'L') {
    cfg::Config c = sampleConfig();
    c.ha.url = "https://ha.example.com:8123";
    c.ha.caPem = maxCaPem();
    for (size_t idLen = 255; idLen >= 40; --idLen) {
        c.controls.clear();
        const std::string id = "media_player." + std::string(idLen - 13, 'x');
        for (int i = 0; i < 24; ++i) {
            cfg::Control k;
            k.entityId = id;
            k.label = std::string(48, labelFill);
            k.action = "volume";
            k.step = 2;
            k.press = "play_pause";
            c.controls.push_back(k);
        }
        if (cfg::toJson(c).size() <= cfg::kMaxConfigJsonBytes) break;
    }
    return c;
}

size_t storedCfgBytes() {
    Preferences p;
    TEST_ASSERT_TRUE(p.begin(kNs, true));
    const size_t n = p.getBytesLength("cfg");
    p.end();
    return n;
}

std::string patchWithCa(const char *url, const std::string &ca, bool insecure, const char *tokenJson) {
    JsonDocument doc;
    doc["ha"]["url"] = url;
    doc["ha"]["ca_pem"] = ca;
    doc["ha"]["tls_insecure"] = insecure;
    std::string out;
    serializeJson(doc, out);
    if (tokenJson != nullptr) {
        out.resize(out.size() - 2);
        out += std::string(",\"token\":") + tokenJson + "}}";
    }
    return out;
}
}  // namespace

static void test_big_blob_save_reload_twice_with_secrets() {
    {
        ConfigStore store(kNs);
        TEST_ASSERT_TRUE(store.begin());
        TEST_ASSERT_TRUE(store.factoryReset());
    }
    const cfg::Config first = bigConfig('L');
    const size_t firstBytes = cfg::toJson(first).size();
    TEST_ASSERT_TRUE_MESSAGE(firstBytes >= 11500 && firstBytes <= cfg::kMaxConfigJsonBytes,
                             std::to_string(firstBytes).c_str());
    {
        ConfigStore store(kNs);
        TEST_ASSERT_TRUE(store.begin());
        const cfg::ValidationResult r = store.save(first, allSecrets());
        TEST_ASSERT_TRUE_MESSAGE(r.ok, (r.field + "/" + r.message).c_str());
    }
    TEST_ASSERT_EQUAL_size_t(firstBytes, storedCfgBytes());
    {
        ConfigStore reloaded(kNs);
        TEST_ASSERT_TRUE(reloaded.begin());
        TEST_ASSERT_EQUAL_INT(static_cast<int>(cfg::LoadStatus::Loaded), static_cast<int>(reloaded.loadStatus()));
        TEST_ASSERT_TRUE(reloaded.config() == first);
        TEST_ASSERT_EQUAL_STRING("token.abc.def", reloaded.haToken().c_str());
        TEST_ASSERT_EQUAL_STRING("wifi-pass-123", reloaded.wifiPassword().c_str());
    }
    // Second save: different labels (same size) and changed secrets; the old blob must be
    // replaced safely.
    cfg::Config second = bigConfig('M');
    second.device.brightness = 61;
    cfg::SecretChanges s = allSecrets();
    s.haToken.value = "token.second";
    s.webPassword.value = "second pass";
    {
        ConfigStore store(kNs);
        TEST_ASSERT_TRUE(store.begin());
        const cfg::ValidationResult r = store.save(second, s);
        TEST_ASSERT_TRUE_MESSAGE(r.ok, (r.field + "/" + r.message).c_str());
    }
    ConfigStore reloaded(kNs);
    TEST_ASSERT_TRUE(reloaded.begin());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(cfg::LoadStatus::Loaded), static_cast<int>(reloaded.loadStatus()));
    TEST_ASSERT_TRUE(reloaded.config() == second);
    TEST_ASSERT_TRUE(reloaded.config() != first);
    TEST_ASSERT_EQUAL_STRING("token.second", reloaded.haToken().c_str());
    TEST_ASSERT_EQUAL_STRING("second pass", reloaded.webPassword().c_str());
    TEST_ASSERT_EQUAL_STRING("wifi-pass-123", reloaded.wifiPassword().c_str());
}

static void test_big_blob_applypatch_ca_resend_skips_write() {
    ConfigStore store(kNs);
    TEST_ASSERT_TRUE(store.begin());
    TEST_ASSERT_TRUE(store.factoryReset());
    const cfg::Config big = bigConfig('L');
    TEST_ASSERT_TRUE(store.save(big, allSecrets()).ok);

    // Same CA with CRLF and trailing blanks, and the same token: no NVS write at all.
    std::string crlf;
    for (char ch : big.ha.caPem) {
        if (ch == '\n') crlf += " \t\r\n";
        else crlf.push_back(ch);
    }
    std::string patchJson = patchWithCa("https://ha.example.com:8123", crlf, false, "\"token.abc.def\"");
    cfg::MergeResult r = patch(store, patchJson.c_str());
    TEST_ASSERT_TRUE_MESSAGE(r.ok, (r.error.field + "/" + r.error.message).c_str());
    TEST_ASSERT_FALSE(r.cfgWritten);
    TEST_ASSERT_FALSE(r.secretsWritten);
    // Without a token in the patch: the token is kept and nothing is written either.
    patchJson = patchWithCa("https://ha.example.com:8123", "\r\n" + crlf + "\r\n", false, nullptr);
    r = patch(store, patchJson.c_str());
    TEST_ASSERT_TRUE_MESSAGE(r.ok, (r.error.field + "/" + r.error.message).c_str());
    TEST_ASSERT_FALSE(r.cfgWritten);
    TEST_ASSERT_FALSE(r.secretsWritten);
    TEST_ASSERT_EQUAL_STRING("token.abc.def", store.haToken().c_str());
    TEST_ASSERT_TRUE(store.config() == big);

    // Weakening (insecure on, no token) is refused and writes nothing.
    patchJson = patchWithCa("https://ha.example.com:8123", "", true, nullptr);
    r = patch(store, patchJson.c_str());
    TEST_ASSERT_FALSE(r.ok);
    TEST_ASSERT_EQUAL_STRING("token_required", r.error.message.c_str());
    TEST_ASSERT_FALSE(r.cfgWritten);
    TEST_ASSERT_FALSE(r.secretsWritten);
    TEST_ASSERT_TRUE(store.config() == big);
    TEST_ASSERT_EQUAL_STRING("token.abc.def", store.haToken().c_str());

    // Tightening (CA removed) keeps the token and does write the cfg blob (it shrinks).
    patchJson = patchWithCa("https://ha.example.com:8123", "", false, nullptr);
    r = patch(store, patchJson.c_str());
    TEST_ASSERT_TRUE_MESSAGE(r.ok, (r.error.field + "/" + r.error.message).c_str());
    TEST_ASSERT_TRUE(r.cfgWritten);
    TEST_ASSERT_FALSE(r.secretsWritten);
    TEST_ASSERT_EQUAL_STRING("token.abc.def", store.haToken().c_str());
    TEST_ASSERT_TRUE(store.config().ha.caPem.empty());

    // Re-adding a CA needs a token; then it reloads equal.
    patchJson = patchWithCa("https://ha.example.com:8123", kCaPem, false, nullptr);
    r = patch(store, patchJson.c_str());
    TEST_ASSERT_FALSE(r.ok);
    patchJson = patchWithCa("https://ha.example.com:8123", kCaPem, false, "\"token.new\"");
    r = patch(store, patchJson.c_str());
    TEST_ASSERT_TRUE_MESSAGE(r.ok, (r.error.field + "/" + r.error.message).c_str());
    TEST_ASSERT_TRUE(r.cfgWritten);
    TEST_ASSERT_TRUE(r.secretsWritten);
    ConfigStore reloaded(kNs);
    TEST_ASSERT_TRUE(reloaded.begin());
    TEST_ASSERT_EQUAL_STRING(kCaPem, reloaded.config().ha.caPem.c_str());
    TEST_ASSERT_EQUAL_STRING("token.new", reloaded.haToken().c_str());
    TEST_ASSERT_FALSE(reloaded.config().ha.tlsInsecure);
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
    RUN_TEST(test_big_blob_save_reload_twice_with_secrets);
    RUN_TEST(test_big_blob_applypatch_ca_resend_skips_write);
    RUN_TEST(test_corrupt_stored_cfg_gives_defaults_and_is_kept);
    wipeNamespace();
    UNITY_END();
}

void loop() {}
