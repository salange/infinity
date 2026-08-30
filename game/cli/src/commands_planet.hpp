#pragma once

#include "core/seed.hpp"

namespace inf::cli {

int cmd_dump_planet(const core::Seed128& seed, const char* type_text);
int cmd_province_map(const core::Seed128& seed, const char* type_text, const char* out_prefix);
int cmd_hash_planet();

}  // namespace inf::cli
