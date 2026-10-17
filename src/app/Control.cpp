#include "app/Control.h"

#include <string>

#include "control/ControlEngine.h"
#include "ha/HaClient.h"
#include "input/Acceleration.h"
#include "ui/Ui.h"
#include "util/Log.h"
#include "util/Text.h"

namespace app {
namespace control {

namespace {

namespace cc = core::control;

// Sends the engine's calls on the HA socket (ha::callService reads millis() itself).
class HaSender : public cc::CallSender {
public:
    bool send(const core::domain_actions::ServiceCall &call, uint32_t tag) override {
        return ha::callService(call, tag) != 0;
    }
};

HaSender sender;
cc::ControlEngine engine(sender);  // after `sender` (same translation unit: declaration order)
core::Acceleration acceleration;
std::vector<core::config::Control> controls;  // as given to the engine

bool haReady() { return ha::status().state == core::ha::State::Ready; }

const core::ha::EntityState *entityFor(size_t page) {
    return page < controls.size() ? ha::entities().find(controls[page].entityId) : nullptr;
}

}  // namespace

void applyConfig(const core::config::DeviceConfig &device,
                 const std::vector<core::config::Control> &newControls) {
    acceleration.setEnabled(device.accel);
    cc::EngineOptions options;
    options.sendIntervalMs = device.sendIntervalMs;
    engine.setOptions(options);
    engine.setControls(newControls);
    controls = newControls;
}

void rotate(size_t page, int detents, uint32_t nowMs) {
    if (detents == 0 || page >= controls.size()) {
        return;
    }
    const int steps = acceleration.onDetents(detents, nowMs);
    engine.rotate(page, steps, entityFor(page), haReady(), nowMs);
}

void press(size_t page, uint32_t nowMs) {
    if (page >= controls.size()) {
        return;
    }
    util::logPrintf("CTL press page=%u press=%s\n", static_cast<unsigned>(page + 1),
                    core::util::logSafe(controls[page].press, 24).c_str());
    engine.press(page, entityFor(page), haReady(), nowMs);
}

void loop(uint32_t nowMs) {
    // Every result before tick(): a lost result would keep its in-flight slot occupied.
    for (const core::ha::CallResult &result : ha::takeCallResults()) {
        engine.onResult(result, nowMs);
    }
    engine.tick(haReady(), nowMs);

    const std::optional<cc::Notice> notice = engine.takeNotice();
    if (notice) {
        util::logPrintf("CTL notice=%s page=%u text=%s\n", cc::noticeKindName(notice->kind),
                        static_cast<unsigned>(notice->control + 1),
                        core::util::logSafe(notice->text, 60).c_str());
        ui::showToast(notice->text, nowMs);
    }
}

std::optional<core::control::LocalValue> local(size_t page, uint32_t nowMs) {
    return engine.local(page, nowMs);
}

uint32_t version() { return engine.version(); }

}  // namespace control
}  // namespace app
