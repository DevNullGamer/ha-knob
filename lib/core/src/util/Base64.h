#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace core {
namespace util {

// Standard Base64 (RFC 4648 section 4) with '=' padding.
std::string base64Encode(const uint8_t *data, size_t len);

// Strict decoder: standard alphabet only, '\r' and '\n' are ignored, padding is required
// (the data characters must form complete groups of 4), '=' only as the last one or two
// characters, nothing after the padding. Anything else (spaces, URL-safe alphabet, missing
// padding) returns false and leaves `out` empty. Non-zero unused trailing bits are accepted.
// An empty input decodes to an empty output (true).
bool base64Decode(const std::string &text, std::vector<uint8_t> &out);

}  // namespace util
}  // namespace core
