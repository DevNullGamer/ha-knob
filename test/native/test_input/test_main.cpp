// Native tests for the pure input logic (plan.md §4 Step 2 and §3.10 amendment).
#include <unity.h>

#include <cstdint>
#include <initializer_list>
#include <vector>

#include "input/Acceleration.h"
#include "input/ButtonFsm.h"
#include "input/QuadratureDecoder.h"
#include "input/SwipeClassifier.h"

using core::Acceleration;
using core::ButtonFsm;
using core::QuadratureDecoder;
using core::Swipe;
using core::SwipeClassifier;

void setUp() {}
void tearDown() {}

// ---------------------------------------------------------------------------
// QuadratureDecoder. AB encoding: bit 1 = A, bit 0 = B.
// Clockwise = 00 -> 10 -> 11 -> 01 -> 00 = positive; 2 valid transitions per detent.
// ---------------------------------------------------------------------------

namespace {
constexpr uint8_t S00 = 0;  // A=0 B=0 (rest)
constexpr uint8_t S01 = 1;  // A=0 B=1
constexpr uint8_t S10 = 2;  // A=1 B=0
constexpr uint8_t S11 = 3;  // A=1 B=1 (rest)

struct FeedResult {
    int sum = 0;
    int nonZero = 0;
    std::vector<int> outputs;
};

FeedResult feed(QuadratureDecoder &dec, std::initializer_list<uint8_t> states) {
    FeedResult r;
    for (uint8_t s : states) {
        const int out = dec.update(s);
        r.outputs.push_back(out);
        r.sum += out;
        if (out != 0) ++r.nonZero;
    }
    return r;
}
}  // namespace

static void test_decoder_clockwise_one_per_detent() {
    QuadratureDecoder dec;
    dec.reset(S00);
    // One full Gray cycle = 2 detents (rest at 11 and back at 00).
    FeedResult r = feed(dec, {S10, S11, S01, S00, S10, S11, S01, S00});
    TEST_ASSERT_EQUAL_INT(4, r.sum);
    TEST_ASSERT_EQUAL_INT(4, r.nonZero);
    // Detent is reported on arrival at a rest state, never at the half-way state.
    const int expected[] = {0, 1, 0, 1, 0, 1, 0, 1};
    TEST_ASSERT_EQUAL_INT_ARRAY(expected, r.outputs.data(), 8);
}

static void test_decoder_counter_clockwise_minus_one_per_detent() {
    QuadratureDecoder dec;
    dec.reset(S00);
    FeedResult r = feed(dec, {S01, S11, S10, S00, S01, S11, S10, S00});
    TEST_ASSERT_EQUAL_INT(-4, r.sum);
    TEST_ASSERT_EQUAL_INT(4, r.nonZero);
    const int expected[] = {0, -1, 0, -1, 0, -1, 0, -1};
    TEST_ASSERT_EQUAL_INT_ARRAY(expected, r.outputs.data(), 8);
}

static void test_decoder_clockwise_from_rest_11() {
    QuadratureDecoder dec;
    dec.reset(S11);
    FeedResult r = feed(dec, {S01, S00, S10, S11});
    TEST_ASSERT_EQUAL_INT(2, r.sum);
    TEST_ASSERT_EQUAL_INT(2, r.nonZero);
}

static void test_decoder_bounce_a_only_counts_nothing() {
    QuadratureDecoder dec;
    dec.reset(S00);
    // A toggles while B stays 0.
    FeedResult r = feed(dec, {S10, S00, S10, S00, S10, S00});
    TEST_ASSERT_EQUAL_INT(0, r.nonZero);
    // A toggles while B stays 1 (at the other rest state).
    dec.reset(S11);
    r = feed(dec, {S01, S11, S01, S11, S01, S11});
    TEST_ASSERT_EQUAL_INT(0, r.nonZero);
}

static void test_decoder_bounce_b_only_counts_nothing() {
    QuadratureDecoder dec;
    dec.reset(S00);
    FeedResult r = feed(dec, {S01, S00, S01, S00});
    TEST_ASSERT_EQUAL_INT(0, r.nonZero);
}

static void test_decoder_bounce_then_real_detent_counts_once() {
    QuadratureDecoder dec;
    dec.reset(S00);
    FeedResult r = feed(dec, {S10, S00, S10, S00, S10, S11});
    TEST_ASSERT_EQUAL_INT(1, r.sum);
    TEST_ASSERT_EQUAL_INT(1, r.nonZero);
}

static void test_decoder_repeated_same_state_counts_nothing() {
    QuadratureDecoder dec;
    dec.reset(S00);
    FeedResult r = feed(dec, {S00, S00, S00});
    TEST_ASSERT_EQUAL_INT(0, r.nonZero);
    r = feed(dec, {S10, S10, S11, S11, S11});
    TEST_ASSERT_EQUAL_INT(1, r.sum);
    TEST_ASSERT_EQUAL_INT(1, r.nonZero);
}

static void test_decoder_invalid_jump_00_to_11_ignored_and_phase_rejoined() {
    QuadratureDecoder dec;
    dec.reset(S00);
    TEST_ASSERT_EQUAL_INT(0, dec.update(S11));  // double-bit jump: ignored
    // Phase rejoined: the next clockwise detent from 11 counts exactly +1.
    FeedResult r = feed(dec, {S01, S00});
    TEST_ASSERT_EQUAL_INT(1, r.sum);
    TEST_ASSERT_EQUAL_INT(1, r.nonZero);
    // And counter-clockwise afterwards works too.
    r = feed(dec, {S01, S11});
    TEST_ASSERT_EQUAL_INT(-1, r.sum);
    TEST_ASSERT_EQUAL_INT(1, r.nonZero);
}

