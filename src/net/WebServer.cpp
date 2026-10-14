#include "net/WebServer.h"

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <esp_heap_caps.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>

#include "config/ConfigStore.h"
#include "hal/PsramAllocator.h"
#include "version.h"
#include "web/RequestGuard.h"

namespace net {
namespace web {

namespace {

namespace guard = core::web;

constexpr uint16_t kPort = 80;
constexpr uint32_t kScanFreshMs = 10000;  // GET /api/wifi/scan reuses results this young
constexpr const char *kRealm = "HA Knob";
constexpr const char *kJson = "application/json";

AsyncWebServer server(kPort);
ConfigStore *store = nullptr;
const char *touchName = "none";
bool fsOk = false;

// State shared between the main loop and the async_tcp task; guarded by snapshotMutex.
std::mutex snapshotMutex;
wifi::Status netStatus;
std::string webPassword;  // cached web_pw ("" = no password); refreshed after each patch
std::vector<wifi::ScanNetwork> scanNetworks;
bool scanHaveResults = false;
bool scanFailed = false;
bool scanPending = false;  // requested or running
uint32_t scanAtMs = 0;

std::atomic<bool> configDirty{false};
std::atomic<bool> scanRequested{false};
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

guard::RequestInfo requestInfo(AsyncWebServerRequest *request) {
    guard::RequestInfo info;
    info.method = guard::methodFromString(asyncsrv::methodToString(request->method()));
    info.host = request->host().c_str();
    info.path = request->url().c_str();
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
    const std::string password = cachedWebPassword();
    if (!password.empty() &&
        !guard::basicAuthMatches(request->header("Authorization").c_str(), guard::kAuthUser, password)) {
        request->requestAuthentication(AsyncAuthType::AUTH_BASIC, kRealm);
        return;
    }
    next();
}

// Body callback for POST /api/config: buffers at most kMaxBodyBytes, and nothing for a
// request the guards will reject anyway.
void onConfigBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index,
                  size_t total) {
    if (index == 0 && request->_tempObject == nullptr) {
        if (total == 0 || total > guard::kMaxBodyBytes ||
            !guard::checkRequest(requestInfo(request), guardContext()).allowed()) {
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
    {
        std::lock_guard<std::mutex> lock(snapshotMutex);
        net = netStatus;
    }
    JsonDocument doc = makeDoc();
    doc["fw"] = FW_VERSION;
    doc["uptime_s"] = millis() / 1000;
    doc["heap_free"] = ESP.getFreeHeap();
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
    JsonObject ha = doc["ha"].to<JsonObject>();
    ha["state"] = "not_available";  // the HA connection comes in a later version
    ha["ha_version"] = "";
    ha["last_error"] = "";
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
    if (result.cfgWritten) {
        // Also after nvs_write_failed: the stored config may have changed, so the main
        // loop re-reads it (and restarts Wi-Fi if the credentials differ).
        restartWifi = store->config().wifi.ssid != ssidBefore || store->wifiPassword() != passwordBefore;
        refreshWebPassword();
        configDirty = true;
    }
    if (!result.ok) {
        const bool writeFailed = result.error.message == "nvs_write_failed";
        Serial.printf("WEB config rejected field=%s error=%s\n", result.error.field.c_str(),
                      result.error.message.c_str());
        sendError(request, writeFailed ? 500 : 400, result.error.message, result.error.field);
        return;
    }
    Serial.printf("WEB config saved restart_wifi=%d\n", restartWifi ? 1 : 0);
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
        if (!scanPending && (!scanHaveResults || now - scanAtMs >= kScanFreshMs)) {
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

void requestSystem(AsyncWebServerRequest *request, SystemRequest what) {
    systemRequest = static_cast<uint8_t>(what);
    Serial.println(what == SystemRequest::Reboot ? "WEB reboot requested" : "WEB factory reset requested");
    JsonDocument doc = makeDoc();
    doc["ok"] = true;
    sendJson(request, 200, doc);
}

void handleNotFound(AsyncWebServerRequest *request) {
    const std::string path = request->url().c_str();
    if (guard::isApiPath(path)) {
        sendError(request, 404, "not_found");
    } else if (!fsOk && (path == "/" || path == "/index.html")) {
        request->send(503, "text/plain",
                      "HA Knob: the web UI files are missing. Flash them with "
                      "pio run -e t-encoder-pro -t uploadfs");
    } else {
        request->send(404, "text/plain", "Not found");
    }
}

}  // namespace

void begin(ConfigStore &configStore, const char *touchVariant) {
    store = &configStore;
    touchName = touchVariant;
    refreshWebPassword();

    // LittleFS holds only web assets; never format it automatically (uploadfs does that).
    fsOk = LittleFS.begin(false);

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
    server.on("/api/reboot", HTTP_POST,
              [](AsyncWebServerRequest *request) { requestSystem(request, SystemRequest::Reboot); });
    server.on("/api/factory-reset", HTTP_POST,
              [](AsyncWebServerRequest *request) { requestSystem(request, SystemRequest::FactoryReset); });
    if (fsOk) {
        server.serveStatic("/", LittleFS, "/")
            .setDefaultFile("index.html")
            .setCacheControl("no-cache")
            .setTryGzipFirst(false);
    }
    server.onNotFound(handleNotFound);
    server.begin();
    Serial.printf("WEB started port=%u fs=%d\n", static_cast<unsigned>(kPort), fsOk ? 1 : 0);
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

SystemRequest takeSystemRequest() {
    return static_cast<SystemRequest>(systemRequest.exchange(static_cast<uint8_t>(SystemRequest::None)));
}

}  // namespace web
}  // namespace net
