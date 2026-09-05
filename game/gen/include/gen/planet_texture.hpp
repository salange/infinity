#pragma once

#include <cstdint>
#include <vector>

#include "gen/terrain.hpp"

namespace inf::gen {

// T0016: far-view planet cube-map baker. Bakes (height, albedo) tiles in
// the engine cube-sphere frame from the live generators — a pure CACHE
// of a pure function (contract 1: nothing is persisted, nothing feeds
// gameplay; the textured path is cosmetic per prototype-v0 section 2).
//
// Height is stored normalized to [-1, 1] over height_amp_m as IEEE half
// floats (the renderer's R16Float layer); albedo is RGBA8 from the
// material/v2 classification, with EarthLike seas baked in (flat height
// at sea level, depth-tinted water) so oceans read from any distance.
//
// Cost model (sources/planet-far-view-texturing.md): the province blend
// dominates, so ONE body-scoped ParamCache serves every texel — the
// warm-cache path is ~43x cheaper than the naive one.
struct PlanetFaceTexture {
  std::vector<std::uint16_t> height_half;  // face_size^2, IEEE half
  std::vector<std::uint8_t> rgba;          // face_size^2 * 4
};

struct PlanetTexture {
  std::uint32_t face_size{0};
  float height_amp_m{1.0f};  // normalization amplitude (metres)
  PlanetFaceTexture faces[6];
};

// albedo_table: optional kMaterialCount x 3 untinted mean albedos of the
// loaded tile library, so the far view matches the near view's tiles;
// nullptr uses the material registry's means.
PlanetTexture bake_planet_texture(const TerrainField& field, std::uint32_t face_size,
                                  const float* albedo_table = nullptr);

}  // namespace inf::gen