static void test_decoder_invalid_jump_11_to_00_ignored() {
    QuadratureDecoder dec;
    dec.reset(S11);
    TEST_ASSERT_EQUAL_INT(0, dec.update(S00));
    FeedResult r = feed(dec, {S10, S11});
    TEST_ASSERT_EQUAL_INT(1, r.sum);
    TEST_ASSERT_EQUAL_INT(1, r.nonZero);
}

static void test_decoder_invalid_jump_between_half_states_ignored() {
    QuadratureDecoder dec;
    dec.reset(S00);
    // 00 -> 10 (valid), 10 -> 01 (invalid), 01 -> 00 back to the old rest: no count.
    FeedResult r = feed(dec, {S10, S01, S00});
    TEST_ASSERT_EQUAL_INT(0, r.nonZero);
    r = feed(dec, {S10, S11});
    TEST_ASSERT_EQUAL_INT(1, r.sum);
    TEST_ASSERT_EQUAL_INT(1, r.nonZero);
}

static void test_decoder_direction_reversal_mid_detent() {
    QuadratureDecoder dec;
    dec.reset(S00);
    // Half a clockwise detent, back to rest, then a full counter-clockwise detent.
    FeedResult r = feed(dec, {S10, S00, S01, S11});
    TEST_ASSERT_EQUAL_INT(-1, r.sum);
    TEST_ASSERT_EQUAL_INT(1, r.nonZero);

    // Full clockwise detent, then half way on and back: only the first counts.
    dec.reset(S00);
    r = feed(dec, {S10, S11, S01, S11});
    TEST_ASSERT_EQUAL_INT(1, r.sum);
    TEST_ASSERT_EQUAL_INT(1, r.nonZero);

    // Clockwise detent followed directly by a counter-clockwise detent.
    dec.reset(S00);
    r = feed(dec, {S10, S11, S10, S00});
    const int expected[] = {0, 1, 0, -1};
    TEST_ASSERT_EQUAL_INT_ARRAY(expected, r.outputs.data(), 4);
}

static void test_decoder_reverse_flag_inverts_direction() {
    QuadratureDecoder dec(true);
    TEST_ASSERT_TRUE(dec.reverse());
    dec.reset(S00);
    FeedResult r = feed(dec, {S10, S11, S01, S00});
    TEST_ASSERT_EQUAL_INT(-2, r.sum);
    r = feed(dec, {S01, S11, S10, S00});
    TEST_ASSERT_EQUAL_INT(2, r.sum);

    dec.setReverse(false);
    TEST_ASSERT_FALSE(dec.reverse());
    r = feed(dec, {S10, S11});
    TEST_ASSERT_EQUAL_INT(1, r.sum);
}

static void test_decoder_transition_step_table() {
    // Valid clockwise steps.
    TEST_ASSERT_EQUAL_INT(1, QuadratureDecoder::transitionStep(S00, S10));
    TEST_ASSERT_EQUAL_INT(1, QuadratureDecoder::transitionStep(S10, S11));
    TEST_ASSERT_EQUAL_INT(1, QuadratureDecoder::transitionStep(S11, S01));
    TEST_ASSERT_EQUAL_INT(1, QuadratureDecoder::transitionStep(S01, S00));
    // Valid counter-clockwise steps.
    TEST_ASSERT_EQUAL_INT(-1, QuadratureDecoder::transitionStep(S00, S01));
    TEST_ASSERT_EQUAL_INT(-1, QuadratureDecoder::transitionStep(S01, S11));
    TEST_ASSERT_EQUAL_INT(-1, QuadratureDecoder::transitionStep(S11, S10));
    TEST_ASSERT_EQUAL_INT(-1, QuadratureDecoder::transitionStep(S10, S00));
    // No change and invalid double-bit jumps.
    for (uint8_t s = 0; s < 4; ++s) {
        TEST_ASSERT_EQUAL_INT(0, QuadratureDecoder::transitionStep(s, s));
    }
    TEST_ASSERT_EQUAL_INT(0, QuadratureDecoder::transitionStep(S00, S11));
    TEST_ASSERT_EQUAL_INT(0, QuadratureDecoder::transitionStep(S11, S00));
    TEST_ASSERT_EQUAL_INT(0, QuadratureDecoder::transitionStep(S01, S10));
    TEST_ASSERT_EQUAL_INT(0, QuadratureDecoder::transitionStep(S10, S01));
}

static void test_decoder_long_clockwise_spin() {
    QuadratureDecoder dec;
    dec.reset(S00);
    const uint8_t cycle[] = {S10, S11, S01, S00};
    int sum = 0;
    for (int i = 0; i < 4 * 100; ++i) sum += dec.update(cycle[i % 4]);
    TEST_ASSERT_EQUAL_INT(200, sum);  // 100 cycles = 200 detents
}

// ---------------------------------------------------------------------------
// ButtonFsm. Debounce 30 ms, long >= 600 ms, very long >= 10 s.
// ---------------------------------------------------------------------------

namespace {
struct TimedEvent {
    ButtonFsm::Event event;
    uint32_t ms;
};

// Feeds a constant raw level every 1 ms for t in [from, to). Appends non-None events.
void drive(ButtonFsm &fsm, bool raw, uint32_t from, uint32_t to, std::vector<TimedEvent> &out) {
    for (uint32_t t = from; t < to; ++t) {
        const ButtonFsm::Event e = fsm.update(raw, t);
        if (e != ButtonFsm::Event::None) out.push_back({e, t});
    }
}

int countOf(const std::vector<TimedEvent> &events, ButtonFsm::Event e) {
    int n = 0;
    for (const auto &te : events) {
        if (te.event == e) ++n;
    }
    return n;
}

const TimedEvent *firstOf(const std::vector<TimedEvent> &events, ButtonFsm::Event e) {
    for (const auto &te : events) {
        if (te.event == e) return &te;
    }
    return nullptr;
}
}  // namespace

