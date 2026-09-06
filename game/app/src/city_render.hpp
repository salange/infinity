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
//
// The scene's draw ranges (one per block, four levels per tower) are
// the units of drawing (WP5): a range is culled by the camera frustum,
// a level group draws the level its distance picks, and shadows come
// from the coarsest real level. T0022 B.1: a frame hands the renderer
// one item per mesh with a list of its ranges (main pass, shadow
// cascades) instead of one item per range; the per-building fine
// ranges of a block are the occlusion candidates when culling is on.
struct CityUploadData {
  struct Piece {
    std::vector<render::Rhi::CityVertex> vertices;  // packed (T0022 B.1)
    std::vector<std::uint32_t> indices;
    bool foliage{false};
    float centre[3]{0.0f, 0.0f, 0.0f};  // bounding sphere relative to the origin
    float radius{0.0f};
  };
  struct Range {
    std::uint32_t piece{0};
    std::uint32_t first{0};  // index offset inside the piece
    std::uint32_t count{0};
    float centre[3]{0.0f, 0.0f, 0.0f};  // relative to the origin (planet frame)
    float radius{0.0f};                 // 0 = unbounded
    int lod_group{-1};
    int lod_level{0};
    float lod_max_distance{1e30f};
    bool has_fine{false};  // a block whose buildings are also in `fine`
  };
  struct Light {
    double position[3];
    float radius;
    float color[3];
    float intensity;
  };
  std::vector<Piece> pieces;
  std::vector<Range> ranges;
  std::vector<Range> fine;  // per-building sub-ranges of block ranges, sorted by (piece, first)
  std::vector<Light> lights;  // planet-local
  double origin[3]{0.0, 0.0, 0.0};
  std::uint32_t triangles{0};
};

struct CityUpload {
  struct Piece {
    std::uint32_t mesh{0};
    bool foliage{false};
    std::uint32_t triangles{0};
  };
  std::vector<Piece> pieces;
  std::vector<CityUploadData::Range> ranges;
  std::vector<CityUploadData::Range> fine;
  bool drawable() const { return !pieces.empty(); }
  double origin[3]{0.0, 0.0, 0.0};  // planet-local metres of the scene's (0, datum, 0)
  using Light = CityUploadData::Light;
  std::vector<Light> lights;  // planet-local
  std::uint32_t triangles{0};
};

// Per-frame drawing statistics.
struct CityDrawStats {
  std::size_t resident_triangles{0};
  std::size_t drawn_triangles{0};
  std::size_t shadow_triangles{0};
  std::size_t items{0};
  std::size_t ranges{0};
};

// How a frame selects ranges (the player-facing options of T0022 B.2
// that the app decides; the renderer's own are in Rhi::CitySettings).
struct CityDrawOptions {
  bool fine_ranges{true};      // per-building occlusion candidates (only useful with occlusion culling on)
  bool shadow_far_lod{false};  // far cascade: tower shells only, no bounded ranges beyond 350 m
};

// The renderer's copy of the city material table (once per material set).
void upload_city_materials(render::Rhi& rhi, const std::vector<city::MaterialDesc>& materials);

// CPU half of an upload (safe on a worker): the scene placed in `frame`
// at `datum_m` above the nominal radius, packed into the renderer's
// vertex layout, split into pieces under the renderer's buffer guard at
// range boundaries.
CityUploadData prepare_city_upload(const city::Scene& scene, const gen::SiteFrame& frame, double datum_m);
// GPU half: creates the meshes.
CityUpload commit_city_upload(render::Rhi& rhi, CityUploadData&& data);
// Both halves.
CityUpload upload_city_scene(render::Rhi& rhi, const city::Scene& scene, const gen::SiteFrame& frame,
                             double datum_m);
void release_city_upload(render::Rhi& rhi, CityUpload* upload);

// Draw items (mode 8) for a frame: one per mesh whose ranges are
// selected, the ranges appended to `ranges` (they must stay alive until
// the frame is rendered: FrameParams::city_ranges). camera_pos in
// planet-local metres.
void draw_city_upload(const CityUpload& upload, const render::Vec3& camera_pos,
                      const render::Mat4& view_projection, const CityDrawOptions& options,
                      std::vector<render::Rhi::DrawItem>* items, std::vector<render::Rhi::CityRange>* ranges,
                      CityDrawStats* stats = nullptr);
// The frame's point lights, camera-relative, nearest first (at most 64).
void city_lights_for_frame(const CityUpload& upload, const render::Vec3& camera_pos, bool night,
                           std::vector<render::Rhi::CityLight>* out);
// The nearest 64 lights over several uploads (appends to `out`).
void city_lights_select(const std::vector<const CityUpload*>& uploads, const render::Vec3& camera_pos,
                        std::vector<render::Rhi::CityLight>* out);

}  // namespace inf::app
