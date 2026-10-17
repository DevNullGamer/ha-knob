#pragma once

#include <cstddef>
#include <string>
#include <string_view>

// Text helpers for HA-supplied strings (error messages, names, states). Pure.
namespace core {
namespace util {

// s cut to at most maxBytes bytes without splitting a UTF-8 sequence (the cut goes before
// the character that does not fit). Strings that fit are returned unchanged.
std::string truncateUtf8(std::string_view s, size_t maxBytes);

// truncateUtf8(s, maxBytes) with every control character (0x00-0x1F, 0x7F) replaced by
// '?', so the text cannot break a log line. Bytes >= 0x80 (UTF-8) are kept.
std::string logSafe(std::string_view s, size_t maxBytes);

}  // namespace util
}  // namespace core