static void test_button_idle_produces_nothing() {
    ButtonFsm fsm;
    std::vector<TimedEvent> ev;
    drive(fsm, false, 0, 2000, ev);
    TEST_ASSERT_EQUAL_size_t(0, ev.size());
    TEST_ASSERT_FALSE(fsm.isDown());
}

static void test_button_press_shorter_than_debounce_ignored() {
    ButtonFsm fsm;
    std::vector<TimedEvent> ev;
    drive(fsm, false, 0, 100, ev);
    drive(fsm, true, 100, 129, ev);  // 29 ms glitch
    drive(fsm, false, 129, 1000, ev);
    TEST_ASSERT_EQUAL_size_t(0, ev.size());
    TEST_ASSERT_FALSE(fsm.isDown());
}

static void test_button_short_press_100ms_fires_short_on_release() {
    ButtonFsm fsm;
    std::vector<TimedEvent> ev;
    drive(fsm, false, 0, 100, ev);
    drive(fsm, true, 100, 200, ev);  // 100 ms press
    // ShortPress must not be emitted while still held.
    TEST_ASSERT_EQUAL_INT(0, countOf(ev, ButtonFsm::Event::ShortPress));
    TEST_ASSERT_EQUAL_INT(1, countOf(ev, ButtonFsm::Event::Pressed));
    TEST_ASSERT_TRUE(fsm.isDown());
    drive(fsm, false, 200, 1000, ev);
    TEST_ASSERT_EQUAL_INT(1, countOf(ev, ButtonFsm::Event::ShortPress));
    TEST_ASSERT_EQUAL_INT(0, countOf(ev, ButtonFsm::Event::LongPress));
    TEST_ASSERT_EQUAL_INT(0, countOf(ev, ButtonFsm::Event::VeryLongPress));
    TEST_ASSERT_EQUAL_INT(0, countOf(ev, ButtonFsm::Event::Released));
    const TimedEvent *s = firstOf(ev, ButtonFsm::Event::ShortPress);
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_TRUE(s->ms >= 200);
    TEST_ASSERT_TRUE(s->ms <= 200 + ButtonFsm::kDebounceMs);
    TEST_ASSERT_FALSE(fsm.isDown());
}

static void test_button_press_just_above_debounce_is_short_press() {
    ButtonFsm fsm;
    std::vector<TimedEvent> ev;
    drive(fsm, true, 0, 40, ev);
    drive(fsm, false, 40, 200, ev);
    TEST_ASSERT_EQUAL_INT(1, countOf(ev, ButtonFsm::Event::Pressed));
    TEST_ASSERT_EQUAL_INT(1, countOf(ev, ButtonFsm::Event::ShortPress));
}

static void test_button_press_just_below_long_is_short_press() {
    ButtonFsm fsm;
    std::vector<TimedEvent> ev;
    drive(fsm, true, 0, 590, ev);
    drive(fsm, false, 590, 1500, ev);
    TEST_ASSERT_EQUAL_INT(0, countOf(ev, ButtonFsm::Event::LongPress));
    TEST_ASSERT_EQUAL_INT(1, countOf(ev, ButtonFsm::Event::ShortPress));
}

static void test_button_700ms_long_at_600_and_no_short_on_release() {
    ButtonFsm fsm;
    std::vector<TimedEvent> ev;
    drive(fsm, false, 0, 1000, ev);
    drive(fsm, true, 1000, 1700, ev);  // 700 ms press starting at t=1000
    const TimedEvent *lp = firstOf(ev, ButtonFsm::Event::LongPress);
    TEST_ASSERT_NOT_NULL_MESSAGE(lp, "LongPress not fired while held");
    TEST_ASSERT_EQUAL_INT(1, countOf(ev, ButtonFsm::Event::LongPress));
    // Fired at 600 ms held (measured from the press edge), never before.
    TEST_ASSERT_EQUAL_UINT32(1600, lp->ms);
    drive(fsm, false, 1700, 3000, ev);
    TEST_ASSERT_EQUAL_INT(0, countOf(ev, ButtonFsm::Event::ShortPress));
    TEST_ASSERT_EQUAL_INT(1, countOf(ev, ButtonFsm::Event::LongPress));
    TEST_ASSERT_EQUAL_INT(1, countOf(ev, ButtonFsm::Event::Released));
    TEST_ASSERT_EQUAL_INT(0, countOf(ev, ButtonFsm::Event::VeryLongPress));
    TEST_ASSERT_FALSE(fsm.isDown());
}

static void test_button_10s_hold_very_long_exactly_once() {
    ButtonFsm fsm;
    std::vector<TimedEvent> ev;
    drive(fsm, true, 0, 15000, ev);  // hold 15 s
    TEST_ASSERT_EQUAL_INT(1, countOf(ev, ButtonFsm::Event::Pressed));
    TEST_ASSERT_EQUAL_INT(1, countOf(ev, ButtonFsm::Event::LongPress));
    TEST_ASSERT_EQUAL_INT(1, countOf(ev, ButtonFsm::Event::VeryLongPress));
    const TimedEvent *vl = firstOf(ev, ButtonFsm::Event::VeryLongPress);
    TEST_ASSERT_NOT_NULL(vl);
    TEST_ASSERT_TRUE(vl->ms >= ButtonFsm::kVeryLongPressMs);
    TEST_ASSERT_TRUE(vl->ms <= ButtonFsm::kVeryLongPressMs + 1);
    drive(fsm, false, 15000, 16000, ev);
    TEST_ASSERT_EQUAL_INT(0, countOf(ev, ButtonFsm::Event::ShortPress));
    TEST_ASSERT_EQUAL_INT(1, countOf(ev, ButtonFsm::Event::VeryLongPress));
    TEST_ASSERT_EQUAL_INT(1, countOf(ev, ButtonFsm::Event::Released));
}

