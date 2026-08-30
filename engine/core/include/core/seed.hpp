#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace inf::core {

// The 128-bit universe seed: the single value a shared universe is derived
// from. Canonical text form is exactly 32 lowercase hex digits.
struct Seed128 {
  std::uint64_t hi{0};
  std::uint64_t lo{0};

  friend bool operator==(const Seed128&, const Seed128&) = default;
};

// Accepts 1..32 hex digits, optionally prefixed "0x"/"0X"; case-insensitive.
// Shorter inputs are zero-extended from the left.
std::optional<Seed128> parse_seed(std::string_view text);

// 32 lowercase hex digits, no prefix.
std::string to_hex(const Seed128& seed);

}  // namespace inf::core
