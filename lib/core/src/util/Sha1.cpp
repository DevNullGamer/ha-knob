#include "util/Sha1.h"

namespace core {
namespace util {

namespace {

inline uint32_t rotl(uint32_t v, unsigned n) { return (v << n) | (v >> (32 - n)); }

void processBlock(uint32_t h[5], const uint8_t *block) {
    uint32_t w[80];
    for (int i = 0; i < 16; ++i) {
        w[i] = (uint32_t(block[i * 4]) << 24) | (uint32_t(block[i * 4 + 1]) << 16) |
               (uint32_t(block[i * 4 + 2]) << 8) | uint32_t(block[i * 4 + 3]);
    }
    for (int i = 16; i < 80; ++i) w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; ++i) {
        uint32_t f, k;
        if (i < 20) {
            f = (b & c) | (~b & d);
            k = 0x5A827999u;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1u;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDCu;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6u;
        }
        uint32_t t = rotl(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rotl(b, 30);
        b = a;
        a = t;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
}

}  // namespace

std::array<uint8_t, 20> sha1(const uint8_t *data, size_t len) {
    uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};

    size_t full = len / 64;
    for (size_t i = 0; i < full; ++i) processBlock(h, data + i * 64);

    // Final one or two blocks: remaining bytes, 0x80, zero padding, 64-bit bit length.
    uint8_t tail[128] = {0};
    size_t rem = len - full * 64;
    for (size_t i = 0; i < rem; ++i) tail[i] = data[full * 64 + i];
    tail[rem] = 0x80;
    size_t tailLen = (rem < 56) ? 64 : 128;
    uint64_t bits = uint64_t(len) * 8u;
    for (int i = 0; i < 8; ++i) tail[tailLen - 1 - i] = uint8_t(bits >> (8 * i));
    processBlock(h, tail);
    if (tailLen == 128) processBlock(h, tail + 64);

    std::array<uint8_t, 20> out{};
    for (int i = 0; i < 5; ++i) {
        out[i * 4] = uint8_t(h[i] >> 24);
        out[i * 4 + 1] = uint8_t(h[i] >> 16);
        out[i * 4 + 2] = uint8_t(h[i] >> 8);
        out[i * 4 + 3] = uint8_t(h[i]);
    }
    return out;
}

}  // namespace util
}  // namespace core
