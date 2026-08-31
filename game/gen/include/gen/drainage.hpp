#pragma once

#include <cstdint>
#include <vector>

#include "core/key.hpp"
#include "gen/geo.hpp"
#include "gen/macro.hpp"
#include "gen/planet.hpp"
#include "gen/provinces.hpp"

namespace inf::gen {

// drainage/v1 (terrain-stack v2 WP6): coarse planet-wide rivers and
// valleys — Derzapf et al. 2011, base phase only. The province
// representatives are the base mesh: classify each sea/land from
// macro/v1, grow rivers MOUTHS-FIRST in deterministic pseudo-random
// order (one river per mouth vertex, <=2 merges per land vertex) until
// the reachable continent is a spanning forest, then read river width
// and carve depth from the Strahler order the forest gives for free
// (T0015 Q3 resolution: no baked stream-power pass).
//
// The pointwise query never walks the graph: at build time every river
// segment is rasterized onto the province cell grid (with a one-cell
// margin), so a carve lookup is one owner-cell fetch plus a few
// point-to-segment distances. Flow direction is implied by the forest's
// parent orientation and never computed at runtime.
class DrainageField {
 public:
  DrainageField(const core::Key& body_key, const PlanetParams& planet);

  bool enabled() const { return enabled_; }

  struct Vertex {
    Dir3 dir{};                // representative direction (unit)
    std::int32_t parent{-1};   // downstream vertex index; -1 = sea/unconnected
    std::uint8_t order{0};     // Strahler order of the segment to parent
    bool sea{false};
  };
  const std::vector<Vertex>& vertices() const { return verts_; }

  // Valley carve depth (metres, >= 0) at a direction. above_sea_m is the
  // caller's current height above sea level — the carve never cuts the
  // valley floor below ~3 m above the sea.
  det::Real carve_m(const Dir3& unit_dir, det::Real above_sea_m) const;

  std::uint32_t cells_per_face() const { return n_; }

 private:
  std::uint32_t index_of(const CellId& cell) const {
    return (static_cast<std::uint32_t>(cell.face) * n_ + cell.ci) * n_ + cell.cj;
  }
  void build(const core::Key& body_key, const PlanetParams& planet);

  ProvinceField provinces_;
  MacroField macro_;
  core::Key drainage_key_;
  std::uint32_t n_{0};
  det::Real radius_m_;
  bool enabled_{false};
  std::vector<Vertex> verts_;
  // Per-cell list of river segments (indexed by the CHILD vertex, whose
  // parent link is the segment) that can reach queries in that cell.
  std::vector<std::vector<std::int32_t>> cell_segments_;
};

}  // namespace inf::gen
