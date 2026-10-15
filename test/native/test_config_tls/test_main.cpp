// Native tests for Step 5b.3: TLS settings in the config (ha.ca_pem, ha.tls_insecure), PEM
// normalisation and validation, the cfg size limit (12288) and the token guard against TLS
// trust weakening (plan-step5b.md §3.4, "Step 5b.3").
//
// Fixtures: test_certs.h holds throwaway PUBLIC certificates only. Cases that need a
// "private key" use hand-crafted DER shapes or fake bodies; no real key is in the repo.
#include <unity.h>

#include <ArduinoJson.h>

#include <cstdint>
#include <string>
#include <vector>

#include "config/ConfigModel.h"
#include "test_certs.h"
#include "util/Base64.h"

namespace cfg = core::config;
using Kind = cfg::SecretChange::Kind;
using Bytes = std::vector<uint8_t>;

void setUp() {}
void tearDown() {}

namespace {

const std::string kRsa = testcerts::kRsa2048;
const std::string kEc = testcerts::kP256;
const std::string kThird = testcerts::kRsaThird;

// ---- DER / PEM builders ------------------------------------------------------------------

Bytes tlv(uint8_t tag, const Bytes &content) {
    Bytes out{tag};
    const size_t n = content.size();
    if (n < 0x80) {
        out.push_back(static_cast<uint8_t>(n));
    } else if (n < 0x100) {
        out.push_back(0x81);
        out.push_back(static_cast<uint8_t>(n));
    } else {
        out.push_back(0x82);
        out.push_back(static_cast<uint8_t>(n >> 8));
        out.push_back(static_cast<uint8_t>(n & 0xFF));
    }
    out.insert(out.end(), content.begin(), content.end());
    return out;
}

Bytes cat(std::initializer_list<Bytes> parts) {
    Bytes out;
    for (const Bytes &p : parts) out.insert(out.end(), p.begin(), p.end());
    return out;
}

Bytes seq(std::initializer_list<Bytes> parts) { return tlv(0x30, cat(parts)); }

// Smallest thing the structure check accepts as a certificate: SEQUENCE { SEQUENCE { } }.
Bytes minimalCertDer(uint8_t marker = 0) { return seq({seq({tlv(0x02, {marker})})}); }

// PKCS#8 / PKCS#1 / SEC1 private key shapes: first inner element is an INTEGER.
Bytes pkcs8Der() {
    return seq({tlv(0x02, {0}),
                seq({tlv(0x06, {0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x01}), tlv(0x05, {})}),
                tlv(0x04, Bytes(40, 0x11))});
}
Bytes pkcs1Der() {
    return seq({tlv(0x02, {0}), tlv(0x02, Bytes(33, 0x7F)), tlv(0x02, {0x01, 0x00, 0x01}),
                tlv(0x02, Bytes(32, 0x55))});
}
Bytes sec1Der() {
    return seq({tlv(0x02, {1}), tlv(0x04, Bytes(32, 0x22)), tlv(0xA0, {0x06, 0x08, 0x2A, 0x86, 0x48})});
}

std::string wrap64(const std::string &b64) {
    std::string out;
    for (size_t pos = 0; pos < b64.size(); pos += 64) out += b64.substr(pos, 64) + "\n";
    return out;
}

std::string pemOf(const Bytes &der, const char *label = "CERTIFICATE") {
    return std::string("-----BEGIN ") + label + "-----\n" +
           wrap64(core::util::base64Encode(der.data(), der.size())) + "-----END " + label + "-----\n";
}

// DER of the first CERTIFICATE block in a PEM.
Bytes derOfFirst(const std::string &pem) {
    const size_t a = pem.find('\n') + 1;
    const size_t b = pem.find("-----END");
    Bytes der;
    TEST_ASSERT_TRUE(core::util::base64Decode(pem.substr(a, b - a), der));
    return der;
}

// A structurally valid (DER shape only) CERTIFICATE block of exactly `total` bytes in
// normalised form (total in [4000, 4200]).
std::string certShapedPem(size_t total) {
    const size_t bodyChars = 3980;  // multiple of 4
    const size_t lines = total - 28 - 26 - bodyChars;
    Bytes der(bodyChars / 4 * 3, 0);
    const size_t inner = der.size() - 8;
    der[0] = 0x30;
    der[1] = 0x82;
    der[2] = static_cast<uint8_t>((der.size() - 4) >> 8);
    der[3] = static_cast<uint8_t>((der.size() - 4) & 0xFF);
    der[4] = 0x30;
    der[5] = 0x82;
    der[6] = static_cast<uint8_t>(inner >> 8);
    der[7] = static_cast<uint8_t>(inner & 0xFF);
    const std::string b64 = core::util::base64Encode(der.data(), der.size());
    std::string out = "-----BEGIN CERTIFICATE-----\n";
    const size_t width = (b64.size() + lines - 1) / lines;
    for (size_t pos = 0; pos < b64.size(); pos += width) out += b64.substr(pos, width) + "\n";
    out += "-----END CERTIFICATE-----\n";
    return out;
}

std::string crlf(const std::string &s) {
    std::string out;
    for (char c : s) {
        if (c == '\n') out += "\r\n";
        else out.push_back(c);
    }
    return out;
}

// CRLF line endings, each preceded by trailing blanks.
std::string crlfSpaced(const std::string &s) {
    std::string out;
    for (char c : s) {
        if (c == '\n') out += " \t \r\n";
        else out.push_back(c);
    }
    return out;
}

std::string trailingSpaces(const std::string &s) {
    std::string out;
    for (char c : s) {
        if (c == '\n') out += " \t ";
        out.push_back(c);
    }
    return out;
}

// ---- config / patch helpers --------------------------------------------------------------

cfg::MergeResult merge(const cfg::Config &current, const std::string &patch) {
    return cfg::mergePatch(current, patch.data(), patch.size());
}

cfg::LoadResult load(const std::string &json) { return cfg::fromJson(json.data(), json.size()); }

cfg::Config haConfig(const std::string &url, const std::string &ca = "", bool insecure = false) {
    cfg::Config c;
    c.ha.url = url;
    c.ha.caPem = ca;
    c.ha.tlsInsecure = insecure;
    return c;
}

// {"ha":{...}} patch built with ArduinoJson so PEM newlines are escaped correctly.
// tokenJson: nullptr = key absent; otherwise raw JSON ("null", "\"tok\"", "\"\"").
std::string haPatch(const std::string &url, const std::string &ca, bool insecure,
                    const char *tokenJson = nullptr) {
    JsonDocument doc;
    JsonObject ha = doc["ha"].to<JsonObject>();
    ha["url"] = url;
    ha["ca_pem"] = ca;
    ha["tls_insecure"] = insecure;
    std::string out;
    serializeJson(doc, out);
    if (tokenJson != nullptr) {
        out.pop_back();  // "}"
        out.pop_back();  // "}" of ha
        out += std::string(",\"token\":") + tokenJson + "}}";
    }
    return out;
}

void assertMergeFails(const cfg::Config &cur, const std::string &patch, const char *field, const char *msg) {
    const cfg::MergeResult r = merge(cur, patch);
    std::string detail = patch.substr(0, 160) + " -> got " + r.error.field + "/" + r.error.message;
    TEST_ASSERT_FALSE_MESSAGE(r.ok, detail.c_str());
    TEST_ASSERT_EQUAL_STRING_MESSAGE(field, r.error.field.c_str(), detail.c_str());
    TEST_ASSERT_EQUAL_STRING_MESSAGE(msg, r.error.message.c_str(), detail.c_str());
    TEST_ASSERT_TRUE_MESSAGE(r.config == cur, "failed merge must leave config unchanged");
}

cfg::MergeResult mergeOk(const cfg::Config &cur, const std::string &patch) {
    cfg::MergeResult r = merge(cur, patch);
    if (!r.ok) {
        std::string m = "merge failed: " + r.error.field + "/" + r.error.message + " for " + patch.substr(0, 160);
        TEST_FAIL_MESSAGE(m.c_str());
    }
    return r;
}

void assertKind(Kind expected, const cfg::MergeResult &r, const char *what) {
    TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(expected), static_cast<int>(r.secrets.haToken.kind), what);
}

