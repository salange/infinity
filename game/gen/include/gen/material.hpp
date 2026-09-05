#pragma once

#include <cstdint>

#include "core/key.hpp"
#include "gen/biome.hpp"
#include "gen/climate.hpp"
#include "gen/geo.hpp"
#include "gen/life.hpp"
#include "gen/planet.hpp"
#include "gen/provinces.hpp"

namespace inf::gen {

// material/v2 (T0019, design/surface-texturing.md section 2.4): surface
// material assignment as a PURE function of a surface point's local
// facts — height above the sea datum, altitude, slope, depth below the
// procedural surface (dug faces and cave walls read as bedrock), the
// dominant province archetype and its landform channels, the climate,
// the biome and the life cover. Rules accumulate (material, weight); the
// two heaviest become the NMS-style pair the renderer blends. Ids are
// texture-array layers in the tile library (see the app's material
// loader) and are gameplay-visible facts: keep the enum append-only.
enum class Material : std::uint8_t {
  None = 0,
  RockGranite = 1,
  RockBasalt = 2,
  RockSandstone = 3,
  RockShale = 4,
  Scree = 5,
  CliffMossy = 6,
  RegolithFine = 7,
  RegolithRubble = 8,
  Gravel = 9,
  Pebbles = 10,
  SandDune = 11,
  SandBeach = 12,
  SandWet = 13,
  SoilDry = 14,
  SoilMud = 15,
  SoilLoam = 16,
  ForestFloor = 17,
  DeadLeaves = 18,
  Grass = 19,
  Meadow = 20,
  Moss = 21,
  Snow = 22,
  SnowDrift = 23,
  SnowDirty = 24,
  IceSheet = 25,
  Permafrost = 26,
  LavaRock = 27,
  MicrobialMat = 28,
  LichenCrust = 29,
  CrystalField = 30,
  Sulfur = 31,
  TholinDust = 32,
  RedBed = 33,
  SaltFlat = 34,
  AmmoniaSlush = 35,
  Seabed = 36,
  Count = 37,
};

// Which per-planet tint a material takes (SurfacePalette below).
enum class TintGroup : std::uint8_t {
  None = 0,
  Rock = 1,
  Sand = 2,
  Soil = 3,
  Life = 4,     // primary pigment (vegetation, crystals, crusts)
  Life2 = 5,    // secondary pigment (mats)
  Sulfur = 6,   // fixed warm tint family
};

struct MaterialInfo {
  const char* name;      // manifest key (assets/manifest.json) or generator id
  float albedo[3];       // mean albedo of the tile (far view / fallback)
  float tile_m;          // metres per texture repeat at the fine scale
  float roughness;       // fallback roughness when no map is loaded
  TintGroup tint;
  bool emissive;         // glows at night when the life pigment says so
};

const MaterialInfo& material_info(Material id);
inline constexpr std::uint32_t kMaterialCount = static_cast<std::uint32_t>(Material::Count);

struct VertexMaterial {
  Material mat0{Material::RockGranite};
  Material mat1{Material::RockGranite};
  float blend{0.0f};  // fraction of mat1
};

// Per-planet tints (multipliers on the tile albedo). Rock/sand/soil come
// from palette_id (mineral families, not a hue rotation); life tints
// come from life/v1.
struct SurfacePalette {
  float rock[3]{1.0f, 1.0f, 1.0f};
  float sand[3]{1.0f, 1.0f, 1.0f};
  float soil[3]{1.0f, 1.0f, 1.0f};
  float life[3]{1.0f, 1.0f, 1.0f};
  float life2[3]{1.0f, 1.0f, 1.0f};
  float emissive{0.0f};
};

// Everything the classifier reads about one surface point. Gathered by
// TerrainField::classify_vertex; tests build it by hand.
struct MaterialInputs {
  double px{0.0}, py{0.0}, pz{0.0};  // planet-local metres (patch noise)
  double height_above_sea_m{0.0};    // procedural surface minus the sea datum
  double altitude_m{0.0};            // procedural surface above the nominal radius
  double slope{0.0};                 // 1 - dot(normal, radial)
  double depth_below_surface_m{0.0}; // how far below the procedural surface
  Archetype archetype{Archetype::Flats};
  double terrace_amount{0.0};
  double dune_amount{0.0};
  double ruggedness{0.0};
  Climate climate;
  BiomeSample biome;
};

class MaterialField {
 public:
  MaterialField(const core::Key& body_entity_key, const PlanetParams& planet,
                const LifeParams& life);

  VertexMaterial classify(const MaterialInputs& in) const;
  // The full rule output: one weight per material id (>= 0). classify()
  // is pick(weights). Meshes choose ONE pair per triangle from the summed
  // vertex weights and blend per vertex from these; the far-view baker
  // averages albedos by them.
  void weights(const MaterialInputs& in, double out[kMaterialCount]) const;
  static VertexMaterial pick(const double w[kMaterialCount]);

  const SurfacePalette& palette() const { return palette_; }
  const LifeParams& life() const { return life_; }
  // Albedo multiplier for a material under this planet's palette.
  void tint(Material id, float rgb[3]) const;
  // Mean albedo x tint: the far-view / no-library colour of a material.
  void mean_albedo(Material id, float rgb[3]) const;

 private:
  PlanetParams planet_;
  LifeParams life_;
  SurfacePalette palette_;
  Material bedrock_{Material::RockGranite};
  Material sand_{Material::SandBeach};
  bool volcanic_{false};
  std::uint64_t lattice_patch_{0};
  std::uint64_t lattice_fine_{0};
  std::uint64_t lattice_vent_{0};
};

}  // namespace inf::gen
