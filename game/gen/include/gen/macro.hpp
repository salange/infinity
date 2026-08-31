#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "core/det/real.hpp"
#include "core/key.hpp"
#include "gen/geo.hpp"
#include "gen/names.hpp"

namespace inf::gen {

// macro/v1 (terrain-stack v2 WP1): continents, ocean basins, and the
// solved sea level. Classic sphere faulting — a seeded list of great-
// circle faults, each raising one side through a smooth sigmoid:
//
//   h(dir) = sum_i a_i * s( k_i * (dot(dir, n_i) - c_i) ),  s(x)=x/sqrt(1+x^2)
//
// standardized to mean 0 / sigma 1 over a fixed direction set. The field
// is DIMENSIONLESS; planet-params converts it to metres via a per-pattern
// amplitude expressed as a fraction of the radius (budget rule in the
// brief section 13 — never metres, or giants inherit 637 km tuning).
//
// Continent count is carried by the amplitude spectrum: each fault's
// sharpness k is drawn from a per-pattern range and its amplitude decays
// as k^-beta, so few broad faults dominate a Supercontinent while an
// Archipelago spreads energy into many narrow ones.
//
// Land/water is DELIBERATELY NOT part of the pattern (T0015 decision,
// 2026-08-31): the water inventory is an independent draw in
// planet-params, and the sea level is SOLVED as the elevation quantile at
// (1 - land_fraction) — which is what makes the land fraction actually
// match its target instead of being an uncorrelated random draw.
enum class MacroPattern : std::uint8_t {
  Supercontinent = 0,  // one dominant mass + fragments
  FewContinents = 1,   // 2-4 masses (Earth today)
  Archipelago = 2,     // many small masses and arcs
  Fractured = 3,       // high-amplitude, many faults, extreme relief
};

const char* to_string(MacroPattern pattern);

// Macro elevation sigma as a fraction of planet radius, per pattern.
det::Real macro_amplitude_fraction(MacroPattern pattern);

class MacroField {
 public:
  // Derives everything from K_body via derive_named(body, "macro/v1").
  explicit MacroField(const core::Key& body_entity_key);

  MacroPattern pattern() const { return pattern_; }

  // Standardized macro elevation at a direction (mean 0, sigma ~1 over
  // the sphere): the direct fault sum. O(fault count) — use the lattice
  // path for per-voxel work.
  det::Real value(const Dir3& unit_dir) const;

  // Canonical band-limited value: the fault sum evaluated on a FIXED
  // lod-7 uv lattice per cube face (finer than the province lattice so
  // continental margins stay reasonably crisp — T0015 decision) and
  // bilinearly interpolated. Bit-exact for every chunk at every lod.
  static constexpr std::uint32_t kMacroLatticeLod = 7;
  static constexpr std::uint32_t kMacroLatticeCells = 1U << kMacroLatticeLod;
  det::Real lattice_value(std::uint8_t face, std::uint32_t ci, std::uint32_t cj) const;
  using Cache = std::unordered_map<std::uint64_t, det::Real>;
  det::Real canonical_value(const FaceUV& face_uv, Cache* cache = nullptr) const;

  // The solved DIMENSIONLESS sea level: the quantile of the macro field
  // at (1 - land_fraction) over the fixed solve-direction set (6x16x16).
  // land >= 0.999 returns a sentinel below the global minimum (dry
  // world); land <= 0.001 one above the maximum (global ocean).
  det::Real solve_sea_level(det::Real land_fraction) const;

 private:
  struct Fault {
    Dir3 normal;
    det::Real offset;
    det::Real sharpness;
    det::Real amplitude;
  };

  det::Real raw_value(const Dir3& unit_dir) const;

  MacroPattern pattern_{MacroPattern::FewContinents};
  std::vector<Fault> faults_;
  det::Real mean_{0.0};
  det::Real inv_sigma_{1.0};
  std::vector<det::Real> solve_sorted_;  // standardized, ascending
};

}  // namespace inf::gen
