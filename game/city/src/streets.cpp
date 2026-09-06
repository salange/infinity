#include "city/streets.hpp"

#include <algorithm>
#include <cmath>

#include "city/flora.hpp"
#include "city/materials.hpp"
#include "city/props.hpp"
#include "city/towers.hpp"

namespace inf::city {

void road_paint(Scene& sc, Vec2 a, Vec2 b, float width, bool artery, float y) {
  Emit wh(&sc.opaque, M_LANE_WHITE);
  Emit yl(&sc.opaque, M_LANE_YELLOW);
  const Vec2 d = normalize(b - a);
  const Vec2 n{d.y, -d.x};
  const float len = length(b - a);
  if (len < 8.0f) return;
  auto strip = [&](float offset, Emit& e, bool dashed) {
    if (!dashed) {
      const Vec2 p0 = a + n * offset, p1 = b + n * offset;
      e.quad_metric(P3(p1 - n * 0.06f, y), P3(p0 - n * 0.06f, y), P3(p0 + n * 0.06f, y), P3(p1 + n * 0.06f, y));
      return;
    }
    for (float t = 4.0f; t + 3.0f < len - 4.0f; t += 9.0f) {
      const Vec2 p0 = a + d * t + n * offset, p1 = a + d * (t + 3.0f) + n * offset;
      e.quad_metric(P3(p1 - n * 0.06f, y), P3(p0 - n * 0.06f, y), P3(p0 + n * 0.06f, y), P3(p1 + n * 0.06f, y));
    }
  };
  if (artery) {
    // median island with hedges is placed by the caller; lanes either side
    strip(width * 0.25f, wh, true);
    strip(-width * 0.25f, wh, true);
    strip(width * 0.5f - 0.6f, wh, false);
    strip(-width * 0.5f + 0.6f, wh, false);
  } else {
    strip(0.12f, yl, false);
    strip(-0.12f, yl, false);
    if (width > 12.0f) {
      strip(width * 0.28f, wh, true);
      strip(-width * 0.28f, wh, true);
    }
  }
}

void crosswalk(Scene& sc, Vec2 centre, Vec2 across, float road_w, float y) {
  Emit wh(&sc.opaque, M_LANE_WHITE);
  const Vec2 along{across.y, -across.x};
  for (int i = -3; i <= 3; ++i) {
    const Vec2 p = centre + along * (static_cast<float>(i) * 1.1f);
    const Vec2 q0 = p - across * (road_w * 0.5f - 0.5f), q1 = p + across * (road_w * 0.5f - 0.5f);
    wh.quad_metric(P3(q1 - along * 0.3f, y), P3(q0 - along * 0.3f, y), P3(q0 + along * 0.3f, y), P3(q1 + along * 0.3f, y));
  }
}

void road_surface(Scene& sc, const std::vector<Vec2>& line, float width, float y) {
  if (line.size() < 2) return;
  Emit a(&sc.opaque, M_ASPHALT);
  const std::vector<Vec2> poly = ribbon(line, width * 0.5f);
  if (poly.size() < 3) return;
  if (plan_area(poly) > 0.0f) {
    a.polygon(poly, y, true);
  } else {
    a.polygon(std::vector<Vec2>(poly.rbegin(), poly.rend()), y, true);
  }
}

void build_median(Scene& sc, Vec2 a, Vec2 b, float width, float curb_h, float y) {
  const Vec2 d = normalize(b - a);
  const Vec2 n{d.y, -d.x};
  if (length(b - a) < 12.0f) return;
  const float hw = width * 0.5f;
  const std::vector<Vec2> med = {a - n * hw, b - n * hw, b + n * hw, a + n * hw};
  const std::vector<Vec2> poly = plan_area(med) > 0.0f ? med : std::vector<Vec2>(med.rbegin(), med.rend());
  Emit m(&sc.opaque, M_MEDIAN);
  m.wall(poly, y, y + curb_h, true, true);
  m.polygon(poly, y + curb_h, true);
  build_hedge(sc, a + d * 2.0f, b - d * 2.0f, std::min(1.2f, width * 0.4f), 0.7f, y + curb_h);
}

void build_plaza(Scene& sc, PlazaKind kind, const std::vector<Vec2>& poly, float y, Rng rng, int detail, int* trees) {
  Mesh& mesh = sc.opaque;
  const Vec2 c = plan_centroid(poly);
  const float r = plan_inradius(poly);
  switch (kind) {
    case PlazaKind::Fountain: {
      Emit floor(&mesh, M_MARBLE_WHITE);
      floor.polygon(poly, y + 0.01f, true);
      build_fountain(sc, c, std::min(9.0f, r * 0.35f), y, rng, detail);
      build_hedge_ring(sc, poly, 2.5f, 0.9f, 0.9f, y, 18.0f, rng);
      const int nb = 6;
      for (int i = 0; i < nb; ++i) {
        const float a = static_cast<float>(i) / nb * 2.0f * kPi;
        gen_bench(sc, P3(c + Vec2{std::cos(a), std::sin(a)} * std::min(14.0f, r * 0.6f), y), a + kPi * 0.5f);
      }
      if (*trees > 0 && detail >= 1) {
        for (int i = 0; i < 4 && *trees > 0; ++i, --*trees) {
          const float a = static_cast<float>(i) / 4.0f * 2.0f * kPi + 0.4f;
          gen_tree(sc, rng.child(i), P3(c + Vec2{std::cos(a), std::sin(a)} * std::min(20.0f, r * 0.8f), y), rng.range(7.0f, 9.0f));
        }
      }
      break;
    }
    case PlazaKind::Formal: {
      Emit floor(&mesh, M_PLAZA);
      floor.polygon(poly, y + 0.01f, true);
      const Vec2 axis = plan_long_axis(poly);
      const Vec2 side{axis.y, -axis.x};
      const float L = std::min(r * 1.4f, 40.0f);
      // two long basins flanking a central path, monument at one end
      build_basin(sc, c + side * (r * 0.35f), L * 0.5f, std::min(4.0f, r * 0.12f), false, y, 0.45f);
      build_basin(sc, c - side * (r * 0.35f), L * 0.5f, std::min(4.0f, r * 0.12f), false, y, 0.45f);
      Emit path(&mesh, M_MARBLE_WHITE);
      const std::vector<Vec2> pth = {c - axis * (L * 0.5f) - side * 3.0f, c + axis * (L * 0.5f) - side * 3.0f, c + axis * (L * 0.5f) + side * 3.0f, c - axis * (L * 0.5f) + side * 3.0f};
      path.polygon(plan_area(pth) > 0 ? pth : std::vector<Vec2>{pth[3], pth[2], pth[1], pth[0]}, y + 0.02f, true);
      build_monument(sc, static_cast<MonumentKind>(rng.irange(0, 3)), c + axis * (L * 0.5f + 5.0f), y, 1.0f, rng, detail);
      build_low_wall(sc, poly, 1.5f, 0.55f, 0.35f, y, M_MARBLE_WHITE, 25.0f, rng);
      for (int i = 0; i < 6; ++i) {
        const Vec2 q = c + axis * (L * (static_cast<float>(i) / 5.0f - 0.5f));
        gen_lamp(sc, P3(q + side * (r * 0.6f), y), std::atan2(-side.y, -side.x));
      }
      break;
    }
    case PlazaKind::Terraced: {
      gen_park(sc, rng.child(1), c, y);
      break;
    }
    case PlazaKind::Monument: {
      Emit floor(&mesh, M_MARBLE_WHITE);
      floor.polygon(poly, y + 0.01f, true);
      // raised marble foundation with stairs, monument on top
      const float hx = std::min(r * 0.55f, 18.0f);
      const std::vector<Vec2> base = plan_rect(hx, hx * 0.8f, c);
      build_foundation(sc, base, y, 1.8f, rng.irange(0, 3), detail);
      build_monument(sc, static_cast<MonumentKind>(rng.irange(0, 3)), c, y + 1.8f, rng.range(1.0f, 1.5f), rng, detail);
      build_hedge_ring(sc, poly, 2.0f, 0.8f, 0.8f, y, 14.0f, rng);
      break;
    }
    case PlazaKind::Garden: {
      Emit lawn(&mesh, M_GRASS);
      lawn.polygon(plan_offset(poly, -2.0f), y + 0.02f, true);
      Emit walk(&mesh, M_SIDEWALK);
      walk.ring_cap(poly, plan_offset(poly, -2.0f), y + 0.01f, true);
      // a curved path through the garden and hedges along it
      const Vec2 axis = plan_long_axis(poly);
      const Vec2 side{axis.y, -axis.x};
      const std::vector<Vec2> ctrl = {c - axis * (r * 0.9f), c - axis * (r * 0.3f) + side * (r * 0.35f), c + axis * (r * 0.3f) - side * (r * 0.35f), c + axis * (r * 0.9f)};
      const std::vector<Vec2> line = spline(ctrl, 8);
      walk.polygon(ribbon(line, 2.0f), y + 0.03f, true);
      for (std::size_t i = 0; i + 2 < line.size(); i += 2) {
        const Vec2 d = normalize(line[i + 2] - line[i]);
        const Vec2 nn{d.y, -d.x};
        if (i % 4 == 0) build_hedge(sc, line[i] + nn * 3.0f, line[i + 2] + nn * 3.0f, 0.8f, 0.7f, y + 0.02f);
      }
      if (detail >= 1) {
        for (int i = 0; i < 5 && *trees > 0; ++i, --*trees) {
          const Vec2 q = c + Vec2{rng.range(-0.6f, 0.6f), rng.range(-0.6f, 0.6f)} * r;
          gen_tree(sc, rng.child(20 + i), P3(q, y + 0.02f), rng.range(7.0f, 10.0f));
        }
      }
      build_basin(sc, c + side * (r * 0.5f), std::min(5.0f, r * 0.2f), std::min(5.0f, r * 0.2f), true, y, 0.4f);
      break;
    }
    case PlazaKind::Landing: {
      Emit floor(&mesh, M_PAD);
      floor.polygon(poly, y + 0.01f, true);
      const float pr = std::min(r * 0.42f, 22.0f);
      build_landing_pad(sc, c, pr, y, rng, detail);
      if (r > 30.0f) {
        const Vec2 axis = plan_long_axis(poly);
        build_landing_pad(sc, c + axis * (pr * 2.3f), pr * 0.7f, y, rng, detail);
        build_landing_pad(sc, c - axis * (pr * 2.3f), pr * 0.7f, y, rng, detail);
      }
      build_low_wall(sc, poly, 1.2f, 0.9f, 0.3f, y, M_CONCRETE_WHITE, 30.0f, rng);
      break;
    }
  }
}

}  // namespace inf::city
