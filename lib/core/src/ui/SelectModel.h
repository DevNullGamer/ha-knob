#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

// The Select list's state (plan-step7.md §3.5, Q7-9): which item is selected while the list
// is open and when it closes for lack of input. Pure and clock-injected; ui:: draws the list
// (the control titles plus "Status") and maps the chosen index to a page. Times are
// millis(); callers pass one `now` per loop pass, so timestamps never decrease between calls
// (an older `now` would count as ~49.7 days idle).
namespace core {
namespace ui {

class SelectModel {
public:
    // Without input for this long the list closes without a change.
    static constexpr uint32_t kIdleMs = 8000;

    // Opens the list with `count` items and `selected` selected (clamped to the last item);
    // starts the idle time. count 0 leaves the list closed. Opening an open list restarts it.
    void open(size_t count, size_t selected, uint32_t nowMs);

    // Moves the selection by delta items, clamped at both ends (no wrap). Every call counts
    // as input and restarts the idle time, also one that hits an end. Ignored while closed.
    void move(int delta, uint32_t nowMs);

    // Closes the list and returns the selected index; nullopt (and no change) while closed.
    std::optional<size_t> choose();

    // True once, when the list is open and kIdleMs have passed since the last input; the
    // list is then closed. Elapsed time is taken modulo 2^32 ms.
    bool tick(uint32_t nowMs);

    bool isOpen() const { return open_; }
    // Closes without a choice (long press, config change, setup screen). No-op when closed.
    void close() { open_ = false; }

    size_t count() const { return count_; }
    // The selected index; meaningful only while open.
    size_t selected() const { return selected_; }

private:
    size_t count_ = 0;
    size_t selected_ = 0;
    uint32_t lastInputMs_ = 0;
    bool open_ = false;
};

}  // namespace ui
}  // namespace core
