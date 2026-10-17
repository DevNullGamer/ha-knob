#pragma once

#include <ArduinoJson.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "ha/HaUrl.h"

// The device configuration: data model, JSON (de)serialisation, validation, redaction and
// patch merging (plan.md §3.4). Pure: persistence lives in src/config/ConfigStore.
//
// Stored (non-secret) JSON, schema 1:
//   {"schema":1,
//    "device":{"hostname","brightness","dim_after_s","dim_level","off_after_s","buzzer",
//              "detent_click","encoder_reverse","accel","send_interval_ms"},
//    "wifi":{"ssid"}, "ha":{"url","ca_pem","tls_insecure"},
//    "controls":[{"entity_id","label","action","step","press"}]}
// Secrets (Wi-Fi password, HA token, web password) are never part of Config; they are
// stored separately and only appear as *_set flags in the redacted JSON.
//
// Every function that builds a JsonDocument takes an optional ArduinoJson allocator
// (nullptr = ArduinoJson's default; the device passes a PSRAM allocator).
namespace core {
namespace config {

constexpr int kSchemaVersion = 1;
constexpr size_t kMaxControls = 24;
constexpr size_t kMaxConfigJsonBytes = 12288;  // NVS blob limit for "cfg" (was 8192 up to v0.5.1)
constexpr size_t kMaxHostnameLen = 32;
constexpr size_t kMaxSsidBytes = 32;
constexpr size_t kMinWifiPasswordLen = 8;
constexpr size_t kMaxWifiPasswordLen = 63;
constexpr size_t kMaxHaTokenLen = 512;
constexpr size_t kMaxWebPasswordLen = 64;
constexpr size_t kMaxEntityIdLen = 255;
constexpr size_t kMaxLabelBytes = 48;
constexpr size_t kMaxCaPemBytes = 4096;  // ha.ca_pem after normalisation
constexpr size_t kMaxCaCerts = 3;        // CERTIFICATE blocks in ha.ca_pem

struct DeviceConfig {
    std::string hostname = "ha-knob";
    int32_t brightness = 80;        // 5-100 %
    uint32_t dimAfterS = 30;        // 0-86400, 0 = never
    int32_t dimLevel = 15;          // 0-100 %
    uint32_t offAfterS = 300;       // 0-86400, 0 = never
    bool buzzer = true;
    bool detentClick = false;       // a click on every detent (only with buzzer)
    bool encoderReverse = false;
    bool accel = true;
    uint32_t sendIntervalMs = 250;  // 100-2000

    bool operator==(const DeviceConfig &o) const;
};

struct WifiConfig {
    std::string ssid;  // empty = not configured

    bool operator==(const WifiConfig &o) const { return ssid == o.ssid; }
};

struct HaConfig {
    std::string url;  // empty = not configured; else http(s)://host[:port][/prefix]
    // TLS settings for an https URL (both must stay at their defaults otherwise):
    //  - caPem: "" = verify against the built-in public CA bundle; else 1-3 PEM CERTIFICATE
    //    blocks to verify against instead, stored normalised (see normalizePem)
    //  - tlsInsecure: skip certificate verification (excludes caPem)
    std::string caPem;
    bool tlsInsecure = false;

    bool operator==(const HaConfig &o) const {
        return url == o.url && caPem == o.caPem && tlsInsecure == o.tlsInsecure;
    }
};

struct Control {
    std::string entityId;
    std::string label;  // empty = use the entity's friendly name
    std::string action;
    double step = 0;
    std::string press;

    bool operator==(const Control &o) const;
};

struct Config {
    DeviceConfig device;
    WifiConfig wifi;
    HaConfig ha;
    std::vector<Control> controls;  // duplicates of one entity_id are allowed