static void test_button_9990ms_hold_no_very_long() {
    ButtonFsm fsm;
    std::vector<TimedEvent> ev;
    drive(fsm, true, 0, 9990, ev);
    drive(fsm, false, 9990, 11000, ev);
    TEST_ASSERT_EQUAL_INT(0, countOf(ev, ButtonFsm::Event::VeryLongPress));
    TEST_ASSERT_EQUAL_INT(1, countOf(ev, ButtonFsm::Event::LongPress));
}

static void test_button_bounce_while_pressed_does_not_retrigger() {
    ButtonFsm fsm;
    std::vector<TimedEvent> ev;
    drive(fsm, true, 0, 100, ev);
    // Short (< 30 ms) open-contact glitches while held.
    for (uint32_t t = 100; t < 300; t += 20) {
        drive(fsm, false, t, t + 5, ev);
        drive(fsm, true, t + 5, t + 20, ev);
    }
    drive(fsm, true, 300, 400, ev);
    drive(fsm, false, 400, 800, ev);
    TEST_ASSERT_EQUAL_INT(1, countOf(ev, ButtonFsm::Event::Pressed));
    TEST_ASSERT_EQUAL_INT(1, countOf(ev, ButtonFsm::Event::ShortPress));
    TEST_ASSERT_EQUAL_INT(0, countOf(ev, ButtonFsm::Event::LongPress));
}

static void test_button_bounce_during_long_hold_single_long() {
    ButtonFsm fsm;
    std::vector<TimedEvent> ev;
    drive(fsm, true, 0, 700, ev);
    drive(fsm, false, 700, 710, ev);  // glitch
    drive(fsm, true, 710, 1500, ev);
    drive(fsm, false, 1500, 2000, ev);
    TEST_ASSERT_EQUAL_INT(1, countOf(ev, ButtonFsm::Event::Pressed));
    TEST_ASSERT_EQUAL_INT(1, countOf(ev, ButtonFsm::Event::LongPress));
    TEST_ASSERT_EQUAL_INT(0, countOf(ev, ButtonFsm::Event::ShortPress));
    TEST_ASSERT_EQUAL_INT(1, countOf(ev, ButtonFsm::Event::Released));
}

static void test_button_bounce_on_press_edge_single_press() {
    ButtonFsm fsm;
    std::vector<TimedEvent> ev;
    // Contact chatter at the press edge, then stable down.
    drive(fsm, true, 0, 3, ev);
    drive(fsm, false, 3, 6, ev);
    drive(fsm, true, 6, 9, ev);
    drive(fsm, false, 9, 12, ev);
    drive(fsm, true, 12, 200, ev);
    // Chatter at release.
    drive(fsm, false, 200, 203, ev);
    drive(fsm, true, 203, 206, ev);
    drive(fsm, false, 206, 600, ev);
    TEST_ASSERT_EQUAL_INT(1, countOf(ev, ButtonFsm::Event::Pressed));
    TEST_ASSERT_EQUAL_INT(1, countOf(ev, ButtonFsm::Event::ShortPress));
}

static void test_button_two_consecutive_presses() {
    ButtonFsm fsm;
    std::vector<TimedEvent> ev;
    drive(fsm, true, 0, 100, ev);
    drive(fsm, false, 100, 300, ev);
    drive(fsm, true, 300, 1000, ev);  // second press is long
    drive(fsm, false, 1000, 1200, ev);
    TEST_ASSERT_EQUAL_INT(2, countOf(ev, ButtonFsm::Event::Pressed));
    TEST_ASSERT_EQUAL_INT(1, countOf(ev, ButtonFsm::Event::ShortPress));
    TEST_ASSERT_EQUAL_INT(1, countOf(ev, ButtonFsm::Event::LongPress));
    TEST_ASSERT_EQUAL_INT(1, countOf(ev, ButtonFsm::Event::Released));
}

static void test_button_millis_wraparound() {
    ButtonFsm fsm;
    std::vector<TimedEvent> ev;
    const uint32_t base = 0xFFFFFF00u;  // millis() wraps during the press
    for (uint32_t i = 0; i < 100; ++i) {
        const auto e = fsm.update(false, base + i);
        if (e != ButtonFsm::Event::None) ev.push_back({e, base + i});
    }
    for (uint32_t i = 100; i < 800; ++i) {
        const auto e = fsm.update(true, base + i);
        if (e != ButtonFsm::Event::None) ev.push_back({e, base + i});
    }
    TEST_ASSERT_EQUAL_INT(1, countOf(ev, ButtonFsm::Event::Pressed));
    TEST_ASSERT_EQUAL_INT(1, countOf(ev, ButtonFsm::Event::LongPress));
    const TimedEvent *lp = firstOf(ev, ButtonFsm::Event::LongPress);
    TEST_ASSERT_EQUAL_UINT32(base + 700, lp->ms);
}

// Regression (Step 2 round 1): a release inside the 30 ms debounce window right before the
// 600 ms / 10 s marks must not turn into LongPress / VeryLongPress.
static void test_button_release_599ms_is_short_not_long() {
    ButtonFsm fsm;
    std::vector<TimedEvent> ev;
    drive(fsm, true, 0, 599, ev);  // raw down for 599 ms
    drive(fsm, false, 599, 1500, ev);
    TEST_ASSERT_EQUAL_INT(0, countOf(ev, ButtonFsm::Event::LongPress));
    TEST_ASSERT_EQUAL_INT(1, countOf(ev, ButtonFsm::Event::ShortPress));
    TEST_ASSERT_EQUAL_INT(0, countOf(ev, ButtonFsm::Event::Released));
}

