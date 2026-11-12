#pragma once
#include <charconv>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
namespace spdif::input {
// Strict decimal capture indices/options. No sign, partial token or overflow.
// Whitespace tokenization belongs to the caller; arbitrary binary input fails.
inline uint64_t unsigned_integer(std::string_view text) {
 if(text.empty())throw std::invalid_argument("Invalid unsigned integer: ");
 uint64_t value=0;const auto parsed=std::from_chars(text.data(),text.data()+text.size(),value);
 if(parsed.ec!=std::errc()||parsed.ptr!=text.data()+text.size())
  throw std::invalid_argument("Invalid unsigned integer: "+std::string(text));
 return value;
}
}