    bool operator==(const Config &o) const;
    bool operator!=(const Config &o) const { return !(*this == o); }
};

// Which secrets are stored (for the redacted JSON). Values are never passed around here.
struct SecretsPresent {
    bool wifiPassword = false;
    bool haToken = false;
    bool webPassword = false;
};

struct ValidationResult {
    bool ok = true;
    std::string field;    // e.g. "device.hostname", "controls[3].step"; empty if ok
    std::string message;  // short machine-friendly reason, e.g. "out_of_range"

    static ValidationResult success() { return {}; }
    static ValidationResult failure(std::string field, std::string message) {
        return {false, std::move(field), std::move(message)};
    }
};

// ---- Loading ---------------------------------------------------------------------------

enum class LoadStatus : uint8_t {
    Loaded,        // valid stored config
    Missing,       // nothing stored -> defaults
    Corrupt,       // not JSON / not an object -> defaults
    SchemaTooNew,  // schema > kSchemaVersion -> defaults
    Invalid,       // wrong types or failed validation -> defaults
};

const char *loadStatusName(LoadStatus status);

struct LoadResult {
    Config config;  // always valid: the stored config, or defaults
    LoadStatus status = LoadStatus::Missing;
    ValidationResult error;  // why it was rejected (Invalid only)
};

// Parses stored JSON. Missing sections and keys take their defaults; unknown keys are
// ignored; an older schema is migrated. Anything wrong gives defaults plus the reason.
LoadResult fromJson(const char *json, size_t length, ArduinoJson::Allocator *allocator = nullptr);

// Upgrades an older stored document in place to kSchemaVersion. Returns false if the
// schema is newer than supported or not a number. (Schema 1 is the first version, so
// today this only stamps a missing/zero schema as 1.) fromJson reports a non-numeric
// schema as Invalid and a newer one as SchemaTooNew.
bool migrate(JsonDocument &doc);

// ---- Saving and the API ----------------------------------------------------------------

// Compact stored JSON (no secrets).
std::string toJson(const Config &config, ArduinoJson::Allocator *allocator = nullptr);

// Same as toJson plus "wifi.password_set", "ha.token_set" and "web":{"password_set"}.
std::string toRedactedJson(const Config &config, const SecretsPresent &present,
                           ArduinoJson::Allocator *allocator = nullptr);

// Checks every rule of plan.md §3.4, including the DomainActions lookup for each control
// and the serialised size (<= kMaxConfigJsonBytes). Returns the first failure.
ValidationResult validate(const Config &config);

// ---- Patching (POST /api/config) -------------------------------------------------------

struct SecretChange {
    enum class Kind : uint8_t { Keep, Set, Clear };
    Kind kind = Kind::Keep;
    std::string value;  // for Set only

