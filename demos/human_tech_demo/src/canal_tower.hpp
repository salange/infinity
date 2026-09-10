#pragma once
#include "scene.hpp"
#include <string_view>

namespace cb {
struct CanalTowerSpec {
  Vec2 centre{-154.37f, 243.40f};
  float base_y = 13.2f;
  float base_radius = 15.5f;
  float neck_radius = 16.7f;
  float crown_floor = 91.2f;
  float crown_radius = 17.7f;
  float crown_top = 98.28f;
  float pavilion_radius = 10.8f;
  float pavilion_roof = 97.2f;
  int columns = 24;
  int cell_rows = 7;
  float cell_height = 10.8f;
};
float canal_tower_radius(const CanalTowerSpec& spec, float y);
void build_canal_tower(Scene& scene, const CanalTowerSpec& spec);
Scene make_canal_tower_sample(std::string_view view);
} // namespace cb
