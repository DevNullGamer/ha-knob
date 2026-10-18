// HA Knob firmware entry point: setup()/loop() wiring only.
//
// Serial log lines:
//   BOOT fw=<v> psram=<bytes> flash=<bytes> touch=<CST816|CHSC5816|none> heap=<bytes>
//        cfg=<defaults|loaded>
//   ENC d=<+1|-1> count=<n>     (after device.encoder_reverse)
//   BTN down | up | short | long | verylong
//   TOUCH swipe=<left|right>
//   HB up=<s> psram=<bytes> flash=<bytes> touch=<..> count=<n> ctl=<controls> cfg=<..>
//      wifi=<off|ap|sta|ap_sta> ip=<ip> ha=<state> heap=<bytes> heap_min=<bytes>
//      loopmax=<ms> ent=<entities>/<ids> cat=<none|fetching|building|ready|failed>
//      stack=<bytes> psram_free=<bytes> disp=<on|dim|off>
//      (every 5 s; ip = STA IP, else 192.168.4.1 with the setup AP up, else 0.0.0.0;
//      ha = HA connection state; heap / heap_min = internal free heap now / lowest since
//      boot; loopmax = longest loop() pass since the previous HB, without its delay;
//      ent = live entities in the store / unique entity ids of the controls; cat = entity
//      list fetch; stack = loop task stack never used so far (high-water mark, bytes);
//      psram_free = free PSRAM now; disp = screen level, src/app/Idle.h)
//   CFG ...   config store events (see src/config/ConfigStore.h)
//   WIFI ...  Wi-Fi events (see src/net/WifiManager.h)
//   WEB ...   web server events (see src/net/WebServer.h)
//   HA ...    Home Assistant connection events (see src/ha/HaClient.h)
//   UI page=<i>/<n> | no_controls | status        page changed (swipe, long press without
//                                                 controls, Select list)
//   UI select open | choose=<i> | status | cancel reason=<long|idle|config|setup>
//                                                 the Select list (src/ui/Ui.h)
//   UI view page=<i>/<n> value=<value><unit> state=<text> arc=<v>[<min>,<max>]|- dimmed=<0|1>
//           unconfirmed=<0|1> title=<t> banner=<0|1>   (one line) the selected control page
//           shows something new (banner=1: HA not Ready, state = the offline reason)
//   UI toast text=<t>                             a control notice (src/ui/Ui.h)
//   UI render ms=<n> flush_ms=<f> px=<p>          a slow lv_timer_handler() pass (ms = whole
//           pass, flush_ms / px = panel transfer time / pixels inside it); test builds: every
//           pass >= 5 ms, normal builds: >= 50 ms, at most once per 5 s
//   CTL ...   control presses and notices (see src/app/Control.h)
//   IDLE ... | DISP ...   screen wake / dim / off (see src/app/Idle.h)
//   RST ...   factory-reset gesture (see src/app/Reset.h)
//   TIN <cmd> | TIN bad <cmd>   test builds only (-DHAKNOB_TEST_INPUT): injected input
//   BUZ ...   test builds only: buzzer requests (see src/hal/Buzzer.h)
//   SYS reboot | SYS factory reset src=<web|button>

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lvgl.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "app/Control.h"
#include "app/Idle.h"
#include "app/Reset.h"
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
#include "util/Text.h"
#include "version.h"

