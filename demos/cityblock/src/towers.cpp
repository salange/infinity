#include "towers.hpp"

#include <algorithm>
#include <cmath>

namespace cb {

Vec3 P3(Vec2 xz, float y) { return Vec3{xz.x, y, xz.y}; }

namespace {

// Outward-facing wall quad for plans that are counter-clockwise on paper
// (A,B bottom left→right seen from outside, C,D the top corners above B,A).
void wall_quad(Emit& e, Vec3 A, Vec3 B, Vec3 C, Vec3 D, Vec2 uvA, Vec2 uvB, Vec2 uvC, Vec2 uvD) {
  e.quad(B, A, D, C, QuadUV{uvB, uvA, uvD, uvC});
}

int sides_for(int detail, int full) { return std::max(4, detail >= 2 ? full : (detail == 1 ? full * 2 / 3 : full / 2)); }

std::vector<Vec2> base_plan(const TowerSpec& s, int detail) {
  const int seg = detail >= 2 ? 96 : (detail == 1 ? 56 : 32);
  switch (s.plan) {
    case PlanKind::Superellipse: return plan_superellipse(s.a, s.b, s.exponent, seg, Vec2{0, 0}, 0.0f);
    case PlanKind::Circle: return plan_circle(s.a, seg);
    case PlanKind::RoundedRect: return plan_rounded_rect(s.a, s.b, std::min(s.a, s.b) * 0.35f, std::max(3, seg / 12));
    case PlanKind::Polygon: return plan_circle(s.a, std::max(3, s.sides));
    case PlanKind::Lens: {
      // half width a, half thickness b: r - d = b, r^2 - d^2 = a^2
      const float r = 0.5f * (s.b + s.a * s.a / s.b);
      const float d = r - s.b;
      return plan_lens(r, d, std::max(8, seg / 2), Vec2{0, 0}, 0.0f);
    }
  }
  return plan_circle(s.a, seg);
}

struct Profile {
  const TowerSpec* spec;
  std::vector<Vec2> unit;  // base plan at scale 1, unrotated, centred at origin
  Vec2 centre;
  float scale_at(int f) const {
    const float F = static_cast<float>(std::max(spec->floors, 1));
    const float t = static_cast<float>(f) / F;
    float sc = 1.0f - spec->taper * t * t;
    if (spec->tip > 0.0f && t > 0.85f) sc *= 1.0f - spec->tip * std::pow((t - 0.85f) / 0.15f, 1.6f);
    if (spec->setback_floor >= 0 && f >= spec->setback_floor) sc *= spec->setback_scale;
    return std::max(sc, 0.15f);
  }
  float rot_at(int f) const {
    const float F = static_cast<float>(std::max(spec->floors, 1));
    return spec->rot + spec->twist * static_cast<float>(f) / F;
  }
  std::vector<Vec2> at(int f) const {
    std::vector<Vec2> p = plan_scale(unit, scale_at(f), Vec2{0, 0});
    return plan_transform(p, centre, rot_at(f));
  }
  // Point on the plan at floor f at perimeter fraction t (0..1), and the
  // outward normal there.
  void point(int f, float t, Vec2* out, Vec2* n) const {
    const std::vector<Vec2> p = at(f);
    const std::size_t nseg = p.size();
    const float total = plan_perimeter(p);
    float target = std::fmod(std::fmod(t, 1.0f) + 1.0f, 1.0f) * total;
    for (std::size_t i = 0; i < nseg; ++i) {
      const Vec2 a = p[i], b = p[(i + 1) % nseg];
      const float len = length(b - a);
      if (target <= len || i + 1 == nseg) {
        const float u = len > 1e-6f ? std::min(target / len, 1.0f) : 0.0f;
        *out = a + (b - a) * u;
        const Vec2 d = normalize(b - a);
        *n = Vec2{d.y, -d.x};
        return;
      }
      target -= len;
    }
  }
};

struct Ctx {
  Scene* sc;
  Mesh* mesh;
  const TowerSpec* s;
  Profile prof;
  Rng rng;
  int detail;
  float base_y;
  float shaft_y0;  // where the shaft facade starts
};

// ---- facade panels ---------------------------------------------------------

void panel_glass(Ctx& c, Vec3 A, Vec3 B, Vec3 C, Vec3 D, float u0, float u1, float y0, float y1, Vec3 n3,
                 float recess, Mat glass) {
  Emit g(c.mesh, glass);
  g.element_random = c.s->random;
  const Vec3 rec = n3 * (-recess);
  wall_quad(g, A + rec, B + rec, C + rec, D + rec, Vec2{u0, y0}, Vec2{u1, y0}, Vec2{u1, y1}, Vec2{u0, y1});
}

void panel_mullion(Ctx& c, Vec3 A, Vec3 D, Vec3 n3, Vec3 along, float w, float d, float recess) {
  Emit e(c.mesh, c.s->frame);
  e.occlusion = 0.9f;
  const Vec3 mid = (A + D) * 0.5f + n3 * (d * 0.5f - recess);
  e.box(mid, Vec3{w * 0.5f, length(D - A) * 0.5f, d * 0.5f}, along, normalize(D - A), n3);
}

void panel_transom(Ctx& c, Vec3 D, Vec3 C, Vec3 n3, Vec3 along, float h, float d, float recess) {
  Emit e(c.mesh, c.s->frame);
  e.occlusion = 0.9f;
  const Vec3 mid = (C + D) * 0.5f + n3 * (d * 0.5f - recess);
  e.box(mid, Vec3{length(C - D) * 0.5f, h * 0.5f, d * 0.5f}, along, Vec3{0, 1, 0}, n3);
}

void panel_fin_h(Ctx& c, Vec3 A, Vec3 B, Vec3 n3, Vec3 along, float depth, float thick, Mat mat) {
  Emit e(c.mesh, mat);
  e.occlusion = 0.95f;
  const Vec3 mid = (A + B) * 0.5f + n3 * (depth * 0.5f - 0.02f) + Vec3{0, thick * 0.5f, 0};
  e.box(mid, Vec3{length(B - A) * 0.5f + 0.02f, thick * 0.5f, depth * 0.5f}, along, Vec3{0, 1, 0}, n3);
}

// One floor of facade between plans p0 (bottom) and p1 (top).
void facade_floor(Ctx& c, int f, const std::vector<Vec2>& p0, const std::vector<Vec2>& p1, float y0, float y1) {
  const TowerSpec& s = *c.s;
  const std::size_t n = p0.size();
  float u = 0.0f;
  const float recess = (s.facade == FacadeKind::Sail) ? 0.0f : 0.06f;
  const bool boxes = c.detail >= 2 || (c.detail == 1 && (s.facade == FacadeKind::Ribbon || s.facade == FacadeKind::FinWeave));
  const float sp = s.spandrel_h;
  for (std::size_t i = 0; i < n; ++i) {
    const std::size_t j = (i + 1) % n;
    const float elen = length(p0[j] - p0[i]);
    const int k = std::max(1, static_cast<int>(std::round(elen / s.module_w)));
    for (int m = 0; m < k; ++m) {
      const float ta = static_cast<float>(m) / k, tb = static_cast<float>(m + 1) / k;
      const Vec2 a0 = p0[i] + (p0[j] - p0[i]) * ta, b0 = p0[i] + (p0[j] - p0[i]) * tb;
      const Vec2 a1 = p1[i] + (p1[j] - p1[i]) * ta, b1 = p1[i] + (p1[j] - p1[i]) * tb;
      const float w = length(b0 - a0);
      const Vec3 A = P3(a0, y0), B = P3(b0, y0), C = P3(b1, y1), D = P3(a1, y1);
      const Vec2 d2 = normalize(b0 - a0);
      const Vec3 n3 = normalize(Vec3{d2.y, 0, -d2.x});
      const Vec3 along = normalize(B - A);
      // glass (above the spandrel band)
      panel_glass(c, A + Vec3{0, sp, 0}, B + Vec3{0, sp, 0}, C, D, u, u + w, y0 + sp, y1, n3, recess, s.glass);
      if (sp > 0.0f) {
        Emit e(c.mesh, M_SPANDREL);
        wall_quad(e, A + n3 * (-recess * 0.5f), B + n3 * (-recess * 0.5f), B + n3 * (-recess * 0.5f) + Vec3{0, sp, 0},
                  A + n3 * (-recess * 0.5f) + Vec3{0, sp, 0}, Vec2{u, 0}, Vec2{u + w, 0}, Vec2{u + w, sp}, Vec2{u, sp});
      }
      switch (s.facade) {
        case FacadeKind::Curtain:
          if (boxes) {
            panel_mullion(c, A, D, n3, along, 0.08f, 0.14f, recess);
            panel_transom(c, D, C, n3, along, 0.3f, 0.14f, recess);
            if (w > 2.2f) panel_mullion(c, lerp(A, B, 0.5f), lerp(D, C, 0.5f), n3, along, 0.05f, 0.1f, recess);
          }
          break;
        case FacadeKind::Sail:
          break;  // smooth glass; the shader draws the mullion grid
        case FacadeKind::Ribbon:
          if (boxes) panel_fin_h(c, A, B, n3, along, s.fin_depth, 0.22f, s.member);
          break;
        case FacadeKind::FinWeave: {
          if (boxes) panel_transom(c, D, C, n3, along, 0.45f, 0.1f, recess);
          if (c.detail >= 1) {
            Emit fin(c.mesh, s.member);
            fin.occlusion = 0.95f;
            const float shift = (f % 2) ? 0.5f : 0.0f;
            const Vec3 base = lerp(A, B, std::fmod(0.5f + shift, 1.0f));
            const float phase = (u + w * 0.5f) / std::max(plan_perimeter(p0), 1.0f) * 2.0f * kPi;
            const float depth = s.fin_depth * (0.55f + 0.45f * std::sin(phase * 3.0f + static_cast<float>(f) * 0.35f) * std::cos(static_cast<float>(f) * 0.11f));
            fin.box(base + Vec3{0, (y1 - y0) * 0.5f, 0} + n3 * (depth * 0.5f), Vec3{0.07f, (y1 - y0) * 0.5f - 0.02f, depth * 0.5f}, along, Vec3{0, 1, 0}, n3);
          }
          break;
        }
        case FacadeKind::Louvre: {
          if (boxes) panel_mullion(c, A, D, n3, along, 0.06f, 0.1f, recess);
          if (c.detail >= 1) {
            Emit bl(c.mesh, s.member);
            bl.occlusion = 0.95f;
            const int blades = c.detail >= 2 ? 4 : 2;
            for (int q = 0; q < blades; ++q) {
              const float yy = y0 + (y1 - y0) * (static_cast<float>(q) + 0.5f) / blades;
              const Vec3 mid = lerp(A, B, 0.5f) + Vec3{0, yy - y0, 0} + n3 * (s.fin_depth * 0.5f);
              const Vec3 tilted = normalize(Vec3{0, 1, 0} * 0.85f + n3 * 0.55f);
              bl.box(mid, Vec3{w * 0.5f, 0.03f, s.fin_depth * 0.5f}, along, tilted, normalize(cross(along, tilted)));
            }
          }
          break;
        }
        case FacadeKind::Diagrid:
        case FacadeKind::XFrame:
        case FacadeKind::HexLattice:
          if (boxes) {
            panel_mullion(c, A, D, n3, along, 0.06f, 0.1f, recess);
            panel_transom(c, D, C, n3, along, 0.25f, 0.1f, recess);
          }
          break;
      }
      u += w;
    }
  }
}

// Dark slab band behind the glass at a floor line.
void floor_band(Ctx& c, const std::vector<Vec2>& plan, float y) {
  Emit e(c.mesh, M_SPANDREL);
  e.wall(plan_offset(plan, -0.09f), y - 0.55f, y + 0.15f, true, true);
}

// ---- lattices ----------------------------------------------------------------

void lattice(Ctx& c, int first_row, int rows_total, float offset, bool crown_rows) {
  const TowerSpec& s = *c.s;
  const int rows_per_cell = std::max(1, s.lattice_rows);
  const float cell_h = static_cast<float>(rows_per_cell) * s.floor_h;
  const float per = plan_perimeter(c.prof.at(0));
  const float module = s.facade == FacadeKind::XFrame ? std::max(6.0f, s.module_w * 3.0f)
                                                      : (s.facade == FacadeKind::HexLattice ? s.module_w * 1.6f : s.module_w * 2.0f);
  const int M = std::max(6, static_cast<int>(std::round(per / module)));
  const int sides = sides_for(c.detail, 12);
  const float r = s.member_r * (c.detail == 0 ? 1.1f : 1.0f);
  Emit mem(c.mesh, s.member);
  auto node = [&](float i, float j) {
    const int f = std::min(static_cast<int>(std::round(j * static_cast<float>(rows_per_cell))), s.floors + rows_per_cell * 2);
    Vec2 p, n;
    c.prof.point(f, i / static_cast<float>(M), &p, &n);
    return P3(p + n * offset, c.shaft_y0 + j * cell_h);
  };
  auto member = [&](Vec3 a, Vec3 b, float rad) {
    if (s.facade == FacadeKind::XFrame) mem.beam(a, b, rad * 1.3f, rad * 2.4f, normalize(Vec3{(a.x + b.x) * 0.5f - c.prof.centre.x, 0, (a.z + b.z) * 0.5f - c.prof.centre.y}));
    else mem.tube(a, b, rad, sides, false);
  };
  const int J = rows_total;
  for (int j = first_row; j < J; ++j) {
    const float shrink = (crown_rows && j >= J - 2) ? 0.8f : 1.0f;
    const float thick = (j == 0 && s.base == BaseKind::Legs) ? 1.35f : 1.0f;
    if (s.facade == FacadeKind::HexLattice) {
      const float shift = (j % 2) ? 0.5f : 0.0f;
      for (int i = 0; i < M; ++i) {
        const float x = static_cast<float>(i) + shift;
        // vertical side of the hexagon (middle half of the row)
        member(node(x, static_cast<float>(j) + 0.25f), node(x, static_cast<float>(j) + 0.75f), r * shrink);
        // bottom zigzag
        member(node(x, static_cast<float>(j) + 0.25f), node(x + 0.5f, static_cast<float>(j)), r * shrink);
        member(node(x + 0.5f, static_cast<float>(j)), node(x + 1.0f, static_cast<float>(j) + 0.25f), r * shrink);
        if (j == J - 1) {
          member(node(x, static_cast<float>(j) + 0.75f), node(x + 0.5f, static_cast<float>(j) + 1.0f), r * shrink);
          member(node(x + 0.5f, static_cast<float>(j) + 1.0f), node(x + 1.0f, static_cast<float>(j) + 0.75f), r * shrink);
        }
      }
      if (c.detail >= 1) {
        for (int i = 0; i < M; ++i) {
          const float x = static_cast<float>(i) + shift;
          mem.sphere(node(x, static_cast<float>(j) + 0.25f), r * 1.15f, 6, 8);
          mem.sphere(node(x, static_cast<float>(j) + 0.75f), r * 1.15f, 6, 8);
        }
      }
    } else {
      for (int i = 0; i < M; ++i) {
        const Vec3 a = node(static_cast<float>(i), static_cast<float>(j));
        const Vec3 b = node(static_cast<float>(i + 1), static_cast<float>(j + 1));
        const Vec3 c2 = node(static_cast<float>(i + 1), static_cast<float>(j));
        const Vec3 d = node(static_cast<float>(i), static_cast<float>(j + 1));
        member(a, b, r * shrink * thick);
        member(c2, d, r * shrink * thick);
        if (s.facade == FacadeKind::XFrame) member(a, c2, r * 0.8f);
      }
      if (s.facade == FacadeKind::Diagrid && c.detail >= 1) {
        for (int i = 0; i < M; ++i) mem.sphere(node(static_cast<float>(i), static_cast<float>(j)), r * 1.25f, 8, sides);
      }
    }
  }
  if (s.facade != FacadeKind::HexLattice) {
    for (int i = 0; i < M; ++i) {
      if (s.facade == FacadeKind::Diagrid && c.detail >= 1) mem.sphere(node(static_cast<float>(i), static_cast<float>(J)), r * 0.95f, 8, sides);
      // top chord
      member(node(static_cast<float>(i), static_cast<float>(J)), node(static_cast<float>(i + 1), static_cast<float>(J)), r * 0.8f);
      if (s.facade == FacadeKind::XFrame) member(node(static_cast<float>(i), 0.0f), node(static_cast<float>(i + 1), 0.0f), r * 0.8f);
    }
  }
  // plinths where members meet the ground
  if (s.base == BaseKind::Legs || s.base == BaseKind::Lobby) {
    Emit pl(c.mesh, M_CONCRETE_WHITE);
    for (int i = 0; i < M; ++i) {
      const Vec3 nd = node(static_cast<float>(i), 0.0f);
      const float h = s.base == BaseKind::Legs ? 1.2f : std::max(0.6f, c.shaft_y0 - c.base_y);
      pl.frustum(Vec3{nd.x, c.base_y, nd.z}, Vec3{nd.x, c.base_y + h, nd.z}, r * 2.4f, r * 1.4f, 10, true);
    }
  }
}

// ---- bases --------------------------------------------------------------------

void build_base(Ctx& c) {
  const TowerSpec& s = *c.s;
  Mesh& mesh = *c.mesh;
  const std::vector<Vec2> plan = c.prof.at(0);
  const float bh = static_cast<float>(s.base_floors) * s.floor_h;
  auto lobby_glass = [&](const std::vector<Vec2>& inner, float y0, float y1, Mat glass) {
    Emit g(&mesh, glass);
    g.element_random = s.random;
    float u = 0.0f;
    for (std::size_t i = 0; i < inner.size(); ++i) {
      const std::size_t j = (i + 1) % inner.size();
      const float w = length(inner[j] - inner[i]);
      wall_quad(g, P3(inner[i], y0), P3(inner[j], y0), P3(inner[j], y1), P3(inner[i], y1), Vec2{u, y0}, Vec2{u + w, y0}, Vec2{u + w, y1}, Vec2{u, y1});
      if (c.detail >= 1) {
        Emit m(&mesh, M_DARK_METAL);
        const Vec2 d2 = normalize(inner[j] - inner[i]);
        const Vec3 n3{d2.y, 0, -d2.x};
        const int k = std::max(1, static_cast<int>(std::round(w / 3.0f)));
        for (int q = 0; q <= k; ++q) {
          const Vec2 p = inner[i] + (inner[j] - inner[i]) * (static_cast<float>(q) / k);
          m.box(P3(p, (y0 + y1) * 0.5f) + n3 * 0.08f, Vec3{0.07f, (y1 - y0) * 0.5f, 0.12f}, Vec3{d2.x, 0, d2.y}, Vec3{0, 1, 0}, n3);
        }
        m.box(P3((inner[i] + inner[j]) * 0.5f, y1 - 0.08f) + n3 * 0.08f, Vec3{w * 0.5f, 0.08f, 0.12f}, Vec3{d2.x, 0, d2.y}, Vec3{0, 1, 0}, n3);
      }
      u += w;
    }
  };
  switch (s.base) {
    case BaseKind::Lobby: {
      const std::vector<Vec2> inner = plan_offset(plan, -2.5f);
      lobby_glass(inner, c.base_y, c.base_y + bh, M_GLASS_CLEAR);
      Emit f(&mesh, M_MARBLE);
      f.polygon(inner, c.base_y + 0.02f, true);
      Emit l(&mesh, M_LOBBY_LIGHT);
      l.polygon(plan_offset(inner, -1.0f), c.base_y + bh - 0.3f, false);
      slab(mesh, plan_offset(plan, 0.8f), c.base_y + bh, 1.0f, s.member == M_WHITE_METAL ? M_WHITE_METAL : M_CONCRETE_WHITE);
      Emit col(&mesh, M_CONCRETE_WHITE);
      const int cols = std::max(8, static_cast<int>(plan_perimeter(plan) / 9.0f));
      for (int i = 0; i < cols; ++i) {
        Vec2 p, n;
        c.prof.point(0, static_cast<float>(i) / cols, &p, &n);
        col.tube(P3(p - n * 1.0f, c.base_y), P3(p - n * 1.0f, c.base_y + bh), 0.45f, sides_for(c.detail, 14), false);
      }
      break;
    }
    case BaseKind::Colonnade: {
      const std::vector<Vec2> inner = plan_offset(plan, -3.2f);
      lobby_glass(inner, c.base_y, c.base_y + bh, M_GLASS_CLEAR);
      Emit f(&mesh, M_TERRAZZO);
      f.polygon(plan, c.base_y + 0.02f, true);
      slab(mesh, plan_offset(plan, 0.4f), c.base_y + bh, 0.9f, M_CONCRETE_DARK);
      Emit col(&mesh, M_CONCRETE_DARK);
      const int cols = std::max(8, static_cast<int>(plan_perimeter(plan) / 7.0f));
      for (int i = 0; i < cols; ++i) {
        Vec2 p, n;
        c.prof.point(0, static_cast<float>(i) / cols, &p, &n);
        col.box(P3(p - n * 0.6f, c.base_y + bh * 0.5f), Vec3{0.45f, bh * 0.5f, 0.45f});
      }
      break;
    }
    case BaseKind::Podium: {
      const std::vector<Vec2> pod = plan_scale(plan_rounded_rect(std::max(s.a, s.b) * s.base_scale, std::max(s.a, s.b) * s.base_scale * 0.8f, 6.0f, 6),
                                              1.0f, Vec2{0, 0});
      const std::vector<Vec2> podium = plan_transform(pod, c.prof.centre, s.rot);
      const float ph = 2.0f * s.floor_h;
      lobby_glass(plan_offset(podium, -0.3f), c.base_y, c.base_y + ph, M_GLASS_CLEAR);
      slab(mesh, podium, c.base_y + ph, 0.8f, M_CONCRETE_WHITE);
      parapet(mesh, podium, c.base_y + ph, 1.1f, 0.3f, M_WHITE_METAL);
      Emit deck(&mesh, M_TERRAZZO);
      deck.polygon(plan_offset(podium, -0.35f), c.base_y + ph + 0.02f, true);
      // roof terrace planting
      if (c.detail >= 1) {
        Rng r2 = c.rng.child(77);
        for (int i = 0; i < 4; ++i) {
          Vec2 p, n;
          c.prof.point(0, static_cast<float>(i) / 4.0f + 0.125f, &p, &n);
          const Vec2 q = p + n * ((std::max(s.a, s.b) * s.base_scale - std::max(s.a, s.b)) * 0.5f);
          if (point_in_polygon(plan_offset(podium, -4.0f), q)) gen_planter(*c.sc, r2.child(i), q, 2.5f, 1.4f, c.base_y + ph);
        }
      }
      // the shaft's own ground floor is the podium roof; a small lobby ring
      lobby_glass(plan_offset(plan, -1.2f), c.base_y + ph, c.base_y + ph + s.floor_h, M_GLASS_CLEAR);
      break;
    }
    case BaseKind::Plinth: {
      Emit pl(&mesh, M_CONCRETE_WHITE);
      const std::vector<Vec2> outer = plan_offset(plan, 0.7f);
      pl.wall(outer, c.base_y, c.base_y + 1.4f, true, true);
      pl.polygon(outer, c.base_y + 1.4f, true);
      for (int k = 1; k <= 3; ++k) {
        const std::vector<Vec2> step = plan_offset(plan, 0.7f + 0.45f * k);
        pl.wall(step, c.base_y, c.base_y + 1.4f - 0.35f * k, true, true);
        pl.polygon(step, c.base_y + 1.4f - 0.35f * k, true);
      }
      lobby_glass(plan_offset(plan, -1.5f), c.base_y + 1.4f, c.base_y + bh, M_GLASS_CLEAR);
      slab(mesh, plan_offset(plan, 0.3f), c.base_y + bh, 0.7f, M_CONCRETE_WHITE);
      break;
    }
    case BaseKind::Legs: {
      const std::vector<Vec2> inner = plan_offset(plan, -2.8f);
      lobby_glass(inner, c.base_y, c.base_y + bh, M_GLASS_CLEAR);
      Emit f(&mesh, M_TERRAZZO);
      f.polygon(plan_offset(plan, 1.0f), c.base_y + 0.02f, true);
      Emit l(&mesh, M_LOBBY_LIGHT);
      l.polygon(plan_offset(inner, -1.0f), c.base_y + bh - 0.3f, false);
      break;
    }
  }
}

// ---- crowns ------------------------------------------------------------------

void build_crown(Ctx& c, float top) {
  const TowerSpec& s = *c.s;
  Mesh& mesh = *c.mesh;
  const std::vector<Vec2> plan = c.prof.at(s.floors);
  const std::vector<Vec2> roof = plan_offset(plan, -0.2f);
  slab(mesh, roof, top, 0.6f, M_ROOF);
  switch (s.crown) {
    case CrownKind::Parapet:
      parapet(mesh, roof, top, 1.1f, 0.35f, s.member);
      if (c.detail >= 1) roof_equipment(mesh, c.rng, plan, top, c.detail >= 2 ? 5 : 2);
      break;
    case CrownKind::Lattice:  // handled by the lattice rows; add a lantern box inside
    case CrownKind::Lantern: {
      parapet(mesh, roof, top, 0.9f, 0.3f, s.member);
      const std::vector<Vec2> lantern = plan_scale(plan, 0.55f, c.prof.centre);
      Emit g(&mesh, M_GLASS_SILVER);
      g.element_random = s.random;
      float u = 0.0f;
      const float lh = s.floor_h * 2.0f;
      for (std::size_t i = 0; i < lantern.size(); ++i) {
        const std::size_t j = (i + 1) % lantern.size();
        const float w = length(lantern[j] - lantern[i]);
        wall_quad(g, P3(lantern[i], top), P3(lantern[j], top), P3(lantern[j], top + lh), P3(lantern[i], top + lh), Vec2{u, 0}, Vec2{u + w, 0}, Vec2{u + w, lh}, Vec2{u, lh});
        u += w;
      }
      slab(mesh, lantern, top + lh, 0.5f, s.member);
      Emit m(&mesh, M_DARK_METAL);
      m.tube(P3(c.prof.centre, top + lh), P3(c.prof.centre, top + lh + 12.0f), 0.25f, 8, true);
      Emit sgn(&mesh, M_SIGN);
      sgn.sphere(P3(c.prof.centre, top + lh + 12.3f), 0.5f, 6, 10);
      break;
    }
    case CrownKind::Mast: {
      parapet(mesh, roof, top, 1.0f, 0.3f, s.member);
      if (c.detail >= 1) roof_equipment(mesh, c.rng, plan, top, 3);
      Emit m(&mesh, M_DARK_METAL);
      const float mh = 0.12f * s.floor_h * static_cast<float>(s.floors) + 8.0f;
      m.frustum(P3(c.prof.centre, top), P3(c.prof.centre, top + mh), 0.6f, 0.15f, 8, true);
      Emit sgn(&mesh, M_SIGN);
      sgn.sphere(P3(c.prof.centre, top + mh + 0.4f), 0.45f, 6, 10);
      break;
    }
    case CrownKind::Louvres: {
      parapet(mesh, roof, top, 0.8f, 0.3f, M_DARK_METAL);
      const std::vector<Vec2> ring = plan_offset(plan, 1.2f);
      Emit e(&mesh, s.member == M_WHITE_METAL ? M_WHITE_METAL : M_DARK_METAL);
      for (int k = 0; k < 4; ++k) {
        const float y = top + 1.2f + 0.75f * static_cast<float>(k);
        e.wall(ring, y, y + 0.35f, true, true);
        e.wall(plan_offset(ring, -0.3f), y, y + 0.35f, true, false);
      }
      Emit cap(&mesh, s.member);
      cap.polygon(plan_offset(ring, 0.2f), top + 4.6f, true);
      cap.polygon(plan_offset(ring, 0.2f), top + 4.3f, false);
      cap.wall(plan_offset(ring, 0.2f), top + 4.3f, top + 4.6f, true, true);
      break;
    }
  }
}

}  // namespace

// ---- public ------------------------------------------------------------------

void build_tower(Scene& sc, const TowerSpec& spec, Vec2 centre, float base_y, Rng rng, int detail) {
  Ctx c{&sc, &sc.opaque, &spec, Profile{&spec, base_plan(spec, detail), centre}, rng, detail, base_y, base_y};
  const bool lattice_facade = spec.facade == FacadeKind::Diagrid || spec.facade == FacadeKind::XFrame || spec.facade == FacadeKind::HexLattice;
  // base
  float shaft_y0 = base_y;
  const float bh = static_cast<float>(spec.base_floors) * spec.floor_h;
  switch (spec.base) {
    case BaseKind::Lobby: case BaseKind::Colonnade: shaft_y0 = base_y + bh; break;
    case BaseKind::Plinth: shaft_y0 = base_y + bh; break;
    case BaseKind::Podium: shaft_y0 = base_y + 2.0f * spec.floor_h + spec.floor_h; break;
    case BaseKind::Legs: shaft_y0 = base_y + bh; break;
  }
  c.shaft_y0 = spec.base == BaseKind::Legs ? base_y : shaft_y0;  // legs: lattice from the ground
  build_base(c);
  // shaft
  const int first_floor = spec.base == BaseKind::Legs ? spec.base_floors : 0;
  const float shaft_base = spec.base == BaseKind::Legs ? base_y : shaft_y0;
  for (int f = first_floor; f < spec.floors; ++f) {
    const float y0 = shaft_base + spec.floor_h * static_cast<float>(f);
    const float y1 = y0 + spec.floor_h;
    const std::vector<Vec2> p0 = c.prof.at(f), p1 = c.prof.at(f + 1);
    facade_floor(c, f, p0, p1, y0, y1);
    if (spec.floor_bands && f > first_floor) floor_band(c, p0, y0);
    if (spec.setback_floor == f && f > 0) {
      slab(sc.opaque, c.prof.at(f - 1), y0, 0.6f, M_ROOF);
      parapet(sc.opaque, plan_offset(c.prof.at(f - 1), -0.2f), y0, 1.0f, 0.3f, spec.member);
    }
  }
  const float top = shaft_base + spec.floor_h * static_cast<float>(spec.floors);
  if (lattice_facade) {
    const int rows = std::max(1, (spec.floors - first_floor * 0) / std::max(1, spec.lattice_rows));
    const int extra = spec.crown == CrownKind::Lattice ? 2 : 0;
    lattice(c, 0, rows + extra, 0.75f, extra > 0);
  }
  build_crown(c, top);
}

// ---- families ------------------------------------------------------------------

TowerSpec spec_diagrid(float half, int floors) {
  TowerSpec s;
  s.plan = PlanKind::Superellipse; s.a = s.b = half; s.exponent = 3.2f;
  s.floors = floors; s.floor_h = 4.0f;
  s.facade = FacadeKind::Diagrid; s.glass = M_GLASS_BLUE; s.frame = M_DARK_METAL; s.member = M_WHITE_METAL;
  s.module_w = 3.0f; s.member_r = 0.42f; s.lattice_rows = 2;
  s.base = BaseKind::Lobby; s.base_floors = 2; s.crown = CrownKind::Lattice;
  return s;
}
TowerSpec spec_lens(float half_w, float half_t, int floors, float rot) {
  TowerSpec s;
  s.plan = PlanKind::Lens; s.a = half_w; s.b = half_t; s.rot = rot;
  s.floors = floors; s.floor_h = 3.9f; s.taper = 0.22f; s.tip = 0.55f;
  s.facade = FacadeKind::Ribbon; s.glass = M_GLASS_SILVER; s.member = M_WHITE_METAL; s.module_w = 3.6f; s.fin_depth = 0.5f;
  s.floor_bands = false;
  s.base = BaseKind::Podium; s.base_floors = 2; s.crown = CrownKind::Parapet;
  return s;
}
TowerSpec spec_sail(float half_w, float half_t, int floors, float rot) {
  TowerSpec s = spec_lens(half_w, half_t, floors, rot);
  s.facade = FacadeKind::Sail; s.floor_bands = true; s.taper = 0.15f; s.tip = 0.4f;
  return s;
}
TowerSpec spec_finweave(float radius, int floors) {
  TowerSpec s;
  s.plan = PlanKind::Circle; s.a = s.b = radius;
  s.floors = floors; s.floor_h = 3.8f;
  s.facade = FacadeKind::FinWeave; s.glass = M_GLASS_DARK; s.frame = M_DARK_METAL; s.member = M_BRONZE;
  s.module_w = 1.6f; s.fin_depth = 1.0f; s.floor_bands = false;
  s.base = BaseKind::Colonnade; s.base_floors = 2; s.crown = CrownKind::Louvres;
  return s;
}
TowerSpec spec_xframe(float hx, float hz, int floors) {
  TowerSpec s;
  s.plan = PlanKind::RoundedRect; s.a = hx; s.b = hz;
  s.floors = floors; s.floor_h = 4.4f;
  s.facade = FacadeKind::XFrame; s.glass = M_GLASS_XFRAME; s.frame = M_DARK_METAL; s.member = M_WHITE_METAL;
  s.module_w = 3.0f; s.member_r = 0.32f; s.lattice_rows = 2;
  s.base = BaseKind::Legs; s.base_floors = 2; s.crown = CrownKind::Parapet;
  return s;
}
TowerSpec spec_hex(float half, int floors) {
  TowerSpec s;
  s.plan = PlanKind::Superellipse; s.a = s.b = half; s.exponent = 2.4f;
  s.floors = floors; s.floor_h = 3.8f;
  s.facade = FacadeKind::HexLattice; s.glass = M_GLASS_GREEN; s.frame = M_DARK_METAL; s.member = M_WHITE_METAL;
  s.module_w = 3.0f; s.member_r = 0.3f; s.lattice_rows = 3;
  s.base = BaseKind::Plinth; s.base_floors = 2; s.crown = CrownKind::Mast;
  return s;
}

namespace {
Mat pick_glass(Rng& r, float floor_h) {
  (void)r;
  return glass_for_floor_height(floor_h);
}
}  // namespace

TowerSpec random_tower(Rng& rng, float half, int max_floors) {
  const float u = rng.next();
  TowerSpec s;
  const int floors = std::max(6, rng.irange(std::max(6, max_floors / 3), max_floors));
  if (u < 0.22f) {
    s = spec_diagrid(half, floors);
    s.exponent = rng.range(2.2f, 4.0f);
    s.b = half * rng.range(0.7f, 1.0f);
    s.crown = rng.chance(0.5f) ? CrownKind::Lattice : CrownKind::Parapet;
    s.base = rng.chance(0.5f) ? BaseKind::Lobby : BaseKind::Legs;
  } else if (u < 0.40f) {
    s = rng.chance(0.5f) ? spec_lens(half * 1.4f, half * 0.55f, floors, rng.range(0, kPi)) : spec_sail(half * 1.3f, half * 0.6f, floors, rng.range(0, kPi));
    s.base = rng.chance(0.6f) ? BaseKind::Podium : BaseKind::Lobby;
    s.tip = rng.range(0.0f, 0.6f); s.taper = rng.range(0.05f, 0.3f);
  } else if (u < 0.55f) {
    s = spec_finweave(half * 0.95f, floors);
    s.member = rng.chance(0.5f) ? M_BRONZE : M_WHITE_METAL;
    s.glass = rng.chance(0.5f) ? M_GLASS_DARK : M_GLASS_BRONZE;
    s.crown = rng.chance(0.5f) ? CrownKind::Louvres : CrownKind::Parapet;
  } else if (u < 0.70f) {
    s = spec_xframe(half * 1.3f, half * 0.7f, std::min(floors, 18));
    s.rot = rng.range(0, kPi);
    s.base = rng.chance(0.6f) ? BaseKind::Legs : BaseKind::Plinth;
  } else if (u < 0.82f) {
    s = spec_hex(half, floors);
    s.exponent = rng.range(2.0f, 3.5f);
    s.crown = rng.chance(0.5f) ? CrownKind::Mast : CrownKind::Lantern;
  } else if (u < 0.92f) {
    s.plan = rng.chance(0.5f) ? PlanKind::RoundedRect : PlanKind::Superellipse; s.a = half; s.b = half * rng.range(0.6f, 1.0f); s.exponent = rng.range(2.5f, 5.0f);
    s.rot = rng.range(0, kPi);
    s.floors = floors; s.floor_h = 4.0f; s.facade = FacadeKind::Curtain; s.glass = M_GLASS_BLUE; s.spandrel_h = rng.chance(0.5f) ? 0.9f : 0.0f;
    s.setback_floor = rng.chance(0.6f) ? floors * 2 / 3 : -1;
    s.base = rng.chance(0.5f) ? BaseKind::Lobby : BaseKind::Plinth; s.crown = rng.chance(0.5f) ? CrownKind::Mast : CrownKind::Parapet;
  } else {
    s.plan = PlanKind::Circle; s.a = s.b = half * 0.9f;
    s.floors = floors; s.floor_h = 3.8f; s.facade = FacadeKind::Louvre; s.glass = M_GLASS_CONTEXT; s.member = M_WHITE_METAL; s.fin_depth = 0.45f;
    s.module_w = 2.4f; s.base = BaseKind::Colonnade; s.crown = CrownKind::Louvres;
  }
  s.twist = rng.chance(0.25f) ? rng.range(-0.6f, 0.6f) : 0.0f;
  s.random = rng.next();
  return s;
}

TowerSpec random_context_tower(Rng& rng, float half, int max_floors) {
  TowerSpec s = random_tower(rng, half, max_floors);
  return s;
}

void build_tower_group(Scene& sc, Rng rng, Vec2 centre, float rot, int detail) {
  // Shared podium (2 floors) with 2–3 towers of one family.
  const float pod_hx = rng.range(38.0f, 48.0f), pod_hz = rng.range(26.0f, 34.0f);
  const std::vector<Vec2> podium = plan_transform(plan_rounded_rect(pod_hx, pod_hz, 8.0f, 6), centre, rot);
  const float ph = 8.0f;
  Emit g(&sc.opaque, M_GLASS_CLEAR);
  g.element_random = rng.next();
  float u = 0.0f;
  const std::vector<Vec2> inner = plan_offset(podium, -0.3f);
  for (std::size_t i = 0; i < inner.size(); ++i) {
    const std::size_t j = (i + 1) % inner.size();
    const float w = length(inner[j] - inner[i]);
    g.quad(P3(inner[j], 0.0f), P3(inner[i], 0.0f), P3(inner[i], ph), P3(inner[j], ph), QuadUV{{u + w, 0}, {u, 0}, {u, ph}, {u + w, ph}});
    u += w;
  }
  slab(sc.opaque, podium, ph, 0.8f, M_CONCRETE_WHITE);
  parapet(sc.opaque, podium, ph, 1.1f, 0.3f, M_WHITE_METAL);
  Emit deck(&sc.opaque, M_TERRAZZO);
  deck.polygon(plan_offset(podium, -0.35f), ph + 0.02f, true);
  const int count = rng.chance(0.4f) ? 3 : 2;
  const float family = rng.next();
  const Vec2 dir{std::cos(rot), std::sin(rot)};
  for (int k = 0; k < count; ++k) {
    const float t = count == 2 ? (k == 0 ? -0.5f : 0.5f) : (static_cast<float>(k) - 1.0f) * 0.62f;
    const Vec2 c = centre + dir * (t * pod_hx * 1.15f);
    const float half = count == 2 ? 13.0f : 10.0f;
    const int floors = rng.irange(18, 34) + (k == 1 ? 8 : 0);
    TowerSpec s;
    if (family < 0.35f) { s = spec_sail(half * 1.3f, half * 0.6f, floors, rot + kPi * 0.5f); s.base = BaseKind::Lobby; s.base_floors = 1; }
    else if (family < 0.65f) { s = spec_diagrid(half, floors); s.base = BaseKind::Lobby; s.base_floors = 1; s.crown = CrownKind::Parapet; }
    else { s = spec_finweave(half, floors); s.base = BaseKind::Lobby; s.base_floors = 1; }
    s.random = rng.next();
    build_tower(sc, s, c, ph, rng.child(k), detail);
  }
  if (detail >= 1) {
    Rng r2 = rng.child(99);
    for (int i = 0; i < 6; ++i) {
      const Vec2 p = centre + Vec2{r2.range(-pod_hx + 6.0f, pod_hx - 6.0f), r2.range(-pod_hz + 5.0f, pod_hz - 5.0f)};
      const Vec2 pr = plan_transform({p - centre}, centre, rot)[0];
      bool clear = true;
      for (int k = 0; k < count; ++k) {
        const float t = count == 2 ? (k == 0 ? -0.5f : 0.5f) : (static_cast<float>(k) - 1.0f) * 0.62f;
        if (length(pr - (centre + dir * (t * pod_hx * 1.15f))) < 20.0f) clear = false;
      }
      if (clear) gen_tree(sc, r2.child(i), P3(pr, ph + 0.02f), r2.range(5.0f, 7.5f));
    }
  }
}

// ---- shared helpers -------------------------------------------------------------

void slab(Mesh& mesh, const std::vector<Vec2>& plan, float y, float thickness, Mat mat) {
  Emit e(&mesh, mat);
  e.polygon(plan, y, true, Vec2{0.25f, 0.25f});
  e.polygon(plan, y - thickness, false, Vec2{0.25f, 0.25f});
  e.wall(plan, y - thickness, y, true, true);
}

void parapet(Mesh& mesh, const std::vector<Vec2>& plan, float y, float height, float thickness, Mat mat) {
  Emit e(&mesh, mat);
  const std::vector<Vec2> inner = plan_offset(plan, -thickness);
  e.wall(plan, y, y + height, true, true);
  e.wall(inner, y, y + height, true, false);
  for (std::size_t i = 0; i < plan.size(); ++i) {
    const std::size_t j = (i + 1) % plan.size();
    e.quad_metric(P3(plan[j], y + height), P3(plan[i], y + height), P3(inner[i], y + height), P3(inner[j], y + height));  // top ring faces up
  }
}

void roof_equipment(Mesh& mesh, Rng& rng, const std::vector<Vec2>& plan, float y, int count) {
  Emit e(&mesh, M_DARK_METAL);
  Emit c(&mesh, M_CONCRETE);
  float minx = 1e9f, maxx = -1e9f, minz = 1e9f, maxz = -1e9f;
  for (const Vec2& p : plan) {
    minx = std::min(minx, p.x); maxx = std::max(maxx, p.x); minz = std::min(minz, p.y); maxz = std::max(maxz, p.y);
  }
  const std::vector<Vec2> inner = plan_offset(plan, -3.0f);
  for (int i = 0; i < count * 6 && count > 0; ++i) {
    const Vec2 p{rng.range(minx, maxx), rng.range(minz, maxz)};
    if (!point_in_polygon(inner, p)) continue;
    const float w = rng.range(1.2f, 3.5f), h = rng.range(1.0f, 2.6f);
    if (rng.chance(0.6f)) e.box(P3(p, y + h * 0.5f), Vec3{w * 0.5f, h * 0.5f, rng.range(1.0f, 2.5f)});
    else c.tube(P3(p, y), P3(p, y + h), w * 0.35f, 16, true);
    if (--count <= 0) break;
  }
}

}  // namespace cb
