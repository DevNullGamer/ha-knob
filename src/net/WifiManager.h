#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Wi-Fi station, setup access point, captive DNS and mDNS (plan.md §3.9).
// Main loop only (not thread-safe); the web server sees the state through the snapshot
// the main loop publishes (net::web::publishNetwork).
//
//  - No SSID configured: setup AP "HA-Knob-XXXX" (WPA2, ap_pw) at 192.168.4.1 with a
//    wildcard DNS server (captive portal). Mode "ap".
//  - SSID configured: STA connect. Not connected within 30 s: the setup AP comes up as
//    well (mode "ap_sta") and STA retries every 60 s. Once STA connects, the AP stays up
//    for 60 s more (so the setup page can show the new IP), then shuts down (mode "sta").
//    A lost connection is retried at once, with the same 30 s fallback.
//  - mDNS "<hostname>.local" with an _http._tcp service, in every mode (retried every
//    30 s while MDNS.begin() fails).
// The driver's own auto-reconnect and NVS credential storage are disabled: the
// credentials live only in the ConfigStore and every retry is scheduled here. The link
// state comes from ESP-IDF (association + STA netif address, polled every 100 ms), not
// from WiFi.status(). Retries never interrupt an association waiting for DHCP or a scan.
//
// Serial lines:
//   WIFI ap ssid=<ap ssid> ip=192.168.4.1     setup AP up
//   WIFI ap off                               setup AP shut down
//   WIFI sta connecting ssid=<ssid>
//   WIFI sta connected ip=<ip> rssi=<dBm>
//   WIFI sta lost
//   WIFI fallback ap_sta                      STA did not connect within 30 s
//   WIFI restart reason=config                credentials changed
//   WIFI mdns host=<hostname>.local ok=<0|1>
//   WIFI scan done networks=<n> failed=<0|1>  |  WIFI scan timeout  |  WIFI scan start failed err=..
//   WIFI scan stop grace expired (no SCAN_DONE)   a stopped scan never reported SCAN_DONE
namespace net {
namespace wifi {

enum class Mode : uint8_t { Off, Ap, Sta, ApSta };

// "off", "ap", "sta", "ap_sta" (as in /api/status and the HB line).
const char *modeName(Mode mode);

struct Status {
    Mode mode = Mode::Off;
    std::string ssid;           // configured STA SSID ("" = not configured)
    bool staConnected = false;
    std::string staIp;          // "" when not connected
    int32_t rssi = 0;           // dBm, 0 when not connected
    bool apActive = false;
    std::string apSsid;         // "HA-Knob-XXXX"
    std::string hostname;

    bool operator==(const Status &o) const;
    bool operator!=(const Status &o) const { return !(*this == o); }
};

struct ScanNetwork {
    std::string ssid;
    int32_t rssi = 0;
    bool secure = true;
};

constexpr const char *kApIp = "192.168.4.1";

// Starts Wi-Fi with the stored settings. apPassword is the stored ap_pw (at least 8
// characters, else the AP is not started and an ERR line is logged).
void begin(const std::string &ssid, const std::string &password, const std::string &hostname,
           const std::string &apPassword, uint32_t nowMs);

// Re-applies the settings after a config change. Wi-Fi is restarted (1 s later, so the
// HTTP response that caused it is delivered first) only if the SSID or the password
// differ from the ones in use; a hostname change only restarts mDNS. Returns true when
// a Wi-Fi restart was scheduled.
bool applyConfig(const std::string &ssid, const std::string &password,
                 const std::string &hostname, uint32_t nowMs);

void loop(uint32_t nowMs);

// Current state (refreshed by loop() about once a second and on every change).
const Status &status();

// Requests an asynchronous scan (no-op while one is pending or running). The start is
// retried for up to 6 s while the driver is busy (e.g. the STA is connecting); a scan
// that cannot start or does not finish within 15 s ends as failed.
void requestScan(uint32_t nowMs);

// After a requested scan ended: fills networks (de-duplicated by SSID,
// strongest first, hidden networks skipped), sets failed and returns true, once.
bool takeScanResults(std::vector<ScanNetwork> &networks, bool &failed);

}  // namespace wifi
}  // namespace net
