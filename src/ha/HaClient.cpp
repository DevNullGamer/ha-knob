#include "ha/HaClient.h"

#include <Arduino.h>

#include <climits>
#include <cstdio>
#include <optional>
#include <utility>

#include "ha/EntityCatalog.h"
#include "ha/WsTransport.h"
#include "hal/PsramAllocator.h"
#include "util/Log.h"

namespace ha {

namespace {

namespace hc = core::ha;

// Unused catalog JSON capacity kept at most (the writer reserves up to 4 bytes per entry,
// 1500 entries, beyond the exact size).
constexpr size_t kCatalogSlackBytes = 8192;
// Catalog JSON slices (R8): writer steps of a few entries (~0.12 ms each) until this much
// time of the loop pass is used, so one pass spends at most about this plus one step.
constexpr uint32_t kCatalogJsonSliceUs = 8000;
constexpr size_t kCatalogJsonStepEntries = 8;

WsTransport transport;
// The session's JSON documents, catalog and entity store live in PSRAM.
hc::HaSession session(transport, PsramAllocator::instance());
Status status_;
uint32_t shownChange = UINT32_MAX;
uint32_t shownStatesChange = UINT32_MAX;
std::string configuredUrl;
std::string configuredToken;
hc::TlsSettings configuredTls;
bool configuredOnce = false;

std::shared_ptr<const std::string> catalogJson_;  // set only while the fetch is Ready
size_t catalogCount_ = 0;
uint32_t catalogChange_ = 0;

// A taken catalog while its JSON is being written (catalogWriter_ points into
// pendingCatalog_; reset the writer first). Both empty when nothing is pending.
std::optional<hc::EntityCatalog> pendingCatalog_;
std::optional<hc::CatalogJsonWriter> catalogWriter_;
uint32_t jsonStartMs_ = 0;
uint32_t jsonPasses_ = 0;
uint32_t jsonSliceMaxUs_ = 0;

// Mirrors the session into status_ and logs every change.
void refreshStatus() {
    status_.tls = session.tlsMode();  // changes only in configure(), which logs it
    if (session.changeCount() == shownChange) {
        return;
    }
    shownChange = session.changeCount();
#if defined(HAKNOB_STATES_ON_READY)
    const bool becameReady = status_.state != hc::State::Ready && session.state() == hc::State::Ready;
#endif
    status_.state = session.state();
    status_.haVersion = session.haVersion();
    status_.lastError = session.lastError();

    char extra[48] = "";
    if (status_.state == hc::State::Backoff && session.networkUp()) {
        const int32_t waitMs = static_cast<int32_t>(session.nextAttemptMs() - millis());
        snprintf(extra, sizeof(extra), " retry_in_ms=%d", static_cast<int>(waitMs > 0 ? waitMs : 0));
    }
    util::logPrintf("HA state=%s%s%s%s%s%s\n", hc::stateName(status_.state),
                    status_.haVersion.empty() ? "" : " ha_version=", status_.haVersion.c_str(),
                    status_.lastError.empty() ? "" : " error=", status_.lastError.c_str(), extra);

#if defined(HAKNOB_STATES_ON_READY)
    // Test builds only (-DHAKNOB_STATES_ON_READY): fetch the entity list on every Ready,
    // so the get_states path can be exercised without a web request (GET /api/ha/entities).
    if (becameReady) {
        session.requestStates(millis());
    }
#endif
}

void dropPendingCatalog() {
    catalogWriter_.reset();  // before the catalog it points into
    pendingCatalog_.reset();
}

// Starts serialising a finished catalog (the GET /api/ha/entities body); loop() writes it
// in slices (writeCatalogSlice). The JSON string reaches PSRAM through the SPIRAM malloc
// threshold (its reserve is >= 4 KB from about 40 entities on).
void startCatalogJson(hc::EntityCatalog &&catalog) {
    dropPendingCatalog();
    jsonStartMs_ = millis();
    jsonPasses_ = 0;
    jsonSliceMaxUs_ = 0;
    pendingCatalog_.emplace(std::move(catalog));
    catalogWriter_.emplace(*pendingCatalog_, jsonStartMs_ / 1000, PsramAllocator::instance());
}

// One budgeted slice of the pending catalog JSON; publishes it when complete and frees
// the catalog (PSRAM).
void writeCatalogSlice() {
    if (!catalogWriter_) {
        return;
    }
    const uint32_t startUs = micros();
    while (!catalogWriter_->step(kCatalogJsonStepEntries) && micros() - startUs < kCatalogJsonSliceUs) {
    }
    const uint32_t sliceUs = micros() - startUs;
    ++jsonPasses_;
    if (sliceUs > jsonSliceMaxUs_) {
        jsonSliceMaxUs_ = sliceUs;
    }
    if (!catalogWriter_->done()) {
        return;
    }
    const size_t regrowths = catalogWriter_->regrowths();
    std::string json = catalogWriter_->take();
    if (json.capacity() - json.size() > kCatalogSlackBytes) {
        json.shrink_to_fit();  // only after a regrowth (the reserve is a lower bound); kept long
    }
    const uint32_t jsonMs = millis() - jsonStartMs_;
    const size_t jsonBytes = json.size();
    const hc::EntityCatalog &catalog = *pendingCatalog_;
    catalogJson_ = std::make_shared<const std::string>(std::move(json));
    catalogCount_ = catalog.size();
    ++catalogChange_;  // catalogState() Building -> Ready, catalogJson() set
    util::logPrintf("HA catalog done total=%u supported=%u skipped=%u build_ms=%u json=%u json_ms=%u "
                    "json_passes=%u json_slice_max_ms=%u json_regrow=%u\n",
                    static_cast<unsigned>(catalog.total()), static_cast<unsigned>(catalog.size()),
                    static_cast<unsigned>(catalog.skipped()), static_cast<unsigned>(session.catalogBuildMs()),
                    static_cast<unsigned>(jsonBytes), static_cast<unsigned>(jsonMs),
                    static_cast<unsigned>(jsonPasses_), static_cast<unsigned>((jsonSliceMaxUs_ + 999) / 1000),
                    static_cast<unsigned>(regrowths));
    dropPendingCatalog();
}

// Takes a finished catalog (its JSON is then written in slices); drops the published or
// pending one when the fetch is no longer Ready (a new fetch, a failure, or another HA
// instance). The session logs the failures.
void refreshCatalog() {
    if (session.statesChangeCount() == shownStatesChange) {
        return;
    }
    shownStatesChange = session.statesChangeCount();
    ++catalogChange_;
    std::optional<hc::EntityCatalog> catalog = session.takeCatalog();
    if (catalog) {
        catalogJson_.reset();
        catalogCount_ = 0;
        startCatalogJson(std::move(*catalog));
    } else if (session.statesState() != hc::StatesState::Ready) {
        catalogJson_.reset();
        catalogCount_ = 0;
        dropPendingCatalog();
    }
}

void refresh() {
    refreshStatus();
    refreshCatalog();
}

}  // namespace

void begin() {
    transport.begin(session);
    // Session lines come without the "HA " prefix. The logger only logs (it must never call
    // back into the session).
    session.setLogger([](const std::string &line) { util::logPrintf("HA %s\n", line.c_str()); });
}

void configure(const std::string &url, const std::string &token, const hc::TlsSettings &tls,
               const std::vector<std::string> &entityIds, uint32_t nowMs) {
    if (!configuredOnce || url != configuredUrl || token != configuredToken || tls != configuredTls) {
        configuredOnce = true;
        configuredUrl = url;
        configuredToken = token;
        configuredTls = tls;
        if (url.empty() || token.empty()) {
            util::logPrintf("HA config none url_set=%d token_set=%d\n", url.empty() ? 0 : 1,
                            token.empty() ? 0 : 1);
        } else {
            const hc::TlsMode mode = hc::tlsModeFor(hc::parseHaUrl(url), tls);
            char caLen[24] = "";
            if (mode == hc::TlsMode::CustomCa) {
                snprintf(caLen, sizeof(caLen), " ca_len=%u", static_cast<unsigned>(tls.caPem.size()));
            }
            util::logPrintf("HA config url=%s token_set len=%u tls=%s%s\n", url.c_str(),
                            static_cast<unsigned>(token.size()), hc::tlsModeName(mode), caLen);
        }
    }
    session.configure(url, token, tls, entityIds, nowMs);
    refresh();
}

void setNetworkUp(bool up, uint32_t nowMs) {
    session.setNetworkUp(up, nowMs);
    refresh();
}

void retry(uint32_t nowMs) {
    const hc::State state = session.state();
    if (state == hc::State::AuthFailed || state == hc::State::Backoff) {
        util::logPrintf("HA retry\n");  // HaSession::retry() acts in exactly these states
    }
    session.retry(nowMs);
    refresh();
}

bool requestStates(uint32_t nowMs) {
    const bool ok = session.requestStates(nowMs);
    refresh();
    return ok;
}

void loop() {
    transport.poll();
    session.tick(millis());  // taken after poll(), which can block during a TCP connect
    refresh();
    writeCatalogSlice();  // here only, so a loop pass writes at most one slice
}

const Status &status() { return status_; }

const hc::EntityStore &entities() { return session.entities(); }

size_t subscribedCount() { return session.entityIds().size(); }

hc::StatesState catalogState() {
    const hc::StatesState state = session.statesState();
    // the session is Ready once the catalog is built; the JSON may still be in progress
    return state == hc::StatesState::Ready && catalogWriter_ ? hc::StatesState::Building : state;
}

const std::string &catalogError() { return session.statesError(); }

std::shared_ptr<const std::string> catalogJson() { return catalogJson_; }

size_t catalogCount() { return catalogCount_; }

uint32_t catalogChangeCount() { return catalogChange_; }

}  // namespace ha