namespace {

constexpr uint32_t kHeartbeatMs = 5000;
constexpr uint32_t kLoopDelayMs = 5;
constexpr uint32_t kSystemRequestDelayMs = 750;  // let the HTTP response go out first
// "UI render" log (plan-step8.md §3.6): test builds log every slow pass, normal builds only
// long pauses and at most one line per kRenderLogGapMs.
#if defined(HAKNOB_TEST_INPUT)
constexpr uint32_t kRenderLogMinUs = 5000;
constexpr uint32_t kRenderLogGapMs = 0;
#else
constexpr uint32_t kRenderLogMinUs = 50000;
constexpr uint32_t kRenderLogGapMs = 5000;
#endif

ConfigStore configStore;
const char *bootCfgSource = "defaults";  // how this boot got its config: defaults | loaded
bool displayOk = false;
int32_t shownCount = 0;
uint32_t lastHeartbeatMs = 0;
uint32_t loopMaxUs = 0;  // longest loop() pass since the last heartbeat
uint32_t lastRenderLogMs = 0;
bool renderLogged = false;  // lastRenderLogMs is valid
std::string apPassword;  // shown on the setup screen
net::wifi::Status shownWifi;
ui::NetworkView shownView;
ha::Status shownHa;
bool shownHaNetworkUp = false;
bool haPublished = false;
uint32_t shownCatalogChange = 0;
bool catalogPublished = false;
bool catalogRepublish = false;  // a catalog request was handled: publish even without a change
// A refresh did not start (HA left Ready after the web check): the old list / error stays
// hidden from the web server (published as none) until the fetch state changes.
bool catalogHidden = false;
uint32_t catalogHiddenChange = 0;  // ha::catalogChangeCount() when it was hidden
size_t shownEntities = 0;
size_t shownSubscribed = 0;
bool entityCountsPublished = false;
// Control pages (plan-step6.md §3.7): the configured controls, and what the shown control
// page was last built from (rebuilt only when one of these changes).
std::vector<core::config::Control> controls;
uint32_t controlsGeneration = 0;  // +1 whenever `controls` is replaced
bool encoderReverse = false;      // device.encoder_reverse
bool buzzerEnabled = true;        // device.buzzer (the click on BTN down)
bool detentClick = false;         // device.detent_click (a click per acted-on detent batch)
struct ViewKey {
    int page = -1;
    uint32_t controlsGeneration = 0;
    uint32_t storeVersion = 0;
    core::ha::State haState = core::ha::State::NoConfig;
    bool waitOver = false;
    uint32_t controlVersion = 0;  // app::control::version(): a local value can have changed
    // The banner text; depends on the Wi-Fi link too (e.g. Backoff -> "No Wi-Fi").
    core::presenter::OfflineReason offline = core::presenter::OfflineReason::None;

    bool operator==(const ViewKey &o) const {
        return page == o.page && controlsGeneration == o.controlsGeneration &&
               storeVersion == o.storeVersion && haState == o.haState && waitOver == o.waitOver &&
               controlVersion == o.controlVersion && offline == o.offline;
    }
};
ViewKey shownViewKey;
bool viewBuilt = false;
core::presenter::ControlView shownControlView;
net::web::SystemRequest pendingSystem = net::web::SystemRequest::None;
uint32_t pendingSystemAtMs = 0;
const char *pendingSystemSource = "web";  // SYS factory reset src=<web|button>

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
                    "heap=%u heap_min=%u loopmax=%u ent=%u/%u cat=%s stack=%u psram_free=%u disp=%s\n",
                    static_cast<unsigned>(nowMs / 1000), static_cast<unsigned>(ESP.getPsramSize()),
                    static_cast<unsigned>(ESP.getFlashChipSize()), hal::touch::variantName(),
                    static_cast<int>(shownCount),
                    static_cast<unsigned>(configStore.controlCount()), bootCfgSource,
                    net::wifi::modeName(wifi.mode), ip, core::ha::stateName(ha::status().state),
                    static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMinFreeHeap()),
                    static_cast<unsigned>((loopMaxUs + 999) / 1000),
                    static_cast<unsigned>(ha::entities().size()), static_cast<unsigned>(ha::subscribedCount()),
                    core::ha::statesStateName(ha::catalogState()),
                    static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)),  // bytes on ESP-IDF
                    static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
                    core::ui::screenLevelName(app::idle::level()));
    loopMaxUs = 0;
}

