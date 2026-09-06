#include "city/flora.hpp"

#include <algorithm>
#include <cmath>

#include "city/materials.hpp"
#include "city/towers.hpp"

namespace inf::city {

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



}  // namespace inf::city
