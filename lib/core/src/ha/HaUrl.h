#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// Home Assistant base URL -> WebSocket endpoint (plan.md §3.7). Pure. This is the only URL
// parser in the project: ConfigModel validates ha.url and compares endpoints (token guard)
// with it as well.
//
// Accepted: http(s)://host[:port][/prefix]
//  - scheme "http" or "https", case-insensitive; https means TLS (wss)
//  - host: letters, digits, '-' and '.', 1-253 characters, lower-cased (no IPv6 literal,
//    no user info)
//  - port: 1-65535, default 80 (http) / 443 (https)
//  - prefix: optional path of printable ASCII without spaces; trailing slashes are dropped.
//    A '?' or '#' in it is accepted (ha.url validation is unchanged since v0.4.0) but
//    flagged: a query or fragment cannot be part of a base URL, so HaSession refuses to
//    connect to such a URL ("invalid_url").
// The WebSocket path is "<prefix>/api/websocket":
//   "http://192.168.1.10:8123"        -> 192.168.1.10, 8123, "/api/websocket"
//   "http://homeassistant.local:8123/" -> homeassistant.local, 8123, "/api/websocket"
//   "https://x.ui.nabu.casa/ha/"       -> x.ui.nabu.casa, 443, "/ha/api/websocket", tls
namespace core {
namespace ha {

constexpr size_t kMaxHostLen = 253;
constexpr const char *kWebSocketPath = "/api/websocket";

struct HaUrl {
    bool ok = false;
    bool tls = false;   // https / wss
    std::string host;   // lower-cased
    uint16_t port = 0;  // explicit, or 80 / 443
    std::string path;   // WebSocket path, e.g. "/api/websocket" or "/prefix/api/websocket"
    bool hasQueryOrFragment = false;  // the prefix contains '?' or '#' (path then unusable)
};

// Parses an HA base URL; ok=false (other fields unspecified) if it is not acceptable.
HaUrl parseHaUrl(const std::string &url);

// "ws://host:port/path" or "wss://..." (for logs). "" if !url.ok.
std::string webSocketUrl(const HaUrl &url);

}  // namespace ha
}  // namespace core
