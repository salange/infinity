#pragma once

#include <cstdint>
#include <vector>

#include "core/seed.hpp"
#include "gen/deep_sky.hpp"
#include "gen/galaxy.hpp"
#include "gen/galaxy_octree.hpp"

namespace inf::app {

// T0018 WP2/WP3 — the CPU side of the deep sky. Rendering-only: nothing
// here feeds generation or gameplay, so it runs in plain double math on
// top of the deterministic gen:: models.

// --- WP2: resolved stars --------------------------------------------------
// Magnitude-limited star catalog around an eye position (galactocentric
// metres), built by descending the systems octree: coarse cells far away
// are counted with luminous_count (never enumerated), near cells are
// enumerated fully. Returns an interleaved vertex soup for
// Rhi::create_mesh_mat, 6 vertices per star:
//   position = unit direction (galactic frame),
//   normal   = (corner.x, corner.y, brightness) with corner scaled by the
//              star's relative quad size,
//   mat_pack = packed 8-bit rgb (r*65536 + g*256 + b),
//   blend    = twinkle phase in [0, 1).
// The mesh is static for a whole system visit (parallax is sub-pixel).
struct StarCatalogStats {
  std::size_t star_count{0};
  std::size_t cells_visited{0};
  double brightest_apparent_mag{99.0};
};
std::vector<float> build_star_field_mesh(const gen::GalaxyOctree& octree,
                                         const gen::Dir3& eye_m,
                                         double apparent_mag_limit,
                                         std::size_t max_stars,
                                         StarCatalogStats* stats = nullptr);

// --- WP3/WP4/WP5: the diffuse deep-sky cube map ---------------------------
// Line integrals of the ONE shared density model (stars emission, dust
// extinction) from the eye, plus nebulae, star clusters and globulars
// composited as bounded splats, the home cluster's neighbour galaxies as
// type-driven impostor splats (WP4), and the zodiacal light + gegenschein
// wedge along the arrival planet's orbital plane (WP5). Encoded for the
// RHI planet-texture pair: height = HDR luminance (IEEE halves),
// material = chromaticity RGBA8. Face frame matches the shader's
// cube_face_uv exactly.
struct SkyView {
  gen::Dir3 eye_m;             // galactocentric bake position
  gen::Dir3 sun_dir;           // unit, planet -> star (galactic frame)
  gen::Dir3 ecliptic_normal;   // unit normal of the planet's orbital plane
};
struct SkyBakeResult {
  std::uint32_t face_size{0};
  std::vector<std::uint16_t> luminance_half[6];
  std::vector<std::uint8_t> chroma_rgba[6];
};
SkyBakeResult bake_deep_sky(const gen::GalaxyDensity& density,
                            const gen::NebulaField& nebulae,
                            const gen::StarClusterField& clusters,
                            const core::Seed128& seed, const SkyView& view,
                            std::uint32_t face_size, int thread_count);

}  // namespace inf::app
