#include "standards.hpp"

#include <algorithm>
#include <cmath>

#include "towers.hpp"

namespace cb {

StandardSpec random_standard(Rng& rng, float area, float city_t) {
  StandardSpec s;
  const float u = rng.next();
  if (u < 0.32f) {
    s.type = StdType::Office; s.floor_h = 3.8f; s.wall = rng.chance(0.5f) ? M_WALL_LIGHT : M_PANEL_DARK;
    s.glass_lo = 0.35f; s.glass_hi = 0.45f; s.storeys = rng.irange(4, 6);
  } else if (u < 0.62f) {
    s.type = StdType::Residential; s.floor_h = 3.2f; s.wall = rng.chance(0.6f) ? M_PANEL_WARM : M_WALL_LIGHT;
    s.glass_lo = 0.95f; s.glass_hi = 0.5f; s.storeys = rng.irange(3, 6); s.balconies = rng.chance(0.6f);
  } else if (u < 0.80f) {
    s.type = StdType::Mixed; s.floor_h = 3.6f; s.wall = M_WALL_LIGHT;
    s.glass_lo = 0.8f; s.glass_hi = 0.5f; s.storeys = rng.irange(3, 5); s.retail_ground = true;
  } else if (u < 0.90f) {
    s.type = StdType::Civic; s.floor_h = 4.4f; s.wall = M_MARBLE_WHITE;
    s.glass_lo = 0.6f; s.glass_hi = 0.7f; s.storeys = rng.irange(3, 4); s.pilasters = true;
  } else {
    s.type = StdType::Lab; s.floor_h = 4.0f; s.wall = M_PANEL_DARK;
    s.glass_lo = 1.2f; s.glass_hi = 0.6f; s.storeys = rng.irange(3, 5);
  }
  // taller toward the centre, smaller lots fewer storeys
  if (city_t < 0.4f) s.storeys = std::min(6, s.storeys + 1);
  if (area < 500.0f) s.storeys = std::max(3, s.storeys - 1);
  const int e = rng.irange(0, 3);
  s.entrance = static_cast<EntranceKind>(e);
  if (s.type == StdType::Civic) s.entrance = rng.chance(0.7f) ? EntranceKind::Stairs : EntranceKind::Portal;
  const float rr = rng.next();
  s.roof = rr < 0.35f ? RoofKind::Parapet : (rr < 0.6f ? RoofKind::Green : (rr < 0.8f ? RoofKind::Monopitch : RoofKind::Flat));
  if (s.type == StdType::Civic) s.roof = RoofKind::Parapet;
  s.random = rng.next();
  return s;
}

void build_standard(Scene& sc, const StandardSpec& spec, const std::vector<Vec2>& fp, float y, Rng rng, int detail) {
  Mesh& mesh = sc.opaque;
  const std::size_t n = fp.size();
  Emit wall(&mesh, spec.wall);
  Emit glass(&mesh, spec.glass);
  glass.element_random = spec.random;
  Emit trim(&mesh, M_WHITE_METAL);
  Emit dark(&mesh, M_DARK_METAL);
  float u = 0.0f;
  // choose the entrance edge: the longest one
  std::size_t best = 0;
  float best_len = -1.0f;
  for (std::size_t i = 0; i < n; ++i) {
    const float l = length(fp[(i + 1) % n] - fp[i]);
    if (l > best_len) { best_len = l; best = i; }
  }
  for (std::size_t i = 0; i < n; ++i) {
    const Vec2 a = fp[i], b = fp[(i + 1) % n];
    const float w = length(b - a);
    const Vec2 d = normalize(b - a);
    const Vec3 n3{d.y, 0, -d.x};
    for (int f = 0; f < spec.storeys; ++f) {
      const float y0 = y + spec.floor_h * static_cast<float>(f), y1 = y0 + spec.floor_h;
      float lo = spec.glass_lo, hi = spec.glass_hi;
      if (f == 0 && spec.retail_ground) { lo = 0.15f; hi = 0.35f; }
      const float g0 = y0 + lo, g1 = y1 - hi;
      const bool punched = (spec.type == StdType::Residential || spec.type == StdType::Civic) && !(f == 0 && spec.retail_ground);
      if (punched) {
        // solid wall with individual windows standing 3 cm proud of it
        wall.quad(P3(b, y0), P3(a, y0), P3(a, y1), P3(b, y1), QuadUV{{u + w, 0}, {u, 0}, {u, y1 - y0}, {u + w, y1 - y0}});
        const float pitch = spec.type == StdType::Civic ? 4.2f : 3.4f;
        const int count = std::max(1, static_cast<int>(w / pitch));
        const float ww = spec.type == StdType::Civic ? 1.6f : 1.5f;
        for (int k = 0; k < count; ++k) {
          const Vec2 q = a + d * (w * (static_cast<float>(k) + 0.5f) / count);
          const Vec2 qa = q - d * (ww * 0.5f), qb = q + d * (ww * 0.5f);
          const Vec3 off = n3 * 0.03f;
          const float uu = u + w * (static_cast<float>(k) + 0.5f) / count;
          glass.quad(P3(qb, g0) + off, P3(qa, g0) + off, P3(qa, g1) + off, P3(qb, g1) + off, QuadUV{{uu + ww * 0.5f, g0}, {uu - ww * 0.5f, g0}, {uu - ww * 0.5f, g1}, {uu + ww * 0.5f, g1}});
          if (detail >= 2) {
            trim.box(P3(q, g0 - 0.04f) + n3 * 0.08f, Vec3{ww * 0.5f + 0.1f, 0.04f, 0.08f}, Vec3{d.x, 0, d.y}, Vec3{0, 1, 0}, n3);
          }
        }
      } else {
        // wall bands
        wall.quad(P3(b, y0), P3(a, y0), P3(a, g0), P3(b, g0), QuadUV{{u + w, 0}, {u, 0}, {u, lo}, {u + w, lo}});
        wall.quad(P3(b, g1), P3(a, g1), P3(a, y1), P3(b, y1), QuadUV{{u + w, 0}, {u, 0}, {u, hi}, {u + w, hi}});
        // glass strip, recessed
        const Vec3 rec = n3 * (-0.18f);
        glass.quad(P3(b, g0) + rec, P3(a, g0) + rec, P3(a, g1) + rec, P3(b, g1) + rec, QuadUV{{u + w, g0}, {u, g0}, {u, g1}, {u + w, g1}});
        if (detail >= 2) {
          trim.box(P3((a + b) * 0.5f, g0 - 0.03f) + n3 * (-0.09f), Vec3{w * 0.5f, 0.03f, 0.09f}, Vec3{d.x, 0, d.y}, Vec3{0, 1, 0}, n3);
        }
      }
      // balconies on the residential long facades
      if (spec.balconies && f > 0 && w > 8.0f && (i == best || w > 14.0f)) {
        const int count = std::max(1, static_cast<int>(w / 6.0f));
        for (int k = 0; k < count; ++k) {
          const Vec2 q = a + d * (w * (static_cast<float>(k) + 0.5f) / count);
          trim.box(P3(q, y0 + 0.08f) + n3 * 0.8f, Vec3{1.6f, 0.08f, 0.8f}, Vec3{d.x, 0, d.y}, Vec3{0, 1, 0}, n3);
          if (detail >= 1) {
            dark.box(P3(q, y0 + 0.6f) + n3 * 1.55f, Vec3{1.6f, 0.5f, 0.02f}, Vec3{d.x, 0, d.y}, Vec3{0, 1, 0}, n3);
          }
        }
      }
    }
    // pilaster at the corner
    if (spec.pilasters) {
      const float h = spec.floor_h * spec.storeys;
      Emit pil(&mesh, spec.wall);
      pil.box(P3(a, y + h * 0.5f) + n3 * 0.12f, Vec3{0.35f, h * 0.5f, 0.24f}, Vec3{d.x, 0, d.y}, Vec3{0, 1, 0}, n3);
    }
    u += w;
  }
  // slab line between storeys as one thin dark band per edge (reads as floors)
  if (detail >= 1) {
    Emit band(&mesh, M_SPANDREL);
    for (int f = 1; f < spec.storeys; ++f) {
      const float yy = y + spec.floor_h * static_cast<float>(f);
      band.wall(plan_offset(fp, 0.02f), yy - 0.12f, yy + 0.06f, true, true);
    }
  }
  const float top = y + spec.floor_h * spec.storeys;
  build_roof(sc, spec.roof, fp, top, rng, detail, spec.wall);
  // entrance on the longest edge
  {
    const Vec2 a = fp[best], b = fp[(best + 1) % n];
    const Vec2 d = normalize(b - a);
    const Vec2 nrm{d.y, -d.x};
    build_entrance(sc, spec.entrance, (a + b) * 0.5f, nrm, y, spec.floor_h, rng, detail);
  }
  // retail awning
  if (spec.retail_ground && detail >= 1) {
    Emit aw(&mesh, M_DARK_METAL);
    for (std::size_t i = 0; i < n; ++i) {
      const Vec2 a = fp[i], b = fp[(i + 1) % n];
      const float w = length(b - a);
      if (w < 6.0f) continue;
      const Vec2 d = normalize(b - a);
      const Vec3 n3{d.y, 0, -d.x};
      aw.box(P3((a + b) * 0.5f, y + spec.floor_h - 0.3f) + n3 * 0.7f, Vec3{w * 0.5f - 0.5f, 0.04f, 0.7f}, Vec3{d.x, 0, d.y}, Vec3{0, 1, 0}, n3);
    }
  }
}

}  // namespace cb