static void test_button_release_600ms_is_long() {
    ButtonFsm fsm;
    std::vector<TimedEvent> ev;
    drive(fsm, true, 0, 601, ev);  // raw down at t=600 -> held 600 ms
    drive(fsm, false, 601, 1500, ev);
    TEST_ASSERT_EQUAL_INT(1, countOf(ev, ButtonFsm::Event::LongPress));
    TEST_ASSERT_EQUAL_UINT32(600, firstOf(ev, ButtonFsm::Event::LongPress)->ms);
    TEST_ASSERT_EQUAL_INT(0, countOf(ev, ButtonFsm::Event::ShortPress));
    TEST_ASSERT_EQUAL_INT(1, countOf(ev, ButtonFsm::Event::Released));
}

static void test_button_chatter_across_600ms_delays_long_fires_once() {
    ButtonFsm fsm;
    std::vector<TimedEvent> ev;
    drive(fsm, true, 0, 595, ev);
    drive(fsm, false, 595, 605, ev);  // 10 ms contact chatter across the 600 ms mark
    TEST_ASSERT_EQUAL_INT(0, countOf(ev, ButtonFsm::Event::LongPress));
    drive(fsm, true, 605, 1200, ev);  // contact recloses, real hold continues
    TEST_ASSERT_EQUAL_INT(1, countOf(ev, ButtonFsm::Event::LongPress));
    TEST_ASSERT_EQUAL_UINT32(605, firstOf(ev, ButtonFsm::Event::LongPress)->ms);
    drive(fsm, false, 1200, 1500, ev);
    TEST_ASSERT_EQUAL_INT(1, countOf(ev, ButtonFsm::Event::Pressed));
    TEST_ASSERT_EQUAL_INT(1, countOf(ev, ButtonFsm::Event::LongPress));
    TEST_ASSERT_EQUAL_INT(0, countOf(ev, ButtonFsm::Event::ShortPress));
    TEST_ASSERT_EQUAL_INT(1, countOf(ev, ButtonFsm::Event::Released));
}

static void test_button_repeated_chatter_after_long_no_refire() {
    ButtonFsm fsm;
    std::vector<TimedEvent> ev;
    drive(fsm, true, 0, 700, ev);
    for (uint32_t t = 700; t < 1500; t += 50) {
        drive(fsm, false, t, t + 10, ev);
        drive(fsm, true, t + 10, t + 50, ev);
    }
    drive(fsm, false, 1500, 1800, ev);
    TEST_ASSERT_EQUAL_INT(1, countOf(ev, ButtonFsm::Event::Pressed));
    TEST_ASSERT_EQUAL_INT(1, countOf(ev, ButtonFsm::Event::LongPress));
    TEST_ASSERT_EQUAL_INT(0, countOf(ev, ButtonFsm::Event::ShortPress));
    TEST_ASSERT_EQUAL_INT(1, countOf(ev, ButtonFsm::Event::Released));
}

static void test_button_chatter_across_10s_delays_very_long_fires_once() {
    ButtonFsm fsm;
    std::vector<TimedEvent> ev;
    drive(fsm, true, 0, 9995, ev);
    drive(fsm, false, 9995, 10005, ev);  // chatter across the 10 s mark
    TEST_ASSERT_EQUAL_INT(0, countOf(ev, ButtonFsm::Event::VeryLongPress));
    drive(fsm, true, 10005, 12000, ev);
    TEST_ASSERT_EQUAL_INT(1, countOf(ev, ButtonFsm::Event::VeryLongPress));
    TEST_ASSERT_EQUAL_UINT32(10005, firstOf(ev, ButtonFsm::Event::VeryLongPress)->ms);
    drive(fsm, false, 12000, 12500, ev);
    TEST_ASSERT_EQUAL_INT(1, countOf(ev, ButtonFsm::Event::Pressed));
    TEST_ASSERT_EQUAL_INT(1, countOf(ev, ButtonFsm::Event::LongPress));
    TEST_ASSERT_EQUAL_INT(1, countOf(ev, ButtonFsm::Event::VeryLongPress));
    TEST_ASSERT_EQUAL_INT(0, countOf(ev, ButtonFsm::Event::ShortPress));
    TEST_ASSERT_EQUAL_INT(1, countOf(ev, ButtonFsm::Event::Released));
}

static void test_button_release_just_before_10s_no_very_long() {
    ButtonFsm fsm;
    std::vector<TimedEvent> ev;
    drive(fsm, true, 0, 9999, ev);
    drive(fsm, false, 9999, 11000, ev);
    TEST_ASSERT_EQUAL_INT(0, countOf(ev, ButtonFsm::Event::VeryLongPress));
    TEST_ASSERT_EQUAL_INT(1, countOf(ev, ButtonFsm::Event::Released));
}

static void test_button_event_names() {
    TEST_ASSERT_EQUAL_STRING("short", core::buttonEventName(ButtonFsm::Event::ShortPress));
    TEST_ASSERT_EQUAL_STRING("long", core::buttonEventName(ButtonFsm::Event::LongPress));
    TEST_ASSERT_EQUAL_STRING("verylong", core::buttonEventName(ButtonFsm::Event::VeryLongPress));
}

// ---------------------------------------------------------------------------
// Acceleration: < 40 ms -> x4, < 80 ms -> x2, else x1 (plan §3.5 text after table).
// ---------------------------------------------------------------------------

static void test_accel_first_detent_is_one() {
    Acceleration acc;
    TEST_ASSERT_EQUAL_INT(1, acc.onDetent(1000));
}

