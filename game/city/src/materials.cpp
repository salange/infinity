#include "city/materials.hpp"

#include <algorithm>
#include <cmath>

namespace inf::city {

std::vector<MaterialDesc> make_materials() {
  std::vector<MaterialDesc> m(M_COUNT);
  auto set = [&](Mat id, const char* name, Vec3 color, float rough, float metal, const char* tex, float scale, std::uint32_t flags = 0) {
    MaterialDesc& d = m[id];
    d.name = name;
    d.base_color = color;
    d.roughness = rough;
    d.metallic = metal;
    d.albedo_set = tex;
    d.uv_scale = scale;
    d.flags = flags;
  };
  set(M_ASPHALT, "asphalt", {0.8f, 0.8f, 0.8f}, 0.9f, 0, "asphalt", 5.0f, kMatPlanarXZ);
  set(M_LANE_WHITE, "lane_white", {0.85f, 0.85f, 0.82f}, 0.7f, 0, "", 1.0f);
  set(M_LANE_YELLOW, "lane_yellow", {0.85f, 0.65f, 0.15f}, 0.7f, 0, "", 1.0f);
  set(M_CURB, "curb", {0.85f, 0.85f, 0.85f}, 0.75f, 0, "concrete_smooth", 3.0f, kMatTriplanar);
  set(M_SIDEWALK, "sidewalk", {0.9f, 0.9f, 0.9f}, 0.85f, 0, "paving_slabs", 3.0f, kMatPlanarXZ);
  m[M_SIDEWALK].normal_strength = 0.6f;
  set(M_PLAZA, "plaza", {0.74f, 0.75f, 0.74f}, 0.8f, 0, "pavement_light", 4.0f, kMatPlanarXZ);
  m[M_PLAZA].normal_strength = 0.6f;
  set(M_TERRAZZO, "terrazzo", {1.0f, 1.0f, 1.0f}, 0.35f, 0, "terrazzo", 3.0f, kMatPlanarXZ);
  set(M_GRASS, "grass", {0.95f, 1.0f, 0.9f}, 0.9f, 0, "grass", 3.0f, kMatPlanarXZ);
  set(M_SOIL, "soil", {0.9f, 0.9f, 0.9f}, 0.95f, 0, "soil", 2.0f, kMatPlanarXZ);
  set(M_CONCRETE_WHITE, "concrete_white", {0.86f, 0.86f, 0.84f}, 0.6f, 0, "concrete_white", 3.0f, kMatTriplanar);
  set(M_CONCRETE, "concrete", {1.0f, 1.0f, 1.0f}, 0.7f, 0, "concrete_smooth", 4.0f, kMatTriplanar);
  set(M_CONCRETE_DARK, "concrete_dark", {0.9f, 0.9f, 0.9f}, 0.7f, 0, "concrete_panels", 3.0f, kMatTriplanar);
  set(M_WHITE_METAL, "white_metal", {0.80f, 0.80f, 0.78f}, 0.42f, 0.0f, "", 1.0f);
  set(M_SILVER, "silver", {1.0f, 1.0f, 1.0f}, 0.55f, 1.0f, "metal_silver", 1.0f, kMatTriplanar);
  set(M_DARK_METAL, "dark_metal", {0.25f, 0.25f, 0.26f}, 0.55f, 0.9f, "metal_black", 1.0f, kMatTriplanar);
  set(M_BRONZE, "bronze", {0.36f, 0.26f, 0.18f}, 0.4f, 1.0f, "", 1.0f);
  // Glass per building so the interior room grid matches the real floors.
  set(M_GLASS_BLUE, "glass_diagrid", {0.6f, 0.75f, 0.9f}, 0.06f, 0, "", 1.0f, kMatGlass);
  m[M_GLASS_BLUE].tint2 = Vec3{0.62f, 0.78f, 0.92f};
  m[M_GLASS_BLUE].room_w = 3.0f; m[M_GLASS_BLUE].room_h = 4.0f; m[M_GLASS_BLUE].room_d = 7.0f;
  set(M_GLASS_SILVER, "glass_lens", {0.8f, 0.85f, 0.9f}, 0.04f, 0, "", 1.0f, kMatGlass);
  m[M_GLASS_SILVER].tint2 = Vec3{0.72f, 0.80f, 0.86f};
  m[M_GLASS_SILVER].room_w = 3.6f; m[M_GLASS_SILVER].room_h = 3.9f; m[M_GLASS_SILVER].room_d = 6.0f;
  set(M_GLASS_DARK, "glass_fin", {0.3f, 0.35f, 0.4f}, 0.05f, 0, "", 1.0f, kMatGlass);
  m[M_GLASS_DARK].tint2 = Vec3{0.28f, 0.34f, 0.40f};
  m[M_GLASS_DARK].lit_probability = 0.45f;
  m[M_GLASS_DARK].room_w = 4.8f; m[M_GLASS_DARK].room_h = 3.8f; m[M_GLASS_DARK].room_d = 6.0f;
  set(M_GLASS_XFRAME, "glass_xframe", {0.6f, 0.75f, 0.9f}, 0.06f, 0, "", 1.0f, kMatGlass);
  m[M_GLASS_XFRAME].tint2 = Vec3{0.62f, 0.78f, 0.92f};
  m[M_GLASS_XFRAME].room_w = 3.0f; m[M_GLASS_XFRAME].room_h = 4.4f; m[M_GLASS_XFRAME].room_d = 8.0f;
  set(M_GLASS_CONTEXT, "glass_context", {0.5f, 0.62f, 0.75f}, 0.08f, 0, "", 1.0f, kMatGlass);
  m[M_GLASS_CONTEXT].tint2 = Vec3{0.55f, 0.66f, 0.78f};
  m[M_GLASS_CONTEXT].room_w = 4.2f; m[M_GLASS_CONTEXT].room_h = 3.8f; m[M_GLASS_CONTEXT].room_d = 7.0f;
  m[M_GLASS_CONTEXT].lit_probability = 0.5f;
  set(M_GLASS_GREEN, "glass_green", {0.55f, 0.72f, 0.66f}, 0.06f, 0, "", 1.0f, kMatGlass);
  m[M_GLASS_GREEN].tint2 = Vec3{0.58f, 0.76f, 0.68f};
  m[M_GLASS_GREEN].room_w = 3.0f; m[M_GLASS_GREEN].room_h = 3.8f; m[M_GLASS_GREEN].room_d = 6.5f;
  set(M_HEDGE, "hedge", {0.42f, 0.62f, 0.32f}, 0.92f, 0, "grass", 1.2f, kMatTriplanar);
  set(M_MARBLE_WHITE, "marble_white", {0.84f, 0.84f, 0.82f}, 0.25f, 0, "marble", 4.0f, kMatTriplanar);
  set(M_PAD, "pad", {0.82f, 0.82f, 0.81f}, 0.4f, 0, "concrete_white", 6.0f, kMatPlanarXZ);
  set(M_CHROME, "chrome", {0.95f, 0.95f, 0.95f}, 0.22f, 1.0f, "metal_silver", 1.0f, kMatTriplanar);
  set(M_WALL_LIGHT, "wall_light", {0.88f, 0.87f, 0.84f}, 0.65f, 0, "concrete_smooth", 3.0f, kMatTriplanar);
  set(M_PANEL_WARM, "panel_warm", {0.92f, 0.86f, 0.78f}, 0.6f, 0, "concrete_white", 3.0f, kMatTriplanar);
  set(M_PANEL_DARK, "panel_dark", {0.55f, 0.56f, 0.58f}, 0.6f, 0, "concrete_panels", 3.0f, kMatTriplanar);
  set(M_ROOF_METAL, "roof_metal", {0.8f, 0.8f, 0.82f}, 0.55f, 1.0f, "metal_silver", 2.0f, kMatTriplanar);
  set(M_GLASS_STD, "glass_std", {0.6f, 0.72f, 0.82f}, 0.07f, 0, "", 1.0f, kMatGlass);
  m[M_GLASS_STD].tint2 = Vec3{0.62f, 0.74f, 0.84f};
  m[M_GLASS_STD].room_w = 3.2f; m[M_GLASS_STD].room_h = 3.6f; m[M_GLASS_STD].room_d = 6.0f; m[M_GLASS_STD].lit_probability = 0.6f;
  set(M_PAD_LIGHT, "pad_light", {0.9f, 0.9f, 0.9f}, 0.5f, 0, "", 1.0f, kMatEmissive | kMatNightOnly);
  m[M_PAD_LIGHT].emissive = 4.0f; m[M_PAD_LIGHT].tint2 = Vec3{0.4f, 0.8f, 1.0f};
  set(M_MEDIAN, "median", {0.9f, 0.9f, 0.88f}, 0.7f, 0, "pavement_light", 3.0f, kMatPlanarXZ);
  set(M_GLASS_BRONZE, "glass_bronze", {0.45f, 0.36f, 0.28f}, 0.06f, 0, "", 1.0f, kMatGlass);
  m[M_GLASS_BRONZE].tint2 = Vec3{0.5f, 0.4f, 0.3f};
  m[M_GLASS_BRONZE].room_w = 4.8f; m[M_GLASS_BRONZE].room_h = 3.8f; m[M_GLASS_BRONZE].room_d = 6.0f;
  m[M_GLASS_BRONZE].lit_probability = 0.45f;
  set(M_GLASS_CLEAR, "glass_clear", {0.9f, 0.95f, 0.95f}, 0.03f, 0, "", 1.0f, kMatGlass);
  m[M_GLASS_CLEAR].tint2 = Vec3{0.9f, 0.95f, 0.93f};
  m[M_GLASS_CLEAR].room_w = 12.0f; m[M_GLASS_CLEAR].room_h = 6.0f; m[M_GLASS_CLEAR].room_d = 14.0f; m[M_GLASS_CLEAR].lit_probability = 0.9f;
  set(M_SPANDREL, "spandrel", {0.16f, 0.17f, 0.19f}, 0.6f, 0.2f, "", 1.0f);
  set(M_ROOF, "roof", {0.42f, 0.42f, 0.44f}, 0.9f, 0, "concrete_smooth", 5.0f, kMatTriplanar);
  set(M_BARK, "bark", {0.9f, 0.9f, 0.9f}, 0.9f, 0, "bark", 1.2f);
  set(M_LEAF, "leaf", {1.0f, 1.0f, 1.0f}, 0.75f, 0, "", 1.0f, kMatFoliage);
  set(M_LAMP, "lamp", {0.9f, 0.9f, 0.9f}, 0.5f, 0, "", 1.0f, kMatEmissive | kMatNightOnly);
  m[M_LAMP].emissive = 6.0f; m[M_LAMP].tint2 = Vec3{1.0f, 0.86f, 0.62f};
  set(M_MARBLE, "marble", {1.0f, 1.0f, 1.0f}, 0.25f, 0, "marble", 3.0f, kMatPlanarXZ);
  set(M_LOBBY_LIGHT, "lobby_light", {0.9f, 0.9f, 0.9f}, 0.5f, 0, "", 1.0f, kMatEmissive);
  m[M_LOBBY_LIGHT].emissive = 1.6f; m[M_LOBBY_LIGHT].tint2 = Vec3{1.0f, 0.93f, 0.8f};
  set(M_SIGN, "sign", {0.2f, 0.55f, 1.0f}, 0.5f, 0, "", 1.0f, kMatEmissive | kMatNightOnly);
  m[M_SIGN].emissive = 3.0f; m[M_SIGN].tint2 = Vec3{0.25f, 0.6f, 1.0f};
  set(M_WATER, "water", {0.05f, 0.08f, 0.1f}, 0.02f, 0, "", 1.0f);
  return m;
}


float glass_floor_height(Mat glass) {
  switch (glass) {
    case M_GLASS_BLUE: return 4.0f;
    case M_GLASS_SILVER: return 3.9f;
    case M_GLASS_XFRAME: return 4.4f;
    default: return 3.8f;
  }
}
Mat glass_for_floor_height(float floor_h) {
  if (floor_h > 4.2f) return M_GLASS_XFRAME;
  if (floor_h > 3.95f) return M_GLASS_BLUE;
  if (floor_h > 3.85f) return M_GLASS_SILVER;
  return M_GLASS_CONTEXT;
}

std::vector<TextureSetSpec> texture_sets() {
  std::vector<TextureSetSpec> sets;
  auto add = [&](const char* name, float r, float g, float b, float rough, int pattern) {
    TextureSetSpec s;
    s.name = name;
    s.fallback_rgb[0] = r; s.fallback_rgb[1] = g; s.fallback_rgb[2] = b;
    s.fallback_roughness = rough;
    s.fallback_pattern = pattern;
    sets.push_back(s);
  };
  add("flat", 1, 1, 1, 0.6f, 0);
  add("asphalt", 0.22f, 0.22f, 0.23f, 0.9f, 5);
  add("pavement_light", 0.62f, 0.60f, 0.57f, 0.7f, 3);
  add("paving_slabs", 0.55f, 0.55f, 0.54f, 0.65f, 3);
  add("terrazzo", 0.72f, 0.70f, 0.66f, 0.4f, 1);
  add("grass", 0.20f, 0.32f, 0.10f, 0.9f, 4);
  add("soil", 0.28f, 0.22f, 0.16f, 0.95f, 1);
  add("concrete_white", 0.78f, 0.77f, 0.74f, 0.6f, 1);
  add("concrete_smooth", 0.62f, 0.62f, 0.61f, 0.7f, 1);
  add("concrete_panels", 0.38f, 0.38f, 0.37f, 0.7f, 1);
  add("metal_silver", 0.80f, 0.80f, 0.82f, 0.3f, 2);
  add("metal_black", 0.08f, 0.08f, 0.09f, 0.45f, 2);
  add("bark", 0.30f, 0.24f, 0.18f, 0.9f, 1);
  add("marble", 0.80f, 0.80f, 0.80f, 0.25f, 1);
  return sets;
}

}  // namespace inf::city
