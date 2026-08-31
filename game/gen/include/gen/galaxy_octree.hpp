#pragma once

#include <cstdint>
#include <vector>

#include "core/key.hpp"
#include "gen/galaxy.hpp"

namespace inf::gen {

// T0017 WP3: the galaxy's systems octree (Stellar-Forge style). The
// galaxy bounding cube is subdivided while a cell's EXPECTED system
// count — a quadrature of GalaxyDensity::stars, the one shared model —
// exceeds the leaf occupancy; occupied leaves each hold one star system
// whose key is its cell address (nothing is stored, ever). Positions,
// counts and star draws are all pure functions of the cell key.
//
// Cells: level L splits the root cube into 2^L per axis; coordinates are
// non-negative, x/y/z in [0, 2^L), cell (x,y,z,L) spanning
// [root_min + x*s, ...+ (x+1)*s) with s = root_size / 2^L. Children of
// (x,y,z,L) are (2x+dx, 2y+dy, 2z+dz, L+1). A cell is meaningful only if
// reached by descent (every ancestor was dense enough to subdivide) —
// the same valid-by-construction contract the planet slots use.
class GalaxyOctree {
 public:
  GalaxyOctree(const core::Key& galaxy_entity_key, const GalaxyParams& params);

  struct CellId {
    std::int64_t x{0}, y{0}, z{0};
    std::int32_t level{0};
  };

  static constexpr int kMaxLevel = 18;             // ~0.4 ly cells on a 100k-ly galaxy
  static constexpr double kLeafOccupancy = 1.5;    // subdivide above this many systems
  static constexpr double kMeanSystemMassSuns = 0.7;

  double root_size_m() const { return root_size_m_; }
  double cell_size_m(std::int32_t level) const;
  Dir3 cell_min_m(const CellId& cell) const;       // corner position
  Dir3 cell_center_m(const CellId& cell) const;

  // Expected stellar mass / system count in a cell: a midpoint quadrature
  // of the shared density model. Closed-form-ish, never instantiates
  // anything.
  det::Real expected_mass_suns(const CellId& cell) const;
  det::Real expected_systems(const CellId& cell) const;

  bool is_leaf(const CellId& cell) const;
  // Occupied leaves hold exactly one star system (P = 1 - exp(-lambda),
  // drawn from the cell key). The galactic core saturates: cells at
  // kMaxLevel with lambda >> 1 still hold one — the unresolved core is
  // rendered from the density integral, not from systems.
  bool occupied(const CellId& cell) const;
  Dir3 system_position_m(const CellId& cell) const;  // jittered inside the cell

  // Descend from the root to the leaf containing p (galactocentric m).
  CellId leaf_at(const Dir3& p_m) const;
  // All OCCUPIED leaves whose system position lies within the ball.
  // Descent prunes empty branches; results are appended (up to max_out).
  void systems_in_ball(const Dir3& center_m, det::Real radius_m, std::size_t max_out,
                       std::vector<CellId>* out) const;

  // --- the T0018 sky API (design constraint, not an afterthought) --------
  // Expected count of stars in this cell brighter than an ABSOLUTE
  // magnitude limit, Poisson-drawn from (cell key, quantised limit).
  // Works on ANY cell at any level and must NOT instantiate stars: the
  // count is the cell's expected system count times the closed-form
  // luminosity-function fraction brighter than the limit.
  std::uint32_t luminous_count(const CellId& cell, det::Real abs_mag_limit) const;

  struct StarSummary {
    Dir3 position_m;        // galactocentric
    det::Real abs_mag;      // absolute magnitude, sampled from the bright tail
    det::Real temperature_k;
  };
  // Instantiate ONLY the i-th star brighter than the limit.
  StarSummary luminous_star(const CellId& cell, det::Real abs_mag_limit,
                            std::uint32_t index) const;

  // Closed-form luminosity-function fraction of stars brighter than
  // (absolute magnitude < ) the limit. Exposed for tests and calibration.
  static det::Real luminous_fraction(det::Real abs_mag_limit);

  const GalaxyDensity& density() const { return density_; }
  core::Key cell_key(const CellId& cell) const;

 private:
  core::Key systems_key_;
  GalaxyDensity density_;
  double root_size_m_{0.0};
  double root_min_m_{0.0};  // = -root_size/2 on every axis
};

// The home (default) system's galactocentric position: mid-disc, the
// Sun-like vantage the sky is designed around. Deterministic function of
// the galaxy params only.
Dir3 home_system_position_m(const GalaxyParams& params);

}  // namespace inf::gen
