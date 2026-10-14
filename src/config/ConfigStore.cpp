#include "config/ConfigStore.h"

#include <Arduino.h>
#include <bootloader_random.h>
#include <esp_random.h>

#include <memory>

#include "hal/PsramAllocator.h"

namespace cfg = core::config;

namespace {

constexpr const char *kKeyConfig = "cfg";
constexpr const char *kKeyWifiPw = "wifi_pw";
constexpr const char *kKeyHaToken = "ha_token";
constexpr const char *kKeyWebPw = "web_pw";
constexpr const char *kKeyApPw = "ap_pw";

}  // namespace

ConfigStore::ConfigStore(const char *nvsNamespace) : namespace_(nvsNamespace) {}

bool ConfigStore::begin() {
    std::lock_guard<std::mutex> lock(mutex_);
    open_ = prefs_.begin(namespace_, false);
    if (!open_) {
        config_ = cfg::Config();
        loadStatus_ = cfg::LoadStatus::Missing;
        Serial.println("ERR CFG nvs open failed, using defaults");
        return false;
    }

    if (!prefs_.isKey(kKeyApPw)) {
        // RF is not up yet, so esp_random() would only be pseudo-random; the bootloader
        // entropy source (SAR ADC noise) makes it a true RNG for this one generation.
        bootloader_random_enable();
        const std::string apPw = cfg::makeApPassword(esp_random);
        bootloader_random_disable();
        prefs_.putString(kKeyApPw, apPw.c_str());
        Serial.println("CFG ap_pw generated");
    }

    // A stored cfg that cannot be read (too large, read error, empty) is Corrupt, not
    // Missing, so it is never overwritten with defaults automatically.
    std::unique_ptr<char[]> json;
    size_t length = 0;
    bool unreadable = false;
    if (prefs_.isKey(kKeyConfig)) {
        const size_t stored = prefs_.getBytesLength(kKeyConfig);
        if (stored == 0 || stored > cfg::kMaxConfigJsonBytes) {
            unreadable = true;
        } else {
            json.reset(new char[stored]);
            length = prefs_.getBytes(kKeyConfig, json.get(), stored);
            unreadable = length != stored;
        }
    }

    cfg::LoadResult result;
    if (unreadable) {
        result.status = cfg::LoadStatus::Corrupt;
    } else {
        result = cfg::fromJson(json.get(), length, PsramAllocator::instance());
    }
    config_ = std::move(result.config);
    loadStatus_ = result.status;
    if (loadStatus_ == cfg::LoadStatus::Loaded) {
        Serial.printf("CFG loaded controls=%u\n", static_cast<unsigned>(config_.controls.size()));
    } else if (loadStatus_ == cfg::LoadStatus::Invalid) {
        Serial.printf("CFG defaults reason=%s field=%s error=%s\n", cfg::loadStatusName(loadStatus_),
                      result.error.field.c_str(), result.error.message.c_str());
    } else {
        Serial.printf("CFG defaults reason=%s\n", cfg::loadStatusName(loadStatus_));
    }

    // First boot: store the defaults so later boots load them. Unusable stored data
    // (corrupt, invalid, newer schema) is left untouched until the next explicit save.
    if (loadStatus_ == cfg::LoadStatus::Missing) {
        saveLocked(config_, cfg::SecretChanges());
        loadStatus_ = cfg::LoadStatus::Missing;  // this boot still ran on defaults
    } else {
        logSecrets();
    }
    return true;
}

cfg::LoadStatus ConfigStore::loadStatus() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return loadStatus_;
}

cfg::Config ConfigStore::config() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return config_;
}

size_t ConfigStore::controlCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return config_.controls.size();
}

cfg::SecretsPresent ConfigStore::secretsPresent() const {
    std::lock_guard<std::mutex> lock(mutex_);
    cfg::SecretsPresent present;
    if (open_) {
        present.wifiPassword = prefs_.isKey(kKeyWifiPw);
        present.haToken = prefs_.isKey(kKeyHaToken);
        present.webPassword = prefs_.isKey(kKeyWebPw);
    }
    return present;
}

namespace {

std::string readString(Preferences &prefs, bool open, const char *key) {
    if (!open || !prefs.isKey(key)) {
        return std::string();
    }
    return std::string(prefs.getString(key).c_str());
}

}  // namespace

std::string ConfigStore::wifiPassword() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return readString(prefs_, open_, kKeyWifiPw);
}

std::string ConfigStore::haToken() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return readString(prefs_, open_, kKeyHaToken);
}

std::string ConfigStore::webPassword() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return readString(prefs_, open_, kKeyWebPw);
}

std::string ConfigStore::apPassword() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return readString(prefs_, open_, kKeyApPw);
}

cfg::ValidationResult ConfigStore::save(const cfg::Config &config,
                                        const cfg::SecretChanges &secrets) {
    std::lock_guard<std::mutex> lock(mutex_);
    return saveLocked(config, secrets);
}

