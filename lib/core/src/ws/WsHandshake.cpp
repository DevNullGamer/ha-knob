#include "ws/WsHandshake.h"

#include <array>

#include "util/Base64.h"
#include "util/Sha1.h"

namespace core {
namespace ws {

namespace {

const char kGuid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

char lower(char c) { return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c; }

bool equalsIgnoreCase(const std::string &a, const char *b) {
    size_t i = 0;
    for (; i < a.size() && b[i] != '\0'; ++i) {
        if (lower(a[i]) != lower(b[i])) return false;
    }
    return i == a.size() && b[i] == '\0';
}

std::string trim(const std::string &s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t')) --e;
    return s.substr(b, e - b);
}

// True if the comma separated list contains `token` (case-insensitive).
bool listHasToken(const std::string &value, const char *token) {
    size_t pos = 0;
    while (pos <= value.size()) {
        size_t comma = value.find(',', pos);
        if (comma == std::string::npos) comma = value.size();
        if (equalsIgnoreCase(trim(value.substr(pos, comma - pos)), token)) return true;
        pos = comma + 1;
    }
    return false;
}

UpgradeResult failed(size_t headerBytes, int httpStatus, const std::string &error) {
    UpgradeResult r;
    r.status = UpgradeResult::Status::Failed;
    r.headerBytes = headerBytes;
    r.httpStatus = httpStatus;
    r.error = error;
    return r;
}

// "HTTP/1.x SSS reason": returns the 3-digit status, 0 if malformed.
int parseStatusLine(const std::string &line) {
    if (line.size() < 12 || line.compare(0, 7, "HTTP/1.") != 0) return 0;
    if (line[7] != '0' && line[7] != '1') return 0;
    if (line[8] != ' ') return 0;
    int code = 0;
    for (int i = 9; i < 12; ++i) {
        if (line[i] < '0' || line[i] > '9') return 0;
        code = code * 10 + (line[i] - '0');
    }
    if (line.size() > 12 && line[12] != ' ') return 0;
    return code;
}

}  // namespace

std::string makeWebSocketKey(uint32_t (*random32)()) {
    uint8_t raw[16];
    for (int i = 0; i < 4; ++i) {
        uint32_t v = random32();
        raw[i * 4] = uint8_t(v >> 24);
        raw[i * 4 + 1] = uint8_t(v >> 16);
        raw[i * 4 + 2] = uint8_t(v >> 8);
        raw[i * 4 + 3] = uint8_t(v);
    }
    return util::base64Encode(raw, sizeof(raw));
}

std::string buildUpgradeRequest(const std::string &host, uint16_t port, bool tls,
                                const std::string &path, const std::string &key) {
    std::string req = "GET " + path + " HTTP/1.1\r\nHost: " + host;
    if (port != (tls ? 443 : 80)) req += ":" + std::to_string(port);
    req += "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + key +
           "\r\nSec-WebSocket-Version: 13\r\n\r\n";
    return req;
}

std::string expectedAccept(const std::string &key) {
    std::string in = key + kGuid;
    std::array<uint8_t, 20> digest = util::sha1(reinterpret_cast<const uint8_t *>(in.data()), in.size());
    return util::base64Encode(digest.data(), digest.size());
}

UpgradeResult parseUpgradeResponse(const char *data, size_t len, const std::string &key) {
    // Find the end of the header block within the size limit.
    size_t scan = len < kMaxUpgradeResponseBytes ? len : kMaxUpgradeResponseBytes;
    size_t end = std::string::npos;
    for (size_t i = 0; i + 4 <= scan; ++i) {
        if (data[i] == '\r' && data[i + 1] == '\n' && data[i + 2] == '\r' && data[i + 3] == '\n') {
            end = i + 4;
            break;
        }
    }
    if (end == std::string::npos) {
        if (len >= kMaxUpgradeResponseBytes) return failed(0, 0, "bad_response");
        return UpgradeResult();  // Incomplete
    }

    std::string head(data, end - 4);  // without the final blank line
    size_t lineEnd = head.find("\r\n");
    std::string statusLine = head.substr(0, lineEnd);
    int code = parseStatusLine(statusLine);
    if (code == 0) return failed(end, 0, "bad_response");
    if (code != 101) return failed(end, code, "http " + std::to_string(code));

    bool upgradeOk = false, connectionOk = false, acceptSeen = false, acceptOk = false;
    std::string expected = expectedAccept(key);
    size_t pos = (lineEnd == std::string::npos) ? head.size() : lineEnd + 2;
    while (pos < head.size()) {
        size_t next = head.find("\r\n", pos);
        if (next == std::string::npos) next = head.size();
        std::string line = head.substr(pos, next - pos);
        pos = next + 2;
        size_t colon = line.find(':');
        if (colon == std::string::npos || colon == 0 || line[0] == ' ' || line[0] == '\t') {
            return failed(end, code, "bad_response");
        }
        std::string name = line.substr(0, colon);
        std::string value = trim(line.substr(colon + 1));
        if (equalsIgnoreCase(name, "upgrade")) {
            if (equalsIgnoreCase(value, "websocket")) upgradeOk = true;
        } else if (equalsIgnoreCase(name, "connection")) {
            if (listHasToken(value, "upgrade")) connectionOk = true;
        } else if (equalsIgnoreCase(name, "sec-websocket-accept")) {
            acceptSeen = true;
            if (value == expected) acceptOk = true;
        }
    }
    if (!upgradeOk || !connectionOk) return failed(end, code, "bad_response");
    if (!acceptSeen || !acceptOk) return failed(end, code, "bad_accept");

    UpgradeResult r;
    r.status = UpgradeResult::Status::Ok;
    r.headerBytes = end;
    r.httpStatus = code;
    return r;
}

}  // namespace ws
}  // namespace core
