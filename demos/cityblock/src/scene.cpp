#include "scene.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "materials.hpp"
#include "rng.hpp"
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
  set(M_SIDEWALK, "sidewalk", {0.95f, 0.95f, 0.95f}, 0.7f, 0, "paving_slabs", 3.0f, kMatPlanarXZ);
  set(M_PLAZA, "plaza", {0.82f, 0.82f, 0.80f}, 0.65f, 0, "pavement_light", 4.0f, kMatPlanarXZ);
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

namespace {

// 5. Folded pavilion: a faceted white shell with triangulated glazing.
void gen_pavilion(Scene& sc, Rng rng, Vec2 centre, float radius, float height) {
  Mesh& mesh = sc.opaque;
  std::vector<Vec2> base;
  const int n = 5;
  for (int i = 0; i < n; ++i) {
    const float a = static_cast<float>(i) / n * 2.0f * kPi + rng.range(-0.15f, 0.15f);
    const float r = radius * rng.range(0.8f, 1.15f);
    base.push_back(centre + Vec2{std::cos(a), std::sin(a)} * r);
  }
  const Vec3 apex = P3(centre + Vec2{rng.range(-0.25f, 0.25f), rng.range(-0.25f, 0.25f)} * radius, height);
  Emit shell(&mesh, M_WHITE_METAL);
  Emit glass(&mesh, M_GLASS_CLEAR);
  glass.element_random = rng.next();
  Emit truss(&mesh, M_WHITE_METAL);
  Emit rib(&mesh, M_WHITE_METAL);
  // each face: triangle (base_i, base_j, apex) subdivided into 4
  for (int i = 0; i < n; ++i) {
    const Vec3 A = P3(base[i], 0.0f), B = P3(base[(i + 1) % n], 0.0f), C = apex;
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
  floor.polygon(plan_offset(base, 0.5f), 0.03f, true);
  Emit light(&mesh, M_LOBBY_LIGHT);
  light.polygon(plan_offset(base, -2.0f), 3.0f, false);
  // plinth
  Emit plinth(&mesh, M_CONCRETE_WHITE);
  plinth.wall(plan_offset(base, 1.2f), -0.3f, 0.35f, true, true);
  plinth.polygon(plan_offset(base, 1.2f), 0.35f, true);
}

// ---------------------------------------------------------------------------
// Landscape and street furniture
// ---------------------------------------------------------------------------

}  // namespace

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

void gen_park(Scene& sc, Rng rng, Vec2 centre) {
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
    p.polygon(fan, 0.01f, true);
  }
  for (int k = 0; k < levels; ++k) {
    const float ri = r0 + dr * static_cast<float>(k), ro = ri + dr;
    const float y = dh * static_cast<float>(k + 1);
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
    w.wall(arc(ro, 48, false), 0.0f, dh * levels + 0.6f, false, true);
    w.wall(arc(ro, 48, false), dh * levels, dh * levels + 0.6f, false, false);
  }
  // Lawn around: handled by the plaza generator (ground). Trees on the terraces.
  for (int k = 0; k < levels; k += 2) {
    const float ri = r0 + dr * static_cast<float>(k) + dr * 0.5f;
    const float y = dh * static_cast<float>(k + 1);
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
    gen_bench(sc, P3(p, dh * 2.0f), a + kPi * 0.5f);  // along the arc, facing inward
  }
  // Water feature in the plaza centre
  {
    Emit rim(&mesh, M_CONCRETE_WHITE);
    const std::vector<Vec2> pool = plan_circle(5.0f, 40, pc + Vec2{0, 4.0f});
    rim.wall(pool, 0.0f, 0.45f, true, true);
    rim.polygon(plan_offset(pool, 0.0f), 0.45f, true);
    Emit w(&mesh, M_WATER);
    w.polygon(plan_circle(4.6f, 40, pc + Vec2{0, 4.0f}), 0.38f, true);
  }
}

// Ground: plaza, sidewalks, streets, curbs, lane paint, context.
struct BlockDims {
  float hx{120.0f}, hz{100.0f};  // lot half extents (curb line)
  float road_w{16.0f};
  float walk_w{6.0f};
};

