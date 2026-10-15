#include "util/Base64.h"

namespace core {
namespace util {

namespace {

const char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int decodeChar(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

}  // namespace

std::string base64Encode(const uint8_t *data, size_t len) {
    std::string out;
    out.reserve((len + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 3 <= len; i += 3) {
        uint32_t v = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8) | data[i + 2];
        out += kAlphabet[(v >> 18) & 63];
        out += kAlphabet[(v >> 12) & 63];
        out += kAlphabet[(v >> 6) & 63];
        out += kAlphabet[v & 63];
    }
    size_t rest = len - i;
    if (rest == 1) {
        uint32_t v = uint32_t(data[i]) << 16;
        out += kAlphabet[(v >> 18) & 63];
        out += kAlphabet[(v >> 12) & 63];
        out += "==";
    } else if (rest == 2) {
        uint32_t v = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8);
        out += kAlphabet[(v >> 18) & 63];
        out += kAlphabet[(v >> 12) & 63];
        out += kAlphabet[(v >> 6) & 63];
        out += '=';
    }
    return out;
}

bool base64Decode(const std::string &text, std::vector<uint8_t> &out) {
    out.clear();
    uint32_t acc = 0;
    int count = 0;   // data characters in the current group of 4
    int padding = 0; // '=' seen in the current (final) group
    for (char c : text) {
        if (c == '\r' || c == '\n') continue;
        if (c == '=') {
            // Only valid as 3rd/4th character of a group.
            if (count < 2) {
                out.clear();
                return false;
            }
            ++padding;
            ++count;
            if (count == 4) {
                // Group complete; emit and require the end of the data.
                if (padding == 1) {
                    out.push_back(uint8_t(acc >> 10));
                    out.push_back(uint8_t(acc >> 2));
                } else {
                    out.push_back(uint8_t(acc >> 4));
                }
                count = 0;
                acc = 0;
                padding = -1;  // nothing may follow
            }
            continue;
        }
        int v = decodeChar(c);
        if (v < 0 || padding != 0) {
            out.clear();
            return false;
        }
        acc = (acc << 6) | uint32_t(v);
        if (++count == 4) {
            out.push_back(uint8_t(acc >> 16));
            out.push_back(uint8_t(acc >> 8));
            out.push_back(uint8_t(acc));
            count = 0;
            acc = 0;
        }
    }
    if (count != 0) {  // incomplete group (missing padding)
        out.clear();
        return false;
    }
    return true;
}

}  // namespace util
}  // namespace core
