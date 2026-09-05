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

// ---------------------------------------------------------------------------
// Materials
// ---------------------------------------------------------------------------
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

// 5. Folded pavilion: a faceted white shell with triangulated glazing.
void gen_pavilion(Scene& sc, Rng rng, Vec2 centre, float radius, float height, float y) {
  Mesh& mesh = sc.opaque;
  std::vector<Vec2> base;
  const int n = 5;
  for (int i = 0; i < n; ++i) {
    const float a = static_cast<float>(i) / n * 2.0f * kPi + rng.range(-0.15f, 0.15f);
    const float r = radius * rng.range(0.8f, 1.15f);
    base.push_back(centre + Vec2{std::cos(a), std::sin(a)} * r);
  }
  const Vec3 apex = P3(centre + Vec2{rng.range(-0.25f, 0.25f), rng.range(-0.25f, 0.25f)} * radius, y + height);
  Emit shell(&mesh, M_WHITE_METAL);
  Emit glass(&mesh, M_GLASS_CLEAR);
  glass.element_random = rng.next();
  Emit truss(&mesh, M_WHITE_METAL);
  Emit rib(&mesh, M_WHITE_METAL);
  // each face: triangle (base_i, base_j, apex) subdivided into 4
  for (int i = 0; i < n; ++i) {
    const Vec3 A = P3(base[i], y), B = P3(base[(i + 1) % n], y), C = apex;
    const Vec3 AB = (A + B) * 0.5f, BC = (B + C) * 0.5f, CA = (C + A) * 0.5f;
    const Vec3 tris[4][3] = {{A, AB, CA}, {AB, B, BC}, {CA, BC, C}, {AB, BC, CA}};
    for (int k = 0; k < 4; ++k) {
      const Vec3 a = tris[k][0], b = tris[k][1], c = tris[k][2];
      const bool is_glass = rng.chance(0.45f) || k == 3;
      const Vec3 nrm = normalize(cross(a - b, c - b));
      if (is_glass) {
        const Vec3 rec = nrm * (-0.35f);
        // facade frame for interior mapping: u along ab, v perpendicular in-plane
        glass.facade = true;
        glass.facade_origin = a + rec;
        glass.facade_u = normalize(a - b);
        glass.facade_v = normalize(cross(nrm, glass.facade_u));
        glass.triangle(b + rec, a + rec, c + rec);
        // inner truss: three struts from the centroid to the edge midpoints
        const Vec3 cen = (a + b + c) * (1.0f / 3.0f) + rec * 0.5f;
        truss.tube(cen, (a + b) * 0.5f + rec * 0.5f, 0.12f, 8, false);
        truss.tube(cen, (b + c) * 0.5f + rec * 0.5f, 0.12f, 8, false);
        truss.tube(cen, (c + a) * 0.5f + rec * 0.5f, 0.12f, 8, false);
        truss.tube((a + b) * 0.5f + rec * 0.5f, (b + c) * 0.5f + rec * 0.5f, 0.09f, 8, false);
        truss.tube((b + c) * 0.5f + rec * 0.5f, (c + a) * 0.5f + rec * 0.5f, 0.09f, 8, false);
        truss.tube((c + a) * 0.5f + rec * 0.5f, (a + b) * 0.5f + rec * 0.5f, 0.09f, 8, false);
      } else {
        shell.triangle(b, a, c);
        // inset panel seams: a slightly recessed inner triangle reads as panels
        const Vec3 cen = (a + b + c) * (1.0f / 3.0f);
        const Vec3 ia = lerp(a, cen, 0.08f) + nrm * 0.02f, ib = lerp(b, cen, 0.08f) + nrm * 0.02f, ic = lerp(c, cen, 0.08f) + nrm * 0.02f;
        shell.triangle(ib, ia, ic);
      }
      // ribs along edges
      rib.tube(a + nrm * 0.02f, b + nrm * 0.02f, 0.2f, 8, false);
      rib.tube(b + nrm * 0.02f, c + nrm * 0.02f, 0.2f, 8, false);
      rib.tube(c + nrm * 0.02f, a + nrm * 0.02f, 0.2f, 8, false);
    }
  }
  // glass base band + floor
  Emit floor(&mesh, M_TERRAZZO);
  floor.polygon(plan_offset(base, 0.5f), y + 0.03f, true);
  Emit light(&mesh, M_LOBBY_LIGHT);
  light.polygon(plan_offset(base, -2.0f), y + 3.0f, false);
  // plinth
  Emit plinth(&mesh, M_CONCRETE_WHITE);
  plinth.wall(plan_offset(base, 1.2f), y - 0.3f, y + 0.35f, true, true);
  plinth.polygon(plan_offset(base, 1.2f), y + 0.35f, true);
}

