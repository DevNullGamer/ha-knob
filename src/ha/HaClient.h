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
// https URLs connect over wss in the TLS mode the settings select (bundle, custom_ca,
// insecure; see src/ha/WsTransport.h).
//
// Serial lines (the token and the CA PEM are never logged, only their lengths):
//   HA config url=<url> token_set len=<n> tls=<none|bundle|custom_ca|insecure>[ ca_len=<n>]
//   HA config none url_set=<0|1> token_set=<0|1>
//   HA state=<state>[ ha_version=<v>][ error=<error>]
//   HA retry                                     web UI Retry accepted
//   HA connect / dns / tcp / socket / rx / frame ...   transport (src/ha/WsTransport.h)
namespace ha {

struct Status {
    core::ha::State state = core::ha::State::NoConfig;
    std::string haVersion;
    std::string lastError;
    core::ha::TlsMode tls = core::ha::TlsMode::None;  // mode of the configured URL

    bool operator==(const Status &o) const {
        return state == o.state && haVersion == o.haVersion && lastError == o.lastError && tls == o.tls;
    }
    bool operator!=(const Status &o) const { return !(*this == o); }
};

void begin();

// Applies the stored HA settings (after boot and after every config change). Only a
// different URL, token or TLS setting restarts the connection.
void configure(const std::string &url, const std::string &token, const core::ha::TlsSettings &tls,
               uint32_t nowMs);

// Wi-Fi station link state (connected with an IP address).
void setNetworkUp(bool up, uint32_t nowMs);

// Web UI "Retry": leaves auth_failed (or ends a backoff wait). No-op in other states.
void retry(uint32_t nowMs);

// Drives the socket and the session timers. Reads millis() itself (a TCP connect can
// block inside).
void loop();

const Status &status();

}  // namespace ha
