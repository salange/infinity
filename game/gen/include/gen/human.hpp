#pragma once

#include <cstdint>
#include <vector>

#include "core/key.hpp"
#include "core/seed.hpp"
#include "gen/civilization.hpp"

namespace inf::gen {

// T0020 WP2: humans and their enclaves (design sections 9 and 11.1).
//
// human/v1 (K_galaxy of the home galaxy): humanity is the exception on
// every axis — fixed home (the default body), constants instead of draws
// for the spread model, one race across most of the home galaxy through
// recovered Precursor gate technology, the most developed living race,
// creators of the two android factions. Only the faction NAMES and
// centres are drawn.
//
// human-enclaves/v1 (K_galaxy of any galaxy): with probability 0.30 a
// non-home galaxy of the home cluster holds 1-3 stranded beachheads —
// human sources with stranded parameters (speed x0.1, settle x0.3,
// reproduction x0.5, small reach, capped levels) and a DEAD gate partner
// in the home galaxy. Data only in v1; travel is a later ticket.

// Human spread constants (design section 9), calibrated for a 50 000 ly
// disc radius and scaled by the actual home galaxy's radius so the
// "72 % of the disc reached at launch, thin at the fringe" promise holds
// for every home galaxy size (seed 83's is a 9 756 ly dwarf).
inline constexpr double kHumanReferenceRadiusLy = 50'000.0;
inline constexpr double kHumanSpeedRefLyPerYear = 5'300.0;
inline constexpr double kHumanFalloffRefLy = 18'000.0;
inline constexpr double kHumanSettleProb = 0.6;
inline constexpr double kHumanReproduction = 1.0;
inline constexpr int kHumanPeakLevel = 7;
inline constexpr int kHumanHomeLevel = 6;
inline constexpr double kHumanDomeAffinity = 0.4;
inline constexpr double kAndroidFactionStartYears = 4.0;  // after t_0_human

// The human race of the home galaxy: sources[0] = the default body's
// system at t_0_human; factions per design section 9.
Race human_race(const core::Key& home_galaxy_key, const GalaxyParams& galaxy);

struct HumanEnclave {
  Source source;              // stranded beachhead in THIS galaxy
  Dir3 gate_partner_m;        // dead gate in the home galaxy (galactocentric)
  std::uint32_t index{0};
};

// Enclaves of one galaxy (empty for the home galaxy itself). The galaxy
// is identified by its cluster cell and index; the home cluster is
// (0,0,0), the home galaxy index 0.
std::vector<HumanEnclave> human_enclaves(const core::Seed128& seed, std::int64_t cx,
                                         std::int64_t cy, std::int64_t cz,
                                         std::uint32_t galaxy_index);

// The human race as seen from a non-home galaxy: humanity's constants
// with the enclave beachheads as its only sources (empty sources = no
// humans there).
Race human_race_in_galaxy(const core::Seed128& seed, std::int64_t cx, std::int64_t cy,
                          std::int64_t cz, std::uint32_t galaxy_index,
                          const GalaxyParams& galaxy);

// Dead gates in the home galaxy: one per enclave of every other galaxy in
// the home cluster (kind Wormhole on the galaxy's deepspace/v1 axis —
// data only). Mutual with human_enclaves by construction.
struct WormholeGate {
  Dir3 position_m;               // in the home galaxy
  std::uint32_t partner_galaxy;  // index in the home cluster
  std::uint32_t partner_enclave;
  Dir3 partner_position_m;       // the beachhead in the partner galaxy
  bool dead{true};
};
std::vector<WormholeGate> home_galaxy_gates(const core::Seed128& seed);

}  // namespace inf::gen
