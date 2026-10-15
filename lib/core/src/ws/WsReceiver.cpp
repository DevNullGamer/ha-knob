#include "ws/WsReceiver.h"

#include <cstring>

#include "ws/WsFrame.h"

namespace core {
namespace ws {

namespace {

constexpr uint8_t kText = uint8_t(Opcode::Text);
constexpr uint8_t kBinary = uint8_t(Opcode::Binary);
constexpr uint8_t kContinuation = uint8_t(Opcode::Continuation);
constexpr uint8_t kClose = uint8_t(Opcode::Close);
constexpr uint8_t kPing = uint8_t(Opcode::Ping);
constexpr uint8_t kPong = uint8_t(Opcode::Pong);

bool isControl(uint8_t op) { return (op & 0x8) != 0; }

size_t clampSize(uint64_t v) { return v > SIZE_MAX ? SIZE_MAX : size_t(v); }

}  // namespace

WsReceiver::WsReceiver(BufferProvider *provider, Limits limits)
    : provider_(provider), limits_(limits) {
    // Keeps "total + 1" (the NUL) from wrapping a 32-bit size_t.
    if (limits_.maxMessageBytes > SIZE_MAX - 1) limits_.maxMessageBytes = SIZE_MAX - 1;
}

WsReceiver::~WsReceiver() { releaseBuffer(); }

void WsReceiver::releaseBuffer() {
    if (buf_ != nullptr && provider_ != nullptr) provider_->release(buf_);
    buf_ = nullptr;
    delivered_ = false;
    stored_ = 0;
    bufCap_ = 0;
}

void WsReceiver::reset() {
    releaseBuffer();
    state_ = State::Header;
    dead_ = false;
    hdrLen_ = 0;
    hdrNeeded_ = 2;
    fin_ = false;
    op_ = 0;
    remaining_ = 0;
    msgOpen_ = false;
    msgKind_ = 0;
    msgTotal_ = 0;
    discard_ = nullptr;
    ctrlLen_ = 0;
}

void WsReceiver::releaseDelivered() {
    if (delivered_) releaseBuffer();
}

size_t WsReceiver::messageBytesSoFar() const { return clampSize(msgTotal_); }

bool WsReceiver::fail(Event &ev, const char *reason) {
    dead_ = true;
    releaseBuffer();
    ev = Event();
    ev.type = EventType::ProtocolError;
    ev.reason = reason;
    return true;
}

void WsReceiver::startDiscard(const char *reason) {
    discard_ = reason;
    releaseBuffer();
}

// Called once the first two header bytes are known. Returns true if an event was produced.
bool WsReceiver::checkFirstBytes(Event &ev) {
    const uint8_t b0 = hdr_[0], b1 = hdr_[1];
    const uint8_t op = b0 & 0x0F;
    const uint8_t len7 = b1 & 0x7F;
    if ((b0 & 0x70) != 0) return fail(ev, "reserved_bits");
    if (op != kContinuation && op != kText && op != kBinary && !isControl(op)) return fail(ev, "bad_opcode");
    if (isControl(op) && op != kClose && op != kPing && op != kPong) return fail(ev, "bad_opcode");
    if ((b1 & 0x80) != 0) return fail(ev, "masked_frame");
    if (isControl(op)) {
        if ((b0 & 0x80) == 0 || len7 > 125) return fail(ev, "bad_control");
    } else if (op == kContinuation) {
        if (!msgOpen_) return fail(ev, "unexpected_continuation");
    } else if (msgOpen_) {
        return fail(ev, "expected_continuation");
    }
    hdrNeeded_ = 2 + (len7 == 126 ? 2 : len7 == 127 ? 8 : 0);
    return false;
}

// Called when the whole header is in hdr_. Returns true if an event was produced.
bool WsReceiver::beginFrame(Event &ev) {
    fin_ = (hdr_[0] & 0x80) != 0;
    op_ = hdr_[0] & 0x0F;
    const uint8_t len7 = hdr_[1] & 0x7F;
    uint64_t len = len7;
    if (len7 == 126) {
        len = (uint64_t(hdr_[2]) << 8) | hdr_[3];
    } else if (len7 == 127) {
        if ((hdr_[2] & 0x80) != 0) return fail(ev, "bad_length");
        len = 0;
        for (int i = 0; i < 8; ++i) len = (len << 8) | hdr_[2 + i];
    }
    remaining_ = len;

    if (isControl(op_)) {
        ctrlLen_ = 0;
        state_ = State::Control;
    } else {
        if (op_ != kContinuation) {
            msgKind_ = op_;
            msgTotal_ = 0;
            stored_ = 0;
            discard_ = (op_ == kBinary) ? "binary" : nullptr;
        }
        if (discard_ == nullptr) {
            if (msgTotal_ + len > limits_.maxMessageBytes) {
                startDiscard("too_large");
            } else {
                const size_t need = size_t(msgTotal_ + len) + 1;  // + NUL
                if (buf_ == nullptr || need > bufCap_) {
                    size_t target = need;
                    if (op_ == kContinuation || !fin_) {
                        // Fragmented message: grow geometrically (capped) so many small
                        // fragments do not cause a realloc each.
                        const size_t capLimit = limits_.maxMessageBytes + 1;
                        size_t doubled = (bufCap_ > capLimit / 2) ? capLimit : bufCap_ * 2;
                        if (doubled > target) target = doubled;
                    }
                    uint8_t *nb = (provider_ != nullptr) ? provider_->grow(buf_, target) : nullptr;
                    if (nb == nullptr && target > need && provider_ != nullptr) {
                        // The doubled size may not fit while the exact one does (buf_ is
                        // still valid after a failed grow).
                        target = need;
                        nb = provider_->grow(buf_, target);
                    }
                    if (nb == nullptr) {
                        startDiscard("no_memory");
                    } else {
                        buf_ = nb;
                        bufCap_ = target;
                    }
                }
            }
        }
        state_ = (discard_ != nullptr) ? State::Skip : State::Store;
    }
    if (remaining_ == 0) return completeFrame(ev);
    return false;
}

// The frame payload is complete. Returns true if an event was produced.
bool WsReceiver::completeFrame(Event &ev) {
    state_ = State::Header;
    hdrLen_ = 0;
    hdrNeeded_ = 2;

    if (isControl(op_)) {
        // RFC 6455 section 5.5.1: a Close body is empty or at least the 2-byte code.
        if (op_ == kClose && ctrlLen_ == 1) return fail(ev, "bad_control");
        ev = Event();
        ev.data = ctrl_;
        ev.length = ctrlLen_;
        if (op_ == kPing) {
            ev.type = EventType::Ping;
        } else if (op_ == kPong) {
            ev.type = EventType::Pong;
        } else {
            ctrl_[ctrlLen_] = 0;  // terminate the reason text
            ev.type = EventType::Close;
            ev.closeCode = (ctrlLen_ >= 2) ? uint16_t((ctrl_[0] << 8) | ctrl_[1]) : uint16_t(1005);
            ev.reason = (ctrlLen_ > 2) ? reinterpret_cast<const char *>(ctrl_ + 2) : "";
        }
        return true;
    }

    if (!fin_) {
        msgOpen_ = true;
        return false;
    }
    msgOpen_ = false;
    ev = Event();
    if (discard_ != nullptr) {
        ev.type = EventType::Dropped;
        ev.length = clampSize(msgTotal_);
        ev.reason = discard_;
        discard_ = nullptr;
    } else {
        buf_[stored_] = 0;
        ev.type = EventType::Text;
        ev.data = buf_;
        ev.length = stored_;
        delivered_ = true;  // released on the next feed()/reset()
    }
    msgTotal_ = 0;
    stored_ = 0;
    return true;
}

size_t WsReceiver::feed(const uint8_t *data, size_t len, Event &event) {
    event = Event();
    if (delivered_) releaseBuffer();
    if (dead_) return len;

    size_t used = 0;
    for (;;) {
        if (state_ == State::Header) {
            while (hdrLen_ < hdrNeeded_) {
                if (used == len) return used;
                hdr_[hdrLen_++] = data[used++];
                if (hdrLen_ == 2 && checkFirstBytes(event)) return used;
            }
            if (beginFrame(event)) return used;
            continue;
        }

        if (remaining_ > 0) {
            if (used == len) return used;
            const size_t avail = len - used;
            const size_t chunk = (remaining_ < avail) ? size_t(remaining_) : avail;
            if (state_ == State::Store) {
                std::memcpy(buf_ + stored_, data + used, chunk);
                stored_ += chunk;
                msgTotal_ += chunk;
            } else if (state_ == State::Skip) {
                msgTotal_ += chunk;
            } else {
                std::memcpy(ctrl_ + ctrlLen_, data + used, chunk);
                ctrlLen_ += chunk;
            }
            used += chunk;
            remaining_ -= chunk;
        }
        if (remaining_ == 0 && completeFrame(event)) return used;
    }
}

}  // namespace ws
}  // namespace core