// checkCaPem failure expectation.
void assertCa(const std::string &pem, const char *message) {
    const cfg::ValidationResult r = cfg::checkCaPem(pem);
    std::string detail = std::string("expected ") + message + ", got " + r.message + " for: " + pem.substr(0, 90);
    TEST_ASSERT_FALSE_MESSAGE(r.ok, detail.c_str());
    TEST_ASSERT_EQUAL_STRING_MESSAGE("ha.ca_pem", r.field.c_str(), detail.c_str());
    TEST_ASSERT_EQUAL_STRING_MESSAGE(message, r.message.c_str(), detail.c_str());
}

void assertCaOk(const std::string &pem) {
    const cfg::ValidationResult r = cfg::checkCaPem(pem);
    std::string detail = "unexpected failure: " + r.field + "/" + r.message + " for: " + pem.substr(0, 90);
    TEST_ASSERT_TRUE_MESSAGE(r.ok, detail.c_str());
}

}  // namespace

// ---------------------------------------------------------------------------
// Defaults, round trip, loading
// ---------------------------------------------------------------------------

static void test_defaults_tls_fields() {
    const cfg::Config c;
    TEST_ASSERT_TRUE(c.ha.caPem.empty());
    TEST_ASSERT_FALSE(c.ha.tlsInsecure);
    TEST_ASSERT_TRUE(cfg::validate(c).ok);
    TEST_ASSERT_EQUAL_size_t(12288, cfg::kMaxConfigJsonBytes);
    TEST_ASSERT_EQUAL_size_t(4096, cfg::kMaxCaPemBytes);
    TEST_ASSERT_EQUAL_size_t(3, cfg::kMaxCaCerts);
}

static void test_old_blob_without_tls_keys_loads_with_defaults() {
    const std::string old =
        "{\"schema\":1,\"device\":{\"hostname\":\"ha-knob\"},\"wifi\":{\"ssid\":\"Net\"},"
        "\"ha\":{\"url\":\"http://192.168.1.10:8123\"},\"controls\":[]}";
    const cfg::LoadResult r = load(old);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(cfg::LoadStatus::Loaded), static_cast<int>(r.status));
    TEST_ASSERT_EQUAL_STRING("http://192.168.1.10:8123", r.config.ha.url.c_str());
    TEST_ASSERT_TRUE(r.config.ha.caPem.empty());
    TEST_ASSERT_FALSE(r.config.ha.tlsInsecure);
    // Same config as loading without any ha section TLS awareness: equals a freshly built one.
    cfg::Config expected;
    expected.wifi.ssid = "Net";
    expected.ha.url = "http://192.168.1.10:8123";
    TEST_ASSERT_TRUE(r.config == expected);
    // And an old blob with an https URL loads too (https is merely not connected yet).
    const cfg::LoadResult h = load("{\"schema\":1,\"ha\":{\"url\":\"https://ha.example.com\"}}");
    TEST_ASSERT_EQUAL_INT(static_cast<int>(cfg::LoadStatus::Loaded), static_cast<int>(h.status));
}

static void test_to_json_ha_section_shape() {
    cfg::Config c = haConfig("http://h:8123");
    const std::string json = cfg::toJson(c);
    TEST_ASSERT_TRUE(json.find("\"ha\":{\"url\":\"http://h:8123\",\"ca_pem\":\"\",\"tls_insecure\":false}") !=
                     std::string::npos);
    c = haConfig("https://h", "", true);
    TEST_ASSERT_TRUE(cfg::toJson(c).find("\"tls_insecure\":true") != std::string::npos);
}

static void test_round_trip_ca_and_insecure() {
    const cfg::Config chain = haConfig("https://ha.example.com", kRsa + kEc + kThird);
    TEST_ASSERT_TRUE(cfg::validate(chain).ok);
    cfg::LoadResult r = load(cfg::toJson(chain));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(cfg::LoadStatus::Loaded), static_cast<int>(r.status));
    TEST_ASSERT_TRUE(r.config == chain);
    TEST_ASSERT_EQUAL_STRING(cfg::toJson(chain).c_str(), cfg::toJson(r.config).c_str());

    const cfg::Config insecure = haConfig("https://ha.example.com", "", true);
    r = load(cfg::toJson(insecure));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(cfg::LoadStatus::Loaded), static_cast<int>(r.status));
    TEST_ASSERT_TRUE(r.config == insecure);
    TEST_ASSERT_TRUE(r.config.ha.tlsInsecure);
}

