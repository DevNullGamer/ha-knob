#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace core {
namespace util {

// SHA-1 (FIPS 180-4) of a byte buffer. Only used for the WebSocket accept key (RFC 6455),
// not for anything security relevant.
std::array<uint8_t, 20> sha1(const uint8_t *data, size_t len);

}  // namespace util
}  // namespace core
