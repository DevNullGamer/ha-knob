// HA Knob firmware entry point: setup()/loop() wiring only.
//
// Serial log lines:
//   BOOT fw=<v> psram=<bytes> flash=<bytes> touch=<CST816|CHSC5816|none> heap=<bytes>
//        cfg=<defaults|loaded>
//   ENC d=<+1|-1> count=<n>
//   BTN down | up | short | long | verylong
//   TOUCH swipe=<left|right>
//   HB up=<s> psram=<bytes> flash=<bytes> touch=<..> count=<n> ctl=<controls> cfg=<..>
//      wifi=<off|ap|sta|ap_sta> ip=<ip> ha=<state> heap=<bytes> heap_min=<bytes>
//      loopmax=<ms>   (every 5 s; ip = STA IP, else 192.168.4.1 with the setup AP up, else
//      0.0.0.0; ha = HA connection state; heap / heap_min = internal free heap now / lowest
//      since boot; loopmax = longest loop() pass since the previous HB, without its delay)
//   CFG ...   config store events (see src/config/ConfigStore.h)
//   WIFI ...  Wi-Fi events (see src/net/WifiManager.h)
//   WEB ...   web server events (see src/net/WebServer.h)
//   HA ...    Home Assistant connection events (see src/ha/HaClient.h)
//   SYS reboot | SYS factory reset

#include <Arduino.h>
#include <lvgl.h>

#include <string>
#include <vector>

#include "config/ConfigStore.h"
#include "ha/HaClient.h"
#include "hal/Button.h"
#include "hal/Buzzer.h"
#include "hal/Display.h"
#include "hal/Encoder.h"
#include "hal/Touch.h"
#include "net/WebServer.h"
#include "net/WifiManager.h"
#include "ui/Ui.h"
#include "util/Log.h"
#include "version.h"

namespace {

constexpr uint32_t kHeartbeatMs = 5000;
constexpr uint32_t kLoopDelayMs = 5;
constexpr uint32_t kSystemRequestDelayMs = 750;  // let the HTTP response go out first

ConfigStore configStore;
const char *bootCfgSource = "defaults";  // how this boot got its config: defaults | loaded
bool displayOk = false;
int32_t shownCount = 0;
uint32_t lastHeartbeatMs = 0;
uint32_t loopMaxUs = 0;  // longest loop() pass since the last heartbeat
std::string apPassword;  // shown on the setup screen
net::wifi::Status shownWifi;
ui::NetworkView shownView;
ha::Status shownHa;
bool shownHaNetworkUp = false;
bool haPublished = false;
net::web::SystemRequest pendingSystem = net::web::SystemRequest::None;
uint32_t pendingSystemAtMs = 0;

void printBootLine() {
    util::logPrintf("BOOT fw=%s psram=%u flash=%u touch=%s heap=%u cfg=%s\n", FW_VERSION,
                    static_cast<unsigned>(ESP.getPsramSize()),
                    static_cast<unsigned>(ESP.getFlashChipSize()), hal::touch::variantName(),
                    static_cast<unsigned>(ESP.getFreeHeap()),
                    bootCfgSource);
}

void printHeartbeat(uint32_t nowMs) {
    const net::wifi::Status &wifi = net::wifi::status();
    const char *ip = wifi.staConnected ? wifi.staIp.c_str()
                     : wifi.apActive   ? net::wifi::kApIp
                                       : "0.0.0.0";
    util::logPrintf("HB up=%u psram=%u flash=%u touch=%s count=%d ctl=%u cfg=%s wifi=%s ip=%s ha=%s "
                    "heap=%u heap_min=%u loopmax=%u\n",
                    static_cast<unsigned>(nowMs / 1000), static_cast<unsigned>(ESP.getPsramSize()),
                    static_cast<unsigned>(ESP.getFlashChipSize()), hal::touch::variantName(),
                    static_cast<int>(shownCount),
                    static_cast<unsigned>(configStore.controlCount()), bootCfgSource,
                    net::wifi::modeName(wifi.mode), ip, core::ha::stateName(ha::status().state),
                    static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMinFreeHeap()),
                    static_cast<unsigned>((loopMaxUs + 999) / 1000));
    loopMaxUs = 0;
}

// Publishes Wi-Fi state changes to the web server snapshot and the screens.
void publishNetwork() {
    const net::wifi::Status &wifi = net::wifi::status();
    if (wifi == shownWifi) {
        return;
    }
    shownWifi = wifi;
    net::web::publishNetwork(wifi);

    ui::NetworkView view;
    view.ssid = wifi.ssid;
    view.staConnected = wifi.staConnected;
    view.staIp = wifi.staIp;
    view.apActive = wifi.apActive;
    view.apSsid = wifi.apSsid;
    view.apPassword = apPassword;
    view.hostname = wifi.hostname;
    if (displayOk && view != shownView) {
        ui::showNetwork(view);
    }
    shownView = view;
}

// Publishes Home Assistant connection changes to the web server snapshot and the screens.
void publishHa() {
    const ha::Status &status = ha::status();
    const bool networkUp = net::wifi::status().staConnected;
    if (haPublished && status == shownHa && networkUp == shownHaNetworkUp) {
        return;
    }
    if (!haPublished || status != shownHa) {
        net::web::publishHa(status);
    }
    haPublished = true;
    shownHa = status;
    shownHaNetworkUp = networkUp;
    if (displayOk) {
        ui::HaView view;
        view.state = status.state;
        view.haVersion = status.haVersion;
        view.lastError = status.lastError;
        view.networkUp = networkUp;
        ui::showHa(view);
    }
}