// ---------------------------------------------------------------------------
// Landscape and street furniture
// ---------------------------------------------------------------------------

void gen_tree(Scene& sc, Rng rng, Vec3 base, float height) {
  Mesh& mesh = sc.opaque;
  Emit bark(&mesh, M_BARK);
  const float trunk_r = height * 0.035f;
  const Vec3 top = base + Vec3{rng.range(-0.3f, 0.3f), height * 0.45f, rng.range(-0.3f, 0.3f)};
  bark.frustum(base, top, trunk_r, trunk_r * 0.7f, 10, false);
  const int branches = 4;
  std::vector<Vec3> tips;
  for (int i = 0; i < branches; ++i) {
    const float a = static_cast<float>(i) / branches * 2.0f * kPi + rng.range(-0.4f, 0.4f);
    const Vec3 tip = top + Vec3{std::cos(a) * height * 0.22f, height * rng.range(0.18f, 0.32f), std::sin(a) * height * 0.22f};
    bark.frustum(top, tip, trunk_r * 0.6f, trunk_r * 0.2f, 7, false);
    tips.push_back(tip);
  }
  // canopy: leaf-cluster quads distributed in an ellipsoid
  Emit leaf(&sc.foliage, M_LEAF);
  const Vec3 cc = top + Vec3{0, height * 0.28f, 0};
  const float rx = height * 0.32f, ry = height * 0.28f;
  const int clusters = 26;
  for (int i = 0; i < clusters; ++i) {
    Vec3 d{rng.range(-1, 1), rng.range(-1, 1), rng.range(-1, 1)};
    d = normalize(d) * std::pow(rng.next(), 0.35f);
    const Vec3 p = cc + Vec3{d.x * rx, d.y * ry, d.z * rx};
    const float size = height * rng.range(0.16f, 0.24f);
    const float ang = rng.range(0, kPi);
    for (int q = 0; q < 2; ++q) {
      const float a = ang + static_cast<float>(q) * kPi * 0.5f;
      const Vec3 right{std::cos(a), 0, std::sin(a)};
      const Vec3 up = normalize(Vec3{rng.range(-0.3f, 0.3f), 1, rng.range(-0.3f, 0.3f)});
      const Vec3 a0 = p - right * size - up * size, b0 = p + right * size - up * size;
      const Vec3 c0 = p + right * size + up * size, d0 = p - right * size + up * size;
      leaf.quad(a0, b0, c0, d0, QuadUV{{0, 1}, {1, 1}, {1, 0}, {0, 0}});
      // canopy shading normal: radial from the canopy centre (smooth volume look)
      for (std::size_t vi = sc.foliage.vertices.size() - 4; vi < sc.foliage.vertices.size(); ++vi) {
        Vertex& v = sc.foliage.vertices[vi];
        const Vec3 rel = Vec3{(v.position.x - cc.x) / rx, (v.position.y - cc.y) / ry, (v.position.z - cc.z) / rx};
        v.normal = normalize(lerp(v.normal, normalize(rel), 0.85f));
        v.aux.w = 0.45f + 0.55f * clampf(length(rel), 0.0f, 1.0f);  // inner leaves darker
      }
    }
  }
}

void gen_lamp(Scene& sc, Vec3 base, float yaw) {
  Mesh& mesh = sc.opaque;
  Emit m(&mesh, M_DARK_METAL);
  const float h = 6.5f;
  m.frustum(base, base + Vec3{0, h, 0}, 0.11f, 0.07f, 10, true);
  const Vec3 dir{std::cos(yaw), 0, std::sin(yaw)};
  const Vec3 arm_end = base + Vec3{0, h, 0} + dir * 1.6f + Vec3{0, 0.25f, 0};
  m.tube(base + Vec3{0, h - 0.1f, 0}, arm_end, 0.05f, 8, false);
  Emit housing(&mesh, M_WHITE_METAL);
  housing.box(arm_end + Vec3{0, -0.08f, 0}, Vec3{0.28f, 0.06f, 0.55f}, dir, Vec3{0, 1, 0}, cross(dir, Vec3{0, 1, 0}));
  Emit l(&mesh, M_LAMP);
  l.box(arm_end + Vec3{0, -0.15f, 0}, Vec3{0.22f, 0.02f, 0.45f}, dir, Vec3{0, 1, 0}, cross(dir, Vec3{0, 1, 0}));
  PointLight pl;
  pl.position = arm_end + Vec3{0, -0.4f, 0};
  pl.radius = 22.0f;
  pl.color = Vec3{1.0f, 0.86f, 0.62f};
  pl.intensity = 160.0f;
  sc.lights.push_back(pl);
}

