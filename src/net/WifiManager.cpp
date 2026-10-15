#include "net/WifiManager.h"

#include <Arduino.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <WiFi.h>
#include <esp_mac.h>
#include <esp_wifi.h>

#include <algorithm>

#include "util/Log.h"

namespace net {
namespace wifi {

namespace {

constexpr uint32_t kApFallbackMs = 30000;      // STA not connected -> AP_STA
constexpr uint32_t kRetryBeforeApMs = 15000;   // one extra STA attempt before the fallback
constexpr uint32_t kRetryWithApMs = 60000;     // STA retry interval while the AP is up
constexpr uint32_t kApLingerMs = 60000;        // AP kept up after STA connected
constexpr uint32_t kApplyDelayMs = 1000;       // let the HTTP response go out first
constexpr uint32_t kLinkPollMs = 100;          // how often the STA link is read
constexpr uint32_t kStatusRefreshMs = 1000;
constexpr uint8_t kDnsPort = 53;
constexpr size_t kMinApPasswordLen = 8;
constexpr size_t kMaxScanResults = 30;
// Scans: short active dwell (the STA leaves its channel for at most this long per scanned
// channel, so HTTP keeps flowing), own timeout, start retried while the driver is busy.
constexpr uint32_t kScanDwellMaxMs = 120;
constexpr uint32_t kScanTimeoutMs = 15000;
constexpr uint32_t kScanStartRetryMs = 1000;
constexpr uint32_t kScanStartGiveUpMs = 6000;
constexpr uint32_t kScanStopGraceMs = 3000;    // wait for the SCAN_DONE of a stopped scan
constexpr uint32_t kMdnsRetryMs = 30000;       // MDNS.begin() retried while it fails

DNSServer dns;

// Settings in use.
std::string ssid_;
std::string password_;
std::string hostname_;
std::string apPassword_;
std::string apSsid_;

bool started = false;
bool apUp = false;
bool staConnected = false;
bool mdnsUp = false;
uint32_t mdnsTriedAtMs = 0;
uint32_t attemptStartMs = 0;  // start of the current connect series (begin, restart, loss)
uint32_t lastBeginMs = 0;
uint32_t connectedAtMs = 0;
bool restartPending = false;
uint32_t restartAtMs = 0;
uint32_t lastLinkPollMs = 0;
uint32_t lastStatusMs = 0;

bool scanRequested = false;  // waiting for the driver to accept a scan start
uint32_t scanRequestedAtMs = 0;
uint32_t scanLastTryMs = 0;
bool scanning = false;
uint32_t scanStartedAtMs = 0;
bool scanStopping = false;  // a stopped scan's late SCAN_DONE is still expected
uint32_t scanStopAtMs = 0;
bool scanDone = false;
bool scanFailed = false;
std::vector<ScanNetwork> scanResults;

Status status_;

std::string makeApSsid() {
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char buf[16];
    snprintf(buf, sizeof(buf), "HA-Knob-%02X%02X", mac[4], mac[5]);
    return buf;
}

// The STA link is read from ESP-IDF itself (association and the STA netif address) at
// every poll, rather than from WiFi.status(), a single value the Arduino event task
// derives from events. "Connected" means associated and an address assigned, which is
// exactly when the device answers on that IP.
bool staAssociated() {
    wifi_ap_record_t info;
    return esp_wifi_sta_get_ap_info(&info) == ESP_OK;
}

uint32_t staIpAddress() { return static_cast<uint32_t>(WiFi.localIP()); }

void startMdns() {
    if (mdnsUp) {
        MDNS.end();
        mdnsUp = false;
    }
    mdnsTriedAtMs = millis();
    mdnsUp = MDNS.begin(hostname_.c_str());
    if (mdnsUp) {
        MDNS.addService("http", "tcp", 80);
    }
    util::logPrintf("WIFI mdns host=%s.local ok=%d\n", hostname_.c_str(), mdnsUp ? 1 : 0);
}

void startAp() {
    if (apUp) {
        return;
    }
    if (apPassword_.size() < kMinApPasswordLen) {
        util::logPrintf("ERR WIFI ap_pw missing, setup AP not started\n");
        return;
    }
    WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1),
                      IPAddress(255, 255, 255, 0));
    if (!WiFi.softAP(apSsid_.c_str(), apPassword_.c_str())) {
        util::logPrintf("ERR WIFI softAP failed\n");
        return;
    }
    dns.setErrorReplyCode(DNSReplyCode::NoError);
    dns.start(kDnsPort, "*", WiFi.softAPIP());
    apUp = true;
    util::logPrintf("WIFI ap ssid=%s ip=%s\n", apSsid_.c_str(), kApIp);
}

void stopAp() {
    if (!apUp) {
        return;
    }
    dns.stop();
    WiFi.softAPdisconnect(false);
    WiFi.mode(WIFI_STA);
    apUp = false;
    util::logPrintf("WIFI ap off\n");
}

void beginSta(uint32_t nowMs) {
    WiFi.disconnect(false, false);
    WiFi.begin(ssid_.c_str(), password_.empty() ? nullptr : password_.c_str());
    lastBeginMs = nowMs;
    util::logPrintf("WIFI sta connecting ssid=%s\n", ssid_.c_str());
}

// (Re)starts Wi-Fi with the settings in use. A setup AP that is up stays up while the
// new STA connection is tried, so a phone on the AP can follow the result.
void start(uint32_t nowMs) {
    const bool keepAp = apUp || ssid_.empty();
    staConnected = false;
    attemptStartMs = nowMs;
    WiFi.setHostname(hostname_.c_str());
    if (keepAp) {
        WiFi.mode(WIFI_AP_STA);  // STA stays idle without an SSID (needed for scans)
        startAp();
    } else {
        WiFi.mode(WIFI_STA);
    }
    if (!ssid_.empty()) {
        beginSta(nowMs);
    } else {
        WiFi.disconnect(false, false);
    }
    if (!mdnsUp) {
        startMdns();
    }
    started = true;
}

void refreshStatus() {
    Status s;
    if (!started) {
        s.mode = Mode::Off;
    } else if (ssid_.empty()) {
        s.mode = Mode::Ap;
    } else {
        s.mode = apUp ? Mode::ApSta : Mode::Sta;
    }
    s.ssid = ssid_;
    s.staConnected = staConnected;
    if (staConnected) {
        s.staIp = WiFi.localIP().toString().c_str();
        s.rssi = WiFi.RSSI();
    }
    s.apActive = apUp;
    s.apSsid = apSsid_;
    s.hostname = hostname_;
    status_ = s;
}

// Connection state machine for a configured SSID. Returns true if the state changed.
bool pollLink(uint32_t nowMs) {
    const bool associated = staAssociated();
    const bool connected = associated && staIpAddress() != 0;
    bool changed = false;
    if (connected && !staConnected) {
        staConnected = true;
        connectedAtMs = nowMs;
        changed = true;
        util::logPrintf("WIFI sta connected ip=%s rssi=%d\n", WiFi.localIP().toString().c_str(),
                        static_cast<int>(WiFi.RSSI()));
    } else if (!connected && staConnected) {
        staConnected = false;
        attemptStartMs = nowMs;
        changed = true;
        util::logPrintf("WIFI sta lost\n");
        if (!associated && !scanning) {
            beginSta(nowMs);
        }
    }

    if (staConnected) {
        if (apUp && nowMs - connectedAtMs >= kApLingerMs) {
            stopAp();
            changed = true;
        }
        return changed;
    }
    if (!apUp && nowMs - attemptStartMs >= kApFallbackMs) {
        util::logPrintf("WIFI fallback ap_sta\n");
        WiFi.mode(WIFI_AP_STA);
        startAp();
        changed = true;
    }
    // Never interrupt an association that is still waiting for DHCP, or a running scan
    // (both would be torn down by the disconnect in beginSta()).
    const uint32_t retryMs = apUp ? kRetryWithApMs : kRetryBeforeApMs;
    if (!associated && !scanning && nowMs - lastBeginMs >= retryMs) {
        beginSta(nowMs);
    }
    return changed;
}

void finishScan(bool failed) {
    scanning = false;
    scanRequested = false;
    scanDone = true;
    scanFailed = failed;
    scanResults.clear();
    const int16_t n = failed ? 0 : WiFi.scanComplete();
    for (int16_t i = 0; i < n; ++i) {
        ScanNetwork net;
        net.ssid = WiFi.SSID(i).c_str();
        if (net.ssid.empty()) {
            continue;  // hidden network
        }
        net.rssi = WiFi.RSSI(i);
        net.secure = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
        auto same = std::find_if(scanResults.begin(), scanResults.end(),
                                 [&net](const ScanNetwork &o) { return o.ssid == net.ssid; });
        if (same == scanResults.end()) {
            scanResults.push_back(net);
        } else if (net.rssi > same->rssi) {
            *same = net;
        }
    }
    WiFi.scanDelete();
    std::sort(scanResults.begin(), scanResults.end(),
              [](const ScanNetwork &a, const ScanNetwork &b) { return a.rssi > b.rssi; });
    if (scanResults.size() > kMaxScanResults) {
        scanResults.resize(kMaxScanResults);
    }
    util::logPrintf("WIFI scan done networks=%u failed=%d\n",
                    static_cast<unsigned>(scanResults.size()), failed ? 1 : 0);
}

// Stops the running scan. The driver still reports SCAN_DONE for it, and the Arduino event
// handler then allocates a result array and sets WIFI_SCAN_DONE_BIT. Arriving after the
// next scan has started, that would end the new scan early (with stale results) and leak
// one array (the handler does not free the previous one). So no scan starts until that
// event has arrived or kScanStopGraceMs have passed, and its results are freed then.
void stopScan(uint32_t nowMs) {
    esp_wifi_scan_stop();
    scanStopping = true;
    scanStopAtMs = nowMs;
}

void pollScanStop(uint32_t nowMs) {
    if (!scanStopping) {
        return;
    }
    const bool doneArrived = (WiFi.getStatusBits() & WIFI_SCAN_DONE_BIT) != 0;
    if (!doneArrived && nowMs - scanStopAtMs < kScanStopGraceMs) {
        return;
    }
    if (!doneArrived) {
        util::logPrintf("WIFI scan stop grace expired (no SCAN_DONE)\n");
    }
    scanStopping = false;
    WiFi.scanDelete();  // frees the late results and clears WIFI_SCAN_DONE_BIT
}

// Scans are started with esp_wifi_scan_start() directly rather than WiFi.scanNetworks():
// core 2.0.17 reports an async scan as failed once max_ms_per_chan * 20 ms have passed
// (6 s by default; a scan with a connected STA often takes longer) and then lets the next
// scanNetworks() start while the driver is still scanning, which fails too. The Arduino
// event handler still collects the results (WIFI_SCAN_DONE_BIT, WiFi.SSID(i) ...).
void pollScan(uint32_t nowMs) {
    pollScanStop(nowMs);
    if (scanRequested && !scanning && !scanStopping && nowMs - scanLastTryMs >= kScanStartRetryMs) {
        scanLastTryMs = nowMs;
        WiFi.scanDelete();  // also clears WIFI_SCAN_DONE_BIT
        wifi_scan_config_t config = {};
        config.show_hidden = false;
        config.scan_type = WIFI_SCAN_TYPE_ACTIVE;
        config.scan_time.active.min = 0;
        config.scan_time.active.max = kScanDwellMaxMs;
        const esp_err_t err = esp_wifi_scan_start(&config, false);
        if (err == ESP_OK) {
            scanning = true;
            scanStartedAtMs = nowMs;
            util::logPrintf("WIFI scan start\n");
        } else if (nowMs - scanRequestedAtMs >= kScanStartGiveUpMs) {
            util::logPrintf("WIFI scan start failed err=0x%x\n", static_cast<unsigned>(err));
            finishScan(true);
        }
        // else: the driver is busy (e.g. the STA is connecting); retried in 1 s
    }
    if (!scanning) {
        return;
    }
    if (WiFi.getStatusBits() & WIFI_SCAN_DONE_BIT) {
        finishScan(false);
    } else if (nowMs - scanStartedAtMs >= kScanTimeoutMs) {
        stopScan(nowMs);
        util::logPrintf("WIFI scan timeout\n");
        finishScan(true);
    }
}

}  // namespace

