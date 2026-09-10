#pragma once

#include "scene.hpp"
#include <functional>
#include <string_view>
#include <vector>

namespace cb {
struct DiagridOpening {
  float angle;
  float floor_y;
  // Connected exterior stairs can require one opening across several floors.
  float head_offset = 5.3f;
};

struct SculptedDiagridSpec {
  Vec2 centre{-345.f, 405.f};
  float base_y = 17.2f;
  float height = 544.f;
  int columns = 22;
  int levels = 31;
  float base_radius = 44.8f;
  float taper_metres = 2.2f;
  // Optional bounded extraction uses precisely the production grid/profile.
  int first_column = 0;
  int column_count = 22;
  int first_level = 0;
  int level_count = 31;
  bool shallow_control = false;
  std::vector<DiagridOpening> openings;
};

std::uint32_t sculpted_diagrid_ceramic(Scene &scene);
void build_sculpted_diagrid(
    Scene &scene, const SculptedDiagridSpec &spec,
    const std::function<std::uint32_t(float)> &material_at_angle);
Scene make_sculpted_diagrid_sample(std::string_view view,
                                   bool shallow_control = false);
} // namespace cb
