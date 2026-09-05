#pragma once
// Deterministic randomness for the demo, on the engine's Philox key
// substrate (design/seeding-hierarchy.md): every generator gets a key
// derived from its parent by name/index and draws counter-indexed values.
#include <algorithm>
#include <cstdint>
#include <string>

#include "core/det/philox.hpp"
#include "core/key.hpp"
#include "core/seed.hpp"

namespace cb {

// A counter RNG under one key: draw i yields a fresh 256-bit block.
class Rng {
 public:
  explicit Rng(const inf::core::Key& key) : key_(key) {}
  Rng child(std::uint32_t index) const {
    return Rng(inf::core::derive_child(key_, static_cast<inf::core::KindId>(0x7000u), index));
  }
  Rng child(std::uint32_t kind, std::int64_t x, std::int64_t y = 0, std::int64_t z = 0) const {
    return Rng(inf::core::derive_child(key_, static_cast<inf::core::KindId>(0x7100u + kind), x, y, z));
  }
  // Uniform in [0, 1).
  float next() {
    if (used_ == 4) {
      refill();
    }
    const std::uint64_t v = block_[used_++];
    return static_cast<float>(v >> 40) * (1.0f / 16777216.0f);
  }
  float range(float lo, float hi) { return lo + (hi - lo) * next(); }
  int irange(int lo, int hi_inclusive) {  // inclusive
    const int n = hi_inclusive - lo + 1;
    return lo + std::min(n - 1, static_cast<int>(next() * static_cast<float>(n)));
  }
  bool chance(float p) { return next() < p; }
  const inf::core::Key& key() const { return key_; }

 private:
  void refill() {
    block_ = inf::core::draw_point(key_, static_cast<inf::core::ChannelId>(1), counter_, 0, 0);
    ++counter_;
    used_ = 0;
  }
  inf::core::Key key_;
  inf::det::PhiloxOutput block_{};
  std::int64_t counter_{0};
  int used_{4};
};

inline Rng root_rng(const std::string& seed_text) {
  const auto seed = inf::core::parse_seed(seed_text);
  return Rng(inf::core::universe_key(seed.value_or(inf::core::Seed128{0, 0x83})));
}

// Stateless hash → [0,1) for shader-mirrored decisions (cosmetic).
inline float hash01(std::uint32_t x, std::uint32_t y, std::uint32_t z) {
  std::uint32_t h = x * 0x8da6b343u ^ y * 0xd8163841u ^ z * 0xcb1ab31fu;
  h ^= h >> 13;
  h *= 0x5bd1e995u;
  h ^= h >> 15;
  return static_cast<float>(h & 0xffffffu) / 16777216.0f;
}

}  // namespace cb
