#include "config/ConfigModel.h"

#include <cmath>
#include <cstring>

#include "control/DomainActions.h"

namespace core {
namespace config {

namespace {

namespace da = core::domain_actions;

constexpr uint32_t kMaxTimerS = 86400;

JsonDocument makeDoc(ArduinoJson::Allocator *allocator) {
    return allocator != nullptr ? JsonDocument(allocator) : JsonDocument();
}

// Finds a member, distinguishing "absent" from "present with value null".
bool findMember(JsonObjectConst obj, const char *key, JsonVariantConst &out) {
    for (JsonPairConst kv : obj) {
        if (std::strcmp(kv.key().c_str(), key) == 0) {
            out = kv.value();
            return true;
        }
    }
    return false;
}

// Strict reader for one JSON object: an absent (or null) key keeps the caller's default,
// a present key of the wrong type records an error. Only the first error is kept.
class SectionReader {
public:
    SectionReader(JsonObjectConst obj, std::string path) : obj_(obj), path_(std::move(path)) {}

    void str(const char *key, std::string &out) {
        JsonVariantConst v = obj_[key];
        if (v.isNull()) return;
        if (!v.is<const char *>()) return fail(key, "not_a_string");
        out = v.as<const char *>();
    }

    void i32(const char *key, int32_t &out) {
        long long value = 0;
        if (integer(key, value)) {
            if (value < INT32_MIN || value > INT32_MAX) return fail(key, "out_of_range");
            out = static_cast<int32_t>(value);
        }
    }

    void u32(const char *key, uint32_t &out) {
        long long value = 0;
        if (integer(key, value)) {
            if (value < 0 || value > static_cast<long long>(UINT32_MAX)) {
                return fail(key, "out_of_range");
            }
            out = static_cast<uint32_t>(value);
        }
    }

    void boolean(const char *key, bool &out) {
        JsonVariantConst v = obj_[key];
        if (v.isNull()) return;
        if (!v.is<bool>()) return fail(key, "not_a_boolean");
        out = v.as<bool>();
    }

    void number(const char *key, double &out) {
        JsonVariantConst v = obj_[key];
        if (v.isNull()) return;
        if (!v.is<double>()) return fail(key, "not_a_number");
        out = v.as<double>();
    }

    // Returns true if key is present (non-null); records "required" otherwise.
    bool require(const char *key) {
        if (!obj_[key].isNull()) return true;
        fail(key, "required");
        return false;
    }

    const ValidationResult &result() const { return result_; }

private:
    bool integer(const char *key, long long &out) {
        JsonVariantConst v = obj_[key];
        if (v.isNull()) return false;
        if (!v.is<long long>()) {
            fail(key, "not_an_integer");
            return false;
        }
        out = v.as<long long>();
        return true;
    }

    void fail(const char *key, const char *message) {
        if (result_.ok) {
            result_ = ValidationResult::failure(path_ + "." + key, message);
        }
    }

