// HA Knob - Step 1 hardware smoke test.
//
// Brings up display (Arduino_GFX CO5300 + LVGL 9), touch (SensorLib, variant probe),
// rotary encoder, push button and buzzer, and logs everything on USB serial:
//   BOOT fw=<v> psram=<bytes> flash=<bytes> touch=<CST816|CHSC5816|none> heap=<bytes>
//   ENC d=<+1|-1> count=<n>
//   ENCRAW ab=<prev>-><cur> step=<+1|-1|0> dt=<us since previous transition>
//   BTN down / BTN up
//   TOUCH x=<x> y=<y>
//   HB up=<s> psram=<bytes> flash=<bytes> touch=<..> count=<n>   (every 5 s)
// This file is replaced by the real module structure in Step 2.

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <TouchDrvCHSC5816.hpp>
#include <TouchDrvCSTXXX.hpp>
#include <Wire.h>
#include <atomic>
#include <esp_heap_caps.h>
#include <lvgl.h>

#include "board_pins.h"
#include "util/MathUtil.h"
#include "version.h"

namespace {

// ---------------------------------------------------------------------------
// Tunables
// ---------------------------------------------------------------------------

// Quadrature transitions per mechanical detent. The vendor decoder implies 2
// (detents rest at both 00 and 11); confirm on hardware with the ENCRAW log.
constexpr int kTransitionsPerDetent = 2;
constexpr uint32_t kButtonDebounceMs = 30;
constexpr uint32_t kBeepMs = 30;
constexpr uint32_t kBeepFreqHz = 2000;
constexpr uint8_t kBuzzerLedcChannel = 0;
constexpr uint32_t kTouchPollMs = 30;
constexpr uint32_t kTouchLogMinIntervalMs = 100;
constexpr uint32_t kHeartbeatMs = 5000;
constexpr int kDrawBufLines = 40;

// ---------------------------------------------------------------------------
// Display
// ---------------------------------------------------------------------------

Arduino_DataBus *bus = new Arduino_ESP32QSPI(pins::kLcdCs, pins::kLcdSclk, pins::kLcdSdio0,
                                             pins::kLcdSdio1, pins::kLcdSdio2, pins::kLcdSdio3);
Arduino_CO5300 *gfx = new Arduino_CO5300(bus, pins::kLcdRst, 0 /* rotation */, false /* ips */,
                                         pins::kLcdWidth, pins::kLcdHeight, 0, 0, 0, 0);

void flushCb(lv_display_t *disp, const lv_area_t *area, uint8_t *pxMap) {
    const int32_t w = lv_area_get_width(area);
    const int32_t h = lv_area_get_height(area);
    gfx->draw16bitRGBBitmap(area->x1, area->y1, reinterpret_cast<uint16_t *>(pxMap), w, h);
    lv_display_flush_ready(disp);
}

// The panel only accepts windows that start on even x/y and have even width/height.
void evenAreaCb(lv_event_t *e) {
    auto *area = static_cast<lv_area_t *>(lv_event_get_param(e));
    area->x1 &= ~1;
    area->y1 &= ~1;
    area->x2 |= 1;  // x2 is inclusive, so an odd x2 gives an even width
    area->y2 |= 1;
}

bool initDisplay() {
    // VCI_EN powers the panel; it must be on before the controller is initialised.
    pinMode(pins::kLcdVciEn, OUTPUT);
    digitalWrite(pins::kLcdVciEn, HIGH);
    delay(10);

    if (!gfx->begin(40000000)) {
        return false;
    }
    gfx->fillScreen(RGB565_BLACK);
    gfx->setBrightness(200);

    lv_init();
    lv_tick_set_cb([]() -> uint32_t { return millis(); });

    const size_t bufBytes = pins::kLcdWidth * kDrawBufLines * sizeof(uint16_t);
    void *buf1 = heap_caps_malloc(bufBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    void *buf2 = heap_caps_malloc(bufBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (buf1 == nullptr || buf2 == nullptr) {
        return false;
    }

    lv_display_t *disp = lv_display_create(pins::kLcdWidth, pins::kLcdHeight);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(disp, flushCb);
    lv_display_set_buffers(disp, buf1, buf2, bufBytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_add_event_cb(disp, evenAreaCb, LV_EVENT_INVALIDATE_AREA, nullptr);
    return true;
}

// ---------------------------------------------------------------------------
// Touch
// ---------------------------------------------------------------------------

enum class TouchVariant { None, Cst816, Chsc5816 };

TouchVariant touchVariant = TouchVariant::None;
TouchDrvCSTXXX touchCst;
TouchDrvCHSC5816 touchChsc;

const char *touchVariantName() {
    switch (touchVariant) {
        case TouchVariant::Cst816: return "CST816";
        case TouchVariant::Chsc5816: return "CHSC5816";
        default: return "none";
    }
}

bool i2cProbe(uint8_t addr) {
    Wire.beginTransmission(addr);
    return Wire.endTransmission() == 0;
}

void initTouch() {
    // Reset the controller so it is awake (CST816 does not ACK while in auto-sleep).
    pinMode(pins::kTouchRst, OUTPUT);
    digitalWrite(pins::kTouchRst, LOW);
    delay(10);
    digitalWrite(pins::kTouchRst, HIGH);
    delay(50);

    // Bus is started here; the drivers get sda/scl = -1 so they do not re-pin it.
    Wire.begin(pins::kTouchSda, pins::kTouchScl);
    if (i2cProbe(pins::kTouchAddrCst816)) {
        touchCst.setTouchDrvModel(TouchDrv_CST8XX);  // skip auto-detect (avoids a CST226 probe error)
        touchCst.setPins(pins::kTouchRst, pins::kTouchInt);
        if (touchCst.begin(Wire, pins::kTouchAddrCst816, -1, -1)) {
            touchCst.setMaxCoordinates(pins::kLcdWidth, pins::kLcdHeight);
            touchVariant = TouchVariant::Cst816;
        }
    } else if (i2cProbe(pins::kTouchAddrChsc5816)) {
        touchChsc.setPins(pins::kTouchRst, pins::kTouchInt);
        if (touchChsc.begin(Wire, pins::kTouchAddrChsc5816, -1, -1)) {
            touchVariant = TouchVariant::Chsc5816;
        }
    }
}

// Returns true and the clamped point while the panel is touched.
bool readTouch(int16_t &x, int16_t &y) {
    uint8_t n = 0;
    if (touchVariant == TouchVariant::Cst816) {
        n = touchCst.getPoint(&x, &y, 1);
    } else if (touchVariant == TouchVariant::Chsc5816) {
        n = touchChsc.getPoint(&x, &y, 1);
    }
    if (n == 0) {
        return false;
    }
    x = static_cast<int16_t>(core::clampInt(x, 0, pins::kLcdWidth - 1));
    y = static_cast<int16_t>(core::clampInt(y, 0, pins::kLcdHeight - 1));
    return true;
}

// ---------------------------------------------------------------------------
// Encoder (ISR quadrature decode)
// ---------------------------------------------------------------------------

// Index: (previous AB << 2) | current AB. +1 / -1 for valid Gray-code steps, 0 otherwise
// (no change, or an invalid double-bit jump caused by bounce).
// DRAM_ATTR keeps it out of flash so the ISR path is IRAM-safe.
DRAM_ATTR const int8_t kQuadTable[16] = {0, -1, +1, 0, +1, 0, 0, -1, -1, 0, 0, +1, 0, +1, -1, 0};

struct RawTransition {
    uint8_t prev;
    uint8_t cur;
    int8_t step;
    uint32_t us;  // micros() at the transition
};

constexpr size_t kRawRingSize = 64;
RawTransition rawRing[kRawRingSize];
std::atomic<uint32_t> rawHead{0};  // written by the ISR only
uint32_t rawTail = 0;              // read by the loop only

std::atomic<int32_t> detentCount{0};
volatile uint8_t encPrevAb = 0;
volatile int8_t encAcc = 0;

uint8_t IRAM_ATTR readEncoderAb() {
    return static_cast<uint8_t>((digitalRead(pins::kEncA) << 1) | digitalRead(pins::kEncB));
}

void IRAM_ATTR encoderIsr() {
    const uint8_t cur = readEncoderAb();
    const uint8_t prev = encPrevAb;
    if (cur == prev) {
        return;
    }
    const int8_t step = kQuadTable[(prev << 2) | cur];
    encPrevAb = cur;

    const uint32_t head = rawHead.load(std::memory_order_relaxed);
    rawRing[head % kRawRingSize] = {prev, cur, step, static_cast<uint32_t>(micros())};
    rawHead.store(head + 1, std::memory_order_release);

    encAcc += step;
    if (encAcc >= kTransitionsPerDetent) {
        encAcc = 0;
        detentCount.fetch_add(1, std::memory_order_relaxed);
    } else if (encAcc <= -kTransitionsPerDetent) {
        encAcc = 0;
        detentCount.fetch_sub(1, std::memory_order_relaxed);
    }
}

void initEncoder() {
    pinMode(pins::kEncA, INPUT_PULLUP);
    pinMode(pins::kEncB, INPUT_PULLUP);
    encPrevAb = readEncoderAb();
    attachInterrupt(digitalPinToInterrupt(pins::kEncA), encoderIsr, CHANGE);
    attachInterrupt(digitalPinToInterrupt(pins::kEncB), encoderIsr, CHANGE);
}

void printAb(uint8_t ab) {
    Serial.print((ab >> 1) & 1);
    Serial.print(ab & 1);
}

uint32_t lastRawUs = 0;

// Drains the raw transition ring (the ISR may overwrite unread entries on very fast spins).
// A long dt marks where the knob rested, i.e. the detent positions.
void logRawTransitions() {
    const uint32_t head = rawHead.load(std::memory_order_acquire);
    if (head - rawTail > kRawRingSize) {
        Serial.printf("ENCRAW dropped=%u\n", static_cast<unsigned>(head - rawTail - kRawRingSize));
        rawTail = head - kRawRingSize;
    }
    while (rawTail != head) {
        const RawTransition t = rawRing[rawTail % kRawRingSize];
        Serial.print("ENCRAW ab=");
        printAb(t.prev);
        Serial.print("->");
        printAb(t.cur);
        Serial.printf(" step=%+d dt=%u\n", t.step, static_cast<unsigned>(t.us - lastRawUs));
        lastRawUs = t.us;
        ++rawTail;
    }
}

// ---------------------------------------------------------------------------
// Button and buzzer
// ---------------------------------------------------------------------------

bool buttonDown = false;       // debounced state
bool buttonRawLast = false;
uint32_t buttonRawChangedMs = 0;
uint32_t beepUntilMs = 0;

void initButtonAndBuzzer() {
    pinMode(pins::kEncKey, INPUT_PULLUP);
    ledcSetup(kBuzzerLedcChannel, kBeepFreqHz, 8);
    ledcAttachPin(pins::kBuzzer, kBuzzerLedcChannel);
    ledcWrite(kBuzzerLedcChannel, 0);
}

void startBeep(uint32_t now) {
    ledcWrite(kBuzzerLedcChannel, 128);  // 50 % duty
    beepUntilMs = now + kBeepMs;
}

// Returns true when the debounced state changed.
bool pollButton(uint32_t now) {
    const bool raw = digitalRead(pins::kEncKey) == LOW;
    if (raw != buttonRawLast) {
        buttonRawLast = raw;
        buttonRawChangedMs = now;
    }
    if (raw != buttonDown && now - buttonRawChangedMs >= kButtonDebounceMs) {
        buttonDown = raw;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// UI
// ---------------------------------------------------------------------------

lv_obj_t *countLabel = nullptr;
lv_obj_t *buttonLabel = nullptr;
lv_obj_t *touchDot = nullptr;

lv_obj_t *addLabel(lv_obj_t *parent, const lv_font_t *font, int32_t yOffset, const char *text) {
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_label_set_text(label, text);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, yOffset);
    return label;
}

void buildUi() {
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    addLabel(scr, &lv_font_montserrat_28, -100, "HA Knob");
    addLabel(scr, &lv_font_montserrat_20, -60, "fw " FW_VERSION);
    lv_label_set_text_fmt(addLabel(scr, &lv_font_montserrat_20, -25, ""), "touch: %s",
                          touchVariantName());
    countLabel = addLabel(scr, &lv_font_montserrat_28, 20, "count: 0");
    buttonLabel = addLabel(scr, &lv_font_montserrat_20, 65, "button: up");

    touchDot = lv_obj_create(scr);
    lv_obj_set_size(touchDot, 16, 16);
    lv_obj_set_style_radius(touchDot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(touchDot, lv_palette_main(LV_PALETTE_RED), 0);
    lv_obj_set_style_border_width(touchDot, 0, 0);
    lv_obj_add_flag(touchDot, LV_OBJ_FLAG_HIDDEN);
}

// ---------------------------------------------------------------------------
// Serial
// ---------------------------------------------------------------------------

void printBootLine() {
    Serial.printf("BOOT fw=%s psram=%u flash=%u touch=%s heap=%u\n", FW_VERSION,
                  static_cast<unsigned>(ESP.getPsramSize()),
                  static_cast<unsigned>(ESP.getFlashChipSize()), touchVariantName(),
                  static_cast<unsigned>(ESP.getFreeHeap()));
}

void printHeartbeat(uint32_t now) {
    Serial.printf("HB up=%u psram=%u flash=%u touch=%s count=%d\n",
                  static_cast<unsigned>(now / 1000), static_cast<unsigned>(ESP.getPsramSize()),
                  static_cast<unsigned>(ESP.getFlashChipSize()), touchVariantName(),
                  static_cast<int>(detentCount.load()));
}

int32_t shownCount = 0;
uint32_t lastTouchPollMs = 0;
uint32_t lastTouchLogMs = 0;
int16_t lastTouchX = -1;
int16_t lastTouchY = -1;
uint32_t lastHeartbeatMs = 0;

}  // namespace

void setup() {
    Serial.begin(115200);
    Serial.setTxTimeoutMs(0);  // never block when no USB host is attached

    const bool displayOk = initDisplay();
    initTouch();
    initEncoder();
    initButtonAndBuzzer();
    if (displayOk) {
        buildUi();
    } else {
        Serial.println("ERR display init failed");
    }
    printBootLine();
}

void loop() {
    const uint32_t now = millis();

    // Encoder
    logRawTransitions();
    const int32_t count = detentCount.load(std::memory_order_relaxed);
    if (count != shownCount) {
        while (shownCount != count) {
            const int d = count > shownCount ? 1 : -1;
            shownCount += d;
            Serial.printf("ENC d=%+d count=%d\n", d, static_cast<int>(shownCount));
        }
        if (countLabel != nullptr) {
            lv_label_set_text_fmt(countLabel, "count: %d", static_cast<int>(shownCount));
        }
    }

    // Button + buzzer
    if (pollButton(now)) {
        Serial.println(buttonDown ? "BTN down" : "BTN up");
        if (buttonLabel != nullptr) {
            lv_label_set_text(buttonLabel, buttonDown ? "button: down" : "button: up");
        }
        if (buttonDown) {
            startBeep(now);
        }
    }
    if (beepUntilMs != 0 && static_cast<int32_t>(now - beepUntilMs) >= 0) {
        ledcWrite(kBuzzerLedcChannel, 0);
        beepUntilMs = 0;
    }

    // Touch
    if (touchVariant != TouchVariant::None && now - lastTouchPollMs >= kTouchPollMs) {
        lastTouchPollMs = now;
        int16_t x = 0;
        int16_t y = 0;
        if (readTouch(x, y) && (x != lastTouchX || y != lastTouchY) &&
            now - lastTouchLogMs >= kTouchLogMinIntervalMs) {
            lastTouchLogMs = now;
            lastTouchX = x;
            lastTouchY = y;
            Serial.printf("TOUCH x=%d y=%d\n", x, y);
            if (touchDot != nullptr) {
                lv_obj_set_pos(touchDot, x - 8, y - 8);
                lv_obj_remove_flag(touchDot, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }

    // Heartbeat
    if (now - lastHeartbeatMs >= kHeartbeatMs) {
        lastHeartbeatMs = now;
        printHeartbeat(now);
    }

    lv_timer_handler();
    delay(5);
}