// Caller holds the mutex.
cfg::ValidationResult ConfigStore::saveLocked(const cfg::Config &config,
                                              const cfg::SecretChanges &secrets) {
    cfg::ValidationResult valid = cfg::validate(config);
    if (!valid.ok) {
        return valid;
    }
    const auto writeFailed = cfg::ValidationResult::failure("", "nvs_write_failed");
    if (!open_) {
        return writeFailed;
    }

    // Write order, so that a failed or interrupted save can never pair the new cfg (e.g. a
    // new HA URL) with an old secret; the worst case is "new cfg, secret missing":
    //  (a) remove the old ha_token / wifi_pw if they are being replaced or cleared; abort
    //      before touching cfg if that fails;
    //  (b) write cfg;
    //  (c) write the new secret values (and clear web_pw if requested). web_pw is
    //      overwritten in place: if that write fails the old password stays, which fails
    //      closed.
    if (!removeIfChanging(kKeyHaToken, secrets.haToken) ||
        !removeIfChanging(kKeyWifiPw, secrets.wifiPassword)) {
        Serial.println("ERR CFG remove old secret failed, cfg not written");
        logSecrets();
        return writeFailed;
    }

    const std::string json = cfg::toJson(config, PsramAllocator::instance());
    if (prefs_.putBytes(kKeyConfig, json.data(), json.size()) != json.size()) {
        Serial.println("ERR CFG write cfg failed");
        logSecrets();
        return writeFailed;
    }
    // The in-memory copy follows what NVS now holds (secrets are always read from NVS),
    // so memory and NVS agree even if a secret write below fails.
    config_ = config;
    loadStatus_ = cfg::LoadStatus::Loaded;

    const bool secretsOk = writeSecret(kKeyHaToken, secrets.haToken) &&
                           writeSecret(kKeyWifiPw, secrets.wifiPassword) &&
                           writeSecret(kKeyWebPw, secrets.webPassword);
    Serial.printf("CFG saved controls=%u bytes=%u\n", static_cast<unsigned>(config.controls.size()),
                  static_cast<unsigned>(json.size()));
    logSecrets();
    if (!secretsOk) {
        Serial.println("ERR CFG write secret failed");
        return writeFailed;
    }
    return cfg::ValidationResult::success();
}

cfg::MergeResult ConfigStore::applyPatch(const char *json, size_t length) {
    // One lock across merge and save, so concurrent patches cannot lose updates.
    std::lock_guard<std::mutex> lock(mutex_);
    cfg::MergeResult merged = cfg::mergePatch(config_, json, length, PsramAllocator::instance());
    if (!merged.ok) {
        return merged;
    }
    cfg::ValidationResult saved = saveLocked(merged.config, merged.secrets);
    if (!saved.ok) {
        merged.ok = false;
        merged.error = saved;
    }
    return merged;
}

bool ConfigStore::factoryReset() {
    std::lock_guard<std::mutex> lock(mutex_);
    config_ = cfg::Config();
    loadStatus_ = cfg::LoadStatus::Missing;
    if (!open_) {
        return false;
    }
    bool ok = true;
    for (const char *key : {kKeyConfig, kKeyWifiPw, kKeyHaToken, kKeyWebPw}) {
        if (prefs_.isKey(key) && !prefs_.remove(key)) {
            ok = false;
        }
    }
    Serial.printf("CFG factory reset ok=%d\n", ok ? 1 : 0);
    return ok;
}

// Caller holds the mutex. Removes key if change is Set or Clear; true if nothing to do.
bool ConfigStore::removeIfChanging(const char *key, const cfg::SecretChange &change) {
    if (change.kind == cfg::SecretChange::Kind::Keep || !prefs_.isKey(key)) {
        return true;
    }
    return prefs_.remove(key);
}

// Caller holds the mutex.
bool ConfigStore::writeSecret(const char *key, const cfg::SecretChange &change) {
    switch (change.kind) {
        case cfg::SecretChange::Kind::Set:
            return prefs_.putString(key, change.value.c_str()) == change.value.size();
        case cfg::SecretChange::Kind::Clear:
            return !prefs_.isKey(key) || prefs_.remove(key);
        case cfg::SecretChange::Kind::Keep:
        default:
            return true;
    }
}

// Caller holds the mutex. Only presence and length, never values.
void ConfigStore::logSecrets() const {
    auto len = [this](const char *key) -> int {
        return prefs_.isKey(key) ? static_cast<int>(prefs_.getString(key).length()) : -1;
    };
    const int wifi = len(kKeyWifiPw);
    const int token = len(kKeyHaToken);
    const int web = len(kKeyWebPw);
    Serial.printf("CFG secrets wifi_pw_set=%d ha_token_set=%d web_pw_set=%d ap_pw_set=%d",
                  wifi >= 0 ? 1 : 0, token >= 0 ? 1 : 0, web >= 0 ? 1 : 0,
                  prefs_.isKey(kKeyApPw) ? 1 : 0);
    if (token >= 0) {
        Serial.printf(" token_set len=%d", token);
    }
    Serial.println();
}
