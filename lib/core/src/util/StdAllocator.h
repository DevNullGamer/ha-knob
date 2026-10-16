#pragma once

#include <ArduinoJson.h>

#include <cstddef>
#include <cstdlib>
#include <new>
#include <string>

// A standard-library allocator on top of an injected ArduinoJson::Allocator, so long-lived
// STL containers in lib/core can live where the caller decides (on the device:
// PsramAllocator::instance(), i.e. PSRAM instead of scarce internal RAM) while lib/core
// stays pure. nullptr = the default heap (operator new / delete), which is also what a
// default-constructed allocator uses (host tests).
//
// Stateful: two allocators are equal when they use the same backend. The propagate_on_*
// traits are the defaults (false), so an assigned container keeps its own backend and the
// elements are copied / moved into its memory; containers with different backends must
// not be swapped. Construct the elements that go into a container with the container's
// allocator (move construction takes the source's allocator along).
namespace core {
namespace util {

template <typename T>
class StdAllocator {
public:
    using value_type = T;

    StdAllocator() noexcept = default;
    explicit StdAllocator(ArduinoJson::Allocator *backend) noexcept : backend_(backend) {}
    template <typename U>
    StdAllocator(const StdAllocator<U> &other) noexcept : backend_(other.backend()) {}

    T *allocate(size_t n) {
        const size_t bytes = n * sizeof(T);
        if (backend_ == nullptr) {
            return static_cast<T *>(::operator new(bytes));
        }
        void *p = backend_->allocate(bytes);
        if (p == nullptr) {
            outOfMemory();
        }
        return static_cast<T *>(p);
    }

    void deallocate(T *p, size_t) noexcept {
        if (backend_ == nullptr) {
            ::operator delete(p);
        } else {
            backend_->deallocate(p);
        }
    }

    ArduinoJson::Allocator *backend() const noexcept { return backend_; }

private:
    // Same outcome as a failed operator new.
    [[noreturn]] static void outOfMemory() {
#if defined(__cpp_exceptions)
        throw std::bad_alloc();
#else
        std::abort();
#endif
    }

    ArduinoJson::Allocator *backend_ = nullptr;
};

template <typename T, typename U>
bool operator==(const StdAllocator<T> &a, const StdAllocator<U> &b) noexcept {
    return a.backend() == b.backend();
}

template <typename T, typename U>
bool operator!=(const StdAllocator<T> &a, const StdAllocator<U> &b) noexcept {
    return !(a == b);
}

// A string whose characters live on the injected allocator. Converts to std::string_view;
// compare with == against const char * / string_view, use c_str() for std::string.
using AllocString = std::basic_string<char, std::char_traits<char>, StdAllocator<char>>;

}  // namespace util
}  // namespace core
