#pragma once

// Minimal reference MD5 (RFC 1321) — TEST SUPPORT ONLY. Used solely to
// verify the frozen name_id registry (first 8 digest bytes, little-endian)
// against the strings it claims to encode. Never linked into the game.

#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace inf::test {

inline std::array<std::uint8_t, 16> md5(std::string_view input) {
  constexpr std::array<std::uint32_t, 64> k = {
      0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613,
      0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193,
      0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d,
      0x02441453, 0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed,
      0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122,
      0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa,
      0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665, 0xf4292244,
      0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
      0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb,
      0xeb86d391};
  constexpr std::array<std::uint32_t, 64> shifts = {
      7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9,  14, 20, 5, 9,
      14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
      4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};

  std::uint32_t a0 = 0x67452301;
  std::uint32_t b0 = 0xefcdab89;
  std::uint32_t c0 = 0x98badcfe;
  std::uint32_t d0 = 0x10325476;

  // Message with padding: 0x80, zeros, 64-bit little-endian bit length.
  const std::uint64_t bit_length = static_cast<std::uint64_t>(input.size()) * 8U;
  std::size_t padded_size = input.size() + 1;
  while (padded_size % 64 != 56) {
    ++padded_size;
  }
  padded_size += 8;

  for (std::size_t chunk_start = 0; chunk_start < padded_size; chunk_start += 64) {
    std::array<std::uint32_t, 16> m{};
    for (std::size_t i = 0; i < 64; ++i) {
      const std::size_t pos = chunk_start + i;
      std::uint8_t byte = 0;
      if (pos < input.size()) {
        byte = static_cast<std::uint8_t>(input[pos]);
      } else if (pos == input.size()) {
        byte = 0x80;
      } else if (pos >= padded_size - 8) {
        byte = static_cast<std::uint8_t>(bit_length >> ((pos - (padded_size - 8)) * 8U));
      }
      m[i / 4] |= static_cast<std::uint32_t>(byte) << ((i % 4) * 8U);
    }

    std::uint32_t a = a0;
    std::uint32_t b = b0;
    std::uint32_t c = c0;
    std::uint32_t d = d0;
    for (std::uint32_t i = 0; i < 64; ++i) {
      std::uint32_t f = 0;
      std::uint32_t g = 0;
      if (i < 16) {
        f = (b & c) | (~b & d);
        g = i;
      } else if (i < 32) {
        f = (d & b) | (~d & c);
        g = (5 * i + 1) % 16;
      } else if (i < 48) {
        f = b ^ c ^ d;
        g = (3 * i + 5) % 16;
      } else {
        f = c ^ (b | ~d);
        g = (7 * i) % 16;
      }
      f = f + a + k[i] + m[g];
      a = d;
      d = c;
      c = b;
      b = b + std::rotl(f, static_cast<int>(shifts[i]));
    }
    a0 += a;
    b0 += b;
    c0 += c;
    d0 += d;
  }

  std::array<std::uint8_t, 16> digest{};
  const std::array<std::uint32_t, 4> words = {a0, b0, c0, d0};
  for (std::size_t i = 0; i < 16; ++i) {
    digest[i] = static_cast<std::uint8_t>(words[i / 4] >> ((i % 4) * 8U));
  }
  return digest;
}

inline std::uint64_t md5_first8_le(std::string_view input) {
  const auto digest = md5(input);
  std::uint64_t value = 0;
  for (int i = 7; i >= 0; --i) {
    value = (value << 8U) | digest[static_cast<std::size_t>(i)];
  }
  return value;
}

}  // namespace inf::test