void gen_ground_and_streets(Scene& sc, Rng rng, const BlockDims& b) {
  Mesh& mesh = sc.opaque;
  const float curb_h = 0.14f;
  // Block plaza (raised by the curb) — whole lot area
  {
    Emit p(&mesh, M_PLAZA);
    p.polygon(plan_rect(b.hx, b.hz), curb_h, true);
    Emit c(&mesh, M_CURB);
    c.wall(plan_rect(b.hx, b.hz), 0.0f, curb_h, true, true);
  }
  // Streets ring: asphalt from the curb to the far sidewalk
  {
    Emit a(&mesh, M_ASPHALT);
    const float ox = b.hx + b.road_w, oz = b.hz + b.road_w;
    // four rectangles
    a.polygon(plan_rect(ox, b.road_w * 0.5f, Vec2{0, -(b.hz + b.road_w * 0.5f)}), 0.0f, true);
    a.polygon(plan_rect(ox, b.road_w * 0.5f, Vec2{0, (b.hz + b.road_w * 0.5f)}), 0.0f, true);
    a.polygon(plan_rect(b.road_w * 0.5f, oz, Vec2{-(b.hx + b.road_w * 0.5f), 0}), 0.0f, true);
    a.polygon(plan_rect(b.road_w * 0.5f, oz, Vec2{(b.hx + b.road_w * 0.5f), 0}), 0.0f, true);
    // far sidewalks and the outer ground
    Emit s(&mesh, M_SIDEWALK);
    const float sx = ox + b.walk_w, sz = oz + b.walk_w;
    s.polygon(plan_rect(sx, b.walk_w * 0.5f, Vec2{0, -(oz + b.walk_w * 0.5f)}), curb_h, true);
    s.polygon(plan_rect(sx, b.walk_w * 0.5f, Vec2{0, (oz + b.walk_w * 0.5f)}), curb_h, true);
    s.polygon(plan_rect(b.walk_w * 0.5f, sz, Vec2{-(ox + b.walk_w * 0.5f), 0}), curb_h, true);
    s.polygon(plan_rect(b.walk_w * 0.5f, sz, Vec2{(ox + b.walk_w * 0.5f), 0}), curb_h, true);
    Emit c(&mesh, M_CURB);
    c.wall(plan_rect(ox, oz), 0.0f, curb_h, true, false);
    // ground beyond (context lots), slightly below the sidewalk
    Emit g(&mesh, M_ASPHALT);
    const float far = 1600.0f;
    g.polygon({{-far, -far}, {far, -far}, {far, -sz}, {-far, -sz}}, -0.02f, true);
    g.polygon({{-far, sz}, {far, sz}, {far, far}, {-far, far}}, -0.02f, true);
    g.polygon({{-far, -sz}, {-sx, -sz}, {-sx, sz}, {-far, sz}}, -0.02f, true);
    g.polygon({{sx, -sz}, {far, -sz}, {far, sz}, {sx, sz}}, -0.02f, true);
  }
  // Lane paint: centre double yellow + dashed white per direction; crosswalks
  {
    Emit w(&mesh, M_LANE_WHITE);
    Emit yl(&mesh, M_LANE_YELLOW);
    const float y = 0.006f;
    auto road_strip = [&](Vec2 a, Vec2 bpt, float width, Emit& e, bool dashed) {
      const Vec2 d = normalize(bpt - a);
      const Vec2 n{d.y, -d.x};
      const float len = length(bpt - a);
      if (!dashed) {
        e.quad_metric(P3(a - n * width * 0.5f, y), P3(bpt - n * width * 0.5f, y), P3(bpt + n * width * 0.5f, y), P3(a + n * width * 0.5f, y));
        return;
      }
      for (float t = 0.0f; t + 3.0f <= len; t += 9.0f) {
        const Vec2 p0 = a + d * t, p1 = a + d * (t + 3.0f);
        e.quad_metric(P3(p0 - n * width * 0.5f, y), P3(p1 - n * width * 0.5f, y), P3(p1 + n * width * 0.5f, y), P3(p0 + n * width * 0.5f, y));
      }
    };
    const float ox = b.hx + b.road_w, oz = b.hz + b.road_w;
    const float cz_n = -(b.hz + b.road_w * 0.5f), cz_s = (b.hz + b.road_w * 0.5f);
    const float cx_w = -(b.hx + b.road_w * 0.5f), cx_e = (b.hx + b.road_w * 0.5f);
    for (float cz : {cz_n, cz_s}) {
      road_strip(Vec2{-ox + 6.0f, cz - 0.12f}, Vec2{ox - 6.0f, cz - 0.12f}, 0.12f, yl, false);
      road_strip(Vec2{-ox + 6.0f, cz + 0.12f}, Vec2{ox - 6.0f, cz + 0.12f}, 0.12f, yl, false);
      road_strip(Vec2{-ox + 6.0f, cz - 4.0f}, Vec2{ox - 6.0f, cz - 4.0f}, 0.12f, w, true);
      road_strip(Vec2{-ox + 6.0f, cz + 4.0f}, Vec2{ox - 6.0f, cz + 4.0f}, 0.12f, w, true);
    }
    for (float cx : {cx_w, cx_e}) {
      road_strip(Vec2{cx - 0.12f, -oz + 6.0f}, Vec2{cx - 0.12f, oz - 6.0f}, 0.12f, yl, false);
      road_strip(Vec2{cx + 0.12f, -oz + 6.0f}, Vec2{cx + 0.12f, oz - 6.0f}, 0.12f, yl, false);
      road_strip(Vec2{cx - 4.0f, -oz + 6.0f}, Vec2{cx - 4.0f, oz - 6.0f}, 0.12f, w, true);
      road_strip(Vec2{cx + 4.0f, -oz + 6.0f}, Vec2{cx + 4.0f, oz - 6.0f}, 0.12f, w, true);
    }
    // crosswalks at the block corners (across each road at the ends)
    auto zebra = [&](Vec2 start, Vec2 dir, Vec2 across, float across_len) {
      for (int i = 0; i < 12; ++i) {
        const Vec2 p = start + dir * (0.6f + 1.2f * static_cast<float>(i));
        const Vec2 q = p + across * across_len;
        const Vec2 n = dir * 0.3f;
        w.quad_metric(P3(p - n, y), P3(q - n, y), P3(q + n, y), P3(p + n, y));
      }
    };
    for (float sx : {-1.0f, 1.0f}) {
      for (float sz : {-1.0f, 1.0f}) {
        // across the east-west road near the corner
        zebra(Vec2{sx * (b.hx - 16.0f), sz * b.hz}, Vec2{sx, 0}, Vec2{0, sz}, b.road_w);
        zebra(Vec2{sx * b.hx, sz * (b.hz - 16.0f)}, Vec2{0, sz}, Vec2{sx, 0}, b.road_w);
      }
    }
  }
  // Street lamps along the block curb + far sidewalk, trees along the far sidewalk
  {
    const float lx = b.hx - 1.2f, lz = b.hz - 1.2f;
    for (float x = -lx + 12.0f; x < lx; x += 26.0f) {
      gen_lamp(sc, Vec3{x, curb_h, -lz}, -kPi * 0.5f);
      gen_lamp(sc, Vec3{x, curb_h, lz}, kPi * 0.5f);
    }
    for (float z = -lz + 14.0f; z < lz - 6.0f; z += 26.0f) {
      gen_lamp(sc, Vec3{-lx, curb_h, z}, kPi);
      gen_lamp(sc, Vec3{lx, curb_h, z}, 0.0f);
    }
    const float tx = b.hx + b.road_w + b.walk_w * 0.55f, tz = b.hz + b.road_w + b.walk_w * 0.55f;
    int i = 0;
    for (float x = -tx + 10.0f; x < tx - 6.0f; x += 13.0f, ++i) {
      gen_tree(sc, rng.child(300 + i), Vec3{x, curb_h, -tz}, rng.range(6.5f, 9.0f));
      gen_tree(sc, rng.child(400 + i), Vec3{x, curb_h, tz}, rng.range(6.5f, 9.0f));
    }
    for (float z = -tz + 10.0f; z < tz - 6.0f; z += 13.0f, ++i) {
      gen_tree(sc, rng.child(500 + i), Vec3{-tx, curb_h, z}, rng.range(6.5f, 9.0f));
      gen_tree(sc, rng.child(600 + i), Vec3{tx, curb_h, z}, rng.range(6.5f, 9.0f));
    }
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


namespace {

// Context: parametric variants of the hero families around the block, at
// reduced detail, so the city outside the centre shares the vocabulary.
void gen_context(Scene& sc, Rng rng, const BlockDims& b, int rings, int force_detail) {
  const float start_x = b.hx + b.road_w + b.walk_w + 30.0f;
  const float start_z = b.hz + b.road_w + b.walk_w + 30.0f;
  struct Slot { Vec2 c; float half; int detail; };
  std::vector<Slot> slots;
  for (int ring = 0; ring < rings; ++ring) {
    const float rx = start_x + 120.0f * static_cast<float>(ring), rz = start_z + 120.0f * static_cast<float>(ring);
    const int detail = force_detail >= 0 ? force_detail : (ring == 0 ? 1 : 0);
    const int n = 2 + ring;
    for (int i = -n; i <= n; ++i) {
      slots.push_back({Vec2{static_cast<float>(i) * 95.0f, -(rz + 32.0f)}, 15.0f, detail});
      slots.push_back({Vec2{static_cast<float>(i) * 95.0f, (rz + 32.0f)}, 15.0f, detail});
    }
    for (int i = -(n - 1); i <= n - 1; ++i) {
      slots.push_back({Vec2{-(rx + 32.0f), static_cast<float>(i) * 95.0f}, 15.0f, detail});
      slots.push_back({Vec2{(rx + 32.0f), static_cast<float>(i) * 95.0f}, 15.0f, detail});
    }
  }
  int idx = 0;
  for (const Slot& s : slots) {
    Rng r = rng.child(idx++);
    if (r.chance(0.12f)) continue;
    if (r.chance(0.18f)) {
      build_tower_group(sc, r.child(1), s.c, r.range(0, kPi), s.detail);
      continue;
    }
    const int max_floors = length(s.c) > 330.0f ? r.irange(20, 48) : r.irange(10, 30);
    TowerSpec spec = random_tower(r, s.half * r.range(0.75f, 1.1f), max_floors);
    build_tower(sc, spec, s.c, 0.0f, r.child(2), s.detail);
  }
}

}  // namespace

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
  BlockDims dims;
  gen_ground_and_streets(sc, root.child(1), dims);
  // Hero lots (x east, z south) — the named families with their parameters.
  build_tower(sc, spec_diagrid(17.5f, 42), Vec2{-68.0f, -52.0f}, 0.0f, root.child(10), 2);
  {
    // twin lens towers on a shared podium
    const float rot = radians(-12.0f);
    const Vec2 centre{62.0f, -48.0f};
    const Vec2 dir{-std::sin(rot), std::cos(rot)};
    TowerSpec lens = spec_lens(31.0f, 12.0f, 34, rot);
    lens.base = BaseKind::Lobby; lens.base_floors = 1;
    lens.random = 0.31f;
    // shared podium
    const std::vector<Vec2> pod = plan_transform(plan_rounded_rect(40.0f, 34.0f, 8.0f, 6), centre, rot);
    Emit g(&sc.opaque, M_GLASS_CLEAR);
    g.element_random = 0.7f;
    float u = 0.0f;
    const std::vector<Vec2> inner = plan_offset(pod, -0.3f);
    for (std::size_t i = 0; i < inner.size(); ++i) {
      const std::size_t j = (i + 1) % inner.size();
      const float w = length(inner[j] - inner[i]);
      g.quad(P3(inner[j], 0.0f), P3(inner[i], 0.0f), P3(inner[i], 5.5f), P3(inner[j], 5.5f), QuadUV{{u + w, 0}, {u, 0}, {u, 5.5f}, {u + w, 5.5f}});
      u += w;
    }
    slab(sc.opaque, pod, 5.5f, 0.8f, M_CONCRETE_WHITE);
    Emit deck(&sc.opaque, M_TERRAZZO);
    deck.polygon(plan_offset(pod, -0.35f), 5.52f, true);
    parapet(sc.opaque, pod, 5.5f, 1.1f, 0.3f, M_WHITE_METAL);
    for (int side = 0; side < 2; ++side) {
      const float sgn = side == 0 ? 1.0f : -1.0f;
      TowerSpec t = lens;
      t.random = 0.31f + 0.2f * side;
      t.floors = side == 0 ? 34 : 30;
      build_tower(sc, t, centre + dir * (sgn * 15.0f), 5.5f, root.child(11 + side), 2);
    }
    // sky bridge
    const float y = 5.5f + 3.9f * 18.0f;
    Emit m(&sc.opaque, M_WHITE_METAL);
    const Vec2 side = perp(dir) * 4.0f;
    m.box(P3(centre, y - 0.4f), Vec3{4.0f, 0.4f, 9.0f}, Vec3{perp(dir).x, 0, perp(dir).y}, Vec3{0, 1, 0}, Vec3{dir.x, 0, dir.y});
    m.box(P3(centre, y + 4.2f), Vec3{4.0f, 0.3f, 9.0f}, Vec3{perp(dir).x, 0, perp(dir).y}, Vec3{0, 1, 0}, Vec3{dir.x, 0, dir.y});
    Emit bg(&sc.opaque, M_GLASS_CLEAR);
    const Vec2 a = centre - dir * 9.0f, b2 = centre + dir * 9.0f;
    bg.quad_metric(P3(a + side, y), P3(b2 + side, y), P3(b2 + side, y + 3.9f), P3(a + side, y + 3.9f));
    bg.quad_metric(P3(b2 - side, y), P3(a - side, y), P3(a - side, y + 3.9f), P3(b2 - side, y + 3.9f));
  }
  build_tower(sc, spec_finweave(17.0f, 26), Vec2{-74.0f, 52.0f}, 0.0f, root.child(12), 2);
  build_tower(sc, spec_xframe(30.0f, 14.0f, 12), Vec2{72.0f, 58.0f}, 0.0f, root.child(13), 2);
  gen_pavilion(sc, root.child(14), Vec2{0.0f, 66.0f}, 16.0f, 21.0f);
  gen_park(sc, root.child(15), Vec2{0.0f, -6.0f});
  // planters between lots
  gen_planter(sc, root.child(20), Vec2{-20.0f, -75.0f}, 9.0f, 3.0f, 0.14f);
  gen_planter(sc, root.child(21), Vec2{-30.0f, -20.0f}, 4.0f, 10.0f, 0.14f);
  gen_planter(sc, root.child(22), Vec2{30.0f, 10.0f}, 3.0f, 12.0f, 0.14f);
  gen_planter(sc, root.child(23), Vec2{-40.0f, 88.0f}, 12.0f, 3.0f, 0.14f);
  gen_planter(sc, root.child(24), Vec2{40.0f, 88.0f}, 10.0f, 3.0f, 0.14f);
  gen_planter(sc, root.child(25), Vec2{100.0f, 0.0f}, 3.0f, 14.0f, 0.14f);
  gen_planter(sc, root.child(26), Vec2{-104.0f, 0.0f}, 3.0f, 14.0f, 0.14f);
  // tree groves on grass circles in the plaza corners, and paths
  {
    Rng g = root.child(40);
    const Vec2 groves[4] = {{-28.0f, -88.0f}, {28.0f, -88.0f}, {-100.0f, -6.0f}, {104.0f, 14.0f}};
    for (int k = 0; k < 4; ++k) {
      const float r = g.range(7.0f, 9.5f);
      Emit lawn(&sc.opaque, M_GRASS);
      const std::vector<Vec2> circle = plan_circle(r, 40, groves[k]);
      lawn.polygon(circle, 0.16f, true);
      Emit rim(&sc.opaque, M_CONCRETE_WHITE);
      rim.wall(circle, 0.14f, 0.34f, true, true);
      rim.polygon(plan_circle(r, 40, groves[k]), 0.34f, true);
      lawn.polygon(plan_circle(r - 0.25f, 40, groves[k]), 0.36f, true);
      for (int i = 0; i < 5; ++i) {
        const float a = g.range(0, 2 * kPi), rr = g.range(1.5f, r - 2.0f);
        gen_tree(sc, g.child(k * 10 + i), P3(groves[k] + Vec2{std::cos(a), std::sin(a)} * rr, 0.36f), g.range(7.5f, 11.0f));
      }
      for (int i = 0; i < 3; ++i) {
        const float a = static_cast<float>(i) / 3.0f * 2.0f * kPi + 0.4f;
        gen_bench(sc, P3(groves[k] + Vec2{std::cos(a), std::sin(a)} * (r + 1.2f), 0.14f), a + kPi * 0.5f);
      }
    }
    Emit path(&sc.opaque, M_SIDEWALK);
    const Vec2 corners[4] = {{-118.0f, -98.0f}, {118.0f, -98.0f}, {-118.0f, 98.0f}, {118.0f, 98.0f}};
    for (int k = 0; k < 4; ++k) {
      const std::vector<Vec2> ctrl = {corners[k], corners[k] * 0.55f + Vec2{g.range(-15.0f, 15.0f), g.range(-15.0f, 15.0f)}, Vec2{0.0f, -6.0f} + normalize(corners[k]) * 24.0f};
      path.polygon(ribbon(spline(ctrl, 10), 2.2f), 0.155f, true);
    }
    Emit bol(&sc.opaque, M_SILVER);
    for (float x = -110.0f; x <= 110.0f; x += 4.0f) bol.tube(Vec3{x, 0.14f, -97.0f}, Vec3{x, 1.0f, -97.0f}, 0.09f, 8, true);
  }
  if (params.context_buildings) gen_context(sc, root.child(30), dims, params.context_rings, params.context_detail);
  sc.camera_position = Vec3{-150.0f, 26.0f, 165.0f};
  sc.camera_target = Vec3{-8.0f, 46.0f, -24.0f};
  return sc;
}

}  // namespace cb
