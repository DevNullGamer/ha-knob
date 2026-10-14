#include "net/WifiManager.h"

#include <Arduino.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <WiFi.h>
#include <esp_mac.h>

#include <algorithm>

namespace net {
namespace wifi {

namespace {

constexpr uint32_t kApFallbackMs = 30000;      // STA not connected -> AP_STA
constexpr uint32_t kRetryBeforeApMs = 15000;   // one extra STA attempt before the fallback
constexpr uint32_t kRetryWithApMs = 60000;     // STA retry interval while the AP is up
constexpr uint32_t kApLingerMs = 60000;        // AP kept up after STA connected
constexpr uint32_t kApplyDelayMs = 1000;       // let the HTTP response go out first
constexpr uint32_t kStatusRefreshMs = 1000;
constexpr uint8_t kDnsPort = 53;
constexpr size_t kMinApPasswordLen = 8;
constexpr size_t kMaxScanResults = 30;

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
uint32_t attemptStartMs = 0;  // start of the current connect series (begin, restart, loss)
uint32_t lastBeginMs = 0;
uint32_t connectedAtMs = 0;
bool restartPending = false;
uint32_t restartAtMs = 0;
uint32_t lastStatusMs = 0;

bool scanning = false;
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

void startMdns() {
    if (mdnsUp) {
        MDNS.end();
        mdnsUp = false;
    }
    mdnsUp = MDNS.begin(hostname_.c_str());
    if (mdnsUp) {
        MDNS.addService("http", "tcp", 80);
    }
    Serial.printf("WIFI mdns host=%s.local ok=%d\n", hostname_.c_str(), mdnsUp ? 1 : 0);
}

void startAp() {
    if (apUp) {
        return;
    }
    if (apPassword_.size() < kMinApPasswordLen) {
        Serial.println("ERR WIFI ap_pw missing, setup AP not started");
        return;
    }
    WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1),
                      IPAddress(255, 255, 255, 0));
    if (!WiFi.softAP(apSsid_.c_str(), apPassword_.c_str())) {
        Serial.println("ERR WIFI softAP failed");
        return;
    }
    dns.setErrorReplyCode(DNSReplyCode::NoError);
    dns.start(kDnsPort, "*", WiFi.softAPIP());
    apUp = true;
    Serial.printf("WIFI ap ssid=%s ip=%s\n", apSsid_.c_str(), kApIp);
}

void stopAp() {
    if (!apUp) {
        return;
    }
    dns.stop();
    WiFi.softAPdisconnect(false);
    WiFi.mode(WIFI_STA);
    apUp = false;
    Serial.println("WIFI ap off");
}

void beginSta(uint32_t nowMs) {
    WiFi.disconnect(false, false);
    WiFi.begin(ssid_.c_str(), password_.empty() ? nullptr : password_.c_str());
    lastBeginMs = nowMs;
    Serial.printf("WIFI sta connecting ssid=%s\n", ssid_.c_str());
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

void pollScan() {
    if (!scanning) {
        return;
    }
    const int16_t n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING) {
        return;
    }
    scanning = false;
    scanDone = true;
    scanResults.clear();
    scanFailed = n < 0;
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
        Serial.println("WIFI restart reason=config");
        start(nowMs);
        changed = true;
    }
    if (apUp) {
        dns.processNextRequest();
    }
    pollScan();

    if (!ssid_.empty() && !restartPending) {
        const bool connected = WiFi.status() == WL_CONNECTED;
        if (connected && !staConnected) {
            staConnected = true;
            connectedAtMs = nowMs;
            changed = true;
            Serial.printf("WIFI sta connected ip=%s rssi=%d\n", WiFi.localIP().toString().c_str(),
                          static_cast<int>(WiFi.RSSI()));
        } else if (!connected && staConnected) {
            staConnected = false;
            attemptStartMs = nowMs;
            changed = true;
            Serial.println("WIFI sta lost");
            beginSta(nowMs);
        }

        if (staConnected) {
            if (apUp && nowMs - connectedAtMs >= kApLingerMs) {
                stopAp();
                changed = true;
            }
        } else {
            if (!apUp && nowMs - attemptStartMs >= kApFallbackMs) {
                Serial.println("WIFI fallback ap_sta");
                WiFi.mode(WIFI_AP_STA);
                startAp();
                beginSta(nowMs);
                changed = true;
            } else if (nowMs - lastBeginMs >= (apUp ? kRetryWithApMs : kRetryBeforeApMs)) {
                beginSta(nowMs);
            }
        }
    }

    if (changed || nowMs - lastStatusMs >= kStatusRefreshMs) {
        lastStatusMs = nowMs;
        refreshStatus();
    }
}

const Status &status() { return status_; }

bool startScan() {
    if (scanning) {
        return true;
    }
    const int16_t r = WiFi.scanNetworks(true /* async */, false /* hidden */);
    if (r == WIFI_SCAN_FAILED) {
        return false;
    }
    scanning = true;
    scanDone = false;
    return true;
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
