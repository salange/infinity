#pragma once
// Material ids shared by the scene and the tower generators.
#include <cstdint>
#include <string>
#include <vector>

#include "city/scene.hpp"

namespace inf::city {

enum Mat : std::uint32_t {
  M_ASPHALT = 0, M_LANE_WHITE, M_LANE_YELLOW, M_CURB, M_SIDEWALK, M_PLAZA, M_TERRAZZO, M_GRASS, M_SOIL,
  M_CONCRETE_WHITE, M_CONCRETE, M_CONCRETE_DARK, M_WHITE_METAL, M_SILVER, M_DARK_METAL, M_BRONZE,
  M_GLASS_BLUE, M_GLASS_SILVER, M_GLASS_DARK, M_GLASS_CLEAR, M_SPANDREL, M_ROOF, M_BARK, M_LEAF, M_LAMP,
  M_MARBLE, M_LOBBY_LIGHT, M_SIGN, M_WATER, M_GLASS_XFRAME, M_GLASS_CONTEXT, M_GLASS_GREEN, M_GLASS_BRONZE,
  M_HEDGE, M_MARBLE_WHITE, M_PAD, M_CHROME, M_WALL_LIGHT, M_PANEL_WARM, M_PANEL_DARK, M_ROOF_METAL, M_GLASS_STD,
  M_PAD_LIGHT, M_MEDIAN,
  M_COUNT
};

std::vector<MaterialDesc> make_materials();

// A texture set the materials refer to by name, with the procedural
// fallback used when the CC0 files are absent.
struct TextureSetSpec {
  std::string name;  // directory under assets/textures
  float fallback_rgb[3]{0.5f, 0.5f, 0.5f};
  float fallback_roughness{0.7f};
  int fallback_pattern{0};  // 0 flat, 1 concrete noise, 2 metal brushed, 3 stone tiles, 4 grass, 5 asphalt
};
// The sets make_materials() references, in layer order ("flat" first).
std::vector<TextureSetSpec> texture_sets();

// Room grids in the glass shader must match the floor height; each glass
// material is bound to one floor height.
float glass_floor_height(Mat glass);
Mat glass_for_floor_height(float floor_h);

}  // namespace inf::city