static void test_redacted_json_has_tls_fields_and_loads_back() {
    const cfg::Config c = haConfig("https://ha.example.com", kEc);
    const std::string red = cfg::toRedactedJson(c, {false, true, false});
    JsonDocument doc;
    TEST_ASSERT_TRUE(deserializeJson(doc, red) == DeserializationError::Ok);
    TEST_ASSERT_EQUAL_STRING(kEc.c_str(), doc["ha"]["ca_pem"].as<const char *>());
    TEST_ASSERT_TRUE(doc["ha"]["tls_insecure"].is<bool>());
    TEST_ASSERT_FALSE(doc["ha"]["tls_insecure"].as<bool>());
    TEST_ASSERT_TRUE(doc["ha"]["token_set"].as<bool>());
    TEST_ASSERT_TRUE(doc["ha"]["token"].isNull());
    TEST_ASSERT_TRUE(red.find("\"token\"") == std::string::npos);
    const cfg::LoadResult r = load(red);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(cfg::LoadStatus::Loaded), static_cast<int>(r.status));
    TEST_ASSERT_TRUE(r.config == c);
}

static void test_stored_pem_is_normalised_on_load() {
    JsonDocument doc;
    doc["schema"] = 1;
    doc["ha"]["url"] = "https://ha.example.com";
    doc["ha"]["ca_pem"] = "  \r\n" + crlfSpaced(kRsa);
    std::string json;
    serializeJson(doc, json);
    const cfg::LoadResult r = load(json);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(cfg::LoadStatus::Loaded), static_cast<int>(r.status));
    TEST_ASSERT_EQUAL_STRING(kRsa.c_str(), r.config.ha.caPem.c_str());
}

static void test_stored_invalid_tls_gives_defaults_invalid() {
    struct Case {
        const char *json;
        const char *field;
        const char *msg;
    } cases[] = {
        {"{\"schema\":1,\"ha\":{\"url\":\"http://h\",\"tls_insecure\":true}}", "ha.tls_insecure", "requires_https"},
        {"{\"schema\":1,\"ha\":{\"url\":\"https://h\",\"ca_pem\":\"hello\"}}", "ha.ca_pem", "not_a_certificate"},
        {"{\"schema\":1,\"ha\":{\"url\":\"https://h\",\"ca_pem\":5}}", "ha.ca_pem", "not_a_string"},
        {"{\"schema\":1,\"ha\":{\"url\":\"https://h\",\"tls_insecure\":\"yes\"}}", "ha.tls_insecure", "not_a_boolean"},
    };
    for (const Case &c : cases) {
        const cfg::LoadResult r = load(c.json);
        TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(cfg::LoadStatus::Invalid), static_cast<int>(r.status), c.json);
        TEST_ASSERT_TRUE(r.config == cfg::Config());
        TEST_ASSERT_EQUAL_STRING_MESSAGE(c.field, r.error.field.c_str(), c.json);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(c.msg, r.error.message.c_str(), c.json);
    }
}

static void test_null_tls_keys_keep_defaults() {
    const cfg::MergeResult r = mergeOk(cfg::Config(), "{\"ha\":{\"url\":\"http://h\",\"ca_pem\":null,\"tls_insecure\":null,\"token\":\"t\"}}");
    TEST_ASSERT_TRUE(r.config.ha.caPem.empty());
    TEST_ASSERT_FALSE(r.config.ha.tlsInsecure);
}

// ---------------------------------------------------------------------------
// Normalisation
// ---------------------------------------------------------------------------

static void test_normalize_pem_rules() {
    TEST_ASSERT_EQUAL_STRING("", cfg::normalizePem("").c_str());
    TEST_ASSERT_EQUAL_STRING("", cfg::normalizePem(" \t\r\n \n\t ").c_str());
    TEST_ASSERT_EQUAL_STRING("a\nb\n", cfg::normalizePem("a\r\nb\r\n").c_str());      // CRLF
    TEST_ASSERT_EQUAL_STRING("a\nb\n", cfg::normalizePem("a\rb\r").c_str());          // CR
    TEST_ASSERT_EQUAL_STRING("a\nb\n", cfg::normalizePem("a\nb").c_str());            // adds one LF
    TEST_ASSERT_EQUAL_STRING("a\nb\n", cfg::normalizePem("\n\n  a\nb\n\n\n  ").c_str());  // trim
    TEST_ASSERT_EQUAL_STRING("a\nb\n", cfg::normalizePem("a \t \nb\t\t\n").c_str());  // trailing ws per line
    TEST_ASSERT_EQUAL_STRING("a\nb\n", cfg::normalizePem("a \r\nb \r").c_str());      // mixed
    TEST_ASSERT_EQUAL_STRING("a\n\nb\n", cfg::normalizePem("a\r\n\r\nb").c_str());    // inner blank kept
    // Idempotent on real PEMs, in every spelling.
    const std::string variants[] = {kRsa, crlf(kRsa), trailingSpaces(kRsa), "\n\n" + kRsa + "\n\n",
                                    crlfSpaced(kRsa), kRsa.substr(0, kRsa.size() - 1)};
    for (const std::string &v : variants) {
        TEST_ASSERT_EQUAL_STRING(kRsa.c_str(), cfg::normalizePem(v).c_str());
        TEST_ASSERT_EQUAL_STRING(kRsa.c_str(), cfg::normalizePem(cfg::normalizePem(v)).c_str());
    }
}

static void test_whitespace_only_ca_is_empty_in_patch() {
    const cfg::Config cur = haConfig("http://h:8123");
    const cfg::MergeResult r = mergeOk(cur, haPatch("http://h:8123", " \r\n\t\n ", false));
    TEST_ASSERT_TRUE(r.config.ha.caPem.empty());
    TEST_ASSERT_TRUE(r.config == cur);
}

static void test_patch_ca_stored_normalised_and_resend_equal() {
    const cfg::Config cur = haConfig("https://ha.example.com");
    const cfg::MergeResult r = mergeOk(cur, haPatch("https://ha.example.com", crlf(kRsa), false, "\"tok\""));
    TEST_ASSERT_EQUAL_STRING(kRsa.c_str(), r.config.ha.caPem.c_str());
    const cfg::Config stored = r.config;
    const std::string resend[] = {kRsa, crlf(kRsa), trailingSpaces(kRsa), "  " + crlf(kRsa) + "\r\n\r\n",
                                  kRsa.substr(0, kRsa.size() - 1)};
    for (const std::string &s : resend) {
        const cfg::MergeResult again = mergeOk(stored, haPatch("https://ha.example.com", s, false));
        TEST_ASSERT_TRUE(again.config == stored);
        assertKind(Kind::Keep, again, "normalised resend keeps the token");
    }
}