    bool operator==(const SecretChange &o) const { return kind == o.kind && value == o.value; }
};

struct SecretChanges {
    SecretChange wifiPassword;
    SecretChange haToken;
    SecretChange webPassword;
};

struct MergeResult {
    bool ok = false;
    ValidationResult error;  // set when !ok
    Config config;           // merged config when ok (unchanged copy of current otherwise)
    SecretChanges secrets;   // what to do with the stored secrets
    // wifi.ssid changed, or wifi.password is set/cleared for a configured SSID. Not set
    // when the SSID was and stays "" (the forced password Clear then changes nothing Wi-Fi
    // uses). A password Set to the value already stored still reports true: mergePatch
    // cannot see stored secrets, so the device compares the actual credentials.
    bool restartWifi = false;
    // Set by ConfigStore::applyPatch (never by mergePatch): the cfg blob was written,
    // so the stored config may have changed even if a later secret write failed. False
    // when the serialised config equals the stored blob (nothing is rewritten then).
    bool cfgWritten = false;
    // Set by ConfigStore::applyPatch (never by mergePatch): a secret was written or
    // removed (an unchanged secret is not rewritten), even if a later write failed.
    bool secretsWritten = false;
};

// Applies a JSON patch to the current config:
//  - "device", "wifi", "ha", "controls": a present section REPLACES that section (keys
//    missing inside it take their defaults); absent sections are kept.
//  - Secrets "wifi.password", "ha.token", "web.password": absent or "" = keep;
//    a string = set (validated); null = clear (e.g. an open Wi-Fi network, or turning
//    the web password off).
//  - Wi-Fi guard: if wifi.ssid changes (byte for byte) to a non-empty value, the same
//    patch must carry wifi.password as a string (new password) or null (open network),
//    else "wifi.password"/"password_required". Whenever the patched SSID is ""
//    (unconfigured), the password is cleared, even if the patch supplied one. An
//    unchanged non-empty SSID keeps the normal secret rules.
//  - Token guard: if ha.url moves to a different host:port (lower-cased host, default
//    port 80/443) or is downgraded from https to http, a non-empty new URL requires a new
//    ha.token in the same patch (else "ha.token"/"token_required"). Whenever the patched
//    URL is "" (unconfigured), the token is cleared, even if the patch supplied one. A
//    path-only change or an http -> https upgrade on the same host:port keeps the token.
//    TLS weakening: if the old and new URL are both https on the same host:port, turning
//    ha.tls_insecure on (false -> true) or setting ha.ca_pem to a non-empty value that
//    differs from the stored one (after normalisation) also requires a new ha.token in the
//    same patch. Tightening (CA removed, insecure turned off) keeps the token. The TLS
//    fields of the merged ha section are checked before the guard, so an unusable CA
//    reports its own error rather than token_required.
//  - The merged config must pass validate().
// Errors use the same {field, message} shape as validate(); malformed JSON gives
// field "" / "invalid_json".
MergeResult mergePatch(const Config &current, const char *patchJson, size_t length,
                       ArduinoJson::Allocator *allocator = nullptr);

// ---- Helpers (exposed for tests and reuse) ---------------------------------------------

constexpr size_t kApPasswordLen = 10;

// Random WPA2 password for the setup AP from a 32-character alphabet without look-alikes
// (no l, o, 0, 1). random32 is the entropy source (esp_random on the device).
std::string makeApPassword(uint32_t (*random32)(), size_t length = kApPasswordLen);

bool isValidHostname(const std::string &hostname);
bool isValidEntityId(const std::string &entityId);
// Well-formed UTF-8 without control characters.
bool isValidLabelText(const std::string &text);

// ha.ca_pem normalisation (applied when parsing stored JSON and patches): CRLF and CR ->
// LF, trailing spaces and tabs removed from every line, surrounding whitespace trimmed,
// then exactly one trailing LF; whitespace-only -> "".
// A PEM resent with other line endings therefore compares equal to the stored one.
std::string normalizePem(const std::string &text);

// Checks a normalised, non-empty ha.ca_pem (field "ha.ca_pem"): <= kMaxCaPemBytes
// ("length"); nothing but 1..kMaxCaCerts "-----BEGIN CERTIFICATE-----" ...
// "-----END CERTIFICATE-----" blocks separated by blank space ("not_a_certificate", e.g. a
// PRIVATE KEY block; "too_many"); each body strict Base64 ("invalid_base64") decoding to
// one DER SEQUENCE whose outer length matches the decoded size ("invalid_der") and whose
// first element is itself a SEQUENCE fitting inside it, the tbsCertificate (else
// "not_a_certificate": a private key relabelled as CERTIFICATE starts with an INTEGER).
// Whether the certificate is usable is only known when mbedTLS parses it on connect.
ValidationResult checkCaPem(const std::string &pem);

// The ha.url parser is core::ha::parseHaUrl (ha/HaUrl.h), the only URL parser; these
// names remain for existing callers.
using UrlEndpoint = core::ha::HaUrl;
inline UrlEndpoint parseUrlEndpoint(const std::string &url) { return core::ha::parseHaUrl(url); }

}  // namespace config
}  // namespace core