// Runs LVGL (timers, render, flush) and logs a slow pass (plan-step8.md §3.6).
void renderUi(uint32_t nowMs) {
    const uint32_t startUs = micros();
    lv_timer_handler();
    const uint32_t renderUs = micros() - startUs;
    uint32_t flushUs = 0;
    uint32_t flushPx = 0;
    hal::display::takeFlushStats(&flushUs, &flushPx);  // always, so the next pass starts at 0
    if (renderUs < kRenderLogMinUs ||
        (renderLogged && nowMs - lastRenderLogMs < kRenderLogGapMs)) {
        return;
    }
    renderLogged = true;
    lastRenderLogMs = nowMs;
    util::logPrintf("UI render ms=%u flush_ms=%u px=%u\n",
                    static_cast<unsigned>((renderUs + 999) / 1000),
                    static_cast<unsigned>((flushUs + 999) / 1000), static_cast<unsigned>(flushPx));
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

// How an https Home Assistant URL is verified (ha.tls_insecure, ha.ca_pem).
core::ha::TlsSettings haTlsSettings(const core::config::Config &config) {
    core::ha::TlsSettings tls;
    tls.insecure = config.ha.tlsInsecure;
    tls.caPem = config.ha.caPem;
    return tls;
}

// The controls' entity ids, each once, in configuration order (the HA subscription).
std::vector<std::string> controlEntityIds(const core::config::Config &config) {
    std::vector<std::string> ids;
    for (const core::config::Control &control : config.controls) {
        if (std::find(ids.begin(), ids.end(), control.entityId) == ids.end()) {
            ids.push_back(control.entityId);
        }
    }
    return ids;
}

// Takes the configured controls for the pages and the control settings (after boot and
// after every config re-read).
void applyControls(const core::config::Config &config) {
    encoderReverse = config.device.encoderReverse;
    buzzerEnabled = config.device.buzzer;
    detentClick = config.device.detentClick;
    app::reset::setBeep(config.device.buzzer);
    app::control::applyConfig(config.device, config.controls);
    if (config.controls == controls && controlsGeneration != 0) {
        return;
    }
    controls = config.controls;
    ++controlsGeneration;
    if (displayOk) {
        ui::setControlCount(controls.size());
    }
}

// Logs the selected page after a swipe, a long press or the Select list changed it.
void logPage() {
    const int page = ui::controlPage();
    if (page >= 0) {
        util::logPrintf("UI page=%d/%u\n", page + 1, static_cast<unsigned>(controls.size()));
    } else {
        util::logPrintf("UI page=%s\n", ui::statusPage() ? "status" : "no_controls");
    }
}

// Builds the view of the selected control page when the store, the controls, the HA state,
// the page, the "waiting..." timer or a local (turned) value changed, and hands it to the
// screen.
void publishControlView(uint32_t nowMs) {
    if (!displayOk) {
        return;
    }
    const core::ha::EntityStore &store = ha::entities();
    const core::ha::State haState = ha::status().state;
    core::presenter::PresentContext context;
    context.haReady = haState == core::ha::State::Ready;
    context.offline = core::presenter::offlineReason(haState, net::wifi::status().staConnected);
    context.snapshotSeen = store.snapshotSeen();
    // No subscription (none sent, or it failed): a missing entity is "not found" at once.
    context.msSinceSubscribe = ha::msSinceSubscribe(nowMs).value_or(UINT32_MAX);

    ViewKey key;
    key.page = ui::controlPage();
    key.controlsGeneration = controlsGeneration;
    key.storeVersion = store.version();
    key.haState = haState;
    key.waitOver = core::presenter::waitOver(context);
    key.controlVersion = app::control::version();
    key.offline = context.offline;
    if (viewBuilt && key == shownViewKey) {
        return;
    }
    const bool pageChanged = !viewBuilt || key.page != shownViewKey.page;
    shownViewKey = key;
    viewBuilt = true;
    if (key.page < 0 || static_cast<size_t>(key.page) >= controls.size()) {
        return;
    }

    const core::config::Control &control = controls[static_cast<size_t>(key.page)];
    context.local = app::control::local(static_cast<size_t>(key.page), nowMs);
    const core::presenter::ControlView view =
        core::presenter::present(control, store.find(control.entityId), context);
    ui::showControl(view);
    if (pageChanged || view != shownControlView) {
        char arc[48] = "-";
        if (view.hasArc) {
            snprintf(arc, sizeof(arc), "%g[%g,%g]", view.arcValue, view.arcMin, view.arcMax);
        }
        using core::util::logSafe;
        util::logPrintf("UI view page=%d/%u value=%s%s state=%s arc=%s dimmed=%d unconfirmed=%d title=%s "
                        "banner=%d\n",
                        key.page + 1, static_cast<unsigned>(controls.size()),
                        logSafe(view.value, 32).c_str(), view.unit.c_str(),
                        logSafe(view.stateText, 40).c_str(), arc, view.dimmed ? 1 : 0,
                        view.unconfirmed ? 1 : 0, logSafe(view.title, 48).c_str(), view.banner ? 1 : 0);
    }
    shownControlView = view;
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
        view.tls = status.tls;
        view.networkUp = networkUp;
        ui::showHa(view);
    }
}

