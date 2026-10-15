#pragma once

#include <cstdint>
#include <string>

#include "ha/HaSession.h"

// Home Assistant WebSocket client (plan.md §3.7): core::ha::HaSession (state machine,
// protocol) on top of ha::WsTransport (the project's own WebSocket client, see
// src/ha/WsTransport.h for the transport phases, limits and its serial lines). Main loop
// only (not thread-safe); the web server sees the state through the snapshot the main loop
// publishes (net::web::publishHa). Reconnects follow the session's backoff, liveness is the
// session's HA ping/pong; server WebSocket PINGs are answered by the transport.
// https endpoints are never connected (HaSession: tls_not_supported; refused by the
// transport too).
//
// Serial lines (the token is never logged, only its length):
//   HA config url=<url> token_set len=<n>       |  HA config none
//   HA state=<state>[ ha_version=<v>][ error=<error>]
//   HA retry                                     web UI Retry accepted
//   HA connect / dns / tcp / socket / rx / frame ...   transport (src/ha/WsTransport.h)
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
