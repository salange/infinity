#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "gen/geo.hpp"
#include "gen/planet.hpp"

namespace inf::gen {

// provinces/v1 (prototype-v0 spec section 4; T0004): the planet's
// non-uniformity mechanism. Jittered cube-sphere cells, each an owner-cell
// entity keyed by (face, ci, cj); per-cell archetype + terrain parameters
// drawn from the cell's key; a query direction blends the nearby cells'
// parameters with a finite-support kernel on chord distance, so the field
// is continuous by construction (weights reach exactly zero before a cell
// can leave the candidate stencil).

enum class Archetype : std::uint8_t {
  // EarthLike
  Flats = 0,
  RollingHills = 1,
  Alpine = 2,
  Canyon = 3,
  HighlandPlateau = 4,
  // Barren
  RegolithPlains = 5,
  Cratered = 6,
  Highlands = 7,
  // Desert
  Dunes = 8,
  Mesas = 9,
  Canyonlands = 10,
  // Ice
  GlacialShield = 11,
  CrevasseField = 12,
  RidgeField = 13,
};

const char* to_string(Archetype archetype);

struct CellId {
  std::uint8_t face{0};
  std::uint32_t ci{0};
  std::uint32_t cj{0};

  friend bool operator==(const CellId&, const CellId&) = default;
  friend auto operator<=>(const CellId&, const CellId&) = default;
};

struct ProvinceParams {
  Archetype archetype{Archetype::Flats};
  det::Real relief_amplitude_m;
  det::Real base_elevation_m;
  det::Real ruggedness;  // [0, 1]
  det::Real carving;     // [0, 1]
  std::uint32_t palette_shift{0};
};

// Blended (continuous) parameters at a direction, plus the dominant
// province for discrete uses (coloring, palette pick).
struct BlendedParams {
  det::Real relief_amplitude_m;
  det::Real base_elevation_m;
  det::Real ruggedness;
  det::Real carving;
  CellId dominant;
  Archetype dominant_archetype{Archetype::Flats};
};

class ProvinceField {
 public:
  ProvinceField(const core::Key& body_key, const PlanetParams& planet);

  // Per-cell parameters (deterministic pure function of the cell id).
  ProvinceParams cell_params(const CellId& cell) const;

  // The cell owning a direction (no blending).
  CellId cell_of(const Dir3& unit_dir) const;

  // Jittered representative point of a cell, on the unit sphere.
  Dir3 representative(const CellId& cell) const;

  // Continuous blended lookup.
  BlendedParams sample(const Dir3& unit_dir) const;

  std::uint32_t cells_per_face() const { return cells_per_face_; }

  // All cells (face-major) — for dumps and the province table payload.
  std::vector<CellId> all_cells() const;

  std::string table_to_json() const;

 private:
  core::Key provinces_key_;
  PlanetType type_;
  std::uint32_t cells_per_face_;
};

}  // namespace inf::gen
