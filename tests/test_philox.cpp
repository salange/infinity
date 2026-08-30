#include <doctest/doctest.h>

#include "core/det/philox.hpp"

using inf::det::philox4x64_10;
using inf::det::PhiloxCounter;
using inf::det::PhiloxKey;
using inf::det::PhiloxOutput;

// Official Random123 known-answer vectors (tests/kat_vectors, line format:
// philox4x64 10 <ctr0..3> <key0..1> -> <out0..3>).
TEST_CASE("philox4x64-10: Random123 known-answer vectors") {
  SUBCASE("all zeros") {
    const PhiloxOutput out = philox4x64_10(PhiloxKey{0, 0}, PhiloxCounter{0, 0, 0, 0});
    CHECK(out[0] == 0x16554d9eca36314cULL);
    CHECK(out[1] == 0xdb20fe9d672d0fdcULL);
    CHECK(out[2] == 0xd7e772cee186176bULL);
    CHECK(out[3] == 0x7e68b68aec7ba23bULL);
  }
  SUBCASE("all ones") {
    const PhiloxOutput out = philox4x64_10(
        PhiloxKey{0xffffffffffffffffULL, 0xffffffffffffffffULL},
        PhiloxCounter{0xffffffffffffffffULL, 0xffffffffffffffffULL, 0xffffffffffffffffULL,
                      0xffffffffffffffffULL});
    CHECK(out[0] == 0x87b092c3013fe90bULL);
    CHECK(out[1] == 0x438c3c67be8d0224ULL);
    CHECK(out[2] == 0x9cc7d7c69cd777b6ULL);
    CHECK(out[3] == 0xa09caebf594f0ba0ULL);
  }
  SUBCASE("pi digits") {
    const PhiloxOutput out = philox4x64_10(
        PhiloxKey{0x452821e638d01377ULL, 0xbe5466cf34e90c6cULL},
        PhiloxCounter{0x243f6a8885a308d3ULL, 0x13198a2e03707344ULL, 0xa4093822299f31d0ULL,
                      0x082efa98ec4e6c89ULL});
    CHECK(out[0] == 0xa528f45403e61d95ULL);
    CHECK(out[1] == 0x38c72dbd566e9788ULL);
    CHECK(out[2] == 0xa5a1610e72fd18b5ULL);
    CHECK(out[3] == 0x57bd43b5e52b7fe6ULL);
  }
}

TEST_CASE("philox4x64-10: counter sensitivity") {
  const PhiloxKey key{1, 2};
  const PhiloxOutput a = philox4x64_10(key, PhiloxCounter{0, 0, 0, 0});
  const PhiloxOutput b = philox4x64_10(key, PhiloxCounter{1, 0, 0, 0});
  CHECK(a != b);
  // Same inputs, same outputs — pure function.
  CHECK(a == philox4x64_10(key, PhiloxCounter{0, 0, 0, 0}));
}
