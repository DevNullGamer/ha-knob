#pragma once

#include <cstddef>
#include <cstdint>

// Iterative structural JSON scanner (plan-step6.md §3.1): finds where strings and values
// end without building anything. No recursion, no allocation, every read is bounds-checked
// against `len`. It checks structure only (strings, escapes, bracket matching); the content
// of skipped values is not validated (ArduinoJson validates whatever is parsed later).
// Positions are byte offsets into `data`.
namespace core {
namespace util {
namespace json_scan {

enum class ScanStatus : uint8_t {
    Ok,
    Truncated,  // the input ended inside the token / value
    Malformed,  // a character that cannot start or continue it, or mismatched brackets
};

// First position >= pos that is not JSON whitespace (space, tab, CR, LF); len if none.
size_t skipWhitespace(const char *data, size_t len, size_t pos);

// data[pos] must be '"'. On Ok, `end` is the position just after the closing quote; the
// raw content (escapes not decoded) is [pos + 1, end - 1).
ScanStatus scanString(const char *data, size_t len, size_t pos, size_t &end);

// Skips one value starting exactly at pos (no leading whitespace): a string, an object or
// array (any depth; brackets are matched for the first 64 levels, deeper ones are only
// counted), or a scalar (a run of letters, digits, '+', '-', '.'). On Ok, `end` is the
// position just after the value.
ScanStatus skipValue(const char *data, size_t len, size_t pos, size_t &end);

// Parses [data, data + len) as a JSON unsigned integer that fits uint32_t (digits only,
// no sign, fraction or exponent, no leading zero unless it is "0").
bool parseUint32(const char *data, size_t len, uint32_t &out);

// True if [data, data + len) equals the NUL-terminated `literal`.
bool equals(const char *data, size_t len, const char *literal);

}  // namespace json_scan
}  // namespace util
}  // namespace core
