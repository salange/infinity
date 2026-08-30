#pragma once

#include <array>
#include <cstdint>

namespace inf::det {

// Philox-4x64-10 (Salmon, Moraes, Dror, Shaw: "Parallel Random Numbers:
// As Easy as 1, 2, 3", SC'11). Constants and round structure follow the
// reference implementation (Random123); validated against its published
// known-answer vectors in tests/test_philox.cpp.
//
// This is the project's only source of randomness: a pure function
// (key, counter) -> 256 bits. Never sequential, never stateful.

struct PhiloxKey {
  std::uint64_t k0{0};
  std::uint64_t k1{0};

  friend bool operator==(const PhiloxKey&, const PhiloxKey&) = default;
};

using PhiloxCounter = std::array<std::uint64_t, 4>;
using PhiloxOutput = std::array<std::uint64_t, 4>;

PhiloxOutput philox4x64_10(const PhiloxKey& key, const PhiloxCounter& counter);

}  // namespace inf::det