// ---------------------------------------------------------------------------
// Validation rows
// ---------------------------------------------------------------------------

static void test_real_certs_accepted() {
    assertCaOk(kRsa);
    assertCaOk(kEc);
    assertCaOk(kRsa + kEc);
    assertCaOk(kRsa + kEc + kThird);
    assertCaOk(kRsa + "\n\n" + kEc);          // blank lines between blocks
    assertCaOk(kRsa + " \t\n" + kEc + "\n");  // blank space between blocks
    TEST_ASSERT_TRUE(cfg::validate(haConfig("https://ha.example.com", kRsa + kEc + kThird)).ok);
    TEST_ASSERT_TRUE(cfg::validate(haConfig("https://ha.example.com:8123/prefix", kEc)).ok);
}

static void test_length_boundary_after_normalisation() {
    const std::string p4096 = certShapedPem(4096);
    const std::string p4097 = certShapedPem(4097);
    TEST_ASSERT_EQUAL_size_t(4096, p4096.size());
    TEST_ASSERT_EQUAL_size_t(4097, p4097.size());
    assertCaOk(p4096);
    assertCa(p4097, "length");
    TEST_ASSERT_TRUE(cfg::validate(haConfig("https://h", p4096)).ok);
    const cfg::ValidationResult r = cfg::validate(haConfig("https://h", p4097));
    TEST_ASSERT_FALSE(r.ok);
    TEST_ASSERT_EQUAL_STRING("ha.ca_pem", r.field.c_str());
    TEST_ASSERT_EQUAL_STRING("length", r.message.c_str());

    // Measured after normalisation: a CRLF + padded copy is far longer raw but accepted;
    // one more character after normalisation is rejected.
    const cfg::Config cur = haConfig("https://h");
    const std::string raw = "\n\n  " + crlfSpaced(p4096) + "\n\n   ";
    TEST_ASSERT_TRUE(raw.size() > 4096 + 100);
    const cfg::MergeResult ok = mergeOk(cur, haPatch("https://h", raw, false, "\"t\""));
    TEST_ASSERT_EQUAL_size_t(4096, ok.config.ha.caPem.size());
    assertMergeFails(cur, haPatch("https://h", crlf(p4097), false, "\"t\""), "ha.ca_pem", "length");
    // Raw 4097 whose normalised size is 4096 (no trailing LF in the input) passes.
    const std::string noLf = p4096.substr(0, p4096.size() - 1);
    TEST_ASSERT_EQUAL_size_t(4095, noLf.size());
    mergeOk(cur, haPatch("https://h", noLf + " ", false, "\"t\""));
}

static void test_not_a_certificate_private_key_blocks() {
    const std::string fakeBody = "AAAA\n";
    const char *labels[] = {"PRIVATE KEY", "RSA PRIVATE KEY", "EC PRIVATE KEY", "ENCRYPTED PRIVATE KEY",
                            "PUBLIC KEY", "TRUSTED CERTIFICATE", "X509 CRL", "CERTIFICATE REQUEST", "certificate"};
    for (const char *l : labels) {
        const std::string pem = std::string("-----BEGIN ") + l + "-----\n" + fakeBody + "-----END " + l + "-----\n";
        assertCa(pem, "not_a_certificate");
    }
    // Real private-key-looking shapes with their proper labels.
    assertCa(pemOf(pkcs8Der(), "PRIVATE KEY"), "not_a_certificate");
    assertCa(pemOf(pkcs1Der(), "RSA PRIVATE KEY"), "not_a_certificate");
    assertCa(pemOf(sec1Der(), "EC PRIVATE KEY"), "not_a_certificate");
    // Certificate plus a key, either order, and with more certs around.
    const std::string key = "-----BEGIN PRIVATE KEY-----\nAAAA\n-----END PRIVATE KEY-----\n";
    assertCa(kRsa + key, "not_a_certificate");
    assertCa(key + kRsa, "not_a_certificate");
    assertCa(kRsa + key + kEc, "not_a_certificate");
}

static void test_not_a_certificate_text_and_malformed_armor() {
    assertCa("hello", "not_a_certificate");
    assertCa("subject=CN = Test\n" + kRsa, "not_a_certificate");
    assertCa(kRsa + "issuer=CN = Test\n", "not_a_certificate");
    assertCa("Bag Attributes\n    friendlyName: x\n" + kRsa, "not_a_certificate");
    assertCa("x" + kRsa, "not_a_certificate");
    assertCa("-----BEGIN CERTIFICATE-----\nAAAA\n", "not_a_certificate");  // no END
    assertCa("-----BEGIN CERTIFICATE-----\nAAAA\n-----END PRIVATE KEY-----\n", "not_a_certificate");  // mismatch
    assertCa(kRsa.substr(0, kRsa.size() - 1) + " junk\n", "not_a_certificate");  // text after END
    assertCa("-----BEGIN CERTIFICATE----- x\nAAAA\n-----END CERTIFICATE-----\n", "not_a_certificate");
    assertCa("-----BEGIN CERTIFICATE-----AAAA\n-----END CERTIFICATE-----\n", "not_a_certificate");
    assertCa("-----END CERTIFICATE-----\n", "not_a_certificate");
}

static void test_not_a_certificate_relabelled_keys() {
    assertCa(pemOf(pkcs8Der()), "not_a_certificate");
    assertCa(pemOf(pkcs1Der()), "not_a_certificate");
    assertCa(pemOf(sec1Der()), "not_a_certificate");
    // A relabelled key next to a good certificate is still rejected.
    assertCa(kRsa + pemOf(pkcs8Der()), "not_a_certificate");
    // Inner element longer than the outer SEQUENCE: 30 05 { 30 09 01 02 03 }.
    assertCa(pemOf({0x30, 0x05, 0x30, 0x09, 0x01, 0x02, 0x03}), "not_a_certificate");
    // Inner element that is not a SEQUENCE at all (OCTET STRING).
    assertCa(pemOf(seq({tlv(0x04, {1, 2, 3})})), "not_a_certificate");
    // Sanity: the minimal certificate shape itself is accepted.
    assertCaOk(pemOf(minimalCertDer()));
}

