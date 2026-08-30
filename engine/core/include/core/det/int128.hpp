#pragma once

#include <cstdint>

#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#endif

namespace inf::det {

// 128-bit product of two u64, split into high/low words. This is the one
// widening primitive Philox and fixed64 arithmetic are built on; both
// implementations below compute the identical mathematical result.
struct Mul128 {
  std::uint64_t hi;
  std::uint64_t lo;
};

inline Mul128 mul_64x64(std::uint64_t a, std::uint64_t b) {
#if defined(_MSC_VER) && !defined(__clang__)
  Mul128 result{};
  result.lo = _umul128(a, b, &result.hi);
  return result;
#else
  const unsigned __int128 product =
      static_cast<unsigned __int128>(a) * static_cast<unsigned __int128>(b);
  return Mul128{static_cast<std::uint64_t>(product >> 64), static_cast<std::uint64_t>(product)};
#endif
}

}  // namespace inf::det
