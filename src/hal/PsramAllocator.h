#pragma once

#include <ArduinoJson.h>
#include <esp_heap_caps.h>

// ArduinoJson allocator that puts JSON document pools in PSRAM (ArduinoJson v7 pools are
// small, so the default malloc would keep them in scarce internal RAM). Falls back to
// internal RAM if PSRAM is exhausted. Pass PsramAllocator::instance() to core APIs.
class PsramAllocator : public ArduinoJson::Allocator {
public:
    static PsramAllocator *instance() {
        static PsramAllocator allocator;
        return &allocator;
    }

    void *allocate(size_t size) override {
        void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        return p != nullptr ? p : heap_caps_malloc(size, MALLOC_CAP_8BIT);
    }

    void deallocate(void *ptr) override { heap_caps_free(ptr); }

    void *reallocate(void *ptr, size_t newSize) override {
        void *p = heap_caps_realloc(ptr, newSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        return p != nullptr ? p : heap_caps_realloc(ptr, newSize, MALLOC_CAP_8BIT);
    }

private:
    PsramAllocator() = default;
};