static void test_accel_intervals_30_60_200() {
    Acceleration acc;
    TEST_ASSERT_EQUAL_INT(1, acc.onDetent(1000));
    TEST_ASSERT_EQUAL_INT(4, acc.onDetent(1030));
    TEST_ASSERT_EQUAL_INT(2, acc.onDetent(1090));
    TEST_ASSERT_EQUAL_INT(1, acc.onDetent(1290));
}

static void test_accel_threshold_boundaries() {
    Acceleration acc;
    acc.onDetent(0);
    TEST_ASSERT_EQUAL_INT(4, acc.onDetent(39));    // 39 < 40
    TEST_ASSERT_EQUAL_INT(2, acc.onDetent(79));    // 40 -> not < 40
    TEST_ASSERT_EQUAL_INT(2, acc.onDetent(158));   // 79 < 80
    TEST_ASSERT_EQUAL_INT(1, acc.onDetent(238));   // 80 -> not < 80
    TEST_ASSERT_EQUAL_INT(4, acc.onDetent(238));   // 0 ms (same tick)
}

static void test_accel_disabled_always_one() {
    Acceleration acc;
    acc.setEnabled(false);
    TEST_ASSERT_FALSE(acc.enabled());
    TEST_ASSERT_EQUAL_INT(1, acc.onDetent(1000));
    TEST_ASSERT_EQUAL_INT(1, acc.onDetent(1001));
    TEST_ASSERT_EQUAL_INT(1, acc.onDetent(1030));
    TEST_ASSERT_EQUAL_INT(1, acc.onDetent(1090));
    TEST_ASSERT_EQUAL_INT(1, acc.onDetent(1290));
    acc.setEnabled(true);
    TEST_ASSERT_EQUAL_INT(4, acc.onDetent(1300));
}

static void test_accel_reset_makes_next_one() {
    Acceleration acc;
    acc.onDetent(1000);
    acc.reset();
    TEST_ASSERT_EQUAL_INT(1, acc.onDetent(1010));
    TEST_ASSERT_EQUAL_INT(4, acc.onDetent(1020));
}

static void test_accel_millis_wraparound() {
    Acceleration acc;
    acc.onDetent(0xFFFFFFF0u);
    TEST_ASSERT_EQUAL_INT(4, acc.onDetent(0x0000000Eu));  // 30 ms later, wrapped
}

// onDetents (plan-step7.md Q7-2): detents spread evenly over the time since the last call.
static void test_accel_ondetents_first_call_is_x1() {
    Acceleration acc;
    TEST_ASSERT_EQUAL_INT(3, acc.onDetents(3, 1000));
}

static void test_accel_ondetents_batches() {
    Acceleration acc;
    acc.onDetents(1, 1000);
    TEST_ASSERT_EQUAL_INT(12, acc.onDetents(3, 1030));    // 10 ms each
    TEST_ASSERT_EQUAL_INT(8, acc.onDetents(4, 1270));     // 240 ms / 4 = 60 ms each
    TEST_ASSERT_EQUAL_INT(4, acc.onDetents(4, 1590));     // 320 ms / 4 = 80 ms each
    TEST_ASSERT_EQUAL_INT(-12, acc.onDetents(-3, 1620));  // 30 ms / 3
}

static void test_accel_ondetents_zero_changes_nothing() {
    Acceleration acc;
    acc.onDetent(1000);
    TEST_ASSERT_EQUAL_INT(0, acc.onDetents(0, 1020));
    TEST_ASSERT_EQUAL_INT(4, acc.onDetent(1030));  // still 30 ms after 1000
    Acceleration fresh;
    TEST_ASSERT_EQUAL_INT(0, fresh.onDetents(0, 500));
    TEST_ASSERT_EQUAL_INT(2, fresh.onDetents(2, 510));  // zero call did not set a timestamp
}

static void test_accel_ondetents_single_detent_boundaries() {
    Acceleration acc;
    acc.onDetents(1, 0);
    TEST_ASSERT_EQUAL_INT(4, acc.onDetents(1, 39));
    TEST_ASSERT_EQUAL_INT(2, acc.onDetents(1, 79));    // 40 ms
    TEST_ASSERT_EQUAL_INT(1, acc.onDetents(1, 159));   // 80 ms
    TEST_ASSERT_EQUAL_INT(-4, acc.onDetents(-1, 160));
}

static void test_accel_ondetents_batch_boundary_x2() {
    Acceleration acc;
    acc.onDetents(1, 0);
    TEST_ASSERT_EQUAL_INT(12, acc.onDetents(3, 119));  // 119 < 120
    TEST_ASSERT_EQUAL_INT(6, acc.onDetents(3, 239));   // exactly 120: not x4
    TEST_ASSERT_EQUAL_INT(6, acc.onDetents(3, 478));   // 239 < 240
    TEST_ASSERT_EQUAL_INT(3, acc.onDetents(3, 718));   // exactly 240: x1
}

static void test_accel_ondetents_disabled_reset_and_mixed() {
    Acceleration acc;
    acc.setEnabled(false);
    acc.onDetents(1, 0);
    TEST_ASSERT_EQUAL_INT(3, acc.onDetents(3, 10));
    TEST_ASSERT_EQUAL_INT(-3, acc.onDetents(-3, 20));
    acc.setEnabled(true);
    TEST_ASSERT_EQUAL_INT(12, acc.onDetents(3, 30));
    acc.reset();
    TEST_ASSERT_EQUAL_INT(3, acc.onDetents(3, 40));
    // mixed: onDetent and onDetents share the timestamp
    acc.reset();
    acc.onDetent(1000);
    TEST_ASSERT_EQUAL_INT(8, acc.onDetents(2, 1060));  // 60 ms / 2
    TEST_ASSERT_EQUAL_INT(4, acc.onDetent(1090));      // 30 ms after the batch
    TEST_ASSERT_EQUAL_INT(8, acc.onDetents(2, 1150));  // 60 ms / 2
}

