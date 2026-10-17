#include "ui/PageRing.h"

namespace core {
namespace ui {

void PageRing::setControlCount(size_t count) {
    if (!onStatus_ && index_ >= count) {
        index_ = 0;
    }
    controlCount_ = count;
}

void PageRing::next() {
    if (onStatus_) {
        onStatus_ = false;
        index_ = 0;
    } else if (index_ + 1 < controlPages()) {
        ++index_;
    } else {
        onStatus_ = true;
    }
}

void PageRing::previous() {
    if (onStatus_) {
        onStatus_ = false;
        index_ = controlPages() - 1;
    } else if (index_ > 0) {
        --index_;
    } else {
        onStatus_ = true;
    }
}

bool PageRing::selectControl(size_t index) {
    if (index >= controlCount_) {
        return false;
    }
    onStatus_ = false;
    index_ = index;
    return true;
}

int PageRing::controlPage() const {
    if (onStatus_ || controlCount_ == 0) {
        return -1;
    }
    return static_cast<int>(index_);
}

}  // namespace ui
}  // namespace core
