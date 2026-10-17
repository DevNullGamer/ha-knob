#pragma once

#include <cstddef>

// The device's page ring (plan-step6.md §3.7, Q6-1): one page per control (or a single
// NoControls page while there are none), followed by the Status page. Pure; ui:: decides
// when navigation is allowed and which screen shows the selected page.
namespace core {
namespace ui {

class PageRing {
public:
    // Sets the number of controls. A control page stays selected while its index is still
    // in range (else the first page is); the Status page stays the Status page.
    void setControlCount(size_t count);
    size_t controlCount() const { return controlCount_; }

    // Next / previous page, wrapping (the ring always has at least 2 pages).
    void next();
    void previous();

    // Selects control page index (the Select list, Step 7.8). Out of range (also with no
    // controls: the NoControls page is not a control) -> false and no change.
    bool selectControl(size_t index);
    // Selects the Status page.
    void selectStatus() { onStatus_ = true; }

    bool onStatus() const { return onStatus_; }
    // True on the NoControls page (no controls, Status not selected).
    bool onNoControls() const { return !onStatus_ && controlCount_ == 0; }
    // Index of the selected control page; -1 for the Status and the NoControls page.
    int controlPage() const;

private:
    // Pages before the Status page (the NoControls page counts as one).
    size_t controlPages() const { return controlCount_ == 0 ? 1 : controlCount_; }

    size_t controlCount_ = 0;
    size_t index_ = 0;  // control page (0 on the NoControls page)
    bool onStatus_ = false;
};

}  // namespace ui
}  // namespace core
