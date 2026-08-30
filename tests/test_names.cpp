#include <doctest/doctest.h>

#include <set>

#include "core/names.hpp"
#include "support/md5.hpp"

TEST_CASE("name registry: reference MD5 sanity (RFC 1321 test vectors)") {
  // MD5("") = d41d8cd98f00b204e9800998ecf8427e -> first 8 bytes LE.
  CHECK(inf::test::md5_first8_le("") == 0x04b2008fd98c1dd4ULL);
  // MD5("abc") = 900150983cd24fb0d6963f7d28e17f72.
  CHECK(inf::test::md5_first8_le("abc") == 0xb04fd23c98500190ULL);
}

TEST_CASE("name registry: frozen ids match their strings (drift check)") {
  for (const auto& entry : inf::core::kNameRegistry) {
    CAPTURE(entry.name);
    CHECK(static_cast<std::uint64_t>(entry.id) == inf::test::md5_first8_le(entry.name));
  }
}

TEST_CASE("name registry: no collisions") {
  std::set<std::uint64_t> ids;
  for (const auto& entry : inf::core::kNameRegistry) {
    CHECK(ids.insert(static_cast<std::uint64_t>(entry.id)).second);
  }
}
