#pragma once

#include <cstdint>
#include <unordered_map>

#include "core/key.hpp"
#include "gen/geo.hpp"
#include "gen/planet.hpp"
#include "gen/provinces.hpp"

namespace inf::gen {

// caves/v1 (terrain-stack v2 WP7): bounded cave-system entities on the
// PROVINCE-resolution cell grid (T0015 Q4 resolution). Each hosted cell
// owns one system keyed by (face, ci, cj): an anchor below the local
// surface, a bounded random-walk skeleton of tunnel/chamber nodes, and
// optionally a mouth capsule extended through the surface so the
// entrance connects by construction (the NMS lesson: break-through is
// authored, never hoped for). The system's density contribution is a
// smooth-min union of capsule SDFs composed into the terrain density —
// outside every bounding sphere the field is provably unchanged and the
// cost is a handful of cell probes.
//
// Shallow NMS-style dimensioning (T0015 Q5 resolution): anchors
// 30-150 m down, bound <= ~400 m, so the streamer's depth budget stays
// ~450 m for hosting columns only.
class CaveField {
 public:
  CaveField(const core::Key& body_key, const PlanetParams& planet);

  bool enabled() const { return enabled_; }

  static constexpr int kMaxNodes = 18;
  static constexpr double kBoundCapM = 400.0;   // hard cap on a system bound
  static constexpr double kDepthBudgetM = 450.0;  // streamer shells below surface
  static constexpr double kSminM = 7.0;         // capsule union blend radius

  struct System {
    bool hosted{false};
    bool has_mouth{false};
    int node_count{0};
    Dir3 anchor_dir{};             // unit direction of the anchor
    Dir3 nodes[kMaxNodes];         // planet-local positions, metres
    det::Real node_r[kMaxNodes];   // tunnel/chamber radii, metres
    Dir3 bound_center{};           // planet-local metres
    det::Real bound_m{};           // covers every capsule + the blend
    Dir3 mouth_top{};              // capsule end above the surface
    det::Real mouth_r{};
  };

  // Cheap host test (one draw) — lets callers skip surface evaluation
  // for the common empty cell.
  bool hosted(const CellId& cell) const;

  // Anchor direction of a (hosted or not) cell — callers evaluate the
  // surface radius there before asking for the full system.
  Dir3 anchor_dir(const CellId& cell) const;

  // Deterministic full construction. surface_r_anchor is the terrain
  // surface radius (metres) at anchor_dir(cell); surface_r_mouth the
  // radius at the promoted top node's direction (pass surface_r_anchor
  // again if unknown — the mouth margin absorbs the difference only for
  // gentle terrain, so callers that can afford it should supply it).
  System build_system(const CellId& cell, det::Real surface_r_anchor,
                      det::Real surface_r_mouth) const;
  // Direction of the node a mouth would be promoted from (for the
  // caller's second surface evaluation). Valid for hosted cells.
  Dir3 mouth_probe_dir(const CellId& cell, det::Real surface_r_anchor) const;

  // Candidate cells whose system could reach a query at unit_dir: the
  // owner cell plus its across-boundary neighbours (bounds are a small
  // fraction of a province cell — asserted at construction).
  static constexpr int kMaxCandidates = 4;
  int candidates(const Dir3& unit_dir, CellId out[kMaxCandidates]) const;

  // Cave SDF (metres, negative inside a tunnel) of one system at a
  // planet-local position. Callers smooth-min it into the terrain
  // density. Positions outside the bound return a large positive value.
  static det::Real system_sdf(const System& system, const Dir3& position_m);

  // Radial intervals (absolute radius, metres) where this system's
  // volume crosses the radial line through unit_dir — the streamer
  // meshes exactly these shells (WP7 Blocker A). Conservative node/mouth
  // sphere cover; returns the count written (at most max_out).
  static int radial_intervals(const System& system, const Dir3& unit_dir, double* lo_out,
                              double* hi_out, int max_out);

  CellId cell_of(const Dir3& unit_dir) const;
  std::uint32_t cells_per_face() const { return cells_per_face_; }

  using Cache = std::unordered_map<std::uint64_t, System>;

 private:
  core::Key caves_key_;
  ProvinceField provinces_;
  PlanetType type_;
  std::uint32_t cells_per_face_{0};
  det::Real radius_m_;
  det::Real probe_uv_;  // candidate probe offset in uv units
  bool enabled_{false};
};

// Polynomial smooth minimum (Quilez): C1 union of SDFs.
inline det::Real smin(det::Real a, det::Real b, det::Real k) {
  const det::Real h = det::clamp(det::Real(0.5) + det::Real(0.5) * (b - a) / k,
                                 det::Real(0.0), det::Real(1.0));
  return b + (a - b) * h - k * h * (det::Real(1.0) - h);
}

}  // namespace inf::gen
