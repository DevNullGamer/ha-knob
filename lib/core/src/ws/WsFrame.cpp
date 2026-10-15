#include "ws/WsFrame.h"

namespace core {
namespace ws {

std::vector<uint8_t> encodeClientFrame(Opcode op, const uint8_t *payload, size_t len,
                                       uint32_t maskKey) {
    std::vector<uint8_t> f;
    size_t headerLen = 2 + (len <= 125 ? 0 : len <= 65535 ? 2 : 8) + 4;
    f.reserve(headerLen + len);
    f.push_back(uint8_t(0x80 | (uint8_t(op) & 0x0F)));
    if (len <= 125) {
        f.push_back(uint8_t(0x80 | len));
    } else if (len <= 65535) {
        f.push_back(0x80 | 126);
        f.push_back(uint8_t(len >> 8));
        f.push_back(uint8_t(len));
    } else {
        f.push_back(0x80 | 127);
        uint64_t l = len;
        for (int i = 7; i >= 0; --i) f.push_back(uint8_t(l >> (8 * i)));
    }
    const uint8_t key[4] = {uint8_t(maskKey >> 24), uint8_t(maskKey >> 16), uint8_t(maskKey >> 8),
                            uint8_t(maskKey)};
    f.insert(f.end(), key, key + 4);
    for (size_t i = 0; i < len; ++i) f.push_back(uint8_t(payload[i] ^ key[i & 3]));
    return f;
}

std::vector<uint8_t> encodeClose(uint16_t code, uint32_t maskKey) {
    const uint8_t body[2] = {uint8_t(code >> 8), uint8_t(code)};
    return encodeClientFrame(Opcode::Close, body, 2, maskKey);
}

}  // namespace ws
}  // namespace core
