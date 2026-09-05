#include "props.hpp"

#include <algorithm>
#include <cmath>

#include "site.hpp"
#include "towers.hpp"

namespace cb {

namespace {

Vec3 X3(Vec2 v) { return Vec3{v.x, 0, v.y}; }

void glass_wall(Emit& g, Vec2 a, Vec2 b, float y0, float y1, float u0) {
  const float w = length(b - a);
  g.quad(P3(b, y0), P3(a, y0), P3(a, y1), P3(b, y1), QuadUV{{u0 + w, y0}, {u0, y0}, {u0, y1}, {u0 + w, y1}});
}

}  // namespace

// ---- entrances -----------------------------------------------------------------

void build_entrance(Scene& sc, EntranceKind kind, Vec2 p, Vec2 n, float y, float storey_h, Rng& rng, int detail) {
  Mesh& mesh = sc.opaque;
  const Vec2 t{-n.y, n.x};  // along the facade
  const Vec3 n3 = X3(n), t3 = X3(t);
  const float door_w = 3.2f, door_h = std::min(storey_h - 0.3f, 3.2f);
  // door: dark frame + glass, set slightly proud of the facade
  {
    Emit frame(&mesh, M_DARK_METAL);
    frame.box(P3(p, y + door_h * 0.5f) + n3 * 0.06f, Vec3{door_w * 0.5f + 0.15f, door_h * 0.5f + 0.1f, 0.06f}, t3, Vec3{0, 1, 0}, n3);
    Emit g(&mesh, M_GLASS_CLEAR);
    g.element_random = rng.next();
    const Vec2 a = p - t * (door_w * 0.5f), b = p + t * (door_w * 0.5f);
    const Vec3 off = n3 * 0.13f;
    g.quad(P3(b, y) + off, P3(a, y) + off, P3(a, y + door_h) + off, P3(b, y + door_h) + off, QuadUV{{door_w, 0}, {0, 0}, {0, door_h}, {door_w, door_h}});
    Emit l(&mesh, M_LOBBY_LIGHT);
    l.box(P3(p, y + door_h + 0.02f) + n3 * 0.12f, Vec3{door_w * 0.5f, 0.04f, 0.04f}, t3, Vec3{0, 1, 0}, n3);
  }
  switch (kind) {
    case EntranceKind::Canopy: {
      Emit c(&mesh, M_WHITE_METAL);
      const float depth = 3.0f, width = door_w + 3.0f;
      c.box(P3(p, y + door_h + 0.5f) + n3 * (depth * 0.5f), Vec3{width * 0.5f, 0.12f, depth * 0.5f}, t3, Vec3{0, 1, 0}, n3);
      Emit post(&mesh, M_DARK_METAL);
      for (float s : {-1.0f, 1.0f}) {
        const Vec3 base = P3(p, y) + n3 * (depth - 0.4f) + t3 * (s * (width * 0.5f - 0.3f));
        post.tube(base, base + Vec3{0, door_h + 0.4f, 0}, 0.08f, detail >= 1 ? 8 : 5, false);
      }
      break;
    }
    case EntranceKind::Portal: {
      if (detail == 0) break;  // far buildings keep only the door
      Emit c(&mesh, M_WHITE_METAL);
      const float pw = door_w + 2.4f, ph = door_h + 1.2f, pd = 1.4f;
      // a proud portal frame: two jambs and a lintel
      c.box(P3(p, y + ph * 0.5f) + n3 * (pd * 0.5f) - t3 * (pw * 0.5f - 0.5f), Vec3{0.5f, ph * 0.5f, pd * 0.5f}, t3, Vec3{0, 1, 0}, n3);
      c.box(P3(p, y + ph * 0.5f) + n3 * (pd * 0.5f) + t3 * (pw * 0.5f - 0.5f), Vec3{0.5f, ph * 0.5f, pd * 0.5f}, t3, Vec3{0, 1, 0}, n3);
      c.box(P3(p, y + ph + 0.35f) + n3 * (pd * 0.5f), Vec3{pw * 0.5f, 0.35f, pd * 0.5f}, t3, Vec3{0, 1, 0}, n3);
      break;
    }
    case EntranceKind::Stairs: {
      Emit c(&mesh, M_MARBLE_WHITE);
      const int steps = 4;
      const float width = door_w + 6.0f;
      for (int k = 0; k < steps; ++k) {
        const float depth = 0.4f * static_cast<float>(steps - k);
        c.box(P3(p, y + 0.16f * static_cast<float>(k) + 0.08f) + n3 * (depth * 0.5f + 0.8f), Vec3{width * 0.5f - 0.3f * k, 0.08f, depth * 0.5f}, t3, Vec3{0, 1, 0}, n3);
      }
      Emit col(&mesh, M_CONCRETE_WHITE);
      for (float s : {-1.0f, 1.0f}) {
        const Vec3 base = P3(p, y) + n3 * 1.6f + t3 * (s * (door_w * 0.5f + 1.4f));
        col.tube(base, base + Vec3{0, door_h + 1.0f, 0}, 0.28f, detail >= 1 ? 10 : 6, false);
      }
      Emit slab(&mesh, M_WHITE_METAL);
      slab.box(P3(p, y + door_h + 1.15f) + n3 * 1.0f, Vec3{door_w * 0.5f + 2.0f, 0.15f, 1.2f}, t3, Vec3{0, 1, 0}, n3);
      break;
    }
    case EntranceKind::Vestibule: {
      Emit g(&mesh, M_GLASS_CLEAR);
      g.element_random = rng.next();
      const float depth = 2.6f, width = door_w + 2.0f, h = door_h + 0.6f;
      const Vec2 c0 = p - t * (width * 0.5f), c1 = p + t * (width * 0.5f);
      const Vec2 f0 = c0 + n * depth, f1 = c1 + n * depth;
      glass_wall(g, f0, f1, y, y + h, 0.0f);       // front
      glass_wall(g, c1, f1, y, y + h, 0.0f);       // right side (outward = +t)
      glass_wall(g, f0, c0, y, y + h, 0.0f);       // left side
      Emit roof(&mesh, M_WHITE_METAL);
      roof.box(P3(p, y + h + 0.1f) + n3 * (depth * 0.5f), Vec3{width * 0.5f + 0.2f, 0.1f, depth * 0.5f + 0.2f}, t3, Vec3{0, 1, 0}, n3);
      break;
    }
  }
}

// ---- roofs -----------------------------------------------------------------------

void build_roof(Scene& sc, RoofKind kind, const std::vector<Vec2>& fp, float y, Rng& rng, int detail, Mat wall) {
  Mesh& mesh = sc.opaque;
  switch (kind) {
    case RoofKind::Flat: {
      slab(mesh, fp, y + 0.3f, 0.3f, M_ROOF);
      break;
    }
    case RoofKind::Parapet: {
      slab(mesh, fp, y + 0.3f, 0.3f, M_ROOF);
      parapet(mesh, fp, y + 0.3f, 0.9f, 0.3f, wall);
      if (detail >= 1) roof_equipment(mesh, rng, fp, y + 0.3f, detail >= 2 ? 3 : 1);
      break;
    }
    case RoofKind::Green: {
      slab(mesh, fp, y + 0.3f, 0.3f, M_ROOF);
      parapet(mesh, fp, y + 0.3f, 1.0f, 0.3f, wall);
      Emit lawn(&mesh, M_HEDGE);
      lawn.polygon(plan_offset(fp, -0.6f), y + 0.45f, true);
      if (detail >= 1) {
        const Vec2 c = plan_centroid(fp);
        const float r = plan_inradius(fp);
        for (int i = 0; i < 3; ++i) {
          const float a = rng.range(0, 2 * kPi), d = rng.range(0.2f, 0.6f) * r;
          const Vec2 q = c + Vec2{std::cos(a), std::sin(a)} * d;
          build_hedge(sc, q - Vec2{1.2f, 0}, q + Vec2{1.2f, 0}, 0.8f, 0.7f, y + 0.45f);
        }
      }
      break;
    }
    case RoofKind::Monopitch: {
      // Sloped metal roof rising along the long axis; gable walls fill the wedge.
      const Vec2 axis = plan_long_axis(fp);
      float lo, hi;
      plan_extent(fp, axis, &lo, &hi);
      const float rise = std::min(2.4f, (hi - lo) * 0.18f);
      auto h_at = [&](Vec2 q) { return y + 0.1f + rise * (dot(q, axis) - lo) / std::max(hi - lo, 1e-3f); };
      Emit w(&mesh, wall);
      for (std::size_t i = 0; i < fp.size(); ++i) {
        const Vec2 a = fp[i], b = fp[(i + 1) % fp.size()];
        const float len = length(b - a);
        w.quad(P3(b, y), P3(a, y), P3(a, h_at(a)), P3(b, h_at(b)), QuadUV{{len, 0}, {0, 0}, {0, h_at(a) - y}, {len, h_at(b) - y}});
      }
      // roof plate (planar, sloped): triangulate the outset footprint at h_at
      const std::vector<Vec2> out = plan_offset(fp, 0.5f);
      const std::vector<std::uint32_t> tris = triangulate(out);
      Emit r(&mesh, M_ROOF_METAL);
      for (std::size_t k = 0; k + 2 < tris.size(); k += 3) {
        const Vec2 a = out[tris[k]], b = out[tris[k + 1]], c = out[tris[k + 2]];
        r.triangle(P3(a, h_at(a) + 0.15f), P3(c, h_at(c) + 0.15f), P3(b, h_at(b) + 0.15f));
        r.triangle(P3(a, h_at(a)), P3(b, h_at(b)), P3(c, h_at(c)));
      }
      break;
    }
  }
}

// ---- water ---------------------------------------------------------------------------

void build_basin(Scene& sc, Vec2 c, float rx, float rz, bool round, float y, float rim_h) {
  Mesh& mesh = sc.opaque;
  const std::vector<Vec2> outer = round ? plan_superellipse(rx, rz, 2.0f, 40, c) : plan_rounded_rect(rx, rz, 0.6f, 3, c);
  const std::vector<Vec2> inner = plan_offset(outer, -0.45f);
  Emit rim(&mesh, M_MARBLE_WHITE);
  rim.wall(outer, y, y + rim_h, true, true);
  rim.wall(inner, y + rim_h - 0.35f, y + rim_h, true, false);
  rim.ring_cap(outer, inner, y + rim_h, true);
  Emit w(&mesh, M_WATER);
  w.polygon(inner, y + rim_h - 0.12f, true);
}

void build_fountain(Scene& sc, Vec2 c, float radius, float y, Rng& rng, int detail) {
  Mesh& mesh = sc.opaque;
  build_basin(sc, c, radius, radius, true, y, 0.5f);
  Emit m(&mesh, M_MARBLE_WHITE);
  const int sides = detail >= 1 ? 24 : 12;
  const int tiers = rng.irange(2, 3);
  float yy = y + 0.38f;
  float r = radius * 0.45f;
  m.tube(P3(c, yy), P3(c, yy + 0.6f), radius * 0.12f, sides, false);
  for (int k = 0; k < tiers; ++k) {
    yy += 0.6f + 0.15f * k;
    m.frustum(P3(c, yy - 0.35f), P3(c, yy), r * 0.35f, r, sides, true);  // bowl
    Emit w(&mesh, M_WATER);
    w.polygon(plan_circle(r * 0.92f, sides, c), yy + 0.02f, true);
    m.tube(P3(c, yy), P3(c, yy + 0.7f), radius * 0.08f, sides, false);
    r *= 0.62f;
  }
  Emit top(&mesh, M_CHROME);
  top.sphere(P3(c, yy + 0.8f), radius * 0.1f, 6, sides);
}

// ---- hedges and walls ---------------------------------------------------------------

void build_hedge(Scene& sc, Vec2 a, Vec2 b, float width, float height, float y) {
  Emit h(&sc.opaque, M_HEDGE);
  h.occlusion = 0.85f;
  const Vec2 d = b - a;
  const float len = length(d);
  if (len < 0.3f) return;
  const Vec2 dir = d * (1.0f / len);
  h.box(P3((a + b) * 0.5f, y + height * 0.5f), Vec3{len * 0.5f, height * 0.5f, width * 0.5f}, X3(dir), Vec3{0, 1, 0}, X3(Vec2{dir.y, -dir.x}));
}

void build_hedge_ring(Scene& sc, const std::vector<Vec2>& poly, float inset, float width, float height, float y, float gap_every, Rng& rng) {
  const std::vector<Vec2> p = plan_offset(poly, -inset);
  for (std::size_t i = 0; i < p.size(); ++i) {
    const Vec2 a = p[i], b = p[(i + 1) % p.size()];
    const float len = length(b - a);
    if (len < 2.0f) continue;
    const Vec2 dir = normalize(b - a);
    const int gaps = gap_every > 0.0f ? static_cast<int>(len / gap_every) : 0;
    float t = 0.0f;
    for (int g = 0; g <= gaps; ++g) {
      const float end = gaps == 0 ? len : std::min(len, (static_cast<float>(g) + 1.0f) * gap_every - 2.5f);
      if (end > t + 1.0f) build_hedge(sc, a + dir * t, a + dir * end, width, height * rng.range(0.9f, 1.1f), y);
      t = end + 2.5f;
    }
  }
}

void build_low_wall(Scene& sc, const std::vector<Vec2>& poly, float inset, float height, float thickness, float y, Mat mat, float gap_every, Rng& rng) {
  (void)rng;
  const std::vector<Vec2> p = plan_offset(poly, -inset);
  Emit w(&sc.opaque, mat);
  for (std::size_t i = 0; i < p.size(); ++i) {
    const Vec2 a = p[i], b = p[(i + 1) % p.size()];
    const float len = length(b - a);
    if (len < 1.0f) continue;
    const Vec2 dir = normalize(b - a);
    const int gaps = gap_every > 0.0f ? static_cast<int>(len / gap_every) : 0;
    float t = 0.0f;
    for (int g = 0; g <= gaps; ++g) {
      const float end = gaps == 0 ? len : std::min(len, (static_cast<float>(g) + 1.0f) * gap_every - 3.0f);
      if (end > t + 0.5f) {
        const Vec2 m = a + dir * ((t + end) * 0.5f);
        w.box(P3(m, y + height * 0.5f), Vec3{(end - t) * 0.5f, height * 0.5f, thickness * 0.5f}, X3(dir), Vec3{0, 1, 0}, X3(Vec2{dir.y, -dir.x}));
        // cap
        w.box(P3(m, y + height + 0.04f), Vec3{(end - t) * 0.5f + 0.05f, 0.04f, thickness * 0.5f + 0.08f}, X3(dir), Vec3{0, 1, 0}, X3(Vec2{dir.y, -dir.x}));
      }
      t = end + 3.0f;
    }
  }
}

// ---- foundation with wide stairs -------------------------------------------------------

std::vector<Vec2> build_foundation(Scene& sc, const std::vector<Vec2>& fp, float y, float height, int stair_edge, int detail) {
  Mesh& mesh = sc.opaque;
  (void)detail;
  Emit m(&mesh, M_MARBLE_WHITE);
  m.wall(fp, y, y + height, true, true);
  m.polygon(fp, y + height, true);
  // stairs: full width of the chosen edge, 0.16 m risers, 0.42 m treads
  const std::size_t n = fp.size();
  const std::size_t i = static_cast<std::size_t>(stair_edge) % n;
  const Vec2 a = fp[i], b = fp[(i + 1) % n];
  const Vec2 dir = normalize(b - a);
  const Vec2 out{dir.y, -dir.x};
  const int steps = std::max(2, static_cast<int>(height / 0.16f));
  const float len = length(b - a) * 0.7f;
  const Vec2 mid = (a + b) * 0.5f;
  for (int k = 0; k < steps; ++k) {
    const float top = y + height - 0.16f * static_cast<float>(k);
    const float depth = 0.42f * static_cast<float>(k + 1);
    m.box(P3(mid, top - 0.08f) + X3(out) * (depth - 0.21f), Vec3{len * 0.5f, 0.08f, 0.21f}, X3(dir), Vec3{0, 1, 0}, X3(out));
  }
  // side cheeks
  for (float s : {-1.0f, 1.0f}) {
    const float depth = 0.42f * static_cast<float>(steps);
    m.box(P3(mid, y + height * 0.5f) + X3(out) * (depth * 0.5f) + X3(dir) * (s * (len * 0.5f + 0.4f)), Vec3{0.4f, height * 0.5f, depth * 0.5f}, X3(dir), Vec3{0, 1, 0}, X3(out));
  }
  return fp;
}

// ---- monuments ------------------------------------------------------------------------

void build_monument(Scene& sc, MonumentKind kind, Vec2 c, float y, float scale, Rng& rng, int detail) {
  Mesh& mesh = sc.opaque;
  const int sides = detail >= 1 ? 16 : 8;
  // podium
  {
    Emit p(&mesh, M_MARBLE_WHITE);
    const std::vector<Vec2> pod = plan_circle(2.2f * scale, 24, c);
    p.wall(pod, y, y + 0.6f, true, true);
    p.polygon(pod, y + 0.6f, true);
  }
  const float base_y = y + 0.6f;
  switch (kind) {
    case MonumentKind::Pillar: {
      Emit m(&mesh, M_MARBLE_WHITE);
      const float h = 9.0f * scale;
      m.frustum(P3(c, base_y), P3(c, base_y + 0.5f), 1.1f * scale, 0.9f * scale, sides, true);
      // fluted shaft: a ring of thin tubes around a core
      m.tube(P3(c, base_y + 0.5f), P3(c, base_y + h), 0.55f * scale, sides, false);
      if (detail >= 1) {
        for (int i = 0; i < 12; ++i) {
          const float a = static_cast<float>(i) / 12.0f * 2.0f * kPi;
          const Vec2 q = c + Vec2{std::cos(a), std::sin(a)} * (0.6f * scale);
          m.tube(P3(q, base_y + 0.5f), P3(q, base_y + h), 0.09f * scale, 6, false);
        }
      }
      m.frustum(P3(c, base_y + h), P3(c, base_y + h + 0.6f * scale), 0.7f * scale, 1.0f * scale, sides, true);
      Emit top(&mesh, M_CHROME);
      top.sphere(P3(c, base_y + h + 1.3f * scale), 0.7f * scale, 8, sides);
      break;
    }
    case MonumentKind::Ribbon: {
      // a twisted metal band: a helix of flat beams
      Emit m(&mesh, M_CHROME);
      const float h = 10.0f * scale, R = 1.6f * scale;
      const int segs = detail >= 1 ? 48 : 24;
      const float turns = rng.range(1.2f, 2.2f);
      Vec3 prev;
      for (int i = 0; i <= segs; ++i) {
        const float t = static_cast<float>(i) / segs;
        const float a = t * turns * 2.0f * kPi;
        const Vec3 p = P3(c + Vec2{std::cos(a), std::sin(a)} * (R * (1.0f - 0.35f * t)), base_y + h * t);
        if (i > 0) m.beam(prev, p, 0.12f * scale, 0.9f * scale, Vec3{std::cos(a), 0.4f, std::sin(a)});
        prev = p;
      }
      break;
    }
    case MonumentKind::Weave: {
      // two strands of the diagrid, standing free and crossing each other
      Emit m(&mesh, M_WHITE_METAL);
      const float h = 12.0f * scale, R = 2.0f * scale;
      const int segs = detail >= 1 ? 40 : 20;
      for (int strand = 0; strand < 2; ++strand) {
        const float dir = strand == 0 ? 1.0f : -1.0f;
        Vec3 prev;
        for (int i = 0; i <= segs; ++i) {
          const float t = static_cast<float>(i) / segs;
          const float a = dir * t * 1.5f * 2.0f * kPi + static_cast<float>(strand) * kPi;
          const Vec3 p = P3(c + Vec2{std::cos(a), std::sin(a)} * R, base_y + h * t);
          if (i > 0) m.tube(prev, p, 0.28f * scale, detail >= 1 ? 10 : 6, false);
          if (i % 8 == 0) m.sphere(p, 0.36f * scale, 6, 8);
          prev = p;
        }
      }
      // ring at the top joining the strands
      m.torus(P3(c, base_y + h), Vec3{0, 1, 0}, R, 0.22f * scale, 32, 8);
      break;
    }
    case MonumentKind::Obelisk: {
      Emit m(&mesh, M_MARBLE_WHITE);
      const float h = 14.0f * scale;
      m.frustum(P3(c, base_y), P3(c, base_y + h), 0.9f * scale, 0.25f * scale, 4, true);
      Emit top(&mesh, M_CHROME);
      top.frustum(P3(c, base_y + h), P3(c, base_y + h + 1.0f * scale), 0.25f * scale, 0.0f, 4, false);
      break;
    }
  }
}

void build_unification_ring(Scene& sc, Vec2 c, float y, float radius, float facing_rot, int detail) {
  Mesh& mesh = sc.opaque;
  // podium: a low wide marble drum with a step
  Emit p(&mesh, M_MARBLE_WHITE);
  const std::vector<Vec2> pod = plan_circle(radius * 0.75f, 48, c);
  p.wall(pod, y, y + 1.0f, true, true);
  p.polygon(pod, y + 1.0f, true);
  const std::vector<Vec2> step = plan_circle(radius * 0.75f + 1.2f, 48, c);
  p.wall(step, y, y + 0.4f, true, true);
  p.ring_cap(step, pod, y + 0.4f, true);
  // the ring stands vertical, facing the given direction
  const Vec3 axis{std::cos(facing_rot), 0, std::sin(facing_rot)};
  Emit ring(&mesh, M_CHROME);
  ring.torus(P3(c, y + 1.0f + radius + 0.3f), axis, radius, radius * 0.06f, detail >= 1 ? 96 : 48, detail >= 1 ? 16 : 8);
  // inner glow band at night
  Emit glow(&mesh, M_SIGN);
  glow.torus(P3(c, y + 1.0f + radius + 0.3f), axis, radius - radius * 0.06f, radius * 0.012f, 64, 6);
  // cradle: two short pylons holding the ring above the podium
  Emit pyl(&mesh, M_WHITE_METAL);
  const Vec3 side = normalize(cross(axis, Vec3{0, 1, 0}));
  for (float s : {-1.0f, 1.0f}) {
    const Vec3 foot = P3(c, y + 1.0f) + side * (s * radius * 0.55f);
    const Vec3 top = P3(c, y + 1.0f + radius * 0.5f) + side * (s * radius * 0.86f);
    pyl.beam(foot, top, 0.6f, 0.8f, Vec3{0, 1, 0});
  }
}

void build_landing_pad(Scene& sc, Vec2 c, float radius, float y, Rng& rng, int detail) {
  Mesh& mesh = sc.opaque;
  const int seg = detail >= 1 ? 64 : 32;
  const std::vector<Vec2> pad = plan_circle(radius, seg, c);
  Emit p(&mesh, M_PAD);
  p.wall(pad, y, y + 0.35f, true, true);
  p.polygon(pad, y + 0.35f, true);
  // markings: outer ring and an inner ring, a cross
  Emit mk(&mesh, M_LANE_YELLOW);
  mk.ring_cap(plan_circle(radius - 0.6f, seg, c), plan_circle(radius - 1.0f, seg, c), y + 0.36f, true);
  mk.ring_cap(plan_circle(radius * 0.45f, seg, c), plan_circle(radius * 0.45f - 0.35f, seg, c), y + 0.36f, true);
  Emit wh(&mesh, M_LANE_WHITE);
  for (int k = 0; k < 2; ++k) {
    const float a = static_cast<float>(k) * kPi * 0.5f;
    const Vec2 d{std::cos(a), std::sin(a)};
    wh.box(P3(c, y + 0.36f), Vec3{radius * 0.4f, 0.005f, 0.25f}, X3(d), Vec3{0, 1, 0}, X3(Vec2{d.y, -d.x}));
  }
  // edge lights
  Emit lt(&mesh, M_PAD_LIGHT);
  const int lights = detail >= 1 ? 16 : 8;
  for (int i = 0; i < lights; ++i) {
    const float a = static_cast<float>(i) / lights * 2.0f * kPi;
    lt.box(P3(c + Vec2{std::cos(a), std::sin(a)} * (radius - 0.3f), y + 0.42f), Vec3{0.18f, 0.07f, 0.18f});
  }
  // control mast
  Emit m(&mesh, M_WHITE_METAL);
  const Vec2 mp = c + Vec2{radius + 3.0f, 0.0f} * (rng.chance(0.5f) ? 1.0f : -1.0f);
  m.tube(P3(mp, y), P3(mp, y + 7.0f), 0.25f, 8, true);
  m.box(P3(mp, y + 7.6f), Vec3{1.2f, 0.6f, 1.2f});
  Emit g(&mesh, M_GLASS_CLEAR);
  g.element_random = rng.next();
  const std::vector<Vec2> cab = plan_rect(1.15f, 1.15f, mp);
  float u = 0.0f;
  for (std::size_t i = 0; i < 4; ++i) {
    const Vec2 a = cab[i], b = cab[(i + 1) % 4];
    g.quad(P3(b, y + 7.6f), P3(a, y + 7.6f), P3(a, y + 8.8f), P3(b, y + 8.8f), QuadUV{{u + 2.3f, 0}, {u, 0}, {u, 1.2f}, {u + 2.3f, 1.2f}});
    u += 2.3f;
  }
  m.box(P3(mp, y + 8.9f), Vec3{1.3f, 0.1f, 1.3f});
}

// ---- overpass --------------------------------------------------------------------------

void build_overpass(Scene& sc, const std::vector<Vec2>& ctrl, float deck_y, float ground_y, Rng& rng, int detail) {
  Mesh& mesh = sc.opaque;
  (void)rng;
  const std::vector<Vec2> path = spline(ctrl, detail >= 1 ? 8 : 4);
  const float half_w = 2.2f;
  Emit deck(&mesh, M_WHITE_METAL);
  Emit rail(&mesh, M_CHROME);
  Emit floor(&mesh, M_TERRAZZO);
  std::vector<Vec2> left, right;
  for (std::size_t i = 0; i < path.size(); ++i) {
    const Vec2 prev = path[i > 0 ? i - 1 : 0], next = path[std::min(i + 1, path.size() - 1)];
    const Vec2 d = normalize(next - prev);
    const Vec2 n{d.y, -d.x};
    left.push_back(path[i] - n * half_w);
    right.push_back(path[i] + n * half_w);
  }
  // deck: a gently arched slab (thicker in the middle of the span)
  for (std::size_t i = 0; i + 1 < path.size(); ++i) {
    const float t0 = static_cast<float>(i) / (path.size() - 1), t1 = static_cast<float>(i + 1) / (path.size() - 1);
    const float th0 = 0.35f + 0.45f * std::sin(t0 * kPi), th1 = 0.35f + 0.45f * std::sin(t1 * kPi);
    const Vec3 L0 = P3(left[i], deck_y), R0 = P3(right[i], deck_y), L1 = P3(left[i + 1], deck_y), R1 = P3(right[i + 1], deck_y);
    const Vec3 dn0{0, th0, 0}, dn1{0, th1, 0};
    floor.quad(L0, R0, R1, L1, QuadUV{{0, t0 * 100.0f}, {half_w * 2.0f, t0 * 100.0f}, {half_w * 2.0f, t1 * 100.0f}, {0, t1 * 100.0f}});
    deck.quad(R0 - dn0, L0 - dn0, L1 - dn1, R1 - dn1, QuadUV{{0, 0}, {1, 0}, {1, 1}, {0, 1}});  // underside
    deck.quad_metric(L0 - dn0, L1 - dn1, L1, L0);                                                  // left side
    deck.quad_metric(R1 - dn1, R0 - dn0, R0, R1);                                                  // right side
    if (detail >= 1) {
      rail.tube(L0 + Vec3{0.1f, 1.1f, 0}, L1 + Vec3{0.1f, 1.1f, 0}, 0.04f, 6, false);
      rail.tube(R0 - Vec3{0.1f, -1.1f, 0}, R1 - Vec3{0.1f, -1.1f, 0}, 0.04f, 6, false);
      if (i % 2 == 0) {
        rail.tube(L0 + Vec3{0.1f, 0, 0}, L0 + Vec3{0.1f, 1.1f, 0}, 0.03f, 5, false);
        rail.tube(R0 - Vec3{0.1f, 0, 0}, R0 + Vec3{-0.1f, 1.1f, 0}, 0.03f, 5, false);
      }
    }
    // slender columns every ~18 m, leaning slightly like a Y
    if (i % 12 == 6) {
      Emit col(&mesh, M_WHITE_METAL);
      const Vec3 base = P3(path[i], ground_y);
      col.frustum(base, P3(path[i], deck_y - 0.6f), 0.45f, 0.3f, detail >= 1 ? 12 : 6, false);
    }
  }
  // stair towers at both ends
  for (int e = 0; e < 2; ++e) {
    const Vec2 end = e == 0 ? path.front() : path.back();
    const Vec2 dir = e == 0 ? normalize(path[0] - path[1]) : normalize(path.back() - path[path.size() - 2]);
    const Vec2 n{dir.y, -dir.x};
    const int steps = static_cast<int>((deck_y - ground_y) / 0.17f);
    Emit st(&mesh, M_CONCRETE_WHITE);
    // a straight flight continuing the deck direction, landing on the ground
    for (int k = 0; k < steps; ++k) {
      const float top = deck_y - 0.17f * static_cast<float>(k);
      const Vec2 q = end + dir * (0.3f * static_cast<float>(k) + 0.15f);
      st.box(P3(q, top - 0.085f), Vec3{half_w, 0.085f, 0.15f}, X3(n), Vec3{0, 1, 0}, X3(dir));
    }
    // side walls of the flight
    const float run = 0.3f * static_cast<float>(steps);
    for (float s : {-1.0f, 1.0f}) {
      const Vec3 a = P3(end + n * (s * (half_w + 0.1f)), ground_y);
      const Vec3 b = P3(end + n * (s * (half_w + 0.1f)) + dir * run, ground_y);
      const Vec3 c = P3(end + n * (s * (half_w + 0.1f)), deck_y + 1.0f);
      if (s > 0) { st.triangle(a, c, b); st.triangle(a, b, c); }
      else { st.triangle(a, b, c); st.triangle(a, c, b); }
    }
  }
}

// ---- government building ----------------------------------------------------------------

void build_settler_house(Scene& sc, Vec2 c, float rot, float y, Rng& rng, int detail, bool deck) {
  Mesh& mesh = sc.opaque;
  // a small glass pavilion for two: rounded plan, floor-to-ceiling glass,
  // a thin white roof slab, a solar array on top
  const std::vector<Vec2> plan = plan_transform(plan_rounded_rect(5.0f, 3.6f, 1.4f, 4), c, rot);
  const float h = 3.4f;
  Emit g(&mesh, M_GLASS_CLEAR);
  g.element_random = rng.next();
  float u = 0.0f;
  for (std::size_t i = 0; i < plan.size(); ++i) {
    const std::size_t j = (i + 1) % plan.size();
    const float w = length(plan[j] - plan[i]);
    glass_wall(g, plan[i], plan[j], y + 0.3f, y + h, u);
    u += w;
  }
  Emit w(&mesh, M_WHITE_METAL);
  w.wall(plan_offset(plan, 0.05f), y, y + 0.3f, true, true);
  Emit fl(&mesh, M_TERRAZZO);
  fl.polygon(plan, y + 0.31f, true);
  Emit l(&mesh, M_LOBBY_LIGHT);
  l.polygon(plan_offset(plan, -0.6f), y + h - 0.1f, false);
  slab(mesh, plan_offset(plan, 0.9f), y + h + 0.25f, 0.25f, M_WHITE_METAL);
  Emit sol(&mesh, M_GLASS_DARK);
  sol.element_random = rng.next();
  sol.polygon(plan_offset(plan, 0.3f), y + h + 0.28f, true);
  if (detail >= 1) {
    for (std::size_t i = 0; i < plan.size(); i += 3) {
      const Vec2 q = plan[i] + normalize(plan[i] - c) * 0.6f;
      w.tube(P3(q, y), P3(q, y + h), 0.06f, 6, false);
    }
  }
  if (deck) {
    const std::vector<Vec2> deck_plan = plan_transform(plan_rounded_rect(9.0f, 7.0f, 2.0f, 4), c, rot);
    Emit d(&mesh, M_CONCRETE_WHITE);
    d.wall(deck_plan, y - 0.3f, y, true, true);
    d.polygon(deck_plan, y, true);
    build_hedge_ring(sc, deck_plan, 0.6f, 0.6f, 0.6f, y, 8.0f, rng);
  }
  // the couple's utilities: a water tank and an antenna
  Emit u2(&mesh, M_SILVER);
  const Vec2 t = c + plan_transform({Vec2{6.5f, 2.5f}}, Vec2{0, 0}, rot)[0];
  u2.tube(P3(t, y), P3(t, y + 1.6f), 0.7f, 12, true);
  u2.tube(P3(t + Vec2{1.5f, 0}, y), P3(t + Vec2{1.5f, 0}, y + 5.0f), 0.05f, 6, true);
}

void build_pod_wreck(Scene& sc, Vec2 c, float y, Rng& rng, int detail, bool memorial) {
  Mesh& mesh = sc.opaque;
  const int seg = detail >= 1 ? 40 : 20;
  // the scorched pad ring the pod came down on
  Emit dark(&mesh, M_PANEL_DARK);
  dark.polygon(plan_circle(9.0f, seg, c), y + 0.005f, true);
  Emit rim(&mesh, memorial ? M_MARBLE_WHITE : M_CONCRETE);
  rim.ring_cap(plan_circle(9.6f, seg, c), plan_circle(8.8f, seg, c), y + 0.15f, true);
  rim.wall(plan_circle(9.6f, seg, c), y, y + 0.15f, true, true);
  // the pod: a truncated cone hull, tilted, landing legs (matte white so it
  // does not read as a light source in the sun)
  Emit hull(&mesh, M_WHITE_METAL);
  const Vec3 base = P3(c + Vec2{1.5f, -0.8f}, y + 0.9f);
  const Vec3 tilt = normalize(Vec3{0.18f, 1.0f, -0.12f});
  hull.frustum(base, base + tilt * 4.2f, 2.6f, 1.4f, detail >= 1 ? 20 : 10, true);
  hull.frustum(base + tilt * 4.2f, base + tilt * 5.4f, 1.4f, 0.4f, detail >= 1 ? 16 : 8, true);
  Emit leg(&mesh, M_DARK_METAL);
  for (int i = 0; i < 4; ++i) {
    const float a = static_cast<float>(i) / 4.0f * 2.0f * kPi + 0.4f;
    const Vec3 foot = P3(c + Vec2{1.5f, -0.8f} + Vec2{std::cos(a), std::sin(a)} * 3.6f, y + 0.15f);
    leg.tube(base + Vec3{std::cos(a) * 2.0f, 0.4f, std::sin(a) * 2.0f}, foot, 0.12f, 6, true);
    leg.frustum(foot, foot + Vec3{0, 0.12f, 0}, 0.45f, 0.5f, 8, true);
  }
  // hatch door lying on the ground, a couple of crates
  Emit door(&mesh, M_SILVER);
  door.box(P3(c + Vec2{-3.0f, 2.5f}, y + 0.2f), Vec3{1.1f, 0.05f, 0.8f}, Vec3{0.9f, 0, 0.44f}, Vec3{0, 1, 0}, Vec3{-0.44f, 0, 0.9f});
  Emit crate(&mesh, M_PANEL_DARK);
  crate.box(P3(c + Vec2{-4.0f, -3.0f}, y + 0.5f), Vec3{0.6f, 0.5f, 0.6f});
  crate.box(P3(c + Vec2{-2.7f, -3.4f}, y + 0.35f), Vec3{0.5f, 0.35f, 0.5f});
  if (memorial) {
    Emit lt(&mesh, M_PAD_LIGHT);
    for (int i = 0; i < 8; ++i) {
      const float a = static_cast<float>(i) / 8.0f * 2.0f * kPi;
      lt.box(P3(c + Vec2{std::cos(a), std::sin(a)} * 9.2f, y + 0.22f), Vec3{0.15f, 0.06f, 0.15f});
    }
  } else {
    Emit deb(&mesh, M_DARK_METAL);
    for (int i = 0; i < 6; ++i) {
      const Vec2 q = c + Vec2{rng.range(-8.0f, 8.0f), rng.range(-8.0f, 8.0f)};
      const float a = rng.range(0.0f, 3.0f);
      deb.box(P3(q, y + 0.1f), Vec3{rng.range(0.2f, 0.6f), 0.08f, rng.range(0.2f, 0.5f)}, Vec3{std::cos(a), 0, std::sin(a)}, Vec3{0, 1, 0}, Vec3{-std::sin(a), 0, std::cos(a)});
    }
  }
}

void build_government(Scene& sc, Vec2 c, float rot, float half, float y, Rng& rng, int detail, int stage) {
  Mesh& mesh = sc.opaque;
  if (stage <= 0) {
    build_settler_house(sc, c, rot, y, rng, detail, false);
    return;
  }
  if (stage == 1) {
    build_settler_house(sc, c, rot, y, rng, detail, true);
    Emit col(&mesh, M_CONCRETE_WHITE);
    for (int i = 0; i < 8; ++i) {
      const float a = static_cast<float>(i) / 8.0f * 2.0f * kPi;
      const Vec2 q = c + Vec2{std::cos(a), std::sin(a)} * 13.0f;
      col.tube(P3(q, y), P3(q, y + 4.5f), 0.3f, 10, false);
    }
    Emit ring(&mesh, M_WHITE_METAL);
    ring.torus(P3(c, y + 4.6f), Vec3{0, 1, 0}, 13.0f, 0.18f, 48, 8);
    return;
  }
  // stage >= 2: a civic building. Marble is for the foundation only; walls,
  // columns and entablature are the clean white the towers use.
  const float body_scale = stage == 2 ? 0.55f : (stage == 3 ? 0.8f : 1.0f);
  const float h2 = half * body_scale;
  const float fh = stage == 2 ? 0.9f : (stage == 3 ? 2.2f : 3.2f);
  const std::vector<Vec2> base = plan_transform(plan_rect(h2 * 1.25f, h2 * 1.05f), c, rot);
  build_foundation(sc, base, y, fh, 2, detail);
  const float top = y + fh;
  const std::vector<Vec2> body = plan_transform(plan_superellipse(h2 * 0.78f, h2 * 0.62f, 4.0f, detail >= 1 ? 64 : 32), c, rot);
  const float storey = stage == 2 ? 4.6f : 5.2f;
  const int storeys = stage == 2 ? 2 : (stage == 3 ? 3 : (stage == 4 ? 4 : 5));
  Emit wall(&mesh, M_WHITE_METAL);
  Emit glass(&mesh, M_GLASS_CLEAR);
  glass.element_random = rng.next();
  const float body_top = top + storey * storeys;
  float u = 0.0f;
  for (std::size_t i = 0; i < body.size(); ++i) {
    const Vec2 a = body[i], b = body[(i + 1) % body.size()];
    const float w = length(b - a);
    for (int f = 0; f < storeys; ++f) {
      const float y0 = top + storey * f, y1 = y0 + storey;
      glass.quad(P3(b, y0 + 0.6f), P3(a, y0 + 0.6f), P3(a, y1 - 0.4f), P3(b, y1 - 0.4f), QuadUV{{u + w, y0 + 0.6f}, {u, y0 + 0.6f}, {u, y1 - 0.4f}, {u + w, y1 - 0.4f}});
      wall.quad(P3(b, y0), P3(a, y0), P3(a, y0 + 0.6f), P3(b, y0 + 0.6f), QuadUV{{u + w, 0}, {u, 0}, {u, 0.6f}, {u + w, 0.6f}});
      wall.quad(P3(b, y1 - 0.4f), P3(a, y1 - 0.4f), P3(a, y1), P3(b, y1), QuadUV{{u + w, 0}, {u, 0}, {u, 0.4f}, {u + w, 0.4f}});
    }
    u += w;
  }
  const std::vector<Vec2> ring = plan_offset(body, stage == 2 ? 3.0f : 4.5f);
  Emit col(&mesh, M_WHITE_METAL);
  const float per = plan_perimeter(ring);
  const int cols = std::max(12, static_cast<int>(per / 6.0f));
  Sampled sm = plan_sample(ring, per / cols);
  for (std::size_t i = 0; i < sm.points.size(); ++i) {
    col.tube(P3(sm.points[i], top), P3(sm.points[i], body_top - 0.2f), 0.5f, detail >= 1 ? 14 : 8, false);
    if (detail >= 1) col.frustum(P3(sm.points[i], body_top - 0.9f), P3(sm.points[i], body_top - 0.2f), 0.5f, 0.75f, 12, false);
  }
  slab(mesh, plan_offset(ring, 1.0f), body_top + 0.6f, 0.8f, M_WHITE_METAL);
  parapet(mesh, plan_offset(ring, 1.0f), body_top + 0.6f, 0.9f, 0.4f, M_WHITE_METAL);
  if (stage >= 3) {
    const float dome_r = h2 * (stage == 3 ? 0.4f : (stage == 5 ? 0.6f : 0.5f));
    const Vec3 dc = P3(c, body_top + 0.6f);
    Emit dome(&mesh, M_GLASS_CLEAR);
    dome.element_random = rng.next();
    dome.sphere(dc, dome_r, detail >= 1 ? 12 : 6, detail >= 1 ? 32 : 16);
    Emit ribs(&mesh, M_WHITE_METAL);
    const int nribs = detail >= 1 ? 16 : 8;
    for (int r = 0; r < nribs; ++r) {
      const float a = static_cast<float>(r) / nribs * 2.0f * kPi;
      Vec3 prev = dc + Vec3{std::cos(a) * dome_r, 0, std::sin(a) * dome_r};
      for (int k = 1; k <= 8; ++k) {
        const float ph = static_cast<float>(k) / 8.0f * kPi * 0.5f;
        const Vec3 p = dc + Vec3{std::cos(a) * std::cos(ph) * dome_r, std::sin(ph) * dome_r, std::sin(a) * std::cos(ph) * dome_r};
        ribs.tube(prev, p, 0.18f, 6, false);
        prev = p;
      }
    }
    ribs.torus(dc + Vec3{0, dome_r * 0.5f, 0}, Vec3{0, 1, 0}, dome_r * 0.866f, 0.16f, 48, 6);
    Emit lantern(&mesh, M_CHROME);
    lantern.tube(dc + Vec3{0, dome_r, 0}, dc + Vec3{0, dome_r + 6.0f, 0}, 0.3f, 8, true);
    Emit beacon(&mesh, M_SIGN);
    beacon.sphere(dc + Vec3{0, dome_r + 6.3f, 0}, 0.6f, 6, 10);
  } else {
    Emit roof(&mesh, M_ROOF);
    roof.polygon(plan_offset(body, -0.3f), body_top + 0.62f, true);
  }
  if (stage >= 4) {
    const Vec2 front = plan_transform({Vec2{0, h2 * 1.05f + 6.0f}}, c, rot)[0];
    const Vec2 side = plan_transform({Vec2{h2 * 0.9f, 0}}, Vec2{0, 0}, rot)[0];
    const int flags = stage == 5 ? 3 : 1;
    for (int k = 0; k < flags; ++k) {
      for (float sgn : {-1.0f, 1.0f}) {
        Emit m(&mesh, M_CHROME);
        const Vec2 q = front + side * (sgn * (1.0f - 0.25f * static_cast<float>(k)));
        m.tube(P3(q, y), P3(q, y + 12.0f), 0.12f, 8, true);
        Emit flag(&mesh, M_SIGN);
        flag.box(P3(q, y + 11.0f), Vec3{0.05f, 0.8f, 1.2f});
      }
    }
  }
}

}  // namespace cb
