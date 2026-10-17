#pragma once

#include <Arduino.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>

// Serial log output that can never block. Use it instead of Serial.print*/printf
// everywhere (all tasks; it is thread-safe, but not for ISRs).
//
// Why: on Arduino-ESP32 2.0.17, HWCDC::write() (USB-Serial/JTAG) busy-loops when the USB
// host is attached but nothing reads the port (COM14 closed) and the TX ring buffer is
// full: it keeps "sending" 0 free bytes and counts its retries down from the TX timeout,
// giving up only when the count reaches 0. With setTxTimeoutMs(0) (used up to Step 4) the
// count wraps around, so the loop runs for about 49 days; setup() now sets 1 ms. That
// stalled whichever task printed (the main loop: stale Wi-Fi snapshot, scans never
// finishing) until a serial monitor was opened. logPrintf() writes a line only if all of
// it fits into the free TX buffer space (plus headroom for ESP-IDF log output, which
// writes to the same buffer without our lock) and otherwise drops it; dropped lines are
// counted and reported as "LOG dropped=<n>" once there is room again.
//
// Header-only so that code compiled outside the firmware build (the embedded
// ConfigStore test) needs nothing extra.
namespace util {

namespace logdetail {

// Line buffer incl. the terminating NUL: a line is at most 318 chars + '\n' (logPrintf cuts
// a longer one to that). The buffer is on the caller's stack (320 B per logPrintf call). Worst-case HB line: DESIGN-NOTES "Step 8.3".
constexpr size_t kMaxLine = 320;
constexpr int kHeadroom = 64;

inline std::mutex &mutex() {
    static std::mutex m;
    return m;
}

inline uint32_t &dropped() {
    static uint32_t n = 0;
    return n;
}

// Caller holds mutex(). True if the whole text was handed to the TX buffer.
inline bool writeIfRoom(const char *text, size_t len) {
    if (Serial.availableForWrite() < static_cast<int>(len) + kHeadroom) {
        return false;
    }
    Serial.write(reinterpret_cast<const uint8_t *>(text), len);
    return true;
}

}  // namespace logdetail

inline void logPrintf(const char *format, ...) __attribute__((format(printf, 1, 2)));

inline void logPrintf(const char *format, ...) {
    using namespace logdetail;
    char line[kMaxLine];
    va_list args;
    va_start(args, format);
    int len = vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    if (len <= 0) {
        return;
    }
    if (static_cast<size_t>(len) >= sizeof(line)) {
        len = sizeof(line) - 1;  // truncated; keep the line ending
        line[len - 1] = '\n';
    }

    std::lock_guard<std::mutex> lock(mutex());
    if (dropped() > 0) {
        char note[32];
        const int n = snprintf(note, sizeof(note), "LOG dropped=%u\n", static_cast<unsigned>(dropped()));
        if (!writeIfRoom(note, static_cast<size_t>(n))) {
            ++dropped();
            return;
        }
        dropped() = 0;
    }
    if (!writeIfRoom(line, static_cast<size_t>(len))) {
        ++dropped();
    }
}

}  // namespace util