static void test_too_many_certs() {
    const std::string tiny[] = {pemOf(minimalCertDer(1)), pemOf(minimalCertDer(2)), pemOf(minimalCertDer(3)),
                                pemOf(minimalCertDer(4))};
    assertCaOk(tiny[0] + tiny[1] + tiny[2]);
    assertCa(tiny[0] + tiny[1] + tiny[2] + tiny[3], "too_many");
    // Real certificates: 3 fit, 4 (4016 bytes, still under the length limit) are too many.
    const std::string four = kRsa + kEc + kThird + kRsa;
    TEST_ASSERT_TRUE(four.size() <= 4096);
    assertCa(four, "too_many");
    // Over both limits: the length check comes first.
    const std::string big = kRsa + kEc + kThird + kRsa + kRsa;
    TEST_ASSERT_TRUE(big.size() > 4096);
    assertCa(big, "length");
    // Through validate.
    const cfg::ValidationResult r = cfg::validate(haConfig("https://h", four));
    TEST_ASSERT_FALSE(r.ok);
    TEST_ASSERT_EQUAL_STRING("ha.ca_pem", r.field.c_str());
    TEST_ASSERT_EQUAL_STRING("too_many", r.message.c_str());
}

static void test_invalid_base64() {
    const std::string h = "-----BEGIN CERTIFICATE-----\n";
    const std::string f = "\n-----END CERTIFICATE-----\n";
    assertCa(h + "MIIB!AAA" + f, "invalid_base64");
    assertCa(h + "MI IBAAA" + f, "invalid_base64");           // space inside a line
    assertCa(h + "MIIB-_AA" + f, "invalid_base64");           // URL-safe alphabet
    assertCa(h + "MIIBAAA" + f, "invalid_base64");            // not a multiple of 4, no padding
    assertCa(h + "MI==IBAA" + f, "invalid_base64");           // padding mid-body
    assertCa(h + "MIIBAAAA====" + f, "invalid_base64");
    assertCa(h + "MIIBAAAA" + "\x80\x81\xFF" + f, "invalid_base64");
    // A real cert with a corrupted character.
    std::string bad = kRsa;
    bad[40] = '*';
    assertCa(bad, "invalid_base64");
    // A real cert cut in the middle of the body, END line re-attached: broken Base64 or DER.
    const std::string cut = kRsa.substr(0, kRsa.find('\n') + 1 + 64 * 5 + 5 + 6) + "\n-----END CERTIFICATE-----\n";
    const cfg::ValidationResult r = cfg::checkCaPem(cut);
    TEST_ASSERT_FALSE(r.ok);
    TEST_ASSERT_EQUAL_STRING("ha.ca_pem", r.field.c_str());
}

static void test_invalid_der() {
    // Truncated DER body (valid Base64, outer length no longer matches).
    Bytes der = derOfFirst(kRsa);
    der.resize(der.size() - 10);
    assertCa(pemOf(der), "invalid_der");
    Bytes der2 = derOfFirst(kEc);
    der2.resize(20);
    assertCa(pemOf(der2), "invalid_der");
    // Trailing garbage after the SEQUENCE.
    Bytes extra = derOfFirst(kRsa);
    extra.push_back(0x00);
    assertCa(pemOf(extra), "invalid_der");
    // Outer length claims more / less than present.
    assertCa(pemOf({0x30, 0x09, 0x30, 0x01, 0x00}), "invalid_der");
    assertCa(pemOf({0x30, 0x01, 0x30, 0x01, 0x00}), "invalid_der");
    // Indefinite length.
    assertCa(pemOf({0x30, 0x80, 0x30, 0x00, 0x00, 0x00}), "invalid_der");
    // Five length bytes.
    assertCa(pemOf({0x30, 0x85, 0x00, 0x00, 0x00, 0x00, 0x03, 0x30, 0x01, 0x00}), "invalid_der");
    // Length announced but its bytes are missing.
    assertCa(pemOf({0x30, 0x82, 0x01}), "invalid_der");
    // Not a SEQUENCE at the outer level; single byte; empty body.
    assertCa(pemOf({0x31, 0x03, 0x30, 0x01, 0x00}), "invalid_der");
    assertCa(pemOf({0x30}), "invalid_der");
    assertCa("-----BEGIN CERTIFICATE-----\n-----END CERTIFICATE-----\n", "invalid_der");
    assertCa("-----BEGIN CERTIFICATE-----\n\n-----END CERTIFICATE-----\n", "invalid_der");
    // A good cert followed by a bad one reports the bad one.
    assertCa(kRsa + pemOf(der), "invalid_der");
}

static void test_requires_https_validate() {
    cfg::ValidationResult r = cfg::validate(haConfig("http://h:8123", kRsa));
    TEST_ASSERT_FALSE(r.ok);
    TEST_ASSERT_EQUAL_STRING("ha.ca_pem", r.field.c_str());
    TEST_ASSERT_EQUAL_STRING("requires_https", r.message.c_str());
    r = cfg::validate(haConfig("", kRsa));
    TEST_ASSERT_EQUAL_STRING("ha.ca_pem", r.field.c_str());
    TEST_ASSERT_EQUAL_STRING("requires_https", r.message.c_str());
    r = cfg::validate(haConfig("http://h:8123", "", true));
    TEST_ASSERT_FALSE(r.ok);
    TEST_ASSERT_EQUAL_STRING("ha.tls_insecure", r.field.c_str());
    TEST_ASSERT_EQUAL_STRING("requires_https", r.message.c_str());
    r = cfg::validate(haConfig("", "", true));
    TEST_ASSERT_EQUAL_STRING("ha.tls_insecure", r.field.c_str());
    TEST_ASSERT_EQUAL_STRING("requires_https", r.message.c_str());
    // https is fine, http without TLS settings is fine.
    TEST_ASSERT_TRUE(cfg::validate(haConfig("https://h", kRsa)).ok);
    TEST_ASSERT_TRUE(cfg::validate(haConfig("https://h", "", true)).ok);
    TEST_ASSERT_TRUE(cfg::validate(haConfig("http://h")).ok);
    // Content errors come before requires_https.
    r = cfg::validate(haConfig("http://h", "hello"));
    TEST_ASSERT_EQUAL_STRING("not_a_certificate", r.message.c_str());
}

static void test_requires_https_via_patch() {
    const cfg::Config none;
    assertMergeFails(none, haPatch("http://h:8123", kRsa, false, "\"t\""), "ha.ca_pem", "requires_https");
    assertMergeFails(none, haPatch("", kRsa, false), "ha.ca_pem", "requires_https");
    assertMergeFails(none, haPatch("http://h:8123", "", true, "\"t\""), "ha.tls_insecure", "requires_https");
    assertMergeFails(none, haPatch("", "", true), "ha.tls_insecure", "requires_https");
    // An existing https config switched to http must also drop the TLS fields.
    const cfg::Config cur = haConfig("https://h:8123", kRsa);
    assertMergeFails(cur, haPatch("http://h:8123", kRsa, false, "\"t\""), "ha.ca_pem", "requires_https");
    // Ordinary http / empty-URL patches still work.
    mergeOk(cur, haPatch("http://h:8123", "", false, "\"t\""));
}

