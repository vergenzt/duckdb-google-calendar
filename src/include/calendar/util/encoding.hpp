#pragma once

#include <string>

namespace duckdb {
namespace gcal {

constexpr unsigned char MASK_TOP6 = 0xFC;
constexpr unsigned char MASK_BOT2 = 0x03;
constexpr unsigned char MASK_TOP4 = 0xF0;
constexpr unsigned char MASK_BOT4 = 0x0F;
constexpr unsigned char MASK_TOP2 = 0xC0;
constexpr unsigned char MASK_BOT6 = 0x3F;

std::string Base64UrlEncode(const unsigned char *data, size_t len);

std::string Base64UrlEncode(const std::string &input);

std::string NormalizePemKey(const std::string &key);

} // namespace gcal
} // namespace duckdb
