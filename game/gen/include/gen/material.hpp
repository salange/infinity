#pragma once

#include <cstdint>

#include "core/det/real.hpp"
#include "core/key.hpp"
#include "gen/geo.hpp"
#include "gen/planet.hpp"

namespace inf::gen {

// material/v1 (T0015 WP3): per-vertex surface materials as a PURE
// function of position and normal — no neighbour queries, no state.
// Inputs are all derivable from the vertex alone: height above the sea
// datum (|p| - R - sea), slope (normal vs radial), latitude (planet
// +Z axis is north, gen/planet.hpp), and a continent-scale climate
// noise drawn from the "material/v1" layer key. The renderer receives
// NMS-style TWO material ids + a blend fraction per vertex.
//
// Climate v1 is deliberately cheap: temperature falls with |latitude|
// and altitude and wobbles with the climate noise; polar caps appear
// where it drops below the snow threshold. Obliquity/tidal-lock inputs
// are wired later (they live in the system layer's SpinState).
enum class Material : std::uint8_t {
  None = 0,  // legacy: shader uses the flat base albedo
  Rock = 1,
  Regolith = 2,
  Sand = 3,
  Grass = 4,
  Snow = 5,
  IceSheet = 6,
  Seabed = 7,
  Scree = 8,
};

struct VertexMaterial {
  Material mat0{Material::Rock};
  Material mat1{Material::Rock};
  float blend{0.0f};  // fraction of mat1
};

class MaterialField {
 public:
  MaterialField(const core::Key& body_entity_key, const PlanetParams& planet);

  // Classify one surface vertex (planet-local meters + unit-ish normal).
  VertexMaterial classify(double px, double py, double pz, double nx, double ny,
                          double nz) const;

 private:
  PlanetParams planet_;
  std::uint64_t climate_lattice_{0};
  double sea_datum_m_{0.0};
  double radius_m_{0.0};
  double inv_radius_{0.0};
};

}  // namespace inf::gen