void gen_bench(Scene& sc, Vec3 pos, float yaw) {
  Mesh& mesh = sc.opaque;
  const Vec3 dir{std::cos(yaw), 0, std::sin(yaw)};
  const Vec3 side = cross(dir, Vec3{0, 1, 0});
  Emit c(&mesh, M_CONCRETE_WHITE);
  c.box(pos + Vec3{0, 0.22f, 0} - dir * 0.8f, Vec3{0.12f, 0.22f, 0.3f}, dir, Vec3{0, 1, 0}, side);
  c.box(pos + Vec3{0, 0.22f, 0} + dir * 0.8f, Vec3{0.12f, 0.22f, 0.3f}, dir, Vec3{0, 1, 0}, side);
  Emit w(&mesh, M_BRONZE);
  for (int i = 0; i < 4; ++i) {
    w.box(pos + Vec3{0, 0.47f, 0} + side * (-0.24f + 0.16f * static_cast<float>(i)), Vec3{1.0f, 0.02f, 0.06f}, dir, Vec3{0, 1, 0}, side);
  }
}

// Ribbon polygon along a polyline (for paths): returns ccw polygon.
std::vector<Vec2> ribbon(const std::vector<Vec2>& line, float half_width) {
  std::vector<Vec2> left, right;
  for (std::size_t i = 0; i < line.size(); ++i) {
    const Vec2 prev = line[i > 0 ? i - 1 : 0], next = line[std::min(i + 1, line.size() - 1)];
    const Vec2 d = normalize(next - prev);
    const Vec2 n{d.y, -d.x};
    left.push_back(line[i] - n * half_width);
    right.push_back(line[i] + n * half_width);
  }
  std::vector<Vec2> poly = right;
  for (auto it = left.rbegin(); it != left.rend(); ++it) poly.push_back(*it);
  if (plan_area(poly) < 0) std::reverse(poly.begin(), poly.end());
  return poly;
}

void gen_park(Scene& sc, Rng rng, Vec2 centre, float y_base) {
  Mesh& mesh = sc.opaque;
  // Terraces: arcs around a sunken plaza, stepping up to the south.
  const Vec2 pc = centre;
  const float a0 = radians(15.0f), a1 = radians(165.0f);  // terraces on the south side, open to the north
  const int levels = 6;
  const float r0 = 21.0f, dr = 3.6f, dh = 0.5f;
  auto arc = [&](float r, int seg, bool reverse) {
    std::vector<Vec2> pts;
    for (int i = 0; i <= seg; ++i) {
      const float t = static_cast<float>(reverse ? seg - i : i) / seg;
      const float a = a0 + (a1 - a0) * t;
      pts.push_back(pc + Vec2{std::cos(a), std::sin(a)} * r);
    }
    return pts;
  };
  // plaza floor (sunken 0.0) as a fan
  {
    Emit p(&mesh, M_PLAZA);
    std::vector<Vec2> fan = arc(r0, 40, false);
    fan.push_back(pc);
    if (plan_area(fan) < 0) std::reverse(fan.begin(), fan.end());
    p.polygon(fan, y_base + 0.01f, true);
  }
  for (int k = 0; k < levels; ++k) {
    const float ri = r0 + dr * static_cast<float>(k), ro = ri + dr;
    const float y = y_base + dh * static_cast<float>(k + 1);
    std::vector<Vec2> ring = arc(ro, 48, false);
    std::vector<Vec2> inner = arc(ri, 48, true);
    ring.insert(ring.end(), inner.begin(), inner.end());
    if (plan_area(ring) < 0) std::reverse(ring.begin(), ring.end());
    Emit top(&mesh, (k % 2 == 0) ? M_GRASS : M_CONCRETE_WHITE);
    top.polygon(ring, y, true);
    // riser
    Emit riser(&mesh, M_CONCRETE_WHITE);
    riser.occlusion = 0.85f;
    std::vector<Vec2> ri_arc = arc(ri, 48, false);
    riser.wall(ri_arc, y - dh, y + 0.02f, false, false);
    // edge strip
    Emit strip(&mesh, M_CONCRETE_WHITE);
    std::vector<Vec2> outer_strip = arc(ri + 0.35f, 48, false);
    std::vector<Vec2> inner_strip = arc(ri, 48, true);
    outer_strip.insert(outer_strip.end(), inner_strip.begin(), inner_strip.end());
    if (plan_area(outer_strip) < 0) std::reverse(outer_strip.begin(), outer_strip.end());
    strip.polygon(outer_strip, y + 0.015f, true);
  }
  // Retaining wall on the outside of the top terrace
  {
    Emit w(&mesh, M_CONCRETE_WHITE);
    const float ro = r0 + dr * levels;
    w.wall(arc(ro, 48, false), y_base, y_base + dh * levels + 0.6f, false, true);
    w.wall(arc(ro, 48, false), y_base + dh * levels, y_base + dh * levels + 0.6f, false, false);
  }
  // Lawn around: handled by the plaza generator (ground). Trees on the terraces.
  for (int k = 0; k < levels; k += 2) {
    const float ri = r0 + dr * static_cast<float>(k) + dr * 0.5f;
    const float y = y_base + dh * static_cast<float>(k + 1);
    for (int i = 0; i < 4 + k; ++i) {
      const float a = a0 + (a1 - a0) * (static_cast<float>(i) + 0.5f) / static_cast<float>(4 + k);
      const Vec2 p = pc + Vec2{std::cos(a), std::sin(a)} * ri;
      gen_tree(sc, rng.child(100 + k * 20 + i), P3(p, y), rng.range(7.0f, 10.5f));
    }
  }
  // Benches facing the plaza on level 1
  for (int i = 0; i < 5; ++i) {
    const float a = a0 + (a1 - a0) * (static_cast<float>(i) + 0.5f) / 5.0f;
    const Vec2 p = pc + Vec2{std::cos(a), std::sin(a)} * (r0 + dr * 1.5f);
    gen_bench(sc, P3(p, y_base + dh * 2.0f), a + kPi * 0.5f);  // along the arc, facing inward
  }
  // Water feature in the plaza centre
  {
    Emit rim(&mesh, M_CONCRETE_WHITE);
    const std::vector<Vec2> pool = plan_circle(5.0f, 40, pc + Vec2{0, 4.0f});
    rim.wall(pool, y_base, y_base + 0.45f, true, true);
    rim.polygon(plan_offset(pool, 0.0f), y_base + 0.45f, true);
    Emit w(&mesh, M_WATER);
    w.polygon(plan_circle(4.6f, 40, pc + Vec2{0, 4.0f}), y_base + 0.38f, true);
  }
}

