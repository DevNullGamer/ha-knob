#include "util/Text.h"

namespace core {
namespace util {

std::string truncateUtf8(std::string_view s, size_t maxBytes) {
    size_t length = s.size();
    if (length > maxBytes) {
        length = maxBytes;
        while (length > 0 && (static_cast<unsigned char>(s[length]) & 0xC0) == 0x80) {
            --length;  // s[length] continues a character: cut before that character
        }
    }
    return std::string(s.substr(0, length));
}

std::string logSafe(std::string_view s, size_t maxBytes) {
    std::string out = truncateUtf8(s, maxBytes);
    for (char &c : out) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x20 || u == 0x7F) {
            c = '?';
        }
    }
    return out;
}

}  // namespace util
}  // namespace core