// Work requested by the web server (whose callbacks run in the async_tcp task).
void handleWebRequests(uint32_t nowMs) {
    if (net::web::takeConfigDirty()) {
        const core::config::Config config = configStore.config();
        if (displayOk) {
            hal::display::setBrightnessPercent(config.device.brightness);
        }
        net::wifi::applyConfig(config.wifi.ssid, configStore.wifiPassword(), config.device.hostname,
                               nowMs);
        ha::configure(config.ha.url, configStore.haToken(), nowMs);
    }

    if (net::web::takeHaRetry()) {
        ha::retry(nowMs);
    }

    if (net::web::takeScanRequest()) {
        net::wifi::requestScan(nowMs);
    }
    std::vector<net::wifi::ScanNetwork> networks;
    bool scanFailed = false;
    if (net::wifi::takeScanResults(networks, scanFailed)) {
        net::web::publishScan(networks, scanFailed, nowMs);
    }

    const net::web::SystemRequest request = net::web::takeSystemRequest();
    if (request != net::web::SystemRequest::None && pendingSystem == net::web::SystemRequest::None) {
        pendingSystem = request;
        pendingSystemAtMs = nowMs + kSystemRequestDelayMs;
    }
    if (pendingSystem != net::web::SystemRequest::None &&
        static_cast<int32_t>(nowMs - pendingSystemAtMs) >= 0) {
        if (pendingSystem == net::web::SystemRequest::FactoryReset) {
            util::logPrintf("SYS factory reset\n");
            configStore.factoryReset();
        } else {
            util::logPrintf("SYS reboot\n");
        }
        Serial.flush();
        delay(50);
        ESP.restart();
    }
}

void handleEncoder() {
    const int32_t delta = hal::encoder::takeDelta();
    if (delta == 0) {
        return;
    }
    const int d = delta > 0 ? 1 : -1;
    for (int32_t i = 0; i != delta; i += d) {
        shownCount += d;
        util::logPrintf("ENC d=%+d count=%d\n", d, static_cast<int>(shownCount));
    }
    if (displayOk) {
        ui::showEncoderCount(shownCount);
    }
}

void handleButton(uint32_t nowMs) {
    using Event = core::ButtonFsm::Event;
    const Event event = hal::button::poll(nowMs);
    if (event == Event::None) {
        return;
    }
    if (event == Event::Pressed) {
        hal::buzzer::beep(nowMs);
    }
    if (event == Event::ShortPress) {
        util::logPrintf("BTN up\n");  // a short press is also a release
    }
    util::logPrintf("BTN %s\n", core::buttonEventName(event));

    if (!displayOk) {
        return;
    }
    switch (event) {
        case Event::ShortPress: ui::showLastInput("short press"); break;
        case Event::LongPress: ui::showLastInput("long press"); break;
        case Event::VeryLongPress: ui::showLastInput("very long press"); break;
        default: break;
    }
}

void handleTouch(uint32_t nowMs) {
    const core::Swipe swipe = hal::touch::poll(nowMs);
    if (swipe == core::Swipe::None) {
        return;
    }
    util::logPrintf("TOUCH swipe=%s\n", core::swipeName(swipe));
    if (displayOk) {
        ui::showLastInput(swipe == core::Swipe::Left ? "swipe left" : "swipe right");
    }
}

}  // namespace

void setup() {
    // All output goes through util::logPrintf (src/util/Log.h), which never lets
    // HWCDC::write() block. The larger TX buffer keeps bursts (boot, CFG saves) intact.
    Serial.setTxBufferSize(4096);  // before begin(), which would create a 256-byte buffer
    Serial.begin(115200);
    // 1 ms, not 0: core 2.0.17 counts its "no progress" retries down from this value and
    // only gives up when the count reaches 0, so 0 wraps around (a ~49-day busy loop in
    // write()) and makes flush() discard the buffer instead of draining it.
    Serial.setTxTimeoutMs(1);

    configStore.begin();
    bootCfgSource = configStore.loadedFromNvs() ? "loaded" : "defaults";
    const core::config::Config config = configStore.config();

    displayOk = hal::display::begin();
    if (displayOk) {
        hal::display::setBrightnessPercent(config.device.brightness);
    }
    hal::touch::begin();
    hal::encoder::begin();
    hal::button::begin();
    hal::buzzer::begin();
    if (displayOk) {
        ui::begin(millis());
    } else {
        util::logPrintf("ERR display init failed\n");
    }

    apPassword = configStore.apPassword();
    net::wifi::begin(config.wifi.ssid, configStore.wifiPassword(), config.device.hostname, apPassword,
                     millis());
    publishNetwork();  // the web server guards need the network state from the start
    ha::begin();
    ha::configure(config.ha.url, configStore.haToken(), millis());
    publishHa();
    net::web::begin(configStore, hal::touch::variantName());
    printBootLine();
}

void loop() {
    const uint32_t startUs = micros();
    const uint32_t nowMs = millis();

    handleEncoder();
    handleButton(nowMs);
    handleTouch(nowMs);
    hal::buzzer::loop(nowMs);
    net::wifi::loop(nowMs);
    handleWebRequests(nowMs);
    publishNetwork();
    ha::setNetworkUp(net::wifi::status().staConnected, nowMs);
    ha::loop();
    publishHa();

    if (nowMs - lastHeartbeatMs >= kHeartbeatMs) {
        lastHeartbeatMs = nowMs;
        printHeartbeat(nowMs);
    }

    if (displayOk) {
        ui::tick(nowMs);
        lv_timer_handler();
    }
    const uint32_t passUs = micros() - startUs;
    if (passUs > loopMaxUs) {
        loopMaxUs = passUs;
    }
    delay(kLoopDelayMs);
}