// Publishes the entity list fetch and the entity counts to the web server snapshot.
void publishCatalog() {
    const uint32_t change = ha::catalogChangeCount();
    if (catalogHidden && change != catalogHiddenChange) {
        catalogHidden = false;  // a new fetch, failure or another HA instance
    }
    if (!catalogPublished || catalogRepublish || change != shownCatalogChange) {
        if (catalogHidden) {
            net::web::publishCatalog(core::ha::StatesState::None, nullptr, 0, std::string());
        } else {
            net::web::publishCatalog(ha::catalogState(), ha::catalogJson(), ha::catalogCount(),
                                     ha::catalogError());
        }
        catalogPublished = true;
        catalogRepublish = false;
        shownCatalogChange = change;
    }
    const size_t entities = ha::entities().size();
    const size_t subscribed = ha::subscribedCount();
    if (!entityCountsPublished || entities != shownEntities || subscribed != shownSubscribed) {
        net::web::publishEntityCounts(entities, subscribed);
        entityCountsPublished = true;
        shownEntities = entities;
        shownSubscribed = subscribed;
    }
}

// Work requested by the web server (whose callbacks run in the async_tcp task).
void handleWebRequests(uint32_t nowMs) {
    if (net::web::takeConfigDirty()) {
        const core::config::Config config = configStore.config();
        // The user is looking at the device (Q8-9): wake it; the new brightness / timeouts
        // apply in app::idle::loop() of this pass.
        app::idle::keepAwake(app::idle::Source::Config, nowMs);
        app::idle::applyConfig(config.device, nowMs);
        if (displayOk) {
            ui::selectCancel("config");  // its titles and pages may be stale (Q7-9)
        }
        net::wifi::applyConfig(config.wifi.ssid, configStore.wifiPassword(), config.device.hostname,
                               nowMs);
        ha::configure(config.ha.url, configStore.haToken(), haTlsSettings(config), controlEntityIds(config),
                      nowMs);
        applyControls(config);
    }

    if (net::web::takeHaRetry()) {
        ha::retry(nowMs);
    }

    // Entity list: never restart a running fetch (also not while its JSON is written); a
    // GET fetches only when there is no list to serve (none, hidden, or Ready without a
    // body), a refresh also replaces a list / error. A refresh that does not start hides
    // the old list / error, so it never looks like a refreshed one (the GET then gets 503).
    const net::web::CatalogRequest catalogRequest = net::web::takeCatalogRequest();
    if (catalogRequest != net::web::CatalogRequest::None) {
        using core::ha::StatesState;
        if (catalogHidden && ha::catalogChangeCount() != catalogHiddenChange) {
            catalogHidden = false;  // as in publishCatalog(): the fetch state changed meanwhile
        }
        const StatesState state = ha::catalogState();
        const bool running = state == StatesState::Fetching || state == StatesState::Building;
        const bool nothingServed = catalogHidden || state == StatesState::None ||
                                   (state == StatesState::Ready && !ha::catalogJson());
        if (!running && (catalogRequest == net::web::CatalogRequest::Refresh || nothingServed)) {
            const uint32_t changeBefore = ha::catalogChangeCount();
            const bool started = ha::requestStates(nowMs);
            // Hide only when nothing changed: a send failure is a new state (failed:
            // connection_lost) that must reach the web server as 502.
            if (!started && catalogRequest == net::web::CatalogRequest::Refresh &&
                ha::catalogChangeCount() == changeBefore && ha::catalogState() != StatesState::Fetching) {
                catalogHidden = true;
                catalogHiddenChange = ha::catalogChangeCount();
            }
        }
        catalogRepublish = true;  // the snapshot may show a refresh that did not start
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
        pendingSystemSource = "web";
    }
    if (pendingSystem != net::web::SystemRequest::None &&
        static_cast<int32_t>(nowMs - pendingSystemAtMs) >= 0) {
        if (pendingSystem == net::web::SystemRequest::FactoryReset) {
            util::logPrintf("SYS factory reset src=%s\n", pendingSystemSource);
            configStore.factoryReset();
        } else {
            util::logPrintf("SYS reboot\n");
        }
        Serial.flush();
        delay(50);
        ESP.restart();
    }
}