static void test_conflicts_with_ca_pem() {
    const cfg::ValidationResult r = cfg::validate(haConfig("https://h", kRsa, true));
    TEST_ASSERT_FALSE(r.ok);
    TEST_ASSERT_EQUAL_STRING("ha.tls_insecure", r.field.c_str());
    TEST_ASSERT_EQUAL_STRING("conflicts_with_ca_pem", r.message.c_str());
    assertMergeFails(cfg::Config(), haPatch("https://h", kRsa, true, "\"t\""), "ha.tls_insecure",
                     "conflicts_with_ca_pem");
    // Reported before the guard (no token in the patch).
    assertMergeFails(haConfig("https://h"), haPatch("https://h", kRsa, true), "ha.tls_insecure",
                     "conflicts_with_ca_pem");
}

static void test_wrong_types_via_patch() {
    const cfg::Config cur = haConfig("https://h");
    assertMergeFails(cur, "{\"ha\":{\"url\":\"https://h\",\"ca_pem\":5}}", "ha.ca_pem", "not_a_string");
    assertMergeFails(cur, "{\"ha\":{\"url\":\"https://h\",\"ca_pem\":true}}", "ha.ca_pem", "not_a_string");
    assertMergeFails(cur, "{\"ha\":{\"url\":\"https://h\",\"ca_pem\":[]}}", "ha.ca_pem", "not_a_string");
    assertMergeFails(cur, "{\"ha\":{\"url\":\"https://h\",\"tls_insecure\":\"yes\"}}", "ha.tls_insecure",
                     "not_a_boolean");
    assertMergeFails(cur, "{\"ha\":{\"url\":\"https://h\",\"tls_insecure\":1}}", "ha.tls_insecure", "not_a_boolean");
    assertMergeFails(cur, "{\"ha\":{\"url\":\"https://h\",\"tls_insecure\":\"\"}}", "ha.tls_insecure", "not_a_boolean");
}

// ---------------------------------------------------------------------------
// Token guard: TLS trust weakening
// ---------------------------------------------------------------------------

static void expectTokenRequiredUnlessToken(const cfg::Config &cur, const std::string &url, const std::string &ca,
                                           bool insecure) {
    assertMergeFails(cur, haPatch(url, ca, insecure), "ha.token", "token_required");
    assertMergeFails(cur, haPatch(url, ca, insecure, "\"\""), "ha.token", "token_required");
    assertMergeFails(cur, haPatch(url, ca, insecure, "null"), "ha.token", "token_required");
    const cfg::MergeResult r = mergeOk(cur, haPatch(url, ca, insecure, "\"new.token\""));
    assertKind(Kind::Set, r, "new token accepted");
    TEST_ASSERT_EQUAL_STRING("new.token", r.secrets.haToken.value.c_str());
    TEST_ASSERT_EQUAL_STRING(url.c_str(), r.config.ha.url.c_str());
    TEST_ASSERT_EQUAL_INT(insecure, r.config.ha.tlsInsecure);
}

static const char *const kHttpsBases[][2] = {
    // {stored URL, URL spelled differently but the same host:port}
    {"https://ha.example.com:8123", "https://ha.example.com:8123"},
    {"https://ha.example.com:8123", "https://HA.Example.COM:8123"},
    {"https://ha.example.com:8123", "https://ha.example.com:8123/other/path"},
    {"https://ha.example.com", "https://ha.example.com:443"},
    {"https://ha.example.com", "https://HA.EXAMPLE.COM/prefix"},
    {"https://ha.example.com:443", "https://ha.example.com"},
};

static void test_guard_insecure_off_to_on() {
    for (const auto &b : kHttpsBases) {
        const cfg::Config cur = haConfig(b[0]);
        expectTokenRequiredUnlessToken(cur, b[1], "", true);
    }
}

static void test_guard_ca_added_or_changed() {
    const std::string chainOfOneThenAppended = kRsa + kEc;
    for (const auto &b : kHttpsBases) {
        // "" -> X
        expectTokenRequiredUnlessToken(haConfig(b[0]), b[1], kRsa, false);
        // X -> Y
        expectTokenRequiredUnlessToken(haConfig(b[0], kRsa), b[1], kEc, false);
        // X -> X + appended certificate
        expectTokenRequiredUnlessToken(haConfig(b[0], kRsa), b[1], chainOfOneThenAppended, false);
        // X -> part of X + other (order changed)
        expectTokenRequiredUnlessToken(haConfig(b[0], kRsa + kEc), b[1], kEc + kRsa, false);
        // CA -> insecure
        expectTokenRequiredUnlessToken(haConfig(b[0], kRsa), b[1], "", true);
        // insecure -> CA
        expectTokenRequiredUnlessToken(haConfig(b[0], "", true), b[1], kRsa, false);
    }
}

static void test_guard_empty_ca_after_clear_in_separate_patch() {
    // Token set, then a separate patch clears the URL (token cleared, TLS fields must be empty).
    cfg::Config cur = haConfig("https://ha.example.com", kRsa);
    const cfg::MergeResult cleared = mergeOk(cur, haPatch("", "", false, "\"ignored\""));
    assertKind(Kind::Clear, cleared, "url cleared clears the token");
    TEST_ASSERT_TRUE(cleared.config.ha.url.empty());
    TEST_ASSERT_TRUE(cleared.config.ha.caPem.empty());
    // A later patch setting an https URL with a CA, without token, is refused.
    assertMergeFails(cleared.config, haPatch("https://ha.example.com", kRsa, false), "ha.token", "token_required");
    assertMergeFails(cleared.config, haPatch("https://ha.example.com", "", true), "ha.token", "token_required");
    assertMergeFails(cleared.config, haPatch("https://ha.example.com", "", false), "ha.token", "token_required");
    mergeOk(cleared.config, haPatch("https://ha.example.com", kRsa, false, "\"fresh\""));
}

