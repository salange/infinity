#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "core/key.hpp"
#include "gen/civ_time.hpp"
#include "gen/civ_types.hpp"
#include "gen/galaxy.hpp"
#include "gen/galaxy_octree.hpp"
#include "gen/system.hpp"
#include "gen/universe.hpp"

namespace inf::gen {

// T0020 WP1: galaxy habitation and the race registry
// (design/civilization-and-settlements.md sections 5-7, 11).
//
//   civilization/v1  (K_galaxy)          how many races this galaxy ever
//                                        started, the type tilt, L_civ
//   races/v1         (K_galaxy, cell)    home worlds per MACRO CELL —
//                                        Poisson-thinned by stellar mass;
//                                        each home = one generated race
//   race-factions/v1 (K_race)            the race's factions
//
// A macro cell is an octree cell at level L_civ (~1 500 ly). A race may
// spread at most kReach cell widths, so every race that can possibly be
// present at a point lives in the (2*kReach+1)^3 cells around it — a
// bounded, cached block; no global knowledge, no visit to the home.
// Everything is a pure function of keys; nothing is stored.

struct CivilizationParams {
  std::uint32_t race_count{0};  // N: races ever started (extinct included), humans not counted
  bool teeming{false};          // the rare 25-100 galaxy
  std::int32_t l_civ{6};        // macro-cell octree level
  double cell_width_ly{1500.0};
  double type_weight[static_cast<int>(RaceType::Count)]{};  // tilted archetype weights
  double extinction_tilt{0.5};  // old galaxies: more extinct races
};

// One draw per galaxy. force_home_minimum applies the home-galaxy rule
// (N >= 6, the home_galaxy_params idiom): a player's first galaxy holds
// several alien races to meet.
CivilizationParams derive_civilization(const core::Key& galaxy_entity_key,
                                       const GalaxyParams& galaxy,
                                       bool force_home_minimum = false);

using MacroCell = GalaxyOctree::CellId;  // always at level l_civ

// What planets/v1 needs to know when a system is a race home (design
// section 6.4): the habitable slot nearest the race's preferred flux gets
// its surface type FORCED to the race's habitat, and organics get a full
// biosphere. Consulted by generate_system through the override argument.
struct SystemOverride {
  PlanetType habitat{PlanetType::EarthLike};
  double preferred_flux{1.0};
  bool force_biosphere{true};
  core::Key race_key;
};

struct Race {
  core::Key key;              // the race's entity key (its home slot key)
  MacroCell cell;             // hosting macro cell
  int index{0};               // home index within the cell
  SystemCell home_system;     // the octree leaf hosting the home world
  bool void_home{false};      // no occupied leaf found: the race does not exist
  RaceParams params;
  std::vector<FactionParams> factions;
};

class RaceRegistry {
 public:
  // Reach in macro cells (design section 6.3): 2 chosen, 3 feasible —
  // one constant, and the block size follows.
  static constexpr int kReach = 2;
  static constexpr int kBlockCells = (2 * kReach + 1) * (2 * kReach + 1) * (2 * kReach + 1);
  static constexpr int kMaxHomesPerCell = 9;

  RaceRegistry(const core::Key& galaxy_entity_key, const GalaxyParams& galaxy,
               const CivilizationParams& civ);

  const CivilizationParams& civ() const { return civ_; }
  const GalaxyOctree& octree() const { return octree_; }
  const GalaxyParams& galaxy() const { return galaxy_; }

  // Galactocentric position of any system: the home system by rule, every
  // other from its octree cell.
  Dir3 system_position_m(const SystemCell& cell) const;
  MacroCell macro_cell_of(const Dir3& p_m) const;
  bool valid_cell(const MacroCell& cell) const;

  // Home worlds hosted by one macro cell (0..9, Poisson-thinned by the
  // cell's stellar mass). Pure function of the cell.
  int home_count(const MacroCell& cell) const;
  // Full race for one home slot (void_home when placement failed).
  Race home(const MacroCell& cell, int index) const;

  // Every non-void race whose home lies in the block around `center` —
  // sorted by race key, cached for the last block queried. Humans (WP2)
  // and enclave sources are appended by the owner resolution, not here.
  const std::vector<Race>& races_around(const MacroCell& center) const;
  const std::vector<Race>& races_around(const Dir3& p_m) const {
    return races_around(macro_cell_of(p_m));
  }

  // Is this system some race's home? Bounded lookup of the <= 9 homes in
  // the system's own macro cell (design section 6.4).
  std::optional<SystemOverride> home_override(const SystemCell& cell) const;

  // The key of a macro cell under races/v1 (tests, goldens).
  core::Key cell_key(const MacroCell& cell) const;

 private:
  core::Key races_key_;
  GalaxyParams galaxy_;
  CivilizationParams civ_;
  GalaxyOctree octree_;
  Dir3 home_position_m_;
  mutable bool block_valid_{false};
  mutable MacroCell block_center_{};
  mutable std::vector<Race> block_;
};

// The override for the system planets/v1 consults, from a registry
// (nullopt for every system that is not a race home — the default
// seed-83 system is never one, asserted in tests).
inline std::optional<SystemOverride> race_home_override(const RaceRegistry& registry,
                                                        const SystemCell& cell) {
  return registry.home_override(cell);
}

// Race-type prior tables (also used by the colony layer).
struct RaceTypeInfo {
  double base_weight;
  Habitat habitat;
  double dome_prior;
  int tech_lo, tech_hi;
  std::uint8_t material_family;
  LayoutFamily layout_primary;
  LayoutFamily layout_secondary;
};
const RaceTypeInfo& race_type_info(RaceType type);

// Deterministic helpers shared by the civ layers.
namespace civ {
inline double u01(std::uint64_t word) {
  return static_cast<double>(word >> 11U) * 0x1.0p-53;
}
inline double uniform(std::uint64_t word, double lo, double hi) {
  return lo + (hi - lo) * u01(word);
}
inline std::uint32_t pick(std::uint64_t word, std::uint32_t count) {
  return static_cast<std::uint32_t>((word >> 32U) % count);
}
// Standard normal via Box-Muller on two words (deterministic kernels).
double normal01(std::uint64_t word_a, std::uint64_t word_b);
// Log-uniform in [lo, hi].
double log_uniform(std::uint64_t word, double lo, double hi);
// Poisson draw (inversion; fixed iteration bound).
std::uint32_t poisson(double lambda, std::uint64_t word_a, std::uint64_t word_b);
// Weighted pick over a table of doubles.
std::size_t weighted_pick(std::uint64_t word, const double* weights, std::size_t count);
}  // namespace civ

}  // namespace inf::gen
