#include <doctest/doctest.h>

#include "core/seed.hpp"

using inf::core::parse_seed;
using inf::core::Seed128;
using inf::core::to_hex;

TEST_CASE("seed: canonical round-trip") {
  const auto seed = parse_seed("00112233445566778899aabbccddeeff");
  REQUIRE(seed.has_value());
  CHECK(seed->hi == 0x0011223344556677ULL);
  CHECK(seed->lo == 0x8899aabbccddeeffULL);
  CHECK(to_hex(*seed) == "00112233445566778899aabbccddeeff");
}

TEST_CASE("seed: 0x prefix and case insensitivity") {
  const auto a = parse_seed("0xDEADBEEF");
  const auto b = parse_seed("deadbeef");
  REQUIRE(a.has_value());
  REQUIRE(b.has_value());
  CHECK(*a == *b);
  CHECK(to_hex(*a) == "000000000000000000000000deadbeef");
}

TEST_CASE("seed: short input is left-zero-extended") {
  const auto seed = parse_seed("1");
  REQUIRE(seed.has_value());
  CHECK(seed->hi == 0);
  CHECK(seed->lo == 1);
}

TEST_CASE("seed: full-width value") {
  const auto seed = parse_seed("ffffffffffffffffffffffffffffffff");
  REQUIRE(seed.has_value());
  CHECK(seed->hi == ~0ULL);
  CHECK(seed->lo == ~0ULL);
}

TEST_CASE("seed: invalid inputs rejected") {
  CHECK_FALSE(parse_seed("").has_value());
  CHECK_FALSE(parse_seed("0x").has_value());
  CHECK_FALSE(parse_seed("xyz").has_value());
  CHECK_FALSE(parse_seed("123g").has_value());
  CHECK_FALSE(parse_seed("000000000000000000000000000000000").has_value());  // 33 digits
  CHECK_FALSE(parse_seed(" 1").has_value());
}
