#include "core/seed.hpp"

#include <array>

namespace inf::core {

namespace {

int hex_digit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

}  // namespace

std::optional<Seed128> parse_seed(std::string_view text) {
  if (text.size() >= 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
    text.remove_prefix(2);
  }
  if (text.empty() || text.size() > 32) {
    return std::nullopt;
  }
  Seed128 seed;
  for (char c : text) {
    const int digit = hex_digit(c);
    if (digit < 0) {
      return std::nullopt;
    }
    seed.hi = (seed.hi << 4) | (seed.lo >> 60);
    seed.lo = (seed.lo << 4) | static_cast<std::uint64_t>(digit);
  }
  return seed;
}

std::string to_hex(const Seed128& seed) {
  static constexpr std::array<char, 16> kDigits = {'0', '1', '2', '3', '4', '5', '6', '7',
                                                   '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
  std::string out(32, '0');
  for (int i = 0; i < 16; ++i) {
    out[static_cast<std::size_t>(15 - i)] = kDigits[(seed.hi >> (i * 4)) & 0xF];
    out[static_cast<std::size_t>(31 - i)] = kDigits[(seed.lo >> (i * 4)) & 0xF];
  }
  return out;
}

}  // namespace inf::core