static void test_accel_ondetents_millis_wraparound() {
    Acceleration acc;
    acc.onDetents(1, 0xFFFFFFF0u);
    TEST_ASSERT_EQUAL_INT(8, acc.onDetents(2, 0x0000000Eu));  // 30 ms / 2
}

// ---------------------------------------------------------------------------
// SwipeClassifier: >= 80 px horizontal, <= 700 ms, |dx| >= 2 |dy|. Taps never act.
// ---------------------------------------------------------------------------

namespace {
// Touch down at (x0,y0) at t0, linearly move to (x1,y1) by t0+durMs, lift at t0+durMs.
Swipe stroke(SwipeClassifier &sc, int x0, int y0, int x1, int y1, uint32_t t0, uint32_t durMs,
             int steps = 10) {
    for (int i = 0; i <= steps; ++i) {
        const int x = x0 + (x1 - x0) * i / steps;
        const int y = y0 + (y1 - y0) * i / steps;
        const uint32_t t = t0 + durMs * static_cast<uint32_t>(i) / static_cast<uint32_t>(steps);
        const Swipe s = sc.update(true, static_cast<int16_t>(x), static_cast<int16_t>(y), t);
        TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(Swipe::None), static_cast<int>(s),
                                      "swipe reported while still touching");
    }
    return sc.update(false, 0, 0, t0 + durMs);
}
}  // namespace

static void test_swipe_left() {
    SwipeClassifier sc;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Swipe::Left),
                          static_cast<int>(stroke(sc, 300, 200, 100, 210, 0, 250)));
}

static void test_swipe_right() {
    SwipeClassifier sc;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Swipe::Right),
                          static_cast<int>(stroke(sc, 100, 200, 300, 190, 0, 250)));
}

static void test_swipe_tap_is_none() {
    SwipeClassifier sc;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Swipe::None),
                          static_cast<int>(stroke(sc, 195, 195, 195, 195, 0, 80)));
    // Tap with small jitter.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Swipe::None),
                          static_cast<int>(stroke(sc, 195, 195, 201, 192, 1000, 120)));
    // Long press on the screen (the knob cap being pressed): still none.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Swipe::None),
                          static_cast<int>(stroke(sc, 195, 195, 198, 197, 2000, 1500)));
}

static void test_swipe_too_slow_is_none() {
    SwipeClassifier sc;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Swipe::None),
                          static_cast<int>(stroke(sc, 300, 200, 100, 200, 0, 1200)));
}

static void test_swipe_too_vertical_is_none() {
    SwipeClassifier sc;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Swipe::None),
                          static_cast<int>(stroke(sc, 195, 50, 195, 340, 0, 250)));
    // Diagonal: |dx| = 150, |dy| = 100 -> 150 < 200.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Swipe::None),
                          static_cast<int>(stroke(sc, 100, 100, 250, 200, 1000, 250)));
}

static void test_swipe_distance_boundary() {
    SwipeClassifier sc;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Swipe::Right),
                          static_cast<int>(stroke(sc, 100, 200, 180, 200, 0, 200)));  // 80 px
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Swipe::None),
                          static_cast<int>(stroke(sc, 100, 200, 179, 200, 1000, 200)));  // 79
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Swipe::Left),
                          static_cast<int>(stroke(sc, 180, 200, 100, 200, 2000, 200)));  // -80
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Swipe::None),
                          static_cast<int>(stroke(sc, 179, 200, 100, 200, 3000, 200)));  // -79
}

static void test_swipe_duration_boundary() {
    SwipeClassifier sc;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Swipe::Right),
                          static_cast<int>(stroke(sc, 50, 200, 300, 200, 0, 700)));  // 700 ms
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Swipe::None),
                          static_cast<int>(stroke(sc, 50, 200, 300, 200, 1000, 701)));  // 701
}

static void test_swipe_axis_ratio_boundary() {
    SwipeClassifier sc;
    // |dx| = 100, |dy| = 50 -> exactly 2:1 -> swipe.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Swipe::Right),
                          static_cast<int>(stroke(sc, 100, 100, 200, 150, 0, 200)));
    // |dx| = 100, |dy| = 51 -> just too vertical.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Swipe::None),
                          static_cast<int>(stroke(sc, 100, 100, 200, 151, 1000, 200)));
    // Same, upward.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Swipe::Left),
                          static_cast<int>(stroke(sc, 200, 150, 100, 100, 2000, 200)));
}

static void test_swipe_full_screen_edges() {
    SwipeClassifier sc;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Swipe::Right),
                          static_cast<int>(stroke(sc, 0, 195, 389, 195, 0, 300)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Swipe::Left),
                          static_cast<int>(stroke(sc, 389, 195, 0, 195, 1000, 300)));
}

static void test_swipe_untouched_polls_are_none_and_state_resets() {
    SwipeClassifier sc;
    for (uint32_t t = 0; t < 100; t += 10) {
        TEST_ASSERT_EQUAL_INT(static_cast<int>(Swipe::None),
                              static_cast<int>(sc.update(false, 0, 0, t)));
    }
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Swipe::Left),
                          static_cast<int>(stroke(sc, 300, 200, 100, 200, 200, 200)));
    // Lift is reported once only.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Swipe::None),
                          static_cast<int>(sc.update(false, 0, 0, 500)));
    // A following tap does not inherit the previous stroke's start.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Swipe::None),
                          static_cast<int>(stroke(sc, 100, 200, 102, 200, 600, 80)));
}

static void test_swipe_uses_last_point_not_max_excursion() {
    SwipeClassifier sc;
    // Move right 150 px, then back to the start before lifting: net ~0 -> none.
    sc.update(true, 100, 200, 0);
    sc.update(true, 250, 200, 100);
    sc.update(true, 105, 200, 200);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Swipe::None),
                          static_cast<int>(sc.update(false, 0, 0, 250)));
}

