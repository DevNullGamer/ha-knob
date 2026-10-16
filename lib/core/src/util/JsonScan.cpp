#include "util/JsonScan.h"

#include <cstring>

namespace core {
namespace util {
namespace json_scan {

namespace {

constexpr uint32_t kMatchedLevels = 64;  // bracket kinds remembered in one uint64_t

bool isWhitespace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

bool isScalarChar(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           c == '+' || c == '-' || c == '.';
}

}  // namespace

size_t skipWhitespace(const char *data, size_t len, size_t pos) {
    while (pos < len && isWhitespace(data[pos])) {
        ++pos;
    }
    return pos < len ? pos : len;
}

ScanStatus scanString(const char *data, size_t len, size_t pos, size_t &end) {
    if (pos >= len || data[pos] != '"') {
        return pos >= len ? ScanStatus::Truncated : ScanStatus::Malformed;
    }
    size_t i = pos + 1;
    while (i < len) {
        const char c = data[i];
        if (c == '\\') {
            i += 2;  // the escaped character can never close the string
            continue;
        }
        if (c == '"') {
            end = i + 1;
            return ScanStatus::Ok;
        }
        ++i;
    }
    return ScanStatus::Truncated;
}

ScanStatus skipValue(const char *data, size_t len, size_t pos, size_t &end) {
    uint32_t depth = 0;
    uint64_t objectBits = 0;  // bit d: level d (0-based) is an object
    size_t i = pos;
    while (true) {
        if (i >= len) {
            return ScanStatus::Truncated;
        }
        const char c = data[i];
        if (c == '"') {
            size_t stringEnd = 0;
            const ScanStatus st = scanString(data, len, i, stringEnd);
            if (st != ScanStatus::Ok) {
                return st;
            }
            i = stringEnd;
        } else if (c == '{' || c == '[') {
            if (depth < kMatchedLevels) {
                const uint64_t bit = uint64_t(1) << depth;
                objectBits = c == '{' ? (objectBits | bit) : (objectBits & ~bit);
            }
            if (depth == UINT32_MAX) {
                return ScanStatus::Malformed;
            }
            ++depth;
            ++i;
        } else if (c == '}' || c == ']') {
            if (depth == 0) {
                return ScanStatus::Malformed;
            }
            --depth;
            if (depth < kMatchedLevels) {
                const bool isObject = (objectBits >> depth) & 1u;
                if (isObject != (c == '}')) {
                    return ScanStatus::Malformed;
                }
            }
            ++i;
        } else if (isScalarChar(c)) {
            while (i < len && isScalarChar(data[i])) {
                ++i;
            }
        } else if (depth > 0 && (isWhitespace(c) || c == ',' || c == ':')) {
            ++i;
        } else {
            return ScanStatus::Malformed;
        }
        if (depth == 0) {
            end = i;
            return ScanStatus::Ok;
        }
    }
}

bool parseUint32(const char *data, size_t len, uint32_t &out) {
    if (len == 0 || len > 10 || (len > 1 && data[0] == '0')) {
        return false;
    }
    uint64_t value = 0;
    for (size_t i = 0; i < len; ++i) {
        if (data[i] < '0' || data[i] > '9') {
            return false;
        }
        value = value * 10 + uint64_t(data[i] - '0');
    }
    if (value > UINT32_MAX) {
        return false;
    }
    out = uint32_t(value);
    return true;
}

bool equals(const char *data, size_t len, const char *literal) {
    const size_t n = std::strlen(literal);
    return n == len && std::memcmp(data, literal, n) == 0;
}

}  // namespace json_scan
}  // namespace util
}  // namespace core
