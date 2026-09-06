#include "scene.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "city.hpp"
#include "materials.hpp"
#include "rng.hpp"
#include "site.hpp"
#include "towers.hpp"

namespace cb {

Scene generate_scene(const SceneParams& params) {
  Scene sc;
  sc.materials = make_materials();
  Rng root = root_rng(params.seed);
  set_far_patterns(params.far_patterns);
  if (params.showcase) {
    generate_showcase(sc, root.child(200), params.showcase_detail);
    sc.city_size = "showcase";
    sc.finalize_draws();
    return sc;
  }
  CitySize size = city_size_for(root);
  if (params.size >= 0) size = static_cast<CitySize>(params.size);
  const CityStats st = generate_city(sc, root.child(100), size);
  sc.finalize_draws();
  sc.city_size = to_string(size);
  sc.city_radius = st.radius;
  sc.stats_blocks = st.blocks;
  sc.stats_towers = st.towers;
  sc.stats_standards = st.standards;
  sc.stats_plazas = st.plazas;
  // keep at most 64 point lights: the ones nearest the centre
  if (sc.lights.size() > 64) {
    std::sort(sc.lights.begin(), sc.lights.end(), [](const PointLight& a, const PointLight& b) {
      return a.position.x * a.position.x + a.position.z * a.position.z < b.position.x * b.position.x + b.position.z * b.position.z;
    });
    sc.lights.resize(64);
  }
  sc.camera_position = Vec3{-st.radius * 0.18f, 28.0f, st.radius * 0.62f};
  sc.camera_target = Vec3{0.0f, 40.0f, 0.0f};
  return sc;
}

}  // namespace cb