    JsonObjectConst obj_;
    std::string path_;
    ValidationResult result_;
};

ValidationResult notAnObject(const std::string &field) {
    return ValidationResult::failure(field, "not_an_object");
}

// Each parser starts from defaults (section-replace semantics) and checks types only;
// value ranges are checked by validate().
ValidationResult parseDevice(JsonVariantConst v, DeviceConfig &out) {
    out = DeviceConfig();
    if (!v.is<JsonObjectConst>()) return notAnObject("device");
    SectionReader r(v.as<JsonObjectConst>(), "device");
    r.str("hostname", out.hostname);
    r.i32("brightness", out.brightness);
    r.u32("dim_after_s", out.dimAfterS);
    r.i32("dim_level", out.dimLevel);
    r.u32("off_after_s", out.offAfterS);
    r.boolean("buzzer", out.buzzer);
    r.boolean("encoder_reverse", out.encoderReverse);
    r.boolean("accel", out.accel);
    r.u32("send_interval_ms", out.sendIntervalMs);
    return r.result();
}

ValidationResult parseWifi(JsonVariantConst v, WifiConfig &out) {
    out = WifiConfig();
    if (!v.is<JsonObjectConst>()) return notAnObject("wifi");
    SectionReader r(v.as<JsonObjectConst>(), "wifi");
    r.str("ssid", out.ssid);
    return r.result();
}

ValidationResult parseHa(JsonVariantConst v, HaConfig &out) {
    out = HaConfig();
    if (!v.is<JsonObjectConst>()) return notAnObject("ha");
    SectionReader r(v.as<JsonObjectConst>(), "ha");
    r.str("url", out.url);
    return r.result();
}

// Missing action / step / press take the domain's defaults (when the domain is known).
ValidationResult parseControl(JsonVariantConst v, size_t index, Control &out) {
    const std::string path = "controls[" + std::to_string(index) + "]";
    out = Control();
    if (!v.is<JsonObjectConst>()) return notAnObject(path);
    SectionReader r(v.as<JsonObjectConst>(), path);
    if (r.require("entity_id")) r.str("entity_id", out.entityId);
    r.str("label", out.label);
    r.str("action", out.action);
    r.str("press", out.press);
    bool hasStep = !v["step"].isNull();
    r.number("step", out.step);
    if (!r.result().ok) return r.result();

    const da::DomainSpec *domain = da::findDomain(da::domainOf(out.entityId));
    if (domain != nullptr) {
        if (out.action.empty()) out.action = domain->actions[0].name;
        if (out.press.empty()) out.press = domain->presses[0];
        const da::ActionSpec *action = da::findAction(*domain, out.action);
        if (!hasStep && action != nullptr) out.step = action->stepDefault;
    }
    return ValidationResult::success();
}

ValidationResult parseControls(JsonVariantConst v, std::vector<Control> &out) {
    out.clear();
    if (!v.is<JsonArrayConst>()) return ValidationResult::failure("controls", "not_an_array");
    JsonArrayConst array = v.as<JsonArrayConst>();
    if (array.size() > kMaxControls) return ValidationResult::failure("controls", "too_many");
    size_t index = 0;
    for (JsonVariantConst item : array) {
        Control control;
        ValidationResult r = parseControl(item, index, control);
        if (!r.ok) return r;
        out.push_back(std::move(control));
        ++index;
    }
    return ValidationResult::success();
}

void writeConfig(JsonObject root, const Config &c) {
    root["schema"] = kSchemaVersion;
    JsonObject device = root["device"].to<JsonObject>();
    device["hostname"] = c.device.hostname;
    device["brightness"] = c.device.brightness;
    device["dim_after_s"] = c.device.dimAfterS;
    device["dim_level"] = c.device.dimLevel;
    device["off_after_s"] = c.device.offAfterS;
    device["buzzer"] = c.device.buzzer;
    device["encoder_reverse"] = c.device.encoderReverse;
    device["accel"] = c.device.accel;
    device["send_interval_ms"] = c.device.sendIntervalMs;
    root["wifi"].to<JsonObject>()["ssid"] = c.wifi.ssid;
    root["ha"].to<JsonObject>()["url"] = c.ha.url;
    JsonArray controls = root["controls"].to<JsonArray>();
    for (const Control &control : c.controls) {
        JsonObject o = controls.add<JsonObject>();
        o["entity_id"] = control.entityId;
        o["label"] = control.label;
        o["action"] = control.action;
        o["step"] = control.step;
        o["press"] = control.press;
    }
}

std::string serialize(const JsonDocument &doc) {
    std::string out;
    serializeJson(doc, out);
    return out;
}

bool hasControlChars(const std::string &s) {
    for (unsigned char c : s) {
        if (c < 0x20 || c == 0x7F) return true;
    }
    return false;
}

bool isPrintableAscii(const std::string &s) {
    for (unsigned char c : s) {
        if (c < 0x20 || c > 0x7E) return false;
    }
    return true;
}

ValidationResult checkWifiPassword(const std::string &pw) {
    if (pw.size() < kMinWifiPasswordLen || pw.size() > kMaxWifiPasswordLen) {
        return ValidationResult::failure("wifi.password", "length");
    }
    if (!isPrintableAscii(pw)) return ValidationResult::failure("wifi.password", "invalid_chars");
    return ValidationResult::success();
}

ValidationResult checkHaToken(const std::string &token) {
    if (token.size() > kMaxHaTokenLen) return ValidationResult::failure("ha.token", "length");
    for (unsigned char c : token) {
        if (c <= 0x20 || c > 0x7E) return ValidationResult::failure("ha.token", "invalid_chars");
    }
    return ValidationResult::success();
}

ValidationResult checkWebPassword(const std::string &pw) {
    if (pw.size() > kMaxWebPasswordLen) return ValidationResult::failure("web.password", "length");
    if (hasControlChars(pw)) return ValidationResult::failure("web.password", "invalid_chars");
    return ValidationResult::success();
}

// Reads a secret: absent or "" = Keep, null = Clear, other string = Set (checked).
ValidationResult readSecret(JsonObjectConst obj, const char *key, const std::string &field,
                            ValidationResult (*check)(const std::string &), SecretChange &out) {
    out = SecretChange();
    JsonVariantConst v;
    if (!findMember(obj, key, v)) return ValidationResult::success();
    if (v.isNull()) {
        out.kind = SecretChange::Kind::Clear;
        return ValidationResult::success();
    }
    if (!v.is<const char *>()) return ValidationResult::failure(field, "not_a_string");
    std::string value = v.as<const char *>();
    if (value.empty()) return ValidationResult::success();
    ValidationResult r = check(value);
    if (!r.ok) return r;
    out.kind = SecretChange::Kind::Set;
    out.value = std::move(value);
    return ValidationResult::success();
}

// True if the stored token must not follow the URL change: a different host:port, or a
// TLS downgrade (https -> http would send the token in cleartext). A path-only change or
// a TLS upgrade on the same host:port keeps the token.
bool tokenEndpointChanged(const std::string &oldUrl, const std::string &newUrl) {
    if (oldUrl.empty() || newUrl.empty()) return oldUrl.empty() != newUrl.empty();
    const UrlEndpoint a = parseUrlEndpoint(oldUrl);
    const UrlEndpoint b = parseUrlEndpoint(newUrl);
    if (!a.ok || !b.ok) return oldUrl != newUrl;
    return a.host != b.host || a.port != b.port || (a.tls && !b.tls);
}

bool isLowerAlpha(char c) { return c >= 'a' && c <= 'z'; }
bool isDigit(char c) { return c >= '0' && c <= '9'; }

}  // namespace

// ---- Equality --------------------------------------------------------------------------

bool DeviceConfig::operator==(const DeviceConfig &o) const {
    return hostname == o.hostname && brightness == o.brightness && dimAfterS == o.dimAfterS &&
           dimLevel == o.dimLevel && offAfterS == o.offAfterS && buzzer == o.buzzer &&
           encoderReverse == o.encoderReverse && accel == o.accel &&
           sendIntervalMs == o.sendIntervalMs;
}

bool Control::operator==(const Control &o) const {
    return entityId == o.entityId && label == o.label && action == o.action && step == o.step &&
           press == o.press;
}

bool Config::operator==(const Config &o) const {
    return device == o.device && wifi == o.wifi && ha == o.ha && controls == o.controls;
}

// ---- Helpers ---------------------------------------------------------------------------

bool isValidHostname(const std::string &hostname) {
    if (hostname.empty() || hostname.size() > kMaxHostnameLen) return false;
    for (char c : hostname) {
        if (!isLowerAlpha(c) && !isDigit(c) && c != '-') return false;
    }
    return true;
}

// ^[a-z_]+\.[a-z0-9_]+$, at most 255 characters.
bool isValidEntityId(const std::string &entityId) {
    if (entityId.empty() || entityId.size() > kMaxEntityIdLen) return false;
    const size_t dot = entityId.find('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 == entityId.size()) return false;
    for (size_t i = 0; i < dot; ++i) {
        if (!isLowerAlpha(entityId[i]) && entityId[i] != '_') return false;
    }
    for (size_t i = dot + 1; i < entityId.size(); ++i) {
        const char c = entityId[i];
        if (!isLowerAlpha(c) && !isDigit(c) && c != '_') return false;
    }
    return true;
}

bool isValidLabelText(const std::string &text) {
    size_t i = 0;
    const size_t n = text.size();
    while (i < n) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c < 0x80) {
            if (c < 0x20 || c == 0x7F) return false;
            ++i;
            continue;
        }
        size_t len = 0;
        uint32_t cp = 0;
        if ((c & 0xE0) == 0xC0) {
            len = 2;
            cp = c & 0x1F;
        } else if ((c & 0xF0) == 0xE0) {
            len = 3;
            cp = c & 0x0F;
        } else if ((c & 0xF8) == 0xF0) {
            len = 4;
            cp = c & 0x07;
        } else {
            return false;
        }
        if (i + len > n) return false;
        for (size_t k = 1; k < len; ++k) {
            const unsigned char cc = static_cast<unsigned char>(text[i + k]);
            if ((cc & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (cc & 0x3F);
        }
        const uint32_t minCp = len == 2 ? 0x80 : len == 3 ? 0x800 : 0x10000;
        if (cp < minCp || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
        if (cp >= 0x80 && cp <= 0x9F) return false;  // C1 control characters
        i += len;
    }
    return true;
}

UrlEndpoint parseUrlEndpoint(const std::string &url) {
    UrlEndpoint ep;
    size_t pos = 0;
    auto startsWithNoCase = [&](const char *prefix) {
        const size_t len = std::strlen(prefix);
        if (url.size() < len) return false;
        for (size_t i = 0; i < len; ++i) {
            char c = url[i];
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            if (c != prefix[i]) return false;
        }
        return true;
    };
    if (startsWithNoCase("https://")) {
        ep.tls = true;
        pos = 8;
    } else if (startsWithNoCase("http://")) {
        pos = 7;
    } else {
        return ep;
    }

    const size_t hostStart = pos;
    while (pos < url.size() && url[pos] != ':' && url[pos] != '/') {
        char c = url[pos];
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (!isLowerAlpha(c) && !isDigit(c) && c != '-' && c != '.') return ep;
        ep.host.push_back(c);
        ++pos;
    }
    if (pos == hostStart || ep.host.size() > kMaxHostLen) return ep;

    ep.port = ep.tls ? 443 : 80;
    if (pos < url.size() && url[pos] == ':') {
        ++pos;
        uint32_t port = 0;
        size_t digits = 0;
        while (pos < url.size() && isDigit(url[pos])) {
            port = port * 10 + static_cast<uint32_t>(url[pos] - '0');
            ++pos;
            if (++digits > 5) return ep;
        }
        if (digits == 0 || port < 1 || port > 65535) return ep;
        ep.port = port;
    }
    if (pos < url.size() && url[pos] != '/') return ep;
    for (; pos < url.size(); ++pos) {
        const unsigned char c = static_cast<unsigned char>(url[pos]);
        if (c <= 0x20 || c >= 0x7F) return ep;
    }
    ep.ok = true;
    return ep;
}

std::string makeApPassword(uint32_t (*random32)(), size_t length) {
    static constexpr char kAlphabet[] = "abcdefghijkmnpqrstuvwxyz23456789";
    static_assert(sizeof(kAlphabet) - 1 == 32, "5 bits per character");
    std::string out;
    out.reserve(length);
    for (size_t i = 0; i < length; ++i) {
        out.push_back(kAlphabet[random32() & 31u]);
    }
    return out;
}

const char *loadStatusName(LoadStatus status) {
    switch (status) {
        case LoadStatus::Loaded: return "loaded";
        case LoadStatus::Missing: return "missing";
        case LoadStatus::Corrupt: return "corrupt";
        case LoadStatus::SchemaTooNew: return "schema";
        case LoadStatus::Invalid: return "invalid";
    }
    return "unknown";
}

// ---- Load ------------------------------------------------------------------------------

bool migrate(JsonDocument &doc) {
    JsonVariant schema = doc["schema"];
    if (schema.isNull()) {
        doc["schema"] = kSchemaVersion;
        return true;
    }
    if (!schema.is<long long>()) return false;
    const long long version = schema.as<long long>();
    if (version > kSchemaVersion) return false;
    // Future: if (version < 2) { ...transform to 2... }
    doc["schema"] = kSchemaVersion;
    return true;
}

LoadResult fromJson(const char *json, size_t length, ArduinoJson::Allocator *allocator) {
    LoadResult result;
    if (json == nullptr || length == 0) {
        result.status = LoadStatus::Missing;
        return result;
    }
    JsonDocument doc = makeDoc(allocator);
    if (deserializeJson(doc, json, length) != DeserializationError::Ok ||
        !doc.is<JsonObject>()) {
        result.status = LoadStatus::Corrupt;
        return result;
    }
    JsonVariantConst schema = doc["schema"];
    if (!schema.isNull() && !schema.is<long long>()) {
        result.status = LoadStatus::Invalid;
        result.error = ValidationResult::failure("schema", "not_an_integer");
        return result;
    }
    if (!migrate(doc)) {
        result.status = LoadStatus::SchemaTooNew;
        return result;
    }

    Config config;
    JsonObjectConst root = doc.as<JsonObjectConst>();
    ValidationResult r = ValidationResult::success();
    if (r.ok && !root["device"].isNull()) r = parseDevice(root["device"], config.device);
    if (r.ok && !root["wifi"].isNull()) r = parseWifi(root["wifi"], config.wifi);
    if (r.ok && !root["ha"].isNull()) r = parseHa(root["ha"], config.ha);
    if (r.ok && !root["controls"].isNull()) r = parseControls(root["controls"], config.controls);
    if (r.ok) r = validate(config);
    if (!r.ok) {
        result.status = LoadStatus::Invalid;
        result.error = r;
        return result;
    }
    result.config = std::move(config);
    result.status = LoadStatus::Loaded;
    return result;
}

// ---- Save ------------------------------------------------------------------------------

std::string toJson(const Config &config, ArduinoJson::Allocator *allocator) {
    JsonDocument doc = makeDoc(allocator);
    writeConfig(doc.to<JsonObject>(), config);
    return serialize(doc);
}

std::string toRedactedJson(const Config &config, const SecretsPresent &present,
                           ArduinoJson::Allocator *allocator) {
    JsonDocument doc = makeDoc(allocator);
    JsonObject root = doc.to<JsonObject>();
    writeConfig(root, config);
    root["wifi"]["password_set"] = present.wifiPassword;
    root["ha"]["token_set"] = present.haToken;
    root["web"].to<JsonObject>()["password_set"] = present.webPassword;
    return serialize(doc);
}

// ---- Validate --------------------------------------------------------------------------

ValidationResult validate(const Config &c) {
    using VR = ValidationResult;
    const DeviceConfig &d = c.device;
    if (!isValidHostname(d.hostname)) return VR::failure("device.hostname", "invalid");
    if (d.brightness < 5 || d.brightness > 100) return VR::failure("device.brightness", "out_of_range");
    if (d.dimAfterS > kMaxTimerS) return VR::failure("device.dim_after_s", "out_of_range");
    if (d.dimLevel < 0 || d.dimLevel > 100) return VR::failure("device.dim_level", "out_of_range");
    if (d.offAfterS > kMaxTimerS) return VR::failure("device.off_after_s", "out_of_range");
    if (d.sendIntervalMs < 100 || d.sendIntervalMs > 2000) {
        return VR::failure("device.send_interval_ms", "out_of_range");
    }

    if (c.wifi.ssid.size() > kMaxSsidBytes) return VR::failure("wifi.ssid", "length");
    if (hasControlChars(c.wifi.ssid)) return VR::failure("wifi.ssid", "invalid_chars");

    if (!c.ha.url.empty() && !parseUrlEndpoint(c.ha.url).ok) return VR::failure("ha.url", "invalid");

    if (c.controls.size() > kMaxControls) return VR::failure("controls", "too_many");
    for (size_t i = 0; i < c.controls.size(); ++i) {
        const Control &ctl = c.controls[i];
        const std::string path = "controls[" + std::to_string(i) + "]";
        if (!isValidEntityId(ctl.entityId)) return VR::failure(path + ".entity_id", "invalid");
        const da::DomainSpec *domain = da::findDomain(da::domainOf(ctl.entityId));
        if (domain == nullptr) return VR::failure(path + ".entity_id", "unsupported_domain");
        if (ctl.label.size() > kMaxLabelBytes) return VR::failure(path + ".label", "length");
        if (!isValidLabelText(ctl.label)) return VR::failure(path + ".label", "invalid_chars");
        const da::ActionSpec *action = da::findAction(*domain, ctl.action);
        if (action == nullptr) return VR::failure(path + ".action", "invalid_for_domain");
        if (!std::isfinite(ctl.step) || !da::stepInRange(*action, ctl.step)) {
            return VR::failure(path + ".step", "out_of_range");
        }
        if (!da::hasPress(*domain, ctl.press)) return VR::failure(path + ".press", "invalid_for_domain");
    }

    if (toJson(c).size() > kMaxConfigJsonBytes) return VR::failure("controls", "too_large");
    return VR::success();
}

// ---- Patch -----------------------------------------------------------------------------

MergeResult mergePatch(const Config &current, const char *patchJson, size_t length,
                       ArduinoJson::Allocator *allocator) {
    MergeResult result;
    result.config = current;
    auto fail = [&result, &current](ValidationResult error) {
        result.ok = false;
        result.error = std::move(error);
        result.config = current;
        result.secrets = SecretChanges();
        result.restartWifi = false;
        return result;
    };

    JsonDocument doc = makeDoc(allocator);
    if (patchJson == nullptr || length == 0 ||
        deserializeJson(doc, patchJson, length) != DeserializationError::Ok ||
        !doc.is<JsonObject>()) {
        return fail(ValidationResult::failure("", "invalid_json"));
    }
    JsonObjectConst root = doc.as<JsonObjectConst>();
    Config merged = current;
    SecretChanges secrets;
    bool restartWifi = false;
    ValidationResult r;

    if (!root["device"].isNull()) {
        r = parseDevice(root["device"], merged.device);
        if (!r.ok) return fail(r);
    }

    if (!root["wifi"].isNull()) {
        r = parseWifi(root["wifi"], merged.wifi);
        if (!r.ok) return fail(r);
        r = readSecret(root["wifi"].as<JsonObjectConst>(), "password", "wifi.password", checkWifiPassword,
                       secrets.wifiPassword);
        if (!r.ok) return fail(r);
        restartWifi = merged.wifi.ssid != current.wifi.ssid ||
                      secrets.wifiPassword.kind != SecretChange::Kind::Keep;
    }

    if (!root["ha"].isNull()) {
        r = parseHa(root["ha"], merged.ha);
        if (!r.ok) return fail(r);
        r = readSecret(root["ha"].as<JsonObjectConst>(), "token", "ha.token", checkHaToken, secrets.haToken);
        if (!r.ok) return fail(r);
        if (tokenEndpointChanged(current.ha.url, merged.ha.url)) {
            // Never send the stored token to a new endpoint (token exfiltration guard).
            if (!merged.ha.url.empty() && secrets.haToken.kind != SecretChange::Kind::Set) {
                return fail(ValidationResult::failure("ha.token", "token_required"));
            }
            if (secrets.haToken.kind == SecretChange::Kind::Keep) {
                secrets.haToken.kind = SecretChange::Kind::Clear;
            }
        }
    }

    if (!root["web"].isNull()) {
        if (!root["web"].is<JsonObjectConst>()) return fail(notAnObject("web"));
        r = readSecret(root["web"].as<JsonObjectConst>(), "password", "web.password", checkWebPassword,
                       secrets.webPassword);
        if (!r.ok) return fail(r);
    }

    if (!root["controls"].isNull()) {
        r = parseControls(root["controls"], merged.controls);
        if (!r.ok) return fail(r);
    }

    r = validate(merged);
    if (!r.ok) return fail(r);

    result.ok = true;
    result.error = ValidationResult::success();
    result.config = std::move(merged);
    result.secrets = std::move(secrets);
    result.restartWifi = restartWifi;
    return result;
}

}  // namespace config
}  // namespace core
