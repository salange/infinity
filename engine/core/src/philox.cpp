#include "core/det/philox.hpp"

#include "core/det/int128.hpp"

namespace inf::det {

namespace {

// Multipliers and Weyl key-schedule constants from the paper / Random123.
constexpr std::uint64_t kMul0 = 0xD2E7470EE14C6C93ULL;
constexpr std::uint64_t kMul1 = 0xCA5A826395121157ULL;
constexpr std::uint64_t kWeyl0 = 0x9E3779B97F4A7C15ULL;  // golden ratio
constexpr std::uint64_t kWeyl1 = 0xBB67AE8584CAA73BULL;  // sqrt(3)-1

inline PhiloxCounter round(const PhiloxCounter& ctr, std::uint64_t k0, std::uint64_t k1) {
  const Mul128 product0 = mul_64x64(ctr[0], kMul0);
  const Mul128 product1 = mul_64x64(ctr[2], kMul1);
  return PhiloxCounter{
      product1.hi ^ ctr[1] ^ k0,
      product1.lo,
      product0.hi ^ ctr[3] ^ k1,
      product0.lo,
  };
}

}  // namespace

PhiloxOutput philox4x64_10(const PhiloxKey& key, const PhiloxCounter& counter) {
  PhiloxCounter state = counter;
  std::uint64_t k0 = key.k0;
  std::uint64_t k1 = key.k1;
  state = round(state, k0, k1);
  for (int i = 1; i < 10; ++i) {
    k0 += kWeyl0;
    k1 += kWeyl1;
    state = round(state, k0, k1);
  }
  return state;
}

}  // namespace inf::det
