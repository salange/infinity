#pragma once
// Material ids shared by the scene and the tower generators.
#include <cstdint>
#include <vector>

#include "scene.hpp"

namespace cb {

enum Mat : std::uint32_t {
  M_ASPHALT = 0, M_LANE_WHITE, M_LANE_YELLOW, M_CURB, M_SIDEWALK, M_PLAZA, M_TERRAZZO, M_GRASS, M_SOIL,
  M_CONCRETE_WHITE, M_CONCRETE, M_CONCRETE_DARK, M_WHITE_METAL, M_SILVER, M_DARK_METAL, M_BRONZE,
  M_GLASS_BLUE, M_GLASS_SILVER, M_GLASS_DARK, M_GLASS_CLEAR, M_SPANDREL, M_ROOF, M_BARK, M_LEAF, M_LAMP,
  M_MARBLE, M_LOBBY_LIGHT, M_SIGN, M_WATER, M_GLASS_XFRAME, M_GLASS_CONTEXT, M_GLASS_GREEN, M_GLASS_BRONZE,
  M_COUNT
};

std::vector<MaterialDesc> make_materials();

// Room grids in the glass shader must match the floor height; each glass
// material is bound to one floor height.
float glass_floor_height(Mat glass);
Mat glass_for_floor_height(float floor_h);

}  // namespace cb