static void test_guard_resend_unchanged_keeps_token() {
    for (const auto &b : kHttpsBases) {
        const cfg::Config withCa = haConfig(b[0], kRsa + kEc);
        const std::string resend[] = {kRsa + kEc, crlf(kRsa + kEc), trailingSpaces(kRsa + kEc),
                                      crlfSpaced(kRsa + kEc), "\r\n" + kRsa + kEc + "\r\n\r\n"};
        for (const std::string &s : resend) {
            const cfg::MergeResult r = mergeOk(withCa, haPatch(b[1], s, false));
            assertKind(Kind::Keep, r, "CA resent in another spelling");
            TEST_ASSERT_EQUAL_STRING((kRsa + kEc).c_str(), r.config.ha.caPem.c_str());
        }
        // Token explicitly empty = still not a new token, still fine because nothing weakened.
        assertKind(Kind::Keep, mergeOk(withCa, haPatch(b[1], kRsa + kEc, false, "\"\"")), "empty token");
        // insecure stays on: not a weakening.
        const cfg::Config ins = haConfig(b[0], "", true);
        assertKind(Kind::Keep, mergeOk(ins, haPatch(b[1], "", true)), "insecure resent");
    }
}

static void test_guard_tightening_keeps_token() {
    for (const auto &b : kHttpsBases) {
        // CA X -> ""
        cfg::MergeResult r = mergeOk(haConfig(b[0], kRsa), haPatch(b[1], "", false));
        assertKind(Kind::Keep, r, "CA removed");
        TEST_ASSERT_TRUE(r.config.ha.caPem.empty());
        // insecure on -> off
        r = mergeOk(haConfig(b[0], "", true), haPatch(b[1], "", false));
        assertKind(Kind::Keep, r, "insecure off");
        TEST_ASSERT_FALSE(r.config.ha.tlsInsecure);
        // insecure + chain to nothing at all is just a tightening too
        r = mergeOk(haConfig(b[0], kRsa + kEc + kThird), haPatch(b[1], "", false));
        assertKind(Kind::Keep, r, "chain removed");
    }
}

static void test_guard_http_to_https_same_host_port_keeps_token() {
    const cfg::Config cur = haConfig("http://ha.example.com:8123");
    cfg::MergeResult r = mergeOk(cur, haPatch("https://ha.example.com:8123", "", true));
    assertKind(Kind::Keep, r, "http->https with insecure");
    TEST_ASSERT_TRUE(r.config.ha.tlsInsecure);
    r = mergeOk(cur, haPatch("https://HA.example.com:8123/x", kRsa, false));
    assertKind(Kind::Keep, r, "http->https with CA");
    TEST_ASSERT_EQUAL_STRING(kRsa.c_str(), r.config.ha.caPem.c_str());
    r = mergeOk(cur, haPatch("https://ha.example.com:8123", "", false));
    assertKind(Kind::Keep, r, "plain upgrade");
    // 80 -> 443 is a port change and therefore a different endpoint.
    assertMergeFails(haConfig("http://ha.example.com"), haPatch("https://ha.example.com", kRsa, false), "ha.token",
                     "token_required");
}

static void test_guard_existing_rules_unchanged() {
    // https -> http downgrade.
    assertMergeFails(haConfig("https://ha.example.com:8123", kRsa), haPatch("http://ha.example.com:8123", "", false),
                     "ha.token", "token_required");
    assertMergeFails(haConfig("https://ha.example.com:8123"), haPatch("http://ha.example.com:8123", "", false),
                     "ha.token", "token_required");
    // Different host / port with TLS fields.
    assertMergeFails(haConfig("https://ha.example.com", kRsa), haPatch("https://evil.example.com", kRsa, false),
                     "ha.token", "token_required");
    assertMergeFails(haConfig("https://ha.example.com", kRsa), haPatch("https://ha.example.com:8443", kRsa, false),
                     "ha.token", "token_required");
    assertMergeFails(haConfig("https://ha.example.com", "", true), haPatch("https://evil.example.com", "", true),
                     "ha.token", "token_required");
    // New host with the token supplied works.
    assertKind(Kind::Set, mergeOk(haConfig("https://ha.example.com", kRsa),
                                  haPatch("https://other.example.com", kEc, false, "\"t2\"")),
               "new host with token");
    // Path-only change with unchanged TLS keeps the token.
    assertKind(Kind::Keep, mergeOk(haConfig("https://ha.example.com", kRsa),
                                   haPatch("https://ha.example.com/new/path", kRsa, false)),
               "path only");
}

static void test_guard_url_cleared_requires_empty_tls() {
    const cfg::Config cur = haConfig("https://ha.example.com", kRsa);
    cfg::MergeResult r = mergeOk(cur, haPatch("", "", false));
    assertKind(Kind::Clear, r, "url cleared");
    TEST_ASSERT_TRUE(r.config.ha.url.empty());
    r = mergeOk(haConfig("https://ha.example.com", "", true), haPatch("", "", false, "\"x\""));
    assertKind(Kind::Clear, r, "url cleared, supplied token ignored");
    assertMergeFails(cur, haPatch("", kRsa, false), "ha.ca_pem", "requires_https");
    assertMergeFails(cur, haPatch("", "", true), "ha.tls_insecure", "requires_https");
    // A ha section without the TLS keys while clearing the URL: defaults apply, so it works.
    r = mergeOk(cur, "{\"ha\":{\"url\":\"\"}}");
    assertKind(Kind::Clear, r, "url cleared without tls keys");
}

static void test_guard_tls_errors_reported_before_guard() {
    const cfg::Config cur = haConfig("https://ha.example.com");
    const std::string key = "-----BEGIN PRIVATE KEY-----\nAAAA\n-----END PRIVATE KEY-----\n";
    assertMergeFails(cur, haPatch("https://ha.example.com", key, false), "ha.ca_pem", "not_a_certificate");
    assertMergeFails(cur, haPatch("https://ha.example.com", kRsa + key, false), "ha.ca_pem", "not_a_certificate");
    assertMergeFails(cur, haPatch("https://ha.example.com", pemOf(pkcs8Der()), false), "ha.ca_pem",
                     "not_a_certificate");
    assertMergeFails(cur, haPatch("https://ha.example.com", kRsa + kEc + kThird + kRsa, false), "ha.ca_pem",
                     "too_many");
    assertMergeFails(cur, haPatch("https://ha.example.com", certShapedPem(4097), false), "ha.ca_pem", "length");
    assertMergeFails(cur, haPatch("https://ha.example.com", kRsa, true), "ha.tls_insecure", "conflicts_with_ca_pem");
    // Also when the URL moves to a new host (where token_required would apply).
    assertMergeFails(cur, haPatch("https://evil.example.com", key, false), "ha.ca_pem", "not_a_certificate");
}

