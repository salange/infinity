#pragma once

#include <cstdint>
#include <unordered_map>

#include "core/key.hpp"
#include "gen/geo.hpp"
#include "gen/planet.hpp"
#include "gen/provinces.hpp"

namespace inf::gen {

// features/v1 (terrain-stack v2 WP5): bounded surface-anchored entities.
// A jittered cube-sphere cell grid (finer than provinces); each cell hosts
// 0..kMaxPerCell features, each an entity keyed by (face, ci, cj, index)
// with its own origin direction, bounding radius and parameters. A query
// gathers only the cells within a fixed probe stencil and rejects by
// bounding radius, so cost outside any feature is a handful of cell
// lookups and no noise — and a body with no features pays one branch.
//
// First client: impact craters (bowl + rim + ejecta height offsets).
// caves/v1 (WP7) reuses the same machinery on a coarser grid.
class FeatureField {
 public:
  FeatureField(const core::Key& body_key, const PlanetParams& planet);

  // True when this body hosts any features at all (crater density > 0).
  bool enabled() const { return enabled_; }

  // --- introspection (tests, dumps) --------------------------------------
  static constexpr int kMaxPerCell = 5;
  struct Crater {
    Dir3 center;           // unit direction of the crater centre
    det::Real bowl_chord;  // bowl radius as a chord on the unit sphere
    det::Real depth_m;     // bowl depth in metres (freshness applied)
    det::Real rim_frac;    // rim height as a fraction of depth
  };
  struct CellCraters {
    Crater craters[kMaxPerCell];
    int count{0};
  };
  CellCraters cell_craters(const CellId& cell) const;
  CellId cell_of(const Dir3& unit_dir) const;
  std::uint32_t cells_per_face() const { return cells_per_face_; }

  // Optional memo of cell contents keyed by packed cell id. Chunk sampling
  // passes one per chunk (neighbouring queries share almost all candidate
  // cells); the arithmetic is identical with or without it.
  using Cache = std::unordered_map<std::uint64_t, CellCraters>;

  // Summed feature height offset (metres) at a surface direction.
  det::Real height_offset_m(const Dir3& unit_dir, Cache* cache = nullptr) const;

 private:
  core::Key features_key_;
  ProvinceField provinces_;  // own instance: archetype drives crater density
  PlanetType type_;
  std::uint32_t cells_per_face_{0};
  det::Real radius_m_;
  det::Real age_;  // [0.35, 1] — old airless bodies saturate with craters
  bool enabled_{false};
};

}  // namespace inf::gen