#if defined(HAKNOB_TEST_INPUT)
int32_t testDelta = 0;  // detents injected by "TIN rot" since the last handleEncoder()
#endif

// The detents of this pass (one batch, plan-step7.md Q7-2): counted for the Status page
// (also when they only wake the screen); then, unless they woke it (plan-step8.md §3.5),
// they cancel the reset confirmation while it is shown, else move the Select list while it
// is open (1 item per detent), else turn the selected control page; only the last two click
// (device.buzzer and device.detent_click).
void handleEncoder(uint32_t nowMs) {
    int32_t delta = hal::encoder::takeDelta();
#if defined(HAKNOB_TEST_INPUT)
    delta += testDelta;
    testDelta = 0;
#endif
    if (delta == 0) {
        return;
    }
    if (encoderReverse) {
        delta = -delta;  // here, never in the ISR
    }
    const int d = delta > 0 ? 1 : -1;
    for (int32_t i = 0; i != delta; i += d) {
        shownCount += d;
        util::logPrintf("ENC d=%+d count=%d\n", d, static_cast<int>(shownCount));
    }
    if (displayOk) {
        ui::showEncoderCount(shownCount);
    }
    if (!app::idle::input(app::idle::Source::Encoder, nowMs)) {
        return;
    }
    if (app::reset::captures()) {
        app::reset::rotate(static_cast<int>(delta), nowMs);  // cancels; no click
        return;
    }
    if (!displayOk) {
        return;
    }
    const int page = ui::controlPage();
    bool actedOn = true;  // Q8-11: click only for a Select move or a control rotation
    if (ui::selectIsOpen()) {
        ui::selectMove(static_cast<int>(delta), nowMs);
    } else if (page >= 0 && ui::pagesActive()) {
        app::control::rotate(static_cast<size_t>(page), static_cast<int>(delta), nowMs);
    } else {
        actedOn = false;  // Status, NoControls, boot or setup screen
    }
    if (actedOn && buzzerEnabled && detentClick) {
        hal::buzzer::click(nowMs);  // at most one per pass (and per 40 ms)
    }
}

