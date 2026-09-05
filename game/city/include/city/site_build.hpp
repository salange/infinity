#pragma once
// sites/v1 -> city scene: walks a site's blocks, turns every visible lot
// into a LotInput in the scene frame and hands it to the lot's
// architecture; adds the civic centre (government building and the
// unification ring) on planetary capitals. Pure function of the site and
// the parameters; every lot's geometry is keyed under buildings/v1 by
// its id, exactly as the mass executor keys it.
#include <cstdint>

#include "city/scene.hpp"
#include "gen/sites.hpp"
#include "gen/terrain.hpp"

namespace inf::city {

struct SiteBuildParams {
  int detail{2};  // 2 full, 1 near context, 0 far context (at the focus)
  // Only blocks whose centre lies within this distance of the focus
  // (site-local east/north metres) are built; 0 = the whole site.
  double focus_x{0.0};
  double focus_y{0.0};
  double focus_radius_m{0.0};
  // With a focus, the level drops one step beyond each of these
  // distances from it (the demo's near/context rings; WP5 replaces the
  // ramp with per-object levels switched by the camera).
  double full_range_m{350.0};
  double context_range_m{900.0};
  int lamp_budget{48};
};

struct SiteBuildStats {
  std::uint32_t lots{0};
  std::uint32_t towers{0};
  std::uint32_t standards{0};
  std::uint32_t key_buildings{0};
  std::uint32_t max_standard_triangles{0};
  std::uint32_t max_tower_triangles{0};
  std::uint32_t triangles{0};
  std::uint32_t dropped{0};  // lots whose geometry came out non-finite (never drawn)
};

// Scene frame: x east, y up from the site datum, z south (metres from
// the site centre). The app maps it onto the sphere through the site
// frame.
void build_site_scene(const gen::SiteField& sites, const gen::Site& site, const gen::TerrainField& field,
                      const SiteBuildParams& params, Scene* scene, SiteBuildStats* stats);

// The civic centre radius kept free of lots on a capital.
double civic_centre_radius_m(const gen::Site& site);

}  // namespace inf::city
