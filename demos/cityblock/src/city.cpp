#include "city.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "materials.hpp"
#include "props.hpp"
#include "site.hpp"
#include "standards.hpp"
#include "towers.hpp"

namespace cb {

const char* to_string(CitySize s) {
  switch (s) {
    case CitySize::Small: return "small";
    case CitySize::Medium: return "medium";
    case CitySize::Large: return "large";
    default: return "metropolis";
  }
}

CitySize city_size_for(Rng& rng) {
  const float u = rng.next();
  if (u < 0.28f) return CitySize::Small;
  if (u < 0.62f) return CitySize::Medium;
  if (u < 0.88f) return CitySize::Large;
  return CitySize::Metropolis;
}

bool parse_city_size(const std::string& t, CitySize* out) {
  if (t == "small") *out = CitySize::Small;
  else if (t == "medium") *out = CitySize::Medium;
  else if (t == "large") *out = CitySize::Large;
  else if (t == "metropolis") *out = CitySize::Metropolis;
  else return false;
  return true;
}

namespace {

constexpr float kCurb = 0.15f;

struct Road {
  bool vertical;  // line of constant u (vertical on the map) or constant v
  float pos;      // coordinate
  float width;
  bool artery;
};

struct Block {
  std::vector<Vec2> poly;  // world xz, ccw on paper
  Vec2 centre;
  float area;
  float t;  // 0 at the centre … 1 at the edge
  bool centre_block{false};
  bool front_block{false};  // the block in front of the government building
};

struct SizeRules {
  float radius;
  int max_floors;        // tallest tower
  float tower_density;   // probability of a tower on an inner block
  bool heroes;           // diagrid / lens / X-frame families allowed
  bool groups;
  int diagonals;
  float plaza_p;
};

SizeRules rules_for(CitySize s) {
  switch (s) {
    case CitySize::Small: return {260.0f, 12, 0.12f, false, false, 0, 0.16f};
    case CitySize::Medium: return {430.0f, 24, 0.45f, false, false, 1, 0.14f};
    case CitySize::Large: return {650.0f, 40, 0.6f, true, true, 1, 0.12f};
    default: return {900.0f, 52, 0.72f, true, true, 2, 0.11f};
  }
}

Vec2 rot2(Vec2 p, float a) {
  const float c = std::cos(a), s = std::sin(a);
  return Vec2{p.x * c - p.y * s, p.x * s + p.y * c};
}

// Marks on a road segment: centre line(s) and dashed lane lines.
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

// Lots inside a block: split along the long axis with alleys.
void subdivide(const std::vector<Vec2>& block, Rng& rng, float max_area, float alley_w, std::vector<std::vector<Vec2>>* out, int depth) {
  const float area = plan_area(block);
  if (area < max_area || depth > 3 || block.size() < 3) {
    if (area > 180.0f) out->push_back(block);
    return;
  }
  const Vec2 axis = plan_long_axis(block);
  float lo, hi;
  plan_extent(block, axis, &lo, &hi);
  const float cut = lo + (hi - lo) * rng.range(0.4f, 0.6f);
  const Vec2 p = axis * cut;
  const float gap = depth == 0 ? alley_w * 0.5f : 1.5f;  // first split is an alley, later ones a thin gap
  std::vector<Vec2> a = clip_halfplane(block, p - axis * gap, axis * -1.0f);
  std::vector<Vec2> b = clip_halfplane(block, p + axis * gap, axis);
  if (a.size() >= 3) subdivide(a, rng, max_area, alley_w, out, depth + 1);
  if (b.size() >= 3) subdivide(b, rng, max_area, alley_w, out, depth + 1);
}

enum class PlazaKind { Fountain, Formal, Terraced, Monument, Garden, Landing };

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

}  // namespace

CityStats generate_city(Scene& sc, Rng root, CitySize size) {
  CityStats st;
  const SizeRules R = rules_for(size);
  st.radius = R.radius;
  Mesh& mesh = sc.opaque;
  const float rot = root.range(-0.35f, 0.35f);
  auto to_world = [&](Vec2 p) { return rot2(p, rot); };
  auto poly_world = [&](const std::vector<Vec2>& p) {
    std::vector<Vec2> out(p.size());
    for (std::size_t i = 0; i < p.size(); ++i) out[i] = to_world(p[i]);
    return out;
  };

  // ---- ground ---------------------------------------------------------------------
  {
    Emit g(&mesh, M_ASPHALT);
    const float far = R.radius + 900.0f;
    g.polygon({{-far, -far}, {far, -far}, {far, far}, {-far, far}}, -0.02f, true);
  }

  // ---- street grid (city-local coordinates) -------------------------------------------
  Rng grid = root.child(1);
  std::vector<float> xs, zs;
  auto lines = [&](std::vector<float>* out, Rng& r) {
    float p = -R.radius;
    out->push_back(p);
    while (p < R.radius) {
      p += r.range(105.0f, 150.0f);
      out->push_back(std::min(p, R.radius + 1.0f));
    }
  };
  lines(&xs, grid);
  lines(&zs, grid);
  // roads: every 3rd line is an artery (offset so one artery passes near the centre)
  auto road_width = [&](std::size_t i, std::size_t n) {
    const std::size_t mid = n / 2;
    return ((i + 3 * 100 - mid) % 3 == 0) ? 26.0f : 14.0f;
  };
  // the centre block: between the two lines around 0
  std::size_t cx = 0, cz = 0;
  for (std::size_t i = 0; i + 1 < xs.size(); ++i) if (xs[i] <= 0.0f && xs[i + 1] > 0.0f) cx = i;
  for (std::size_t i = 0; i + 1 < zs.size(); ++i) if (zs[i] <= 0.0f && zs[i + 1] > 0.0f) cz = i;
  // widen the centre cell by merging it with its southern neighbour (front plaza)
  std::vector<Block> blocks;
  for (std::size_t i = 0; i + 1 < xs.size(); ++i) {
    for (std::size_t j = 0; j + 1 < zs.size(); ++j) {
      const float x0 = xs[i] + road_width(i, xs.size()) * 0.5f, x1 = xs[i + 1] - road_width(i + 1, xs.size()) * 0.5f;
      const float z0 = zs[j] + road_width(j, zs.size()) * 0.5f, z1 = zs[j + 1] - road_width(j + 1, zs.size()) * 0.5f;
      if (x1 - x0 < 30.0f || z1 - z0 < 30.0f) continue;
      Block b;
      b.poly = {{x0, z0}, {x1, z0}, {x1, z1}, {x0, z1}};
      b.centre = Vec2{(x0 + x1) * 0.5f, (z0 + z1) * 0.5f};
      if (length(b.centre) > R.radius * 1.02f) continue;
      b.centre_block = (i == cx && j == cz);
      b.front_block = (i == cx && j == cz + 1);
      blocks.push_back(b);
    }
  }
  // diagonal arteries through the centre area (never through the centre block itself)
  struct Diagonal { Vec2 p, d; float w; };
  std::vector<Diagonal> diags;
  for (int k = 0; k < R.diagonals; ++k) {
    const float a = grid.range(0.35f, 1.2f) * (k == 0 ? 1.0f : -1.0f);
    Diagonal dg{Vec2{grid.range(-60.0f, 60.0f), grid.range(120.0f, 200.0f) * (k == 0 ? -1.0f : 1.0f)}, Vec2{std::cos(a), std::sin(a)}, 22.0f};
    diags.push_back(dg);
  }
  {
    std::vector<Block> cut;
    for (const Block& b : blocks) {
      std::vector<std::vector<Vec2>> pieces{b.poly};
      if (!b.centre_block && !b.front_block) {
        for (const Diagonal& dg : diags) {
          std::vector<std::vector<Vec2>> next;
          const Vec2 n{dg.d.y, -dg.d.x};
          for (const std::vector<Vec2>& pc : pieces) {
            const std::vector<Vec2> left = clip_halfplane(pc, dg.p + n * (dg.w * 0.5f), n);
            const std::vector<Vec2> right = clip_halfplane(pc, dg.p - n * (dg.w * 0.5f), n * -1.0f);
            if (left.size() >= 3 && plan_area(left) > 500.0f) next.push_back(left);
            if (right.size() >= 3 && plan_area(right) > 500.0f) next.push_back(right);
          }
          pieces = next;
        }
      }
      for (const std::vector<Vec2>& pc : pieces) {
        Block nb = b;
        nb.poly = pc;
        nb.centre = plan_centroid(pc);
        nb.area = plan_area(pc);
        nb.t = clampf(length(nb.centre) / R.radius, 0.0f, 1.0f);
        if (nb.poly.size() > 8) continue;
        cut.push_back(nb);
      }
    }
    blocks.swap(cut);
  }
  st.blocks = static_cast<int>(blocks.size());

  // ---- road paint, crosswalks, artery medians -----------------------------------------
  {
    Rng rp = root.child(2);
    int tree_budget_medians = size == CitySize::Small ? 10 : 40;
    for (std::size_t i = 0; i < xs.size(); ++i) {
      const float w = road_width(i, xs.size());
      const Vec2 a = to_world(Vec2{xs[i], -R.radius}), b = to_world(Vec2{xs[i], R.radius});
      road_paint(sc, a, b, w, w > 20.0f, 0.004f);
      if (w > 20.0f) {
        // median: a raised strip with hedges and sparse trees, broken at every cross road
        for (std::size_t j = 0; j + 1 < zs.size(); ++j) {
          const float z0 = zs[j] + road_width(j, zs.size()) * 0.5f + 6.0f, z1 = zs[j + 1] - road_width(j + 1, zs.size()) * 0.5f - 6.0f;
          if (z1 - z0 < 20.0f) continue;
          const std::vector<Vec2> med = poly_world({{xs[i] - 1.6f, z0}, {xs[i] + 1.6f, z0}, {xs[i] + 1.6f, z1}, {xs[i] - 1.6f, z1}});
          Emit m(&mesh, M_MEDIAN);
          m.wall(med, 0.0f, kCurb, true, true);
          m.polygon(med, kCurb, true);
          build_hedge(sc, to_world(Vec2{xs[i], z0 + 2.0f}), to_world(Vec2{xs[i], z1 - 2.0f}), 1.2f, 0.7f, kCurb);
          if (tree_budget_medians > 0 && rp.chance(0.5f)) {
            gen_tree(sc, rp.child(static_cast<std::uint32_t>(i * 64 + j)), P3(to_world(Vec2{xs[i], (z0 + z1) * 0.5f}), kCurb), rp.range(6.0f, 8.0f));
            --tree_budget_medians;
            ++st.trees;
          }
        }
      }
    }
    for (std::size_t j = 0; j < zs.size(); ++j) {
      const float w = road_width(j, zs.size());
      const Vec2 a = to_world(Vec2{-R.radius, zs[j]}), b = to_world(Vec2{R.radius, zs[j]});
      road_paint(sc, a, b, w, w > 20.0f, 0.004f);
    }
    for (const Diagonal& dg : diags) {
      road_paint(sc, to_world(dg.p - dg.d * R.radius * 1.5f), to_world(dg.p + dg.d * R.radius * 1.5f), dg.w, true, 0.005f);
    }
    // crosswalks at grid intersections (inner half of the city only, cost)
    for (std::size_t i = 1; i + 1 < xs.size(); ++i) {
      for (std::size_t j = 1; j + 1 < zs.size(); ++j) {
        const Vec2 c{xs[i], zs[j]};
        if (length(c) > R.radius * 0.7f) continue;
        const float wx = road_width(i, xs.size()), wz = road_width(j, zs.size());
        crosswalk(sc, to_world(c + Vec2{0, -(wz * 0.5f + 2.0f)}), rot2(Vec2{1, 0}, rot), wx, 0.006f);
        crosswalk(sc, to_world(c + Vec2{0, (wz * 0.5f + 2.0f)}), rot2(Vec2{1, 0}, rot), wx, 0.006f);
        crosswalk(sc, to_world(c + Vec2{-(wx * 0.5f + 2.0f), 0}), rot2(Vec2{0, 1}, rot), wz, 0.006f);
        crosswalk(sc, to_world(c + Vec2{(wx * 0.5f + 2.0f), 0}), rot2(Vec2{0, 1}, rot), wz, 0.006f);
      }
    }
  }

  // ---- blocks -------------------------------------------------------------------------
  int tree_budget = size == CitySize::Small ? 40 : (size == CitySize::Medium ? 90 : 140);
  int lamp_budget = 60;
  std::vector<Vec2> plaza_centres;
  std::vector<std::vector<Vec2>> plaza_polys;
  Rng br = root.child(3);
  int idx = 0;
  const float gov_rot = rot;  // government faces "south" of the grid
  for (const Block& b : blocks) {
    Rng r = br.child(idx++);
    const std::vector<Vec2> plate = poly_world(b.poly);
    // sidewalk plate with curb
    {
      Emit s(&mesh, M_SIDEWALK);
      s.polygon(plate, kCurb, true);
      Emit c(&mesh, M_CURB);
      c.wall(plate, 0.0f, kCurb, true, true);
    }
    const int detail = b.t < 0.3f ? 2 : (b.t < 0.65f ? 1 : 0);
    if (b.centre_block) {
      // government building on its block, facing the front block (+z on the grid)
      const float half = std::min(plan_inradius(b.poly) * 0.9f, 42.0f);
      build_government(sc, to_world(b.centre + Vec2{0, -half * 0.15f}), gov_rot, half, kCurb, r, 2);
      // lamps around
      for (int k = 0; k < 4 && lamp_budget > 0; ++k, --lamp_budget) {
        const Vec2 q = plate[static_cast<std::size_t>(k) % plate.size()];
        gen_lamp(sc, P3(q + normalize(to_world(b.centre) - q) * 3.0f, kCurb), std::atan2(q.y - to_world(b.centre).y, q.x - to_world(b.centre).x));
      }
      continue;
    }
    if (b.front_block) {
      // unification plaza: white marble, the ring facing the government building
      Emit floor(&mesh, M_MARBLE_WHITE);
      floor.polygon(plan_offset(plate, -1.0f), kCurb + 0.01f, true);
      const Vec2 cw = to_world(b.centre);
      build_unification_ring(sc, cw, kCurb, std::min(14.0f, plan_inradius(b.poly) * 0.35f), rot + kPi * 0.5f, 2);
      build_hedge_ring(sc, plate, 3.0f, 0.9f, 0.9f, kCurb, 16.0f, r);
      const Vec2 axis = rot2(Vec2{1, 0}, rot);
      for (float sgn : {-1.0f, 1.0f}) {
        build_basin(sc, cw + axis * (sgn * std::min(30.0f, plan_inradius(b.poly) * 0.7f)), 12.0f, 4.0f, false, kCurb, 0.45f);
      }
      for (int k = 0; k < 8 && lamp_budget > 0; ++k, --lamp_budget) {
        const float a = static_cast<float>(k) / 8.0f * 2.0f * kPi;
        gen_lamp(sc, P3(cw + Vec2{std::cos(a), std::sin(a)} * (plan_inradius(b.poly) * 0.8f), kCurb), a + kPi);
      }
      plaza_centres.push_back(cw);
      plaza_polys.push_back(plate);
      ++st.plazas;
      continue;
    }
    // plaza?
    if (r.chance(R.plaza_p) && b.area > 3000.0f) {
      PlazaKind kind = static_cast<PlazaKind>(r.irange(0, 5));
      if (kind == PlazaKind::Landing && size == CitySize::Small) kind = PlazaKind::Garden;
      build_plaza(sc, kind, plan_offset(plate, -1.0f), kCurb, r.child(5), detail, &tree_budget);
      plaza_centres.push_back(to_world(b.centre));
      plaza_polys.push_back(plate);
      ++st.plazas;
      continue;
    }
    // tower?
    const float inrad = plan_inradius(b.poly);
    const float tower_p = b.t < 0.42f ? R.tower_density : (b.t < 0.7f ? R.tower_density * 0.4f : 0.0f);
    if (inrad > 18.0f && r.chance(tower_p)) {
      const float half = std::min(inrad - 9.0f, 20.0f);
      TowerSpec spec;
      const int max_floors = std::max(10, static_cast<int>(R.max_floors * (1.0f - 0.6f * b.t)));
      if (R.groups && inrad > 40.0f && r.chance(0.25f)) {
        build_tower_group(sc, r.child(7), to_world(b.centre), rot + (r.chance(0.5f) ? 0.0f : kPi * 0.5f), detail);
        st.towers += 2;
      } else {
        spec = random_tower(r, half, max_floors);
        if (!R.heroes && (spec.facade == FacadeKind::Diagrid || spec.facade == FacadeKind::XFrame || spec.facade == FacadeKind::HexLattice)) {
          spec.facade = r.chance(0.5f) ? FacadeKind::Curtain : FacadeKind::Ribbon;
          spec.crown = CrownKind::Parapet;
        }
        spec.rot += rot;
        build_tower(sc, spec, to_world(b.centre), kCurb, r.child(8), detail);
        ++st.towers;
      }
      // a plaza floor around the tower with hedges and a basin
      Emit floor(&mesh, M_PLAZA);
      floor.polygon(plan_offset(plate, -1.0f), kCurb + 0.01f, true);
      build_hedge_ring(sc, plate, 2.5f, 0.9f, 0.8f, kCurb, 20.0f, r);
      if (b.t < 0.3f && lamp_budget > 0) {
        for (std::size_t k = 0; k < plate.size() && lamp_budget > 0; ++k, --lamp_budget) {
          const Vec2 q = plate[k] + normalize(to_world(b.centre) - plate[k]) * 4.0f;
          gen_lamp(sc, P3(q, kCurb), std::atan2(plate[k].y - q.y, plate[k].x - q.x));
        }
      }
      continue;
    }
    // standard buildings on lots
    std::vector<std::vector<Vec2>> lots;
    subdivide(b.poly, r, r.range(1400.0f, 2600.0f), 7.0f, &lots, 0);
    for (const std::vector<Vec2>& lot : lots) {
      const std::vector<Vec2> fp_local = plan_offset(lot, -3.0f);
      if (fp_local.size() < 3 || plan_area(fp_local) < 120.0f) continue;
      const std::vector<Vec2> fp = poly_world(fp_local);
      StandardSpec s = random_standard(r, plan_area(fp_local), b.t);
      build_standard(sc, s, fp, kCurb, r.child(static_cast<std::uint32_t>(st.lots)), detail);
      ++st.lots;
      ++st.standards;
      // occasional low wall around the lot
      if (r.chance(0.25f)) build_low_wall(sc, poly_world(lot), 0.8f, 0.5f, 0.3f, kCurb, M_CONCRETE_WHITE, 18.0f, r);
    }
  }

  // ---- overpasses between plazas across an artery ---------------------------------------
  {
    Rng orr = root.child(4);
    const int wanted = size == CitySize::Small ? 1 : (size == CitySize::Medium ? 2 : 4);
    for (std::size_t i = 0; i < plaza_centres.size() && st.overpasses < wanted; ++i) {
      // find the nearest other plaza 60–220 m away
      int best = -1;
      float best_d = 1e9f;
      for (std::size_t j = 0; j < plaza_centres.size(); ++j) {
        if (j == i) continue;
        const float d = length(plaza_centres[j] - plaza_centres[i]);
        if (d > 60.0f && d < 220.0f && d < best_d) { best_d = d; best = static_cast<int>(j); }
      }
      if (best < 0) continue;
      const Vec2 a = plaza_centres[i], b = plaza_centres[static_cast<std::size_t>(best)];
      const Vec2 d = normalize(b - a);
      const Vec2 n{d.y, -d.x};
      const float sway = orr.range(8.0f, 16.0f) * (orr.chance(0.5f) ? 1.0f : -1.0f);
      const std::vector<Vec2> ctrl = {a + d * 6.0f, a + d * (best_d * 0.33f) + n * sway, a + d * (best_d * 0.66f) - n * sway, b - d * 6.0f};
      build_overpass(sc, ctrl, kCurb + 6.5f, kCurb, orr, 1);
      ++st.overpasses;
    }
  }
  st.trees += tree_budget;  // remaining budget unused; report placed count
  return st;
}

}  // namespace cb
