#pragma once

#include <cstdint>
#include <vector>

#include "ha/HaClient.h"
#include "net/WifiManager.h"

class ConfigStore;

// HTTP server on port 80 (ESPAsyncWebServer): REST API and the setup UI from LittleFS
// (plan.md §3.8). Every request passes core::web::checkRequest (Host allow-list, captive
// portal redirect, methods, CSRF header, body limit) and, when a web password is set,
// HTTP Basic auth (user "admin").
//
// Endpoints:
//   GET  /api/status         device, Wi-Fi and HA state (ha:{state, ha_version, last_error})
//   GET  /api/config         redacted config (secrets only as *_set flags)
//   POST /api/config         patch (ConfigStore::applyPatch): 200 {ok,restart_wifi},
//                            400 {error,field}, 500 {error:"nvs_write_failed",field}
//   GET  /api/wifi/scan      cached scan results; starts a new scan if none is fresh (or it failed)
//   POST /api/ha/retry       200 {ok:true}; the main loop leaves auth_failed (or ends a
//                            backoff wait) and reconnects to Home Assistant
//   POST /api/reboot         200 {ok:true}, the main loop reboots shortly after
//   POST /api/factory-reset  200 {ok:true}, the main loop resets the config and reboots
//   GET  /*                  static files from LittleFS (index.html by default)
//
// Threading: request callbacks run in the async_tcp task. They never touch LVGL or Wi-Fi.
// They read the snapshot published by the main loop (under a mutex), write the config
// only through ConfigStore::applyPatch (thread-safe), and hand work to the main loop with
// atomic flags (take* functions below, main loop only).
//
// Serial lines:
//   WEB started port=80 fs=<0|1> files=<n>
//   WEB config saved restart_wifi=<0|1>   |   WEB config rejected field=.. error=..
//   WEB reboot requested  |  WEB factory reset requested  |  WEB ha retry requested
namespace net {
namespace web {

enum class SystemRequest : uint8_t { None, Reboot, FactoryReset };

// Mounts LittleFS and starts the server. Call after wifi::begin(). touchVariant must stay
// valid (a string literal).
void begin(ConfigStore &store, const char *touchVariant);

// Main loop: the network state the API and the guards use.
void publishNetwork(const wifi::Status &status);

// Main loop: results of a scan requested through takeScanRequest().
void publishScan(const std::vector<wifi::ScanNetwork> &networks, bool failed, uint32_t nowMs);

// Main loop: the Home Assistant connection state for /api/status.
void publishHa(const ha::Status &status);

// Main loop: true once after a POST /api/config wrote the config (re-read it then).
bool takeConfigDirty();

// Main loop: true once after GET /api/wifi/scan asked for a new scan.
bool takeScanRequest();

// Main loop: true once after POST /api/ha/retry.
bool takeHaRetry();

// Main loop: a pending reboot / factory reset request (once).
SystemRequest takeSystemRequest();

}  // namespace web
}  // namespace net