static void test_guard_ca_change_with_unrelated_sections() {
    // Only the ha section is guarded; patches without ha never touch TLS settings.
    const cfg::Config cur = haConfig("https://ha.example.com", kRsa);
    const cfg::MergeResult r = mergeOk(cur, "{\"device\":{\"brightness\":50}}");
    TEST_ASSERT_TRUE(r.config.ha == cur.ha);
    assertKind(Kind::Keep, r, "no ha section");
    // Omitting the TLS keys inside a ha section resets them (section replace) = tightening.
    const cfg::MergeResult w = mergeOk(cur, "{\"ha\":{\"url\":\"https://ha.example.com\"}}");
    TEST_ASSERT_TRUE(w.config.ha.caPem.empty());
    assertKind(Kind::Keep, w, "tls keys omitted");
}

// ---------------------------------------------------------------------------
// Size
// ---------------------------------------------------------------------------

static cfg::Config typicalControls24() {
    cfg::Config c;
    const char *entities[] = {"media_player.living_room_speaker_group", "light.kitchen_ceiling_spots",
                              "climate.living_room_thermostat", "cover.bedroom_window_blind"};
    const char *actions[] = {"volume", "brightness", "target_temp", "position"};
    const double steps[] = {2, 5, 0.5, 10};
    const char *presses[] = {"play_pause", "toggle", "none", "toggle"};
    for (int i = 0; i < 24; ++i) {
        cfg::Control k;
        k.entityId = entities[i % 4];
        k.label = std::string(48, 'W');
        k.action = actions[i % 4];
        k.step = steps[i % 4];
        k.press = presses[i % 4];
        c.controls.push_back(k);
    }
    return c;
}

static void test_size_24_controls_plus_4096_pem_validates() {
    cfg::Config c = typicalControls24();
    c.ha.url = "https://ha.example.com";
    c.ha.caPem = certShapedPem(4096);
    const std::string json = cfg::toJson(c);
    TEST_ASSERT_TRUE_MESSAGE(json.size() <= cfg::kMaxConfigJsonBytes, std::to_string(json.size()).c_str());
    TEST_ASSERT_TRUE(cfg::validate(c).ok);
    const cfg::LoadResult r = load(json);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(cfg::LoadStatus::Loaded), static_cast<int>(r.status));
    TEST_ASSERT_TRUE(r.config == c);
    // Same through a patch.
    const cfg::MergeResult m = mergeOk(typicalControls24(), haPatch("https://ha.example.com", c.ha.caPem, false, "\"t\""));
    TEST_ASSERT_TRUE(m.config == c);
}

static void test_size_over_limit_is_too_large() {
    cfg::Config c;
    const std::string id = "media_player." + std::string(255 - 13, 'x');
    for (int i = 0; i < 24; ++i) {
        cfg::Control k;
        k.entityId = id;
        k.label = std::string(48, 'L');
        k.action = "volume";
        k.step = 2;
        k.press = "play_pause";
        c.controls.push_back(k);
    }
    TEST_ASSERT_TRUE(cfg::validate(c).ok);  // 9361 bytes: fine now
    c.ha.url = "https://ha.example.com";
    c.ha.caPem = certShapedPem(4096);
    TEST_ASSERT_TRUE(cfg::toJson(c).size() > cfg::kMaxConfigJsonBytes);
    cfg::ValidationResult r = cfg::validate(c);
    TEST_ASSERT_FALSE(r.ok);
    TEST_ASSERT_EQUAL_STRING("controls", r.field.c_str());
    TEST_ASSERT_EQUAL_STRING("too_large", r.message.c_str());
    // Via patch: controls first, then the CA that tips it over.
    cfg::Config base = c;
    base.ha = cfg::HaConfig();
    assertMergeFails(base, haPatch("https://ha.example.com", c.ha.caPem, false, "\"t\""), "controls", "too_large");
    // Stored blob over the limit does not load.
    JsonDocument doc;
    deserializeJson(doc, cfg::toJson(c));
    std::string big;
    serializeJson(doc, big);
    const cfg::LoadResult l = load(big);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(cfg::LoadStatus::Invalid), static_cast<int>(l.status));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_defaults_tls_fields);
    RUN_TEST(test_old_blob_without_tls_keys_loads_with_defaults);
    RUN_TEST(test_to_json_ha_section_shape);
    RUN_TEST(test_round_trip_ca_and_insecure);
    RUN_TEST(test_redacted_json_has_tls_fields_and_loads_back);
    RUN_TEST(test_stored_pem_is_normalised_on_load);
    RUN_TEST(test_stored_invalid_tls_gives_defaults_invalid);
    RUN_TEST(test_null_tls_keys_keep_defaults);
    RUN_TEST(test_normalize_pem_rules);
    RUN_TEST(test_whitespace_only_ca_is_empty_in_patch);
    RUN_TEST(test_patch_ca_stored_normalised_and_resend_equal);
    RUN_TEST(test_real_certs_accepted);
    RUN_TEST(test_length_boundary_after_normalisation);
    RUN_TEST(test_not_a_certificate_private_key_blocks);
    RUN_TEST(test_not_a_certificate_text_and_malformed_armor);
    RUN_TEST(test_not_a_certificate_relabelled_keys);
    RUN_TEST(test_too_many_certs);
    RUN_TEST(test_invalid_base64);
    RUN_TEST(test_invalid_der);
    RUN_TEST(test_requires_https_validate);
    RUN_TEST(test_requires_https_via_patch);
    RUN_TEST(test_conflicts_with_ca_pem);
    RUN_TEST(test_wrong_types_via_patch);
    RUN_TEST(test_guard_insecure_off_to_on);
    RUN_TEST(test_guard_ca_added_or_changed);
    RUN_TEST(test_guard_empty_ca_after_clear_in_separate_patch);
    RUN_TEST(test_guard_resend_unchanged_keeps_token);
    RUN_TEST(test_guard_tightening_keeps_token);
    RUN_TEST(test_guard_http_to_https_same_host_port_keeps_token);
    RUN_TEST(test_guard_existing_rules_unchanged);
    RUN_TEST(test_guard_url_cleared_requires_empty_tls);
    RUN_TEST(test_guard_tls_errors_reported_before_guard);
    RUN_TEST(test_guard_ca_change_with_unrelated_sections);
    RUN_TEST(test_size_24_controls_plus_4096_pem_validates);
    RUN_TEST(test_size_over_limit_is_too_large);
    return UNITY_END();
}
