#pragma once

#include <cstdint>

namespace inf::core {

// Canonical FNV-1a-64 over little-endian byte streams — the golden-hash
// primitive used by the determinism harness. Not a quality hash; a
// fingerprint for byte-identical comparison across platforms.
class GoldenHash {
 public:
  void feed(std::uint64_t value) {
    for (int i = 0; i < 8; ++i) {
      hash_ ^= (value >> (i * 8)) & 0xFFU;
      hash_ *= 0x100000001B3ULL;
    }
  }
  std::uint64_t value() const { return hash_; }

 private:
  std::uint64_t hash_ = 0xCBF29CE484222325ULL;
};


}  // namespace inf::core
