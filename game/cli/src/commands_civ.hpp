#pragma once

#include "core/seed.hpp"

namespace inf::cli {

// T0020: civilization inspection. All commands take the home galaxy of
// the seed; positions are galactocentric light-years, "here" = the home
// system.
int cmd_civ_races(const core::Seed128& seed, const double* at_ly);
int cmd_hash_civ();

}  // namespace inf::cli
