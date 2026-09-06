#pragma once

#include <cstdint>
#include <string>

#include "core/seed.hpp"

namespace inf::gen {

// Deterministic-core golden script (M1): exercises every counter layout,
// the cheap mixer and a Fixed64 op sequence. Lives in the game because it
// uses the game's frozen name/kind/channel registries.
std::uint64_t hash_core_script(const core::Seed128& seed);
std::string hash_core_report();


// Golden fingerprint of the M2 pipeline for one seed x forced type:
// planet params, the full province table, and blended samples on a fixed
// set of integer-lattice directions (no trig — fully deterministic).
std::uint64_t hash_planet_script(const core::Seed128& seed, std::uint32_t forced_type);

// Multi-seed x all-types report compared by ci/check.sh
// (tests/goldens/hash-planet.txt).
std::uint64_t hash_surface_script(const core::Seed128& seed, std::uint32_t forced_type);
std::string hash_planet_report();

// M3: density-grid fingerprints for a fixed chunk set (surface, elevated
// shell, other faces, coarse lod, deep-interior chunk crossing the core).
// The hashed artifact is the mesh INPUT, never the mesh (T0005).
std::string hash_density_report();

// M7/M8: effective-density fingerprints with a scripted edit sequence
// (dig, overlapping dig, refill, deep carve) — locks the CSG fold order,
// the fixed64 edit encoding, and the edit-aware ground query.
std::string hash_edits_report();

// T0020: civilization fingerprints (race registry, later owners, states,
// plans, lots) at fixed ManualClock offsets from the launch reference.
// Append-only sections so earlier lines stay byte-identical.
std::string hash_civ_report();

}  // namespace inf::gen
