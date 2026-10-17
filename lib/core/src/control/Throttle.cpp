#include "control/Throttle.h"

#include <cmath>

namespace core {
namespace control {

namespace {
constexpr double kSameValue = 1e-9;

bool differs(double a, double b) { return std::fabs(a - b) > kSameValue; }
}  // namespace

Throttle::Throttle(uint32_t intervalMs, uint32_t settleMs)
    : intervalMs_(intervalMs), settleMs_(settleMs) {}

void Throttle::change(double value, uint32_t nowMs) {
    if (!holding(nowMs)) {
        hasLastSent_ = false;  // a new hold: HA's value may have moved since the last send
    }
    target_ = value;
    unsent_ = !hasLastSent_ || differs(value, lastSent_);
    hasChange_ = true;
    lastChangeMs_ = nowMs;
    released_ = false;
}

std::optional<double> Throttle::due(uint32_t nowMs, bool canSend) const {
    if (!unsent_ || !canSend) {
        return std::nullopt;
    }
    if (hasSendTime_ && nowMs - lastSendMs_ < intervalMs_) {
        return std::nullopt;
    }
    return target_;
}

void Throttle::sent(double value, uint32_t nowMs) {
    lastSent_ = value;
    hasLastSent_ = true;
    lastSendMs_ = nowMs;
    hasSendTime_ = true;
    ++inFlight_;
    unsent_ = differs(target_, value);
}

void Throttle::answered(bool ok) {
    if (inFlight_ > 0) {
        --inFlight_;
    }
    if (!ok) {
        failed();
    }
}

void Throttle::failed() {
    unsent_ = false;
    hasLastSent_ = false;
    released_ = true;
}

void Throttle::drop() {
    unsent_ = false;
    released_ = true;
}

bool Throttle::holding(uint32_t nowMs) const {
    if (released_) {
        return false;
    }
    if (unsent_ || inFlight_ > 0) {
        return true;
    }
    return hasChange_ && nowMs - lastChangeMs_ < settleMs_;
}

}  // namespace control
}  // namespace core
