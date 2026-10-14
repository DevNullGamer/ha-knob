#pragma once

#include <Preferences.h>

#include <mutex>
#include <string>

#include "config/ConfigModel.h"

// Persists the configuration in NVS (namespace "haknob" by default):
//   cfg       blob    non-secret config JSON (core::config::toJson), <= 8 KB
//   wifi_pw   string  Wi-Fi password            (secret)
//   ha_token  string  HA long-lived token       (secret)
//   web_pw    string  optional web UI password  (secret)
//   ap_pw     string  setup-AP WPA2 password, generated on first boot, kept on factory reset
//
// Thread-safe: every public method takes the internal mutex, so the web server task
// (Step 4) and the main loop can both use it. Secret values are never logged.
class ConfigStore {
public:
    explicit ConfigStore(const char *nvsNamespace = "haknob");

    // Opens NVS, generates ap_pw if missing and loads the config (defaults if missing or
    // unusable). Logs "CFG defaults reason=<..>" or "CFG loaded controls=N". When nothing
    // was stored, the defaults are saved so the next boot loads them. Returns false only
    // if NVS cannot be opened (the defaults are still available then).
    bool begin();

    // How begin() obtained the config.
    core::config::LoadStatus loadStatus() const;
    bool loadedFromNvs() const { return loadStatus() == core::config::LoadStatus::Loaded; }

    core::config::Config config() const;  // copy
    size_t controlCount() const;          // cheap, for status lines
    core::config::SecretsPresent secretsPresent() const;

    // Secret getters, for the modules that need the values (Wi-Fi, HA client, web auth).
    std::string wifiPassword() const;
    std::string haToken() const;
    std::string webPassword() const;
    std::string apPassword() const;

    // Validates, then: removes the old ha_token / wifi_pw if they change (abort on
    // failure), writes cfg, then writes the new secrets. A failed or interrupted save can
    // leave "new cfg, secret missing" but never a new URL with the old token. The
    // in-memory copy is updated once cfg is written (it always mirrors NVS). Returns the
    // validation error or a failure with message "nvs_write_failed".
    core::config::ValidationResult save(const core::config::Config &config,
                                        const core::config::SecretChanges &secrets = {});

    // mergePatch() against the current config, then save() on success.
    core::config::MergeResult applyPatch(const char *json, size_t length);

    // Removes cfg and all secrets except ap_pw; the in-memory config becomes defaults.
    bool factoryReset();

private:
    core::config::ValidationResult saveLocked(const core::config::Config &config,
                                              const core::config::SecretChanges &secrets);
    bool removeIfChanging(const char *key, const core::config::SecretChange &change);
    bool writeSecret(const char *key, const core::config::SecretChange &change);
    void logSecrets() const;

    const char *namespace_;
    mutable std::mutex mutex_;
    mutable Preferences prefs_;
    bool open_ = false;
    core::config::Config config_;
    core::config::LoadStatus loadStatus_ = core::config::LoadStatus::Missing;
};
