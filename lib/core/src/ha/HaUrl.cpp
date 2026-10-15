#include "ha/HaUrl.h"

#include <cstring>

namespace core {
namespace ha {

namespace {

char toLowerAscii(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }
bool isLowerAlpha(char c) { return c >= 'a' && c <= 'z'; }
bool isDigit(char c) { return c >= '0' && c <= '9'; }

bool startsWithNoCase(const std::string &s, const char *prefix) {
    const size_t len = std::strlen(prefix);
    if (s.size() < len) return false;
    for (size_t i = 0; i < len; ++i) {
        if (toLowerAscii(s[i]) != prefix[i]) return false;
    }
    return true;
}

}  // namespace

HaUrl parseHaUrl(const std::string &url) {
    HaUrl out;
    size_t pos = 0;
    if (startsWithNoCase(url, "https://")) {
        out.tls = true;
        pos = 8;
    } else if (startsWithNoCase(url, "http://")) {
        pos = 7;
    } else {
        return out;
    }

    const size_t hostStart = pos;
    while (pos < url.size() && url[pos] != ':' && url[pos] != '/') {
        const char c = toLowerAscii(url[pos]);
        if (!isLowerAlpha(c) && !isDigit(c) && c != '-' && c != '.') return out;
        out.host.push_back(c);
        ++pos;
    }
    if (pos == hostStart || out.host.size() > kMaxHostLen) return out;

    uint32_t port = out.tls ? 443 : 80;
    if (pos < url.size() && url[pos] == ':') {
        ++pos;
        port = 0;
        size_t digits = 0;
        while (pos < url.size() && isDigit(url[pos])) {
            port = port * 10 + static_cast<uint32_t>(url[pos] - '0');
            ++pos;
            if (++digits > 5) return out;
        }
        if (digits == 0 || port < 1 || port > 65535) return out;
    }
    if (pos < url.size() && url[pos] != '/') return out;

    std::string prefix = url.substr(pos);
    for (const char ch : prefix) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c <= 0x20 || c >= 0x7F) return out;
        if (c == '?' || c == '#') out.hasQueryOrFragment = true;
    }
    while (!prefix.empty() && prefix.back() == '/') prefix.pop_back();

    out.port = static_cast<uint16_t>(port);
    out.path = prefix + kWebSocketPath;
    out.ok = true;
    return out;
}

std::string webSocketUrl(const HaUrl &url) {
    if (!url.ok) return std::string();
    return std::string(url.tls ? "wss://" : "ws://") + url.host + ":" + std::to_string(url.port) + url.path;
}

}  // namespace ha
}  // namespace core
