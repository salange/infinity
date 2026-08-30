#include <doctest/doctest.h>

#include "core/det/mix.hpp"

using inf::det::mix64;

TEST_CASE("mix64: frozen reference values (SplitMix64 finalizer / Mix13)") {
  CHECK(mix64(0x0000000000000000ULL) == 0x0000000000000000ULL);  // documented fixed point
  CHECK(mix64(0x0000000000000001ULL) == 0x5692161d100b05e5ULL);
  CHECK(mix64(0x0123456789abcdefULL) == 0xb2c058e4ebb5112cULL);
  CHECK(mix64(0xffffffffffffffffULL) == 0xb4d055fcf2cbbd7bULL);
}
