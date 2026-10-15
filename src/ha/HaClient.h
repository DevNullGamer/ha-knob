#pragma once

#include <cstdint>
#include <string>

#include "ha/HaSession.h"

// Home Assistant WebSocket client (plan.md §3.7): core::ha::HaSession (state machine,
// protocol) on top of links2004 WebSocketsClient. Main loop only (not thread-safe); the
// web server sees the state through the snapshot the main loop publishes
// (net::web::publishHa).
//
// Transport: the host name is first resolved asynchronously (lwIP DNS in the tcpip task,
// incl. mDNS for ".local"), so a name that does not resolve never blocks the main loop.
// The WebSocket library then connects synchronously with the address from the DNS cache
// (the TCP connect blocks the main loop for up to 2 s, -DWEBSOCKETS_TCP_TIMEOUT=2000, when
// the host does not answer at all). The library's own reconnect and heartbeat are off:
// reconnects follow the session's backoff, liveness is the session's HA ping/pong.
// https endpoints are never connected (HaSession: tls_not_supported; refused here too).
//
// Serial lines (the token is never logged, only its length):
//   HA config url=<url> token_set len=<n>       |  HA config none
//   HA connect attempt=<n> url=ws://<host>:<port><path>
//   HA dns host=<host> ok=<0|1>[ ip=<ip>]
//   HA socket open                               WebSocket handshake done
//   HA state=<state>[ ha_version=<v>][ error=<error>]
//   HA retry                                     web UI Retry accepted
namespace ha {

struct Status {
    core::ha::State state = core::ha::State::NoConfig;
    std::string haVersion;
    std::string lastError;

    bool operator==(const Status &o) const {
        return state == o.state && haVersion == o.haVersion && lastError == o.lastError;
    }
    bool operator!=(const Status &o) const { return !(*this == o); }
};

void begin();

// Applies the stored HA settings (after boot and after every config change). Only a
// different URL or token restarts the connection.
void configure(const std::string &url, const std::string &token, uint32_t nowMs);

// Wi-Fi station link state (connected with an IP address).
void setNetworkUp(bool up, uint32_t nowMs);

// Web UI "Retry": leaves auth_failed (or ends a backoff wait). No-op in other states.
void retry(uint32_t nowMs);

// Drives the socket and the session timers. Reads millis() itself (a TCP connect can
// block inside).
void loop();

const Status &status();

}  // namespace ha
