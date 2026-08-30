#pragma once

#include <cstdint>

namespace inf::det {

// SplitMix64 finalizer (Stafford "Mix13"). The ONLY sanctioned cheap mixer
// (seeding spec section 1): permitted for high-volume, visually-consumed,
// single-value noise (lattice hashing, jitter, dither) and always fed as
// derived_key ^ mix64(coords) — never raw coordinates, never for key
// derivation (that is Philox's job).
//
// Beware: mix64(0) == 0 (fixed point). The XOR with a derived key is what
// guarantees nonzero entropy at the origin.
inline std::uint64_t mix64(std::uint64_t z) {
  z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31U);
}

}  // namespace inf::det
