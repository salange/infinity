#pragma once

#include <string>

#include "core/seed.hpp"

namespace inf::gen {

// Golden fingerprint of the M2 pipeline for one seed x forced type:
// planet params, the full province table, and blended samples on a fixed
// set of integer-lattice directions (no trig — fully deterministic).
std::uint64_t hash_planet_script(const core::Seed128& seed, std::uint32_t forced_type);

// Multi-seed x all-types report compared by ci/check.sh
// (tests/goldens/hash-planet.txt).
std::string hash_planet_report();

// M3: density-grid fingerprints for a fixed chunk set (surface, elevated
// shell, other faces, coarse lod, deep-interior chunk crossing the core).
// The hashed artifact is the mesh INPUT, never the mesh (T0005).
std::string hash_density_report();

}  // namespace inf::gen
