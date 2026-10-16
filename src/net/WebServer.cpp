#include "net/WebServer.h"

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <esp_heap_caps.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "config/ConfigStore.h"
#include "control/DomainActions.h"
#include "ha/HaSession.h"
#include "hal/PsramAllocator.h"
#include "version.h"
#include "web/RequestGuard.h"
#include "util/Log.h"

namespace net {
namespace web {

namespace {

namespace guard = core::web;

constexpr uint16_t kPort = 80;
constexpr uint32_t kScanFreshMs = 10000;  // GET /api/wifi/scan reuses results this young
constexpr const char *kRealm = "HA Knob";
constexpr const char *kJson = "application/json";
// GET /api/ha/entities 200 bodies streamed at the same time. Each stream holds a send
// buffer (2 x MSS) and up to a TCP send window of pbufs in internal RAM (~9 KB); more
// parallel requests get 503 {"error":"busy"} instead of exhausting the internal heap.
constexpr int kMaxCatalogStreams = 4;

AsyncWebServer server(kPort);
ConfigStore *store = nullptr;
const char *touchName = "none";
bool fsOk = false;
// Files in the LittleFS root, listed once in begin() (read-only afterwards). The static
// handler only takes requests for these, so misses never reach LittleFS (whose failed
// open() logs "[E][vfs_api.cpp] ... does not exist" several times per request).
std::vector<std::string> staticFiles;

// State shared between the main loop and the async_tcp task; guarded by snapshotMutex.
std::mutex snapshotMutex;
wifi::Status netStatus;
std::string webPassword;  // cached web_pw ("" = no password); refreshed after each patch
std::vector<wifi::ScanNetwork> scanNetworks;
bool scanHaveResults = false;
bool scanFailed = false;
bool scanPending = false;  // requested or running
uint32_t scanAtMs = 0;
ha::Status haStatus;
// Entity list (plan-step6.md §3.5): the main loop's catalog state and its prebuilt GET
// /api/ha/entities body (shared, never copied per request), plus the pending request.
// A refresh clears the published catalog at once; publishCatalog() leaves it cleared until
// the main loop has taken the refresh, so no GET in between returns the old list.
core::ha::StatesState catalogState = core::ha::StatesState::None;
std::shared_ptr<const std::string> catalogBody;
size_t catalogCount = 0;
std::string catalogError;
size_t entityCount = 0;
size_t subscribedCount = 0;
CatalogRequest catalogRequest = CatalogRequest::None;
std::string metaBody;  // GET /api/meta/actions, built once in begin() (read-only afterwards)

std::atomic<int> catalogStreams{0};  // running 200 streams of GET /api/ha/entities

std::atomic<bool> configDirty{false};
std::atomic<bool> scanRequested{false};
std::atomic<bool> haRetryRequested{false};
std::atomic<uint8_t> systemRequest{static_cast<uint8_t>(SystemRequest::None)};

// POST body collected by the body callback; freed by the library (free()) with the request.
struct BodyBuffer {
    size_t capacity;
    size_t length;
    bool overflow;
    char *data() { return reinterpret_cast<char *>(this + 1); }
};

JsonDocument makeDoc() { return JsonDocument(PsramAllocator::instance()); }

void sendJson(AsyncWebServerRequest *request, int code, const JsonDocument &doc) {
    std::string body;
    serializeJson(doc, body);
    AsyncWebServerResponse *response = request->beginResponse(code, kJson, body.c_str());
    response->addHeader("Cache-Control", "no-store");
    request->send(response);
}

void sendRawJson(AsyncWebServerRequest *request, int code, const std::string &body) {
    AsyncWebServerResponse *response = request->beginResponse(code, kJson, body.c_str());
    response->addHeader("Cache-Control", "no-store");
    request->send(response);
}

void sendError(AsyncWebServerRequest *request, int code, const std::string &error,
               const std::string &field = std::string()) {
    JsonDocument doc = makeDoc();
    doc["error"] = error;
    if (!field.empty()) {
        doc["field"] = field;
    }
    sendJson(request, code, doc);
}

guard::GuardContext guardContext() {
    std::lock_guard<std::mutex> lock(snapshotMutex);
    guard::GuardContext context;
    context.hostname = netStatus.hostname;
    context.deviceIp = netStatus.staIp;
    context.apActive = netStatus.apActive;
    return context;
}

// The decoded URL path with all its bytes: c_str() would end it at a decoded "%00"
// (so "/index.html%00" must not be taken for "/index.html").
std::string urlPath(AsyncWebServerRequest *request) {
    const String &url = request->url();
    return std::string(url.c_str(), url.length());
}

guard::RequestInfo requestInfo(AsyncWebServerRequest *request) {
    guard::RequestInfo info;
    info.method = guard::methodFromString(asyncsrv::methodToString(request->method()));
    info.host = request->host().c_str();
    info.path = urlPath(request);
    info.contentType = request->header("Content-Type").c_str();
    info.hasCsrfHeader = request->hasHeader(guard::kCsrfHeaderName);
    info.csrfHeader = request->header(guard::kCsrfHeaderName).c_str();
    info.contentLength = request->contentLength();
    String te = request->header("Transfer-Encoding");
    te.toLowerCase();
    info.chunked = te.indexOf("chunked") >= 0;
    return info;
}

std::string cachedWebPassword() {
    std::lock_guard<std::mutex> lock(snapshotMutex);
    return webPassword;
}

void refreshWebPassword() {
    std::string pw = store->webPassword();
    std::lock_guard<std::mutex> lock(snapshotMutex);
    webPassword = std::move(pw);
}

// True if no web password is set or the request carries the right Basic credentials.
bool authorized(AsyncWebServerRequest *request) {
    const std::string password = cachedWebPassword();
    return password.empty() ||
           guard::basicAuthMatches(request->header("Authorization").c_str(), guard::kAuthUser, password);
}

// Server-wide middleware: guards, then optional Basic auth, then the handler.
void guardMiddleware(AsyncWebServerRequest *request, ArMiddlewareNext next) {
    const guard::Decision decision = guard::checkRequest(requestInfo(request), guardContext());
    if (decision.verdict == guard::Verdict::Redirect) {
        request->redirect(guard::kCaptivePortalUrl);
        return;
    }
    if (!decision.allowed()) {
        sendError(request, decision.status, decision.reason);
        return;
    }
    if (!authorized(request)) {
        request->requestAuthentication(AsyncAuthType::AUTH_BASIC, kRealm);
        return;
    }
    next();
}

// Body callback for POST /api/config: buffers at most kMaxBodyBytes, and nothing for a
// request the guards or the web password will reject anyway (the body arrives before
// the middleware runs).
void onConfigBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index,
                  size_t total) {
    if (index == 0 && request->_tempObject == nullptr) {
        if (total == 0 || total > guard::kMaxBodyBytes ||
            !guard::checkRequest(requestInfo(request), guardContext()).allowed() || !authorized(request)) {
            return;
        }
        void *mem = heap_caps_malloc(sizeof(BodyBuffer) + total + 1,
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (mem == nullptr) {
            return;
        }
        BodyBuffer *buffer = static_cast<BodyBuffer *>(mem);
        buffer->capacity = total;
        buffer->length = 0;
        buffer->overflow = false;
        request->_tempObject = buffer;
    }
    BodyBuffer *buffer = static_cast<BodyBuffer *>(request->_tempObject);
    if (buffer == nullptr || buffer->overflow) {
        return;
    }
    if (index != buffer->length || len > buffer->capacity - buffer->length) {
        buffer->overflow = true;
        return;
    }
    memcpy(buffer->data() + index, data, len);
    buffer->length += len;
}

void handleStatus(AsyncWebServerRequest *request) {
    wifi::Status net;
    ha::Status haNow;
    size_t subscribed = 0;
    size_t entities = 0;
    core::ha::StatesState catState = core::ha::StatesState::None;
    size_t catCount = 0;
    std::string catError;
    {
        std::lock_guard<std::mutex> lock(snapshotMutex);
        net = netStatus;
        haNow = haStatus;
        subscribed = subscribedCount;
        entities = entityCount;
        catState = catalogState;
        catCount = catalogCount;
        catError = catalogError;
    }
    JsonDocument doc = makeDoc();
    doc["fw"] = FW_VERSION;
    doc["uptime_s"] = millis() / 1000;
    doc["heap_free"] = ESP.getFreeHeap();
    doc["heap_min_free"] = ESP.getMinFreeHeap();
    doc["psram_free"] = ESP.getFreePsram();
    doc["touch_variant"] = touchName;
    JsonObject w = doc["wifi"].to<JsonObject>();
    w["mode"] = wifi::modeName(net.mode);
    w["ssid"] = net.ssid;
    w["connected"] = net.staConnected;
    w["ip"] = net.staIp;
    w["rssi"] = net.rssi;
    w["ap_active"] = net.apActive;
    w["ap_ssid"] = net.apSsid;
    w["ap_ip"] = net.apActive ? wifi::kApIp : "";
    w["hostname"] = net.hostname;
    JsonObject h = doc["ha"].to<JsonObject>();
    h["state"] = core::ha::stateName(haNow.state);
    h["ha_version"] = haNow.haVersion;
    h["last_error"] = haNow.lastError;
    h["tls"] = core::ha::tlsModeName(haNow.tls);
    h["subscribed"] = subscribed;
    h["entities"] = entities;
    JsonObject c = h["catalog"].to<JsonObject>();
    c["state"] = core::ha::statesStateName(catState);
    c["count"] = catCount;
    c["error"] = catError;
    sendJson(request, 200, doc);
}

void handleConfigGet(AsyncWebServerRequest *request) {
    const core::config::Config config = store->config();
    sendRawJson(request, 200,
                core::config::toRedactedJson(config, store->secretsPresent(), PsramAllocator::instance()));
}

void handleConfigPost(AsyncWebServerRequest *request) {
    BodyBuffer *buffer = static_cast<BodyBuffer *>(request->_tempObject);
    if (buffer == nullptr || buffer->overflow || buffer->length != buffer->capacity) {
        sendError(request, request->contentLength() > guard::kMaxBodyBytes ? 413 : 400,
                  request->contentLength() > guard::kMaxBodyBytes ? "body_too_large" : "invalid_json");
        return;
    }
    // The Wi-Fi credentials before and after tell whether Wi-Fi really changes (a
    // password set to the stored value, or a forced Clear of nothing, is no change).
    const std::string ssidBefore = store->config().wifi.ssid;
    const std::string passwordBefore = store->wifiPassword();

    const core::config::MergeResult result = store->applyPatch(buffer->data(), buffer->length);

    bool restartWifi = false;
    if (result.cfgWritten || result.secretsWritten) {
        // Also after nvs_write_failed: the stored config may have changed, so the main
        // loop re-reads it (and restarts Wi-Fi if the credentials differ). A patch that
        // changes nothing writes nothing and needs no re-read.
        restartWifi = store->config().wifi.ssid != ssidBefore || store->wifiPassword() != passwordBefore;
        refreshWebPassword();
        configDirty = true;
    }
    if (!result.ok) {
        const bool writeFailed = result.error.message == "nvs_write_failed";
        util::logPrintf("WEB config rejected field=%s error=%s\n", result.error.field.c_str(),
                        result.error.message.c_str());
        sendError(request, writeFailed ? 500 : 400, result.error.message, result.error.field);
        return;
    }
    util::logPrintf("WEB config saved restart_wifi=%d\n", restartWifi ? 1 : 0);
    JsonDocument doc = makeDoc();
    doc["ok"] = true;
    doc["restart_wifi"] = restartWifi;
    sendJson(request, 200, doc);
}

void handleScan(AsyncWebServerRequest *request) {
    JsonDocument doc = makeDoc();
    {
        std::lock_guard<std::mutex> lock(snapshotMutex);
        const uint32_t now = millis();
        // A failed scan is never reused: the next request starts a new one.
        if (!scanPending && (!scanHaveResults || scanFailed || now - scanAtMs >= kScanFreshMs)) {
            scanPending = true;
            scanRequested = true;
        }
        doc["scanning"] = scanPending;
        if (scanHaveResults) {
            doc["age_s"] = (now - scanAtMs) / 1000;
            doc["failed"] = scanFailed;
        } else {
            doc["age_s"] = nullptr;
            doc["failed"] = false;
        }
        JsonArray list = doc["networks"].to<JsonArray>();
        for (const wifi::ScanNetwork &net : scanNetworks) {
            JsonObject o = list.add<JsonObject>();
            o["ssid"] = net.ssid;
            o["rssi"] = net.rssi;
            o["secure"] = net.secure;
        }
    }
    sendJson(request, 200, doc);
}

void handleHaRetry(AsyncWebServerRequest *request) {
    haRetryRequested = true;
    util::logPrintf("WEB ha retry requested\n");
    JsonDocument doc = makeDoc();
    doc["ok"] = true;
    sendJson(request, 200, doc);
}

void requestSystem(AsyncWebServerRequest *request, SystemRequest what) {
    systemRequest = static_cast<uint8_t>(what);
    util::logPrintf("%s\n", what == SystemRequest::Reboot ? "WEB reboot requested" : "WEB factory reset requested");
    JsonDocument doc = makeDoc();
    doc["ok"] = true;
    sendJson(request, 200, doc);
}

void sendPending(AsyncWebServerRequest *request) {
    JsonDocument doc = makeDoc();
    doc["pending"] = true;
    sendJson(request, 202, doc);
}

// One running catalog stream. The response's filler owns it, so the count drops when the
// library deletes the response (sent, aborted by the client, or never sent).
struct CatalogStreamSlot {
    ~CatalogStreamSlot() { --catalogStreams; }
};

// 200: streams the shared catalog body. The filler keeps the body alive and copies only
// what fits into the library's send buffer; no mutex is held while sending.
void sendCatalog(AsyncWebServerRequest *request, std::shared_ptr<const std::string> body) {
    if (catalogStreams.fetch_add(1) >= kMaxCatalogStreams) {
        --catalogStreams;
        AsyncWebServerResponse *response = request->beginResponse(503, kJson, "{\"error\":\"busy\"}");
        response->addHeader("Cache-Control", "no-store");
        response->addHeader("Retry-After", "1");
        request->send(response);
        return;
    }
    std::shared_ptr<CatalogStreamSlot> slot = std::make_shared<CatalogStreamSlot>();
    const size_t length = body->size();
    AsyncWebServerResponse *response = request->beginResponse(
        kJson, length, [body, slot](uint8_t *buffer, size_t maxLen, size_t index) -> size_t {
            if (index >= body->size()) {
                return 0;
            }
            const size_t n = std::min(maxLen, body->size() - index);
            memcpy(buffer, body->data() + index, n);
            return n;
        });
    response->addHeader("Cache-Control", "no-store");
    request->send(response);
}

// GET /api/ha/entities: the first matching row of plan-step6.md §3.5 wins.
void handleEntitiesGet(AsyncWebServerRequest *request) {
    using core::ha::StatesState;
    std::shared_ptr<const std::string> body;
    std::string error;
    int code = 0;
    {
        std::lock_guard<std::mutex> lock(snapshotMutex);
        if (catalogState == StatesState::Fetching || catalogState == StatesState::Building) {
            code = 202;
        } else if (catalogBody) {
            body = catalogBody;
            code = 200;
        } else if (catalogState == StatesState::Failed) {
            error = catalogError;
            code = 502;
        } else if (haStatus.state != core::ha::State::Ready) {
            code = 503;
        } else {
            // none, or ready without a body (treated alike): the main loop fetches unless it
            // has a list to serve by then
            if (catalogRequest == CatalogRequest::None) {
                catalogRequest = CatalogRequest::Fetch;
            }
            code = 202;
        }
    }
    if (code == 200) {
        sendCatalog(request, std::move(body));
    } else if (code == 202) {
        sendPending(request);
    } else if (code == 502) {
        JsonDocument doc = makeDoc();
        doc["error"] = "fetch_failed";
        doc["detail"] = error;
        sendJson(request, 502, doc);
    } else {
        sendError(request, 503, "ha_not_connected");
    }
}

// POST /api/ha/entities/refresh: drops the published catalog / error and asks the main
// loop for a new fetch (one already running is kept).
void handleEntitiesRefresh(AsyncWebServerRequest *request) {
    using core::ha::StatesState;
    bool ready = false;
    {
        std::lock_guard<std::mutex> lock(snapshotMutex);
        ready = haStatus.state == core::ha::State::Ready;
        if (ready) {
            catalogRequest = CatalogRequest::Refresh;
            if (catalogState != StatesState::Building) {
                catalogState = StatesState::Fetching;  // corrected by the main loop's next publish
            }
            catalogBody.reset();  // HaClient still holds the body: never the last reference here
            catalogCount = 0;
            catalogError.clear();
        }
    }
    if (!ready) {
        sendError(request, 503, "ha_not_connected");
        return;
    }
    util::logPrintf("WEB entities refresh requested\n");
    sendPending(request);
}

void handleMeta(AsyncWebServerRequest *request) { sendRawJson(request, 200, metaBody); }

bool isListed(const std::string &path) {
    return std::find(staticFiles.begin(), staticFiles.end(), path) != staticFiles.end();
}

void handleNotFound(AsyncWebServerRequest *request) {
    const std::string path = urlPath(request);
    if (guard::isApiPath(path)) {
        sendError(request, 404, "not_found");
    } else if ((path == "/" || path == "/index.html") && !isListed("/index.html")) {
        request->send(503, "text/plain",
                      "HA Knob: the web UI files are missing. Flash them with "
                      "pio run -e t-encoder-pro -t uploadfs");
    } else {
        request->send(404, "text/plain", "Not found");
    }
}

// "/" -> "/index.html"; true if the path names a file listed in staticFiles. (Only GET
// reaches the static handler; a HEAD request ends in handleNotFound as 404.)
bool isStaticFile(AsyncWebServerRequest *request) {
    std::string path = urlPath(request);
    if (path == "/") {
        path = "/index.html";
    }
    return isListed(path);
}

void listStaticFiles() {
    File root = LittleFS.open("/");
    if (!root || !root.isDirectory()) {
        return;
    }
    for (File f = root.openNextFile(); f; f = root.openNextFile()) {
        if (!f.isDirectory()) {
            std::string name = f.name();
            staticFiles.push_back(name.empty() || name[0] != '/' ? "/" + name : name);
        }
    }
}

}  // namespace

void begin(ConfigStore &configStore, const char *touchVariant) {
    store = &configStore;
    touchName = touchVariant;
    refreshWebPassword();
    metaBody = core::domain_actions::metaJson(PsramAllocator::instance());

    // LittleFS holds only web assets; never format it automatically (uploadfs does that).
    fsOk = LittleFS.begin(false);
    if (fsOk) {
        listStaticFiles();
    }

    // Defence in depth on every response: no framing (clickjacking), no MIME sniffing,
    // scripts and styles only from the device itself. No CORS headers, ever.
    DefaultHeaders::Instance().addHeader("X-Frame-Options", "DENY");
    DefaultHeaders::Instance().addHeader("X-Content-Type-Options", "nosniff");
    DefaultHeaders::Instance().addHeader("Referrer-Policy", "no-referrer");
    DefaultHeaders::Instance().addHeader(
        "Content-Security-Policy",
        "default-src 'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'self'");

    server.addMiddleware(guardMiddleware);

    using namespace AsyncWebRequestMethod;
    server.on("/api/status", HTTP_GET, handleStatus);
    server.on("/api/config", HTTP_GET, handleConfigGet);
    server.on("/api/config", HTTP_POST, handleConfigPost, nullptr, onConfigBody);
    server.on("/api/wifi/scan", HTTP_GET, handleScan);
    server.on("/api/ha/retry", HTTP_POST, handleHaRetry);
    // Exact matches: a plain path would also take "<path>/..." (the library's default).
    server.on(AsyncURIMatcher::exact("/api/meta/actions"), HTTP_GET, handleMeta);
    server.on(AsyncURIMatcher::exact("/api/ha/entities"), HTTP_GET, handleEntitiesGet);
    server.on(AsyncURIMatcher::exact("/api/ha/entities/refresh"), HTTP_POST, handleEntitiesRefresh);
    server.on("/api/reboot", HTTP_POST,
              [](AsyncWebServerRequest *request) { requestSystem(request, SystemRequest::Reboot); });
    server.on("/api/factory-reset", HTTP_POST,
              [](AsyncWebServerRequest *request) { requestSystem(request, SystemRequest::FactoryReset); });
    if (fsOk) {
        server.serveStatic("/", LittleFS, "/")
            .setDefaultFile("index.html")
            .setCacheControl("no-cache")
            .setTryGzipFirst(false)
            .setFilter(isStaticFile);
    }
    server.onNotFound(handleNotFound);
    server.begin();
    util::logPrintf("WEB started port=%u fs=%d files=%u\n", static_cast<unsigned>(kPort), fsOk ? 1 : 0,
                    static_cast<unsigned>(staticFiles.size()));
}

void publishNetwork(const wifi::Status &status) {
    std::lock_guard<std::mutex> lock(snapshotMutex);
    netStatus = status;
}

void publishScan(const std::vector<wifi::ScanNetwork> &networks, bool failed, uint32_t nowMs) {
    std::lock_guard<std::mutex> lock(snapshotMutex);
    scanNetworks = networks;
    scanFailed = failed;
    scanHaveResults = true;
    scanPending = false;
    scanAtMs = nowMs;
}

bool takeConfigDirty() { return configDirty.exchange(false); }

bool takeScanRequest() { return scanRequested.exchange(false); }

void publishHa(const ha::Status &status) {
    std::lock_guard<std::mutex> lock(snapshotMutex);
    haStatus = status;
}

void publishCatalog(core::ha::StatesState state, std::shared_ptr<const std::string> body, size_t count,
                    const std::string &error) {
    std::lock_guard<std::mutex> lock(snapshotMutex);
    if (catalogRequest == CatalogRequest::Refresh) {
        return;  // the refresh is not taken yet: keep the catalog it cleared
    }
    catalogState = state;
    catalogBody = std::move(body);
    catalogCount = count;
    catalogError = error;
}

void publishEntityCounts(size_t entities, size_t subscribed) {
    std::lock_guard<std::mutex> lock(snapshotMutex);
    entityCount = entities;
    subscribedCount = subscribed;
}

CatalogRequest takeCatalogRequest() {
    std::lock_guard<std::mutex> lock(snapshotMutex);
    const CatalogRequest request = catalogRequest;
    catalogRequest = CatalogRequest::None;
    return request;
}

bool takeHaRetry() { return haRetryRequested.exchange(false); }

SystemRequest takeSystemRequest() {
    return static_cast<SystemRequest>(systemRequest.exchange(static_cast<uint8_t>(SystemRequest::None)));
}

}  // namespace web
}  // namespace net
