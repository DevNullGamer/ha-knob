#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

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
// Entities (plan-step6.md §3.1, §3.3): the configured controls' entity ids are subscribed
// (subscribe_entities, resubscribed on the same connection when the set changes) and kept
// live in entities(), in PSRAM. requestStates() fetches the entity list (get_states); the
// session builds the catalog in slices, then loop() serialises it in slices of about 8 ms
// (core::ha::CatalogJsonWriter, PSRAM; catalogState() stays Building meanwhile) into
// catalogJson(), which is published only when complete and stays available while
// catalogState() is Ready.
//
// Serial lines (the token and the CA PEM are never logged, only their lengths):
//   HA config url=<url> token_set len=<n> tls=<none|bundle|custom_ca|insecure>[ ca_len=<n>]
//   HA config none url_set=<0|1> token_set=<0|1>
//   HA state=<state>[ ha_version=<v>][ error=<error>]
//   HA retry                                     web UI Retry accepted
//   HA subscribe id=<n> ids=<k> | HA unsubscribe id=<n> subscription=<old>
//   HA unsubscribe failed: <msg> | HA snapshot entities=<n> | HA event dropped error=<e>
//   HA states request id=<n> | HA states retained len=<bytes>
//   HA states late result dropped len=<bytes>
//   HA catalog done total=<n> supported=<n> skipped=<n> build_ms=<n> json=<bytes> json_ms=<n>
//                   json_passes=<n> json_slice_max_ms=<n> json_regrow=<n>   (one line)
//   HA catalog failed error=<e>
//   HA connect / dns / tcp / tls / socket / rx / frame ...   transport (src/ha/WsTransport.h)
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

// Applies the stored HA settings and the controls' entity ids (after boot and after every
// config change). Only a different URL, token or TLS setting restarts the connection; a
// different id set (order and duplicates do not matter) resubscribes on the same one.
void configure(const std::string &url, const std::string &token, const core::ha::TlsSettings &tls,
               const std::vector<std::string> &entityIds, uint32_t nowMs);

// Wi-Fi station link state (connected with an IP address).
void setNetworkUp(bool up, uint32_t nowMs);

// Web UI "Retry": leaves auth_failed (or ends a backoff wait). No-op in other states.
void retry(uint32_t nowMs);

// Starts fetching the entity list (HaSession::requestStates: Ready only, a running fetch
// is kept); a started fetch drops the previous catalog. False when HA is not Ready or
// the send failed.
bool requestStates(uint32_t nowMs);

// Drives the socket and the session timers, takes a finished catalog and writes one slice
// of its JSON. Reads millis() itself (a TCP connect can block inside).
void loop();

const Status &status();

// Live state of the subscribed entities; version() changes with every effective update.
const core::ha::EntityStore &entities();
// Unique entity ids configured (the subscription's size when subscribed).
size_t subscribedCount();
// Milliseconds since the current subscribe_entities was sent (nullopt: none active or
// pending), for the screens' "waiting..." / "not found".
std::optional<uint32_t> msSinceSubscribe(uint32_t nowMs);

// The entity list fetch: state, error (Failed) and, while Ready, the catalog as the
// GET /api/ha/entities body (nullptr otherwise) with its number of entities.
core::ha::StatesState catalogState();
const std::string &catalogError();
std::shared_ptr<const std::string> catalogJson();
size_t catalogCount();
// Incremented whenever catalogState(), catalogError() or catalogJson() change.
uint32_t catalogChangeCount();

}  // namespace ha
