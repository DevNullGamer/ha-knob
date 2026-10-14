#pragma once

#include <cstdint>

// Header-only on purpose: update() is called from the encoder ISR. Being always_inline,
// the whole decode path is compiled into the HAL's IRAM_ATTR ISR, and the transition
// table is packed into a 32-bit constant, so nothing on the ISR path lives in flash.

#if defined(__GNUC__)
#define CORE_ALWAYS_INLINE inline __attribute__((always_inline))
#else
#define CORE_ALWAYS_INLINE inline
#endif

namespace core {

namespace detail {

// Step per transition, index (prev << 2) | cur. Only used at compile time.
constexpr int8_t kQuadSteps[16] = {0, -1, +1, 0, +1, 0, 0, -1, -1, 0, 0, +1, 0, +1, -1, 0};

// Packs kQuadSteps into 2 bits per entry: 0 -> 0, +1 -> 1, -1 -> 3.
constexpr uint32_t packQuadSteps() {
    uint32_t packed = 0;
    for (uint32_t i = 0; i < 16; ++i) {
        const uint32_t code = kQuadSteps[i] < 0 ? 3u : static_cast<uint32_t>(kQuadSteps[i]);
        packed |= code << (2u * i);
    }
    return packed;
}

constexpr uint32_t kQuadPackedTable = packQuadSteps();

}  // namespace detail

// Table-driven Gray-code decoder for encoders with two valid transitions per detent
// (mechanical rest positions at AB = 00 and AB = 11, as on the T-Encoder Pro).
//
// A detent is counted when the inputs arrive at a rest state (00 or 11) that differs
// from the previous rest state, via a valid single-bit transition. Its direction is the
// direction of that final transition. Consequences:
//  - bounce on one input (e.g. 00 -> 10 -> 00) never counts;
//  - turning half-way and back (mid-detent reversal) never counts;
//  - an invalid double-bit jump counts nothing; if it lands on a rest state, that state
//    becomes the new reference, so the decoder rejoins the correct phase immediately.
class QuadratureDecoder {
public:
    explicit QuadratureDecoder(bool reverse = false) : reverse_(reverse) {}

    void setReverse(bool reverse) { reverse_ = reverse; }
    bool reverse() const { return reverse_; }

    // Sets the current input state without counting (call once at startup).
    CORE_ALWAYS_INLINE void reset(uint8_t ab) {
        prev_ = static_cast<uint8_t>(ab & 3u);
        lastRest_ = isRest(prev_) ? prev_ : kNoRest;
    }

    // Feeds the current input state (bit 1 = A, bit 0 = B).
    // Returns +1 / -1 when a detent completes, 0 otherwise.
    CORE_ALWAYS_INLINE int8_t update(uint8_t ab) {
        ab = static_cast<uint8_t>(ab & 3u);
        if (ab == prev_) {
            return 0;
        }
        const int8_t step = transitionStep(prev_, ab);
        prev_ = ab;
        if (!isRest(ab)) {
            return 0;
        }
        int8_t detent = 0;
        if (step != 0 && lastRest_ != kNoRest && ab != lastRest_) {
            detent = reverse_ ? static_cast<int8_t>(-step) : step;
        }
        lastRest_ = ab;
        return detent;
    }

    // +1 / -1 for a valid Gray-code step from prev to cur; 0 for no change or an invalid
    // double-bit jump. The sequence 00 -> 10 -> 11 -> 01 -> 00 is +1 (clockwise on the
    // T-Encoder Pro, confirmed on hardware).
    static CORE_ALWAYS_INLINE int8_t transitionStep(uint8_t prev, uint8_t cur) {
        const uint32_t index = ((prev & 3u) << 2) | (cur & 3u);
        const uint32_t code = (detail::kQuadPackedTable >> (2u * index)) & 3u;
        return code == 3u ? int8_t{-1} : static_cast<int8_t>(code);
    }

private:
    static constexpr uint8_t kNoRest = 0xFF;

    static CORE_ALWAYS_INLINE bool isRest(uint8_t ab) { return ab == 0u || ab == 3u; }

    uint8_t prev_ = 0;
    uint8_t lastRest_ = kNoRest;
    bool reverse_;
};

}  // namespace core
