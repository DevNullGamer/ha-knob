#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// WebSocket client opening handshake (RFC 6455 section 4). Pure: the device adapter does
// the socket I/O and feeds the bytes it reads to parseUpgradeResponse().
namespace core {
namespace ws {

// An upgrade response whose header block (up to and including the blank line) is longer
// than this is rejected ("bad_response").
constexpr size_t kMaxUpgradeResponseBytes = 4096;

// Sec-WebSocket-Key: base64 of 16 random bytes taken from random32() (4 bytes per call,
// big-endian). The caller provides the entropy source (esp_random on the device).
std::string makeWebSocketKey(uint32_t (*random32)());

// "GET <path> HTTP/1.1" request, headers separated by CRLF, ending with a blank line.
// Host carries ":port" only if it is not the scheme default (80 for ws, 443 for wss).
// No Origin and no subprotocol header.
std::string buildUpgradeRequest(const std::string &host, uint16_t port, bool tls,
                                const std::string &path, const std::string &key);

// base64(SHA1(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"))
std::string expectedAccept(const std::string &key);

struct UpgradeResult {
    enum class Status { Incomplete, Ok, Failed };
    Status status = Status::Incomplete;
    size_t headerBytes = 0;  // bytes up to and including "\r\n\r\n" (valid for Ok and Failed
                             // when the terminator was found); the rest belongs to the receiver
    int httpStatus = 0;      // 0 if the status line could not be parsed
    std::string error;       // Failed only: "http <code>", "bad_accept" or "bad_response"
};

// Parses the bytes received so far. Incomplete until "\r\n\r\n" is seen. Ok needs status
// 101, "Upgrade: websocket", a Connection header containing the token "upgrade" and a
// Sec-WebSocket-Accept equal to expectedAccept(key). Header names and the Upgrade/Connection
// values are case-insensitive; the accept value is compared exactly.
UpgradeResult parseUpgradeResponse(const char *data, size_t len, const std::string &key);

}  // namespace ws
}  // namespace core
