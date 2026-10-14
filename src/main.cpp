// HA Knob firmware entry point: setup()/loop() wiring only.
//
// Serial log lines:
//   BOOT fw=<v> psram=<bytes> flash=<bytes> touch=<CST816|CHSC5816|none> heap=<bytes>
//        cfg=<defaults|loaded>
//   ENC d=<+1|-1> count=<n>
//   BTN down | up | short | long | verylong
//   TOUCH swipe=<left|right>
//   HB up=<s> psram=<bytes> flash=<bytes> touch=<..> count=<n> ctl=<controls> cfg=<..>   (every 5 s)
//   CFG ...   config store events (see src/config/ConfigStore.h)

#include <Arduino.h>
#include <lvgl.h>

#include "config/ConfigStore.h"
#include "hal/Button.h"
#include "hal/Buzzer.h"
#include "hal/Display.h"
#include "hal/Encoder.h"
#include "hal/Touch.h"
#include "ui/Ui.h"
#include "version.h"

namespace {

constexpr uint32_t kHeartbeatMs = 5000;
constexpr uint32_t kLoopDelayMs = 5;

ConfigStore configStore;
const char *bootCfgSource = "defaults";  // how this boot got its config: defaults | loaded
bool displayOk = false;
int32_t shownCount = 0;
uint32_t lastHeartbeatMs = 0;

void printBootLine() {
    Serial.printf("BOOT fw=%s psram=%u flash=%u touch=%s heap=%u cfg=%s\n", FW_VERSION,
                  static_cast<unsigned>(ESP.getPsramSize()),
                  static_cast<unsigned>(ESP.getFlashChipSize()), hal::touch::variantName(),
                  static_cast<unsigned>(ESP.getFreeHeap()),
                  bootCfgSource);
}

void printHeartbeat(uint32_t nowMs) {
    Serial.printf("HB up=%u psram=%u flash=%u touch=%s count=%d ctl=%u cfg=%s\n",
                  static_cast<unsigned>(nowMs / 1000), static_cast<unsigned>(ESP.getPsramSize()),
                  static_cast<unsigned>(ESP.getFlashChipSize()), hal::touch::variantName(),
                  static_cast<int>(shownCount),
                  static_cast<unsigned>(configStore.controlCount()), bootCfgSource);
}

void handleEncoder() {
    const int32_t delta = hal::encoder::takeDelta();
    if (delta == 0) {
        return;
    }
    const int d = delta > 0 ? 1 : -1;
    for (int32_t i = 0; i != delta; i += d) {
        shownCount += d;
        Serial.printf("ENC d=%+d count=%d\n", d, static_cast<int>(shownCount));
    }
    if (displayOk) {
        ui::showEncoderCount(shownCount);
    }
}

void handleButton(uint32_t nowMs) {
    using Event = core::ButtonFsm::Event;
    const Event event = hal::button::poll(nowMs);
    if (event == Event::None) {
        return;
    }
    if (event == Event::Pressed) {
        hal::buzzer::beep(nowMs);
    }
    if (event == Event::ShortPress) {
        Serial.println("BTN up");  // a short press is also a release
    }
    Serial.printf("BTN %s\n", core::buttonEventName(event));

    if (!displayOk) {
        return;
    }
    switch (event) {
        case Event::ShortPress: ui::showLastInput("short press"); break;
        case Event::LongPress: ui::showLastInput("long press"); break;
        case Event::VeryLongPress: ui::showLastInput("very long press"); break;
        default: break;
    }
}

void handleTouch(uint32_t nowMs) {
    const core::Swipe swipe = hal::touch::poll(nowMs);
    if (swipe == core::Swipe::None) {
        return;
    }
    Serial.printf("TOUCH swipe=%s\n", core::swipeName(swipe));
    if (displayOk) {
        ui::showLastInput(swipe == core::Swipe::Left ? "swipe left" : "swipe right");
    }
}

}  // namespace

void setup() {
    Serial.begin(115200);
    Serial.setTxTimeoutMs(0);  // never block when no USB host is attached

    configStore.begin();
    bootCfgSource = configStore.loadedFromNvs() ? "loaded" : "defaults";
    const core::config::Config config = configStore.config();

    displayOk = hal::display::begin();
    if (displayOk) {
        hal::display::setBrightnessPercent(config.device.brightness);
    }
    hal::touch::begin();
    hal::encoder::begin();
    hal::button::begin();
    hal::buzzer::begin();
    if (displayOk) {
        ui::begin(millis());
    } else {
        Serial.println("ERR display init failed");
    }
    printBootLine();
}

void loop() {
    const uint32_t nowMs = millis();

    handleEncoder();
    handleButton(nowMs);
    handleTouch(nowMs);
    hal::buzzer::loop(nowMs);

    if (nowMs - lastHeartbeatMs >= kHeartbeatMs) {
        lastHeartbeatMs = nowMs;
        printHeartbeat(nowMs);
    }

    if (displayOk) {
        ui::tick(nowMs);
        lv_timer_handler();
    }
    delay(kLoopDelayMs);
}
