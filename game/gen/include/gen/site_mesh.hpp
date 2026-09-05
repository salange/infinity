#pragma once

#include <cstdint>
#include <vector>

#include "gen/buildings.hpp"
#include "gen/sites.hpp"
#include "world/mesher.hpp"

namespace inf::gen {

// T0020 WP5: mass models for a site — every lot extruded from its
// footprint into a box (or a low dome for domed sites), in the terrain
// mesh format (10 floats: position, normal, four palette weights) so the
// masses ride the lit, textured terrain pipeline with an urban palette.
// WP6 replaces the box with the building executor's output through the
// same interface; the app never learns the difference.
//
// LOD: `detail` 0 = every lot; 1 = every lot taller than 1.5x the
// site's mean, plus one merged block mass per lattice block; 2 = one
// mass per block only (far view).
struct SiteMeshParams {
  int detail{0};
  // The near-LOD terminal set (comparison renders switch it; the default
  // is the WP6 decision).
  BuildingMethod method{BuildingMethod::GrammarParts};
  // Camera-relative culling: only blocks whose centre lies within this
  // distance of `focus` (site-local metres) are built; 0 = all.
  double focus_x{0.0};
  double focus_y{0.0};
  double focus_radius_m{0.0};
};

struct SiteMesh {
  world::ChunkMesh mesh;  // origin = the site centre on the plateau (planet-local m)
  std::uint32_t lot_count{0};
  std::uint32_t triangle_count{0};
};

// Builds the mesh. The TerrainField supplies each lot's ground elevation
// (the civil-modified surface) so masses sit on the plateau and on the
// terraces exactly where the terrain is.
SiteMesh build_site_mesh(const SiteField& sites, const Site& site, const TerrainField& field,
                         const SiteMeshParams& params);

// The four-material palette a site's buildings use: walls, roof, glass,
// accent (building_palette of the site style); the mid LOD swaps the
// walls for the facade-with-windows tile.
void site_palette(const Site& site, int detail, std::uint8_t out[4]);

}  // namespace inf::gen