const char *modeName(Mode mode) {
    switch (mode) {
        case Mode::Ap: return "ap";
        case Mode::Sta: return "sta";
        case Mode::ApSta: return "ap_sta";
        case Mode::Off:
        default: return "off";
    }
}

bool Status::operator==(const Status &o) const {
    return mode == o.mode && ssid == o.ssid && staConnected == o.staConnected &&
           staIp == o.staIp && rssi == o.rssi && apActive == o.apActive && apSsid == o.apSsid &&
           hostname == o.hostname;
}

void begin(const std::string &ssid, const std::string &password, const std::string &hostname,
           const std::string &apPassword, uint32_t nowMs) {
    ssid_ = ssid;
    password_ = password;
    hostname_ = hostname;
    apPassword_ = apPassword;
    apSsid_ = makeApSsid();
    WiFi.persistent(false);         // never copy the credentials into the driver's NVS
    WiFi.setAutoReconnect(false);   // retries are scheduled in loop()
    start(nowMs);
    refreshStatus();
    lastStatusMs = nowMs;
}

bool applyConfig(const std::string &ssid, const std::string &password,
                 const std::string &hostname, uint32_t nowMs) {
    if (hostname != hostname_) {
        hostname_ = hostname;
        WiFi.setHostname(hostname_.c_str());  // DHCP uses it from the next connect
        startMdns();
    }
    if (ssid == ssid_ && password == password_) {
        refreshStatus();
        return false;
    }
    ssid_ = ssid;
    password_ = password;
    restartPending = true;
    restartAtMs = nowMs + kApplyDelayMs;
    return true;
}

