#include "ui/SelectModel.h"

namespace core {
namespace ui {

void SelectModel::open(size_t count, size_t selected, uint32_t nowMs) {
    if (count == 0) {
        open_ = false;
        return;
    }
    count_ = count;
    selected_ = selected < count ? selected : count - 1;
    lastInputMs_ = nowMs;
    open_ = true;
}

void SelectModel::move(int delta, uint32_t nowMs) {
    if (!open_) {
        return;
    }
    lastInputMs_ = nowMs;
    // In 64 bits: selected_ + delta cannot overflow for any int delta and list size.
    const long long last = static_cast<long long>(count_) - 1;
    long long next = static_cast<long long>(selected_) + delta;
    if (next < 0) next = 0;
    if (next > last) next = last;
    selected_ = static_cast<size_t>(next);
}

std::optional<size_t> SelectModel::choose() {
    if (!open_) {
        return std::nullopt;
    }
    open_ = false;
    return selected_;
}

bool SelectModel::tick(uint32_t nowMs) {
    if (!open_ || static_cast<uint32_t>(nowMs - lastInputMs_) < kIdleMs) {
        return false;
    }
    open_ = false;
    return true;
}

}  // namespace ui
}  // namespace core