static void test_swipe_custom_config() {
    SwipeClassifier::Config cfg;
    cfg.minDistancePx = 40;
    cfg.maxDurationMs = 300;
    cfg.minAxisRatio = 1;
    SwipeClassifier sc(cfg);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Swipe::Right),
                          static_cast<int>(stroke(sc, 100, 100, 140, 140, 0, 300)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Swipe::None),
                          static_cast<int>(stroke(sc, 100, 100, 139, 100, 1000, 100)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Swipe::None),
                          static_cast<int>(stroke(sc, 100, 100, 200, 100, 2000, 301)));
}

static void test_swipe_default_config_values() {
    SwipeClassifier::Config cfg;
    TEST_ASSERT_EQUAL_INT32(80, cfg.minDistancePx);
    TEST_ASSERT_EQUAL_UINT32(700, cfg.maxDurationMs);
    TEST_ASSERT_EQUAL_INT32(2, cfg.minAxisRatio);
}

static void test_swipe_names() {
    TEST_ASSERT_EQUAL_STRING("left", core::swipeName(Swipe::Left));
    TEST_ASSERT_EQUAL_STRING("right", core::swipeName(Swipe::Right));
    TEST_ASSERT_EQUAL_STRING("none", core::swipeName(Swipe::None));
}

int main() {
    UNITY_BEGIN();
    // QuadratureDecoder
    RUN_TEST(test_decoder_clockwise_one_per_detent);
    RUN_TEST(test_decoder_counter_clockwise_minus_one_per_detent);
    RUN_TEST(test_decoder_clockwise_from_rest_11);
    RUN_TEST(test_decoder_bounce_a_only_counts_nothing);
    RUN_TEST(test_decoder_bounce_b_only_counts_nothing);
    RUN_TEST(test_decoder_bounce_then_real_detent_counts_once);
    RUN_TEST(test_decoder_repeated_same_state_counts_nothing);
    RUN_TEST(test_decoder_invalid_jump_00_to_11_ignored_and_phase_rejoined);
    RUN_TEST(test_decoder_invalid_jump_11_to_00_ignored);
    RUN_TEST(test_decoder_invalid_jump_between_half_states_ignored);
    RUN_TEST(test_decoder_direction_reversal_mid_detent);
    RUN_TEST(test_decoder_reverse_flag_inverts_direction);
    RUN_TEST(test_decoder_transition_step_table);
    RUN_TEST(test_decoder_long_clockwise_spin);
    // ButtonFsm
    RUN_TEST(test_button_idle_produces_nothing);
    RUN_TEST(test_button_press_shorter_than_debounce_ignored);
    RUN_TEST(test_button_short_press_100ms_fires_short_on_release);
    RUN_TEST(test_button_press_just_above_debounce_is_short_press);
    RUN_TEST(test_button_press_just_below_long_is_short_press);
    RUN_TEST(test_button_700ms_long_at_600_and_no_short_on_release);
    RUN_TEST(test_button_10s_hold_very_long_exactly_once);
    RUN_TEST(test_button_9990ms_hold_no_very_long);
    RUN_TEST(test_button_bounce_while_pressed_does_not_retrigger);
    RUN_TEST(test_button_bounce_during_long_hold_single_long);
    RUN_TEST(test_button_bounce_on_press_edge_single_press);
    RUN_TEST(test_button_two_consecutive_presses);
    RUN_TEST(test_button_millis_wraparound);
    RUN_TEST(test_button_release_599ms_is_short_not_long);
    RUN_TEST(test_button_release_600ms_is_long);
    RUN_TEST(test_button_chatter_across_600ms_delays_long_fires_once);
    RUN_TEST(test_button_repeated_chatter_after_long_no_refire);
    RUN_TEST(test_button_chatter_across_10s_delays_very_long_fires_once);
    RUN_TEST(test_button_release_just_before_10s_no_very_long);
    RUN_TEST(test_button_event_names);
    // Acceleration
    RUN_TEST(test_accel_first_detent_is_one);
    RUN_TEST(test_accel_intervals_30_60_200);
    RUN_TEST(test_accel_threshold_boundaries);
    RUN_TEST(test_accel_disabled_always_one);
    RUN_TEST(test_accel_reset_makes_next_one);
    RUN_TEST(test_accel_millis_wraparound);
    RUN_TEST(test_accel_ondetents_first_call_is_x1);
    RUN_TEST(test_accel_ondetents_batches);
    RUN_TEST(test_accel_ondetents_zero_changes_nothing);
    RUN_TEST(test_accel_ondetents_single_detent_boundaries);
    RUN_TEST(test_accel_ondetents_batch_boundary_x2);
    RUN_TEST(test_accel_ondetents_disabled_reset_and_mixed);
    RUN_TEST(test_accel_ondetents_millis_wraparound);
    // SwipeClassifier
    RUN_TEST(test_swipe_left);
    RUN_TEST(test_swipe_right);
    RUN_TEST(test_swipe_tap_is_none);
    RUN_TEST(test_swipe_too_slow_is_none);
    RUN_TEST(test_swipe_too_vertical_is_none);
    RUN_TEST(test_swipe_distance_boundary);
    RUN_TEST(test_swipe_duration_boundary);
    RUN_TEST(test_swipe_axis_ratio_boundary);
    RUN_TEST(test_swipe_full_screen_edges);
    RUN_TEST(test_swipe_untouched_polls_are_none_and_state_resets);
    RUN_TEST(test_swipe_uses_last_point_not_max_excursion);
    RUN_TEST(test_swipe_custom_config);
    RUN_TEST(test_swipe_default_config_values);
    RUN_TEST(test_swipe_names);
    return UNITY_END();
}
