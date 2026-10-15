#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// WebSocket frame encoding for the client side (RFC 6455 section 5). Pure.
namespace core {
namespace ws {

enum class Opcode : uint8_t {
    Continuation = 0x0,
    Text = 0x1,
    Binary = 0x2,
    Close = 0x8,
    Ping = 0x9,
    Pong = 0xA,
};

// One unfragmented frame: FIN=1, MASK=1, 7/16/64-bit payload length (0-125, 126-65535,
// larger), the 4 mask key bytes (maskKey big-endian: byte 0 = bits 31..24) and the payload
// XORed with the key. payload may be nullptr when len is 0.
std::vector<uint8_t> encodeClientFrame(Opcode op, const uint8_t *payload, size_t len,
                                       uint32_t maskKey);

// Close frame with a 2-byte big-endian status code (RFC 6455 section 7.4) and no reason.
std::vector<uint8_t> encodeClose(uint16_t code, uint32_t maskKey);

}  // namespace ws
}  // namespace core
