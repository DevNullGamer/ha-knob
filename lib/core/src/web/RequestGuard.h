#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// Request guards of the device web server (plan.md §3.8). Pure: the web server copies the
// relevant request fields into RequestInfo and acts on the Decision.
//
// checkRequest() applies, in this order:
//  1. Host allow-list (DNS-rebinding defence): the device's STA IPv4, 192.168.4.1 (setup
//     AP), "<hostname>" and "<hostname>.local" (case-insensitive, optional trailing dot,
//     optional ":port"). Anything else is 403, except that while the setup AP is up a
//     GET/HEAD of a non-API path is redirected (302) to http://192.168.4.1/ (captive
//     portal).
//  2. Methods: GET, HEAD and POST only; OPTIONS and everything else is 405. No CORS
//     headers are ever sent, so cross-origin preflights fail.
//  3. POST (CSRF): Content-Type must be application/json (parameters such as charset
//     allowed) and the header X-HAKnob must be "1", else 403. A chunked POST body (no
//     Content-Length) is 411.
//  4. Body limit: Content-Length above kMaxBodyBytes is 413 (any method).
// Authentication (optional web password) is separate: basicAuthMatches().
namespace core {
namespace web {

constexpr size_t kMaxBodyBytes = 16 * 1024;
constexpr const char *kApIp = "192.168.4.1";
constexpr const char *kCaptivePortalUrl = "http://192.168.4.1/";
constexpr const char *kCsrfHeaderName = "X-HAKnob";
constexpr const char *kAuthUser = "admin";

enum class Method : uint8_t { Get, Head, Post, Options, Other };

// What the device currently is; filled from the main loop's network snapshot.
struct GuardContext {
    std::string hostname;   // device.hostname, e.g. "ha-knob"
    std::string deviceIp;   // STA IPv4 ("192.168.1.50"); empty when not connected
    bool apActive = false;  // setup AP is up (enables the captive-portal redirect)
};

struct RequestInfo {
    Method method = Method::Get;
    std::string host;         // Host header value, e.g. "ha-knob.local:80"
    std::string path;         // URL path without query, e.g. "/api/status"
    std::string contentType;  // Content-Type header ("" if absent)
    bool hasCsrfHeader = false;
    std::string csrfHeader;   // value of X-HAKnob when present
    size_t contentLength = 0;  // Content-Length (0 if absent)
    bool chunked = false;      // Transfer-Encoding: chunked
};

enum class Verdict : uint8_t {
    Allow,
    Redirect,          // 302 to kCaptivePortalUrl
    Forbidden,         // 403
    MethodNotAllowed,  // 405
    LengthRequired,    // 411
    PayloadTooLarge,   // 413
};

struct Decision {
    Verdict verdict = Verdict::Allow;
    int status = 200;            // HTTP status for the verdict (200 for Allow)
    const char *reason = "";     // machine-friendly, e.g. "host", "csrf", "content_type"

    bool allowed() const { return verdict == Verdict::Allow; }
};

Decision checkRequest(const RequestInfo &request, const GuardContext &context);

// ---- Helpers (exposed for tests and reuse) ---------------------------------------------

// "GET" -> Method::Get etc. (case-sensitive, as HTTP methods are).
Method methodFromString(const std::string &method);

// Splits a Host header into its lower-cased name without ":port" and trailing dot.
// Returns false for an empty name, an IPv6 literal, a bad port (not 1-65535 digits) or
// characters outside [A-Za-z0-9.-].
bool normaliseHost(const std::string &hostHeader, std::string &name);

bool isAllowedHost(const std::string &hostHeader, const GuardContext &context);

// "application/json", optionally followed by ";" parameters; case-insensitive.
bool isJsonContentType(const std::string &contentType);

// "/api" or anything below "/api/".
bool isApiPath(const std::string &path);

// Checks an Authorization header ("Basic <base64(user:password)>") against the given
// credentials. The comparison does not stop at the first differing byte.
bool basicAuthMatches(const std::string &authorizationHeader, const std::string &user,
                      const std::string &password);

// Strict base64 (RFC 4648, with padding). Returns false on any invalid input.
bool base64Decode(const std::string &in, std::string &out);

// Equality whose running time depends only on the lengths, not on where they differ.
bool constantTimeEquals(const std::string &a, const std::string &b);

}  // namespace web
}  // namespace core
