#pragma once

#include <cstdint>
#include <vector>

#include "city/scene.hpp"
#include "gen/sites.hpp"
#include "render/math.hpp"
#include "render/rhi.hpp"

namespace inf::app {

// T0021: a city scene on the planet. The generators work in a flat
// y-up frame (x right, y up, z toward the viewer, metres); the upload
// maps every vertex onto the sphere through the site frame (x -> east,
// -z -> north, y -> up from the datum) so an 8 km site follows the
// curvature, and rotates normals and tangents by the frame at the
// centre. Everything the renderer keeps is cosmetic: mesh handles and
// per-frame draw items.
struct CityUpload {
  // The scene's meshes: the generators emit unshared vertices, so a big
  // scene is split at lot boundaries into pieces under the renderer's
  // buffer guard (opaque pieces first, then foliage).
  struct Piece {
    std::uint32_t mesh{0};
    bool foliage{false};
    std::uint32_t triangles{0};
    float centre[3]{0.0f, 0.0f, 0.0f};  // bounding sphere relative to the origin
    float radius{0.0f};
  };
  std::vector<Piece> pieces;
  bool drawable() const { return !pieces.empty(); }
  double origin[3]{0.0, 0.0, 0.0};  // planet-local metres of the scene's (0, datum, 0)
  struct Light {
    double position[3];
    float radius;
    float color[3];
    float intensity;
  };
  std::vector<Light> lights;  // planet-local
  std::vector<city::DrawRange> draws;
  std::uint32_t triangles{0};
};

// The renderer's copy of the city material table (once per material set).
void upload_city_materials(render::Rhi& rhi, const std::vector<city::MaterialDesc>& materials);

// Uploads a scene placed in `frame` at `datum_m` above the nominal radius.
CityUpload upload_city_scene(render::Rhi& rhi, const city::Scene& scene, const gen::SiteFrame& frame,
                             double datum_m);
void release_city_upload(render::Rhi& rhi, CityUpload* upload);

// Draw items (mode 8) for a frame; camera_pos in planet-local metres.
void draw_city_upload(const CityUpload& upload, const render::Vec3& camera_pos,
                      const render::Mat4& view_projection, std::vector<render::Rhi::DrawItem>* items);
// The frame's point lights, camera-relative, nearest first (at most 64).
void city_lights_for_frame(const CityUpload& upload, const render::Vec3& camera_pos, bool night,
                           std::vector<render::Rhi::CityLight>* out);
// The nearest 64 lights over several uploads (appends to `out`).
void city_lights_select(const std::vector<const CityUpload*>& uploads, const render::Vec3& camera_pos,
                        std::vector<render::Rhi::CityLight>* out);

}  // namespace inf::app