// Planting beds with shrubs (small foliage clusters) between the buildings.
void gen_planter(Scene& sc, Rng rng, Vec2 centre, float hx, float hz, float y) {
  Mesh& mesh = sc.opaque;
  const std::vector<Vec2> plan = plan_rounded_rect(hx, hz, std::min(hx, hz) * 0.5f, 6, centre);
  Emit c(&mesh, M_CONCRETE_WHITE);
  c.wall(plan, y, y + 0.6f, true, true);
  c.wall(plan_offset(plan, -0.2f), y + 0.2f, y + 0.6f, true, false);
  for (std::size_t i = 0; i < plan.size(); ++i) {
    const std::size_t j = (i + 1) % plan.size();
    const std::vector<Vec2> inner = plan_offset(plan, -0.2f);
    c.quad_metric(P3(plan[j], y + 0.6f), P3(plan[i], y + 0.6f), P3(inner[i], y + 0.6f), P3(inner[j], y + 0.6f));
  }
  Emit soil(&mesh, M_GRASS);
  soil.polygon(plan_offset(plan, -0.2f), y + 0.5f, true);
  Emit leaf(&sc.foliage, M_LEAF);
  const int shrubs = static_cast<int>(hx * hz * 0.25f);
  for (int i = 0; i < shrubs; ++i) {
    const Vec2 p = centre + Vec2{rng.range(-hx + 0.8f, hx - 0.8f), rng.range(-hz + 0.8f, hz - 0.8f)};
    const float s = rng.range(0.5f, 0.9f);
    const Vec3 b = P3(p, y + 0.5f + s * 0.6f);
    for (int q = 0; q < 3; ++q) {
      const float a = rng.range(0, kPi);
      const Vec3 right{std::cos(a), 0, std::sin(a)};
      const Vec3 up{0, 1, 0};
      leaf.quad(b - right * s - up * s * 0.7f, b + right * s - up * s * 0.7f, b + right * s + up * s * 0.7f, b - right * s + up * s * 0.7f,
                QuadUV{{0, 1}, {1, 1}, {1, 0}, {0, 0}});
    }
  }
  if (hx > 3.0f) gen_tree(sc, rng.child(9), P3(centre, y + 0.5f), rng.range(6.0f, 8.0f));
}



std::vector<TextureSetSpec> Scene::texture_sets() const {
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

Scene generate_scene(const SceneParams& params) {
  Scene sc;
  sc.materials = make_materials();
  Rng root = root_rng(params.seed);
  CitySize size = city_size_for(root);
  if (params.size >= 0) size = static_cast<CitySize>(params.size);
  const CityStats st = generate_city(sc, root.child(100), size);
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
