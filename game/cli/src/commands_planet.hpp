#pragma once

#include "core/seed.hpp"

namespace inf::cli {

int cmd_dump_planet(const core::Seed128& seed, const char* type_text);
int cmd_province_map(const core::Seed128& seed, const char* type_text, const char* out_prefix);
int cmd_hash_planet();
// T0015 verification tools: equirect land/ocean elevation map, and the
// land-fraction / pattern-distribution report across seeds.
int cmd_terrain_map(const core::Seed128& seed, const char* type_text, const char* out_prefix);
int cmd_macro_stats(int seed_count);
int cmd_terrain_stats(const core::Seed128& seed);

}  // namespace inf::cli
