#include "hal/Display.h"

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <esp_heap_caps.h>
#include <lvgl.h>

#include "board_pins.h"
#include "util/Log.h"
#include "util/MathUtil.h"

namespace hal {
namespace display {

namespace {

constexpr int32_t kQspiHz = 40000000;
// One 390x80 px buffer (plan-step8.md Q8-16): flushCb is synchronous, so a second buffer
// never overlaps a transfer; fewer, taller bands mean less per-band render overhead.
constexpr int32_t kDrawBufLines = 80;
constexpr int32_t kFallbackBufLines = 40;  // two of these if the single buffer does not fit
constexpr int32_t kDefaultBrightnessPercent = 80;
constexpr uint8_t kCmdDisplayOff = 0x28;  // CO5300_C_DISPOFF (MIPI DCS)
constexpr uint8_t kCmdDisplayOn = 0x29;   // CO5300_C_DISPON

Arduino_DataBus *bus = nullptr;
Arduino_CO5300 *gfx = nullptr;
uint32_t flushUs = 0;  // takeFlushStats(); flushCb runs inside lv_timer_handler (main loop)
uint32_t flushPx = 0;

void flushCb(lv_display_t *disp, const lv_area_t *area, uint8_t *pxMap) {
    const uint32_t startUs = micros();
    const int32_t w = lv_area_get_width(area);
    const int32_t h = lv_area_get_height(area);
    // Swap to the panel's big-endian order in place (LVGL's unrolled 32-bit loop) and send
    // the buffer as is: the QSPI bus then DMAs straight from it instead of copying every
    // 1024 px chunk through its own swap loop (Step 8.8). LVGL re-renders the buffer for
    // every area, so the swapped content is never reused.
    lv_draw_sw_rgb565_swap(pxMap, static_cast<uint32_t>(w * h));
    gfx->draw16bitBeRGBBitmap(area->x1, area->y1, reinterpret_cast<uint16_t *>(pxMap), w, h);
    lv_display_flush_ready(disp);
    flushUs += micros() - startUs;
    flushPx += static_cast<uint32_t>(w * h);
}

// The panel only accepts windows that start on even x/y and have even width/height.
void evenAreaCb(lv_event_t *e) {
    auto *area = static_cast<lv_area_t *>(lv_event_get_param(e));
    area->x1 &= ~1;
    area->y1 &= ~1;
    area->x2 |= 1;  // x2 is inclusive, so an odd x2 gives an even width
    area->y2 |= 1;
}

}  // namespace

bool begin() {
    // VCI_EN powers the panel; it must be on before the controller is initialised.
    pinMode(pins::kLcdVciEn, OUTPUT);
    digitalWrite(pins::kLcdVciEn, HIGH);
    delay(10);

    bus = new Arduino_ESP32QSPI(pins::kLcdCs, pins::kLcdSclk, pins::kLcdSdio0, pins::kLcdSdio1,
                                pins::kLcdSdio2, pins::kLcdSdio3);
    gfx = new Arduino_CO5300(bus, pins::kLcdRst, 0 /* rotation */, false /* ips */,
                             pins::kLcdWidth, pins::kLcdHeight, 0, 0, 0, 0);
    if (!gfx->begin(kQspiHz)) {
        return false;
    }
    gfx->fillScreen(RGB565_BLACK);
    setBrightnessPercent(kDefaultBrightnessPercent);

    lv_init();
    lv_tick_set_cb([]() -> uint32_t { return millis(); });

    constexpr uint32_t kBufCaps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    size_t bufBytes = pins::kLcdWidth * kDrawBufLines * sizeof(uint16_t);
    void *buf1 = heap_caps_malloc(bufBytes, kBufCaps);
    void *buf2 = nullptr;
    if (buf1 == nullptr) {
        // No 62 400 B block: the previous layout, two 31 200 B buffers (same total).
        util::logPrintf("ERR draw buffer fallback\n");
        bufBytes = pins::kLcdWidth * kFallbackBufLines * sizeof(uint16_t);
        buf1 = heap_caps_malloc(bufBytes, kBufCaps);
        buf2 = heap_caps_malloc(bufBytes, kBufCaps);
        if (buf1 == nullptr || buf2 == nullptr) {
            heap_caps_free(buf1);  // either may be null (a no-op then)
            heap_caps_free(buf2);
            return false;
        }
    }

    lv_display_t *disp = lv_display_create(pins::kLcdWidth, pins::kLcdHeight);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(disp, flushCb);
    lv_display_set_buffers(disp, buf1, buf2, bufBytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_add_event_cb(disp, evenAreaCb, LV_EVENT_INVALIDATE_AREA, nullptr);
    return true;
}

void setBrightnessPercent(int32_t percent) {
    if (gfx != nullptr) {
        gfx->setBrightness(core::percentToByte(percent));
    }
}

void setPanelOn(bool on) {
    if (bus != nullptr && gfx != nullptr) {
        bus->sendCommand(on ? kCmdDisplayOn : kCmdDisplayOff);  // own begin/endWrite
    }
}

void takeFlushStats(uint32_t *us, uint32_t *px) {
    *us = flushUs;
    *px = flushPx;
    flushUs = 0;
    flushPx = 0;
}

}  // namespace display
}  // namespace hal
