#pragma once

#include <cstdint>
#include <string>

#include "core/seed.hpp"

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

// Runs the fixed M1 derivation/arithmetic script for one seed and returns
// its fingerprint. The script exercises every counter layout (seeding spec
// section 9), the cheap mixer, and a Fixed64 op sequence. Any change to
// core's deterministic behavior changes these values — goldens live in
// tests/goldens/hash-core.txt.
std::uint64_t hash_core_script(const Seed128& seed);

// Formats the full multi-seed report compared by ci/check.sh.
std::string hash_core_report();

}  // namespace inf::core
