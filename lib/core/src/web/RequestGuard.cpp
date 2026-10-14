#include "web/RequestGuard.h"

namespace core {
namespace web {

namespace {

char toLowerAscii(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

std::string toLowerAscii(const std::string &s) {
    std::string out(s);
    for (char &c : out) c = toLowerAscii(c);
    return out;
}

bool isSpace(char c) { return c == ' ' || c == '\t'; }

std::string trim(const std::string &s) {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && isSpace(s[b])) ++b;
    while (e > b && isSpace(s[e - 1])) --e;
    return s.substr(b, e - b);
}

bool isHostChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
           c == '-';
}

bool isValidPort(const std::string &port) {
    if (port.empty() || port.size() > 5) return false;
    uint32_t value = 0;
    for (char c : port) {
        if (c < '0' || c > '9') return false;
        value = value * 10 + static_cast<uint32_t>(c - '0');
    }
    return value >= 1 && value <= 65535;
}

Decision decide(Verdict verdict, int status, const char *reason) {
    Decision d;
    d.verdict = verdict;
    d.status = status;
    d.reason = reason;
    return d;
}

int base64Value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

}  // namespace

Method methodFromString(const std::string &method) {
    if (method == "GET") return Method::Get;
    if (method == "HEAD") return Method::Head;
    if (method == "POST") return Method::Post;
    if (method == "OPTIONS") return Method::Options;
    return Method::Other;
}

bool normaliseHost(const std::string &hostHeader, std::string &name) {
    name.clear();
    const std::string host = trim(hostHeader);
    if (host.empty() || host[0] == '[') return false;  // IPv6 literals are not served
    std::string n = host;
    const size_t colon = host.find(':');
    if (colon != std::string::npos) {
        if (!isValidPort(host.substr(colon + 1))) return false;  // also rejects a second ':'
        n = host.substr(0, colon);
    }
    if (!n.empty() && n.back() == '.') n.pop_back();  // "ha-knob.local." is the same name
    if (n.empty()) return false;
    for (char c : n) {
        if (!isHostChar(c)) return false;
    }
    name = toLowerAscii(n);
    return true;
}

bool isAllowedHost(const std::string &hostHeader, const GuardContext &context) {
    std::string name;
    if (!normaliseHost(hostHeader, name)) return false;
    if (name == kApIp) return true;
    if (!context.deviceIp.empty() && name == toLowerAscii(context.deviceIp)) return true;
    const std::string hostname = toLowerAscii(context.hostname);
    return !hostname.empty() && (name == hostname || name == hostname + ".local");
}

bool isJsonContentType(const std::string &contentType) {
    std::string type = contentType;
    const size_t semicolon = type.find(';');
    if (semicolon != std::string::npos) type = type.substr(0, semicolon);
    return toLowerAscii(trim(type)) == "application/json";
}

bool isApiPath(const std::string &path) {
    return path == "/api" || path.compare(0, 5, "/api/") == 0;
}

Decision checkRequest(const RequestInfo &request, const GuardContext &context) {
    if (!isAllowedHost(request.host, context)) {
        const bool safeMethod = request.method == Method::Get || request.method == Method::Head;
        if (context.apActive && safeMethod && !isApiPath(request.path)) {
            return decide(Verdict::Redirect, 302, "captive_portal");
        }
        return decide(Verdict::Forbidden, 403, "host");
    }
    if (request.method != Method::Get && request.method != Method::Head &&
        request.method != Method::Post) {
        return decide(Verdict::MethodNotAllowed, 405, "method");
    }
    if (request.method == Method::Post) {
        if (!isJsonContentType(request.contentType)) {
            return decide(Verdict::Forbidden, 403, "content_type");
        }
        if (!request.hasCsrfHeader || trim(request.csrfHeader) != "1") {
            return decide(Verdict::Forbidden, 403, "csrf");
        }
        if (request.chunked) {
            return decide(Verdict::LengthRequired, 411, "length_required");
        }
    }
    if (request.contentLength > kMaxBodyBytes) {
        return decide(Verdict::PayloadTooLarge, 413, "body_too_large");
    }
    return Decision();
}

bool base64Decode(const std::string &in, std::string &out) {
    out.clear();
    if (in.size() % 4 != 0) return false;
    out.reserve(in.size() / 4 * 3);
    for (size_t i = 0; i < in.size(); i += 4) {
        const bool last = i + 4 == in.size();
        int v[4];
        int pad = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = in[i + k];
            if (c == '=' && last && k >= 2) {
                v[k] = 0;
                ++pad;
                continue;
            }
            if (pad > 0) return false;  // data after padding
            v[k] = base64Value(c);
            if (v[k] < 0) return false;
        }
        const uint32_t triple = (static_cast<uint32_t>(v[0]) << 18) | (static_cast<uint32_t>(v[1]) << 12) |
                                (static_cast<uint32_t>(v[2]) << 6) | static_cast<uint32_t>(v[3]);
        out.push_back(static_cast<char>((triple >> 16) & 0xFF));
        if (pad < 2) out.push_back(static_cast<char>((triple >> 8) & 0xFF));
        if (pad < 1) out.push_back(static_cast<char>(triple & 0xFF));
    }
    return true;
}

bool constantTimeEquals(const std::string &a, const std::string &b) {
    // Always walk the longer string so the time depends only on the lengths.
    const size_t n = a.size() > b.size() ? a.size() : b.size();
    unsigned diff = a.size() == b.size() ? 0u : 1u;
    for (size_t i = 0; i < n; ++i) {
        const unsigned char ca = i < a.size() ? static_cast<unsigned char>(a[i]) : 0;
        const unsigned char cb = i < b.size() ? static_cast<unsigned char>(b[i]) : 0;
        diff |= static_cast<unsigned>(ca ^ cb);
    }
    return diff == 0;
}

bool basicAuthMatches(const std::string &authorizationHeader, const std::string &user,
                      const std::string &password) {
    const std::string header = trim(authorizationHeader);
    const size_t space = header.find(' ');
    if (space == std::string::npos || toLowerAscii(header.substr(0, space)) != "basic") return false;
    std::string decoded;
    if (!base64Decode(trim(header.substr(space + 1)), decoded)) return false;
    return constantTimeEquals(decoded, user + ":" + password);
}

}  // namespace web
}  // namespace core
