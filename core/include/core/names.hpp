#pragma once

#include <cstdint>

namespace inf::core {

// Frozen layer-name registry (seeding spec section 9): name_id is the first
// 8 bytes, little-endian, of MD5(name string, UTF-8). Values are frozen
// here and verified against a reference MD5 in tests/test_names.cpp
// (drift + collision check). Append-only; a version bump is a NEW name.
enum class NameId : std::uint64_t {
  PlanetParamsV1 = 0x620c7e80903e60e7ULL,  // "planet-params/v1"
  ProvincesV1 = 0xe930f9d5ff800838ULL,     // "provinces/v1"
  TerrainV1 = 0xf9b59960daf8dc2eULL,       // "terrain/v1"
  MaterialV1 = 0x81f55ecfb63bb426ULL,      // "material/v1"
  TestV1 = 0x5f75b2f645a90e20ULL,          // "test/v1" (tests only)
};

// The registry as (name, id) pairs for the drift/collision test.
struct NameEntry {
  const char* name;
  NameId id;
};

inline constexpr NameEntry kNameRegistry[] = {
    {"planet-params/v1", NameId::PlanetParamsV1},
    {"provinces/v1", NameId::ProvincesV1},
    {"terrain/v1", NameId::TerrainV1},
    {"material/v1", NameId::MaterialV1},
    {"test/v1", NameId::TestV1},
};

}  // namespace inf::core