void loop(uint32_t nowMs) {
    if (!started) {
        return;
    }
    bool changed = false;
    if (restartPending && static_cast<int32_t>(nowMs - restartAtMs) >= 0) {
        restartPending = false;
        if (scanning) {
            stopScan(nowMs);
            finishScan(true);
        }
        util::logPrintf("WIFI restart reason=config\n");
        start(nowMs);
        changed = true;
    }
    if (apUp) {
        dns.processNextRequest();
    }
    if (!mdnsUp && nowMs - mdnsTriedAtMs >= kMdnsRetryMs) {
        startMdns();
    }
    pollScan(nowMs);

    if (!ssid_.empty() && !restartPending && nowMs - lastLinkPollMs >= kLinkPollMs) {
        lastLinkPollMs = nowMs;
        changed = pollLink(nowMs) || changed;
    }

    if (changed || nowMs - lastStatusMs >= kStatusRefreshMs) {
        lastStatusMs = nowMs;
        refreshStatus();
    }
}

const Status &status() { return status_; }

void requestScan(uint32_t nowMs) {
    if (scanRequested || scanning) {
        return;  // the running request will deliver results
    }
    scanRequested = true;
    scanRequestedAtMs = nowMs;
    scanLastTryMs = nowMs - kScanStartRetryMs;  // try at once
}

bool takeScanResults(std::vector<ScanNetwork> &networks, bool &failed) {
    if (!scanDone) {
        return false;
    }
    scanDone = false;
    networks = scanResults;
    failed = scanFailed;
    return true;
}

}  // namespace wifi
}  // namespace net