void handleButtonEvent(core::ButtonFsm::Event event, uint32_t nowMs) {
    using Event = core::ButtonFsm::Event;
    if (event == Event::None) {
        return;
    }
    if (event == Event::ShortPress) {
        util::logPrintf("BTN up\n");  // a short press is also a release
    }
    util::logPrintf("BTN %s\n", core::buttonEventName(event));
    // A gesture that woke the screen is swallowed up to its release (Q8-3): no click, no action.
    if (!app::idle::button(event, nowMs)) {
        return;
    }
    if (event == Event::Pressed && buzzerEnabled) {
        hal::buzzer::beep(nowMs);
    }
    // The factory-reset gesture (plan-step8.md §3.5): the confirmation takes every button
    // event while it is shown; a very long press that opens it goes nowhere else.
    if (app::reset::button(event, nowMs)) {
        return;
    }

    if (!displayOk) {
        return;
    }
    // plan-step7.md Q7-9: a long press opens / cancels the Select list (without controls it
    // toggles NoControls <-> Status); a short press chooses in the Select list, else runs the
    // control page's press action (§3.6).
    switch (event) {
        case Event::ShortPress: {
            ui::showLastInput("short press");
            const int page = ui::controlPage();
            if (ui::selectIsOpen()) {
                if (ui::selectChoose()) {
                    logPage();
                }
            } else if (page >= 0 && ui::pagesActive()) {
                app::control::press(static_cast<size_t>(page), nowMs);
            }
            break;
        }
        case Event::LongPress:
            ui::showLastInput("long press");
            if (ui::selectIsOpen()) {
                ui::selectCancel("long");
            } else if (controls.empty()) {
                if (ui::nextPage()) {
                    logPage();
                }
            } else if (ui::pagesActive()) {
                std::vector<std::string> titles;
                titles.reserve(controls.size());
                for (const core::config::Control &control : controls) {
                    titles.push_back(core::presenter::titleFor(control, ha::entities().find(control.entityId)));
                }
                ui::selectOpen(titles, nowMs);
            }
            break;
        case Event::VeryLongPress: ui::showLastInput("very long press"); break;
        default: break;
    }
}

void handleButton(uint32_t nowMs) { handleButtonEvent(hal::button::poll(nowMs), nowMs); }

void handleSwipe(core::Swipe swipe, uint32_t nowMs) {
    if (swipe == core::Swipe::None) {
        return;
    }
    util::logPrintf("TOUCH swipe=%s\n", core::swipeName(swipe));
    if (!app::idle::input(app::idle::Source::Swipe, nowMs)) {
        return;
    }
    if (app::reset::captures()) {
        app::reset::swipe(nowMs);  // cancels; the page stays
        return;
    }
    if (!displayOk) {
        return;
    }
    // Swipe left = next page, swipe right = previous page (wrapping); ignored while the
    // Select list is open (ui:: refuses to navigate). Taps never act.
    ui::showLastInput(swipe == core::Swipe::Left ? "swipe left" : "swipe right");
    if (swipe == core::Swipe::Left ? ui::nextPage() : ui::previousPage()) {
        logPage();
    }
}

void handleTouch(uint32_t nowMs) { handleSwipe(hal::touch::poll(nowMs), nowMs); }

#if defined(HAKNOB_TEST_INPUT)
// Test builds only (plan-step7.md Q7-10, plan-step8.md §3.5): "TIN rot <+-n>", "TIN short",
// "TIN long", "TIN down" (Pressed), "TIN up" (Released), "TIN verylong", "TIN swipe
// left|right" lines on the serial port are injected through the same paths as the HAL
// events (rotations join this pass's encoder batch, so encoder_reverse and the
// acceleration apply). Non-blocking; lines longer than kTestLineMax characters are dropped
// whole, lines without the "TIN " prefix are ignored.
constexpr size_t kTestLineMax = 40;
constexpr long kTestRotMax = 100;
std::string testLine;
bool testLineTooLong = false;

void runTestLine(const std::string &line, uint32_t nowMs) {
    using Event = core::ButtonFsm::Event;
    if (line.compare(0, 4, "TIN ") != 0) {
        return;
    }
    const std::string cmd = line.substr(4);
    bool ok = true;
    if (cmd.compare(0, 4, "rot ") == 0) {
        // A sign or a digit right after "rot " (strtol would skip whitespace).
        const char first = cmd.size() > 4 ? cmd[4] : '\0';
        char *end = nullptr;
        const long n = strtol(cmd.c_str() + 4, &end, 10);
        ok = (first == '+' || first == '-' || (first >= '0' && first <= '9')) &&
             end != cmd.c_str() + 4 && *end == '\0' && n != 0 && n >= -kTestRotMax && n <= kTestRotMax;
        if (ok) {
            util::logPrintf("TIN %s\n", core::util::logSafe(cmd, kTestLineMax).c_str());
            testDelta += static_cast<int32_t>(n);
        }
    } else if (cmd == "short" || cmd == "long" || cmd == "down" || cmd == "up" || cmd == "verylong") {
        util::logPrintf("TIN %s\n", cmd.c_str());
        const Event event = cmd == "short"  ? Event::ShortPress
                            : cmd == "long" ? Event::LongPress
                            : cmd == "down" ? Event::Pressed
                            : cmd == "up"   ? Event::Released
                                            : Event::VeryLongPress;
        handleButtonEvent(event, nowMs);
    } else if (cmd == "swipe left" || cmd == "swipe right") {
        util::logPrintf("TIN %s\n", cmd.c_str());
        handleSwipe(cmd == "swipe left" ? core::Swipe::Left : core::Swipe::Right, nowMs);
    } else {
        ok = false;
    }
    if (!ok) {
        util::logPrintf("TIN bad %s\n", core::util::logSafe(cmd, kTestLineMax).c_str());
    }
}

