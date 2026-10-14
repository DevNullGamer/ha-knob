#include "hal/Encoder.h"

#include <Arduino.h>
#include <atomic>
#include <driver/gpio.h>
#include <hal/gpio_ll.h>
#include <soc/gpio_struct.h>

#include "board_pins.h"
#include "input/QuadratureDecoder.h"

namespace hal {
namespace encoder {

namespace {

// The ISR is registered with the ESP-IDF GPIO ISR service using ESP_INTR_FLAG_IRAM, so it
// can run while the flash cache is disabled. Everything it touches is therefore in IRAM or
// DRAM: the functions below are IRAM_ATTR, QuadratureDecoder::update() is always_inline
// (compiled into the ISR), pins are read with the inline gpio_ll_get_level() register
// access (Arduino's digitalRead/attachInterrupt live in flash on core 2.0.17), and the
// state is global data (DRAM).
core::QuadratureDecoder decoder;
std::atomic<int32_t> detents{0};  // written by the ISR, read by the main loop
int32_t lastRead = 0;             // main loop only

static_assert(std::atomic<int32_t>::is_always_lock_free, "ISR needs a lock-free counter");

uint8_t IRAM_ATTR readAb() {
    const int a = gpio_ll_get_level(&GPIO, static_cast<gpio_num_t>(pins::kEncA));
    const int b = gpio_ll_get_level(&GPIO, static_cast<gpio_num_t>(pins::kEncB));
    return static_cast<uint8_t>((a << 1) | b);
}

void IRAM_ATTR encoderIsr(void * /*arg*/) {
    const int8_t d = decoder.update(readAb());
    if (d != 0) {
        detents.fetch_add(d, std::memory_order_relaxed);
    }
}

}  // namespace

void begin(bool reverse) {
    gpio_config_t io = {};
    io.pin_bit_mask = (1ULL << pins::kEncA) | (1ULL << pins::kEncB);
    io.mode = GPIO_MODE_INPUT;
    io.pull_up_en = GPIO_PULLUP_ENABLE;
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io.intr_type = GPIO_INTR_ANYEDGE;
    gpio_config(&io);

    decoder.setReverse(reverse);
    decoder.reset(readAb());

    // If another module installed the service first (without the IRAM flag), this returns
    // ESP_ERR_INVALID_STATE and the handler still works, just not with the cache disabled.
    const esp_err_t err = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    if (err != ESP_OK) {
        Serial.printf("ERR encoder isr service err=%d\n", static_cast<int>(err));
    }
    gpio_isr_handler_add(static_cast<gpio_num_t>(pins::kEncA), encoderIsr, nullptr);
    gpio_isr_handler_add(static_cast<gpio_num_t>(pins::kEncB), encoderIsr, nullptr);
}

int32_t takeDelta() {
    const int32_t now = detents.load(std::memory_order_relaxed);
    const int32_t delta = now - lastRead;
    lastRead = now;
    return delta;
}

int32_t count() {
    return detents.load(std::memory_order_relaxed);
}

}  // namespace encoder
}  // namespace hal
