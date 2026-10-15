#include "ha/HaClient.h"

#include <Arduino.h>

#include <climits>
#include <cstdio>
#include <vector>

#include "ha/WsTransport.h"
#include "hal/PsramAllocator.h"
#include "util/Log.h"

namespace ha {

namespace {

namespace hc = core::ha;

WsTransport transport;
hc::HaSession session(transport, PsramAllocator::instance());
Status status_;
uint32_t shownChange = UINT32_MAX;
std::string configuredUrl;
std::string configuredToken;
hc::TlsSettings configuredTls;
bool configuredOnce = false;

// Mirrors the session into status_ and logs every change.
void refresh() {
    status_.tls = session.tlsMode();  // changes only in configure(), which logs it
    if (session.changeCount() == shownChange) {
        return;
    }
    shownChange = session.changeCount();
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
}

}  // namespace

void begin() {
    transport.begin(session);
}

void configure(const std::string &url, const std::string &token, const hc::TlsSettings &tls, uint32_t nowMs) {
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
    // Step 5 subscribes to nothing; Step 6 passes the configured controls' entity ids.
    session.configure(url, token, tls, std::vector<std::string>(), nowMs);
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

void loop() {
    transport.poll();
    session.tick(millis());  // taken after poll(), which can block during a TCP connect
    refresh();
}

const Status &status() { return status_; }

}  // namespace ha