void handleTestInput(uint32_t nowMs) {
    while (Serial.available() > 0) {
        const int c = Serial.read();
        if (c < 0) {
            break;
        }
        if (c == '\n' || c == '\r') {
            if (!testLineTooLong && !testLine.empty()) {
                runTestLine(testLine, nowMs);
            }
            testLine.clear();
            testLineTooLong = false;
        } else if (testLine.size() >= kTestLineMax) {
            testLineTooLong = true;
        } else {
            testLine.push_back(static_cast<char>(c));
        }
    }
}
#endif

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
    app::idle::applyConfig(config.device, millis());
    if (displayOk) {
        app::idle::loop(millis());  // the configured brightness (logs DISP level=on)
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
    ha::configure(config.ha.url, configStore.haToken(), haTlsSettings(config), controlEntityIds(config),
                  millis());
    applyControls(config);
    publishHa();
    publishCatalog();
    net::web::begin(configStore, hal::touch::variantName());
    printBootLine();
}

void loop() {
    const uint32_t startUs = micros();
    const uint32_t nowMs = millis();

#if defined(HAKNOB_TEST_INPUT)
    handleTestInput(nowMs);
#endif
    handleEncoder(nowMs);
    handleButton(nowMs);
    handleTouch(nowMs);
    // After the inputs: arming, the reset timeout and its countdown. A confirmed reset shows
    // "Resetting..." and goes through the delayed factory-reset path in handleWebRequests().
    app::reset::loop(nowMs);
    if (app::reset::takeConfirmed()) {
        if (displayOk) {
            ui::showResetting();
        }
        // A pending web reboot becomes the reset; a pending web reset keeps its time.
        if (pendingSystem != net::web::SystemRequest::FactoryReset) {
            pendingSystem = net::web::SystemRequest::FactoryReset;
            pendingSystemAtMs = nowMs + kSystemRequestDelayMs;  // also lets "Resetting..." render
            pendingSystemSource = "button";
        }
    }
    hal::buzzer::loop(nowMs);
    net::wifi::loop(nowMs);
    handleWebRequests(nowMs);
    publishNetwork();
    ha::setNetworkUp(net::wifi::status().staConnected, nowMs);
    ha::loop();
    // After ha::loop() (its call results), before the view. A toast wakes the screen (Q8-9).
    if (app::control::loop(nowMs)) {
        app::idle::keepAwake(app::idle::Source::Toast, nowMs);
    }
    if (app::reset::captures()) {
        app::idle::keepAwake(app::idle::Source::Reset, nowMs);  // logs only on a wake
    }
    // After every input, the config re-read and the toast, so a wake applies in this pass.
    if (displayOk) {
        app::idle::loop(nowMs);
    }
    publishHa();
    publishCatalog();
    publishControlView(nowMs);

    if (nowMs - lastHeartbeatMs >= kHeartbeatMs) {
        lastHeartbeatMs = nowMs;
        printHeartbeat(nowMs);
    }

    if (displayOk) {
        ui::tick(nowMs);
        renderUi(nowMs);
    }
    const uint32_t passUs = micros() - startUs;
    if (passUs > loopMaxUs) {
        loopMaxUs = passUs;
    }
    delay(kLoopDelayMs);
}
