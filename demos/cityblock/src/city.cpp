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
    case CitySize::Outpost: return "outpost";
    case CitySize::Village: return "village";
    case CitySize::Small: return "small";
    case CitySize::Medium: return "medium";
    case CitySize::Large: return "large";
    default: return "metropolis";
  }
}

CitySize city_size_for(Rng& rng) {
  const float u = rng.next();
  if (u < 0.12f) return CitySize::Outpost;
  if (u < 0.30f) return CitySize::Village;
  if (u < 0.52f) return CitySize::Small;
  if (u < 0.74f) return CitySize::Medium;
  if (u < 0.91f) return CitySize::Large;
  return CitySize::Metropolis;
}

bool parse_city_size(const std::string& t, CitySize* out) {
  if (t == "outpost") *out = CitySize::Outpost;
  else if (t == "village") *out = CitySize::Village;
  else if (t == "small") *out = CitySize::Small;
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
  float cell_min, cell_max;  // grid spacing
  float lot_area;            // target lot area before splitting stops (bigger = sparser)
  float fill_p;              // probability a lot receives a building (else lawn / void)
  int capitol_stage;
  float tower_zone;          // fraction of the radius with full tower density
};

// Sizes grow non-linearly: each step roughly doubles the radius, from a
// single settler couple to a metropolis that reaches the horizon.
SizeRules rules_for(CitySize s) {
  switch (s) {
    case CitySize::Outpost: return {40.0f, 0, 0.0f, false, false, 0, 0.0f, 80.0f, 80.0f, 9000.0f, 0.0f, 0, 0.0f};
    case CitySize::Village: return {150.0f, 0, 0.0f, false, false, 0, 0.30f, 130.0f, 170.0f, 6000.0f, 0.45f, 1, 0.0f};
    case CitySize::Small: return {320.0f, 12, 0.10f, false, false, 0, 0.20f, 125.0f, 165.0f, 3800.0f, 0.65f, 2, 0.25f};
    case CitySize::Medium: return {620.0f, 24, 0.45f, false, false, 1, 0.14f, 115.0f, 160.0f, 2800.0f, 0.82f, 3, 0.35f};
    case CitySize::Large: return {1250.0f, 40, 0.7f, true, true, 2, 0.12f, 110.0f, 155.0f, 2200.0f, 0.92f, 4, 0.36f};
    default: return {2500.0f, 56, 0.9f, true, true, 3, 0.10f, 105.0f, 150.0f, 1900.0f, 1.0f, 5, 0.26f};
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

  if (size == CitySize::Outpost) {
    // one settler couple: a glass house on a small plate, the wreck of their
    // landing pod where the unification ring will one day stand, a path, a
    // few crates and a tree — nothing else
    Rng r = root.child(2);
    const std::vector<Vec2> plate = poly_world(plan_rounded_rect(28.0f, 20.0f, 6.0f, 6));
    Emit s(&mesh, M_SIDEWALK);
    s.polygon(plate, kCurb, true);
    Emit c(&mesh, M_CURB);
    c.wall(plate, 0.0f, kCurb, true, true);
    build_government(sc, to_world(Vec2{-10.0f, -6.0f}), rot, 0.0f, kCurb, r, 2, 0);
    build_pod_wreck(sc, to_world(Vec2{12.0f, 6.0f}), kCurb, r, 2, false);
    Emit path(&mesh, M_PLAZA);
    path.polygon(ribbon(spline({to_world(Vec2{-6.0f, -2.0f}), to_world(Vec2{2.0f, 1.0f}), to_world(Vec2{8.0f, 4.0f})}, 6), 1.2f), kCurb + 0.01f, true);
    gen_tree(sc, r.child(1), P3(to_world(Vec2{-22.0f, 10.0f}), kCurb), 7.0f);
    gen_lamp(sc, P3(to_world(Vec2{0.0f, -14.0f}), kCurb), rot + kPi * 0.5f);
    st.blocks = 1;
    st.plazas = 1;
    return st;
  }

  // ---- street grid (city-local coordinates) -------------------------------------------
  Rng grid = root.child(1);
  std::vector<float> xs, zs;
  auto lines = [&](std::vector<float>* out, Rng& r) {
    float p = -R.radius;
    out->push_back(p);
    while (p < R.radius) {
      p += r.range(R.cell_min, R.cell_max);
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
    int tree_budget_medians = size <= CitySize::Small ? 10 : 40;
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
        if (length(c) > std::min(R.radius * 0.7f, 700.0f)) continue;
        const float wx = road_width(i, xs.size()), wz = road_width(j, zs.size());
        crosswalk(sc, to_world(c + Vec2{0, -(wz * 0.5f + 2.0f)}), rot2(Vec2{1, 0}, rot), wx, 0.006f);
        crosswalk(sc, to_world(c + Vec2{0, (wz * 0.5f + 2.0f)}), rot2(Vec2{1, 0}, rot), wx, 0.006f);
        crosswalk(sc, to_world(c + Vec2{-(wx * 0.5f + 2.0f), 0}), rot2(Vec2{0, 1}, rot), wz, 0.006f);
        crosswalk(sc, to_world(c + Vec2{(wx * 0.5f + 2.0f), 0}), rot2(Vec2{0, 1}, rot), wz, 0.006f);
      }
    }
  }

  // ---- blocks -------------------------------------------------------------------------
  int tree_budget = size <= CitySize::Small ? 40 : (size == CitySize::Medium ? 90 : (size == CitySize::Large ? 140 : 200));
  int lamp_budget = 60;
  std::vector<Vec2> plaza_centres;
  std::vector<std::vector<Vec2>> plaza_polys;
  Rng br = root.child(3);
  int idx = 0;
  int forced_family = 0;
  int hi_budget = size == CitySize::Metropolis ? 14 : 10;
  int mid_budget = size == CitySize::Metropolis ? 40 : 24;
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
      build_government(sc, to_world(b.centre + Vec2{0, -half * 0.15f}), gov_rot, half, kCurb, r, 2, R.capitol_stage);
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
      if (R.capitol_stage >= 3) build_unification_ring(sc, cw, kCurb, std::min(14.0f, plan_inradius(b.poly) * 0.35f), rot + kPi * 0.5f, 2);
      else build_pod_wreck(sc, cw, kCurb, r, 2, R.capitol_stage >= 1);  // the founders' pod, kept where the ring will stand
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
      if (kind == PlazaKind::Landing && size <= CitySize::Small) kind = PlazaKind::Garden;
      build_plaza(sc, kind, plan_offset(plate, -1.0f), kCurb, r.child(5), detail, &tree_budget);
      plaza_centres.push_back(to_world(b.centre));
      plaza_polys.push_back(plate);
      ++st.plazas;
      continue;
    }
    // tower?
    const float inrad = plan_inradius(b.poly);
    const float tower_p = b.t < R.tower_zone ? R.tower_density : (b.t < R.tower_zone + 0.22f ? R.tower_density * 0.35f : 0.0f);
    if (inrad > 18.0f && r.chance(tower_p)) {
      const float half = std::min(inrad - 9.0f, 20.0f);
      TowerSpec spec;
      const int max_floors = std::max(10, static_cast<int>(R.max_floors * (1.0f - 0.6f * b.t)));
      // Three detail levels of the same object, chosen per frame by camera
      // distance: fins, mullions and lattice spheres below a pixel would
      // otherwise alias into moire whenever the camera moves.
      // Geometry budget: full detail exists only for the core towers (and at
      // most `hi_budget` of them), mid detail out to the tower zone, the
      // rest is generated once at the coarse level. Levels a tower lacks are
      // simply the next coarser one.
      const int group = sc.lod_groups++;
      const float switch_hi = 200.0f, switch_mid = 500.0f;
      const int finest = (b.t < 0.28f && hi_budget > 0) ? 0 : (b.t < R.tower_zone + 0.1f && mid_budget > 0 ? 1 : 2);
      if (finest == 0) --hi_budget;
      if (finest == 1) --mid_budget;
      auto register_lod = [&](std::uint32_t first, std::uint32_t end, int level, float height) {
        const Vec2 cw = to_world(b.centre);
        const float max_d = level == 0 ? switch_hi : (level == 1 ? switch_mid : 1e30f);
        sc.register_range(first, end, Vec3{cw.x, height * 0.5f, cw.y}, height * 0.5f + inrad, group, level, max_d);
      };
      if (R.groups && inrad > 40.0f && r.chance(0.25f)) {
        const float grot = rot + (r.chance(0.5f) ? 0.0f : kPi * 0.5f);
        const Rng gr = r.child(7);
        for (int level = finest; level < 3; ++level) {
          const std::uint32_t first = static_cast<std::uint32_t>(sc.opaque.indices.size());
          build_tower_group(sc, gr, to_world(b.centre), grot, 2 - level);
          register_lod(first, static_cast<std::uint32_t>(sc.opaque.indices.size()), level, 180.0f);
        }
        st.towers += 2;
      } else {
        spec = random_tower(r, half, max_floors);
        if (size == CitySize::Metropolis && forced_family < 6 && b.t < 0.35f) {
          // a metropolis shows every family in its core, in a fixed order
          switch (forced_family) {
            case 0: spec = spec_diagrid(half, max_floors); break;
            case 1: spec = spec_lens(half * 1.35f, half * 0.55f, max_floors - 4, r.range(0, kPi)); spec.base = BaseKind::Lobby; break;
            case 2: spec = spec_finweave(half * 0.95f, max_floors - 8); break;
            case 3: spec = spec_xframe(half * 1.3f, half * 0.7f, 16); break;
            case 4: spec = spec_hex(half, max_floors - 6); break;
            default: spec = spec_sail(half * 1.3f, half * 0.6f, max_floors - 2, r.range(0, kPi)); spec.base = BaseKind::Podium; break;
          }
          spec.random = r.next();
          ++forced_family;
        }
        if (!R.heroes && (spec.facade == FacadeKind::Diagrid || spec.facade == FacadeKind::XFrame || spec.facade == FacadeKind::HexLattice)) {
          spec.facade = r.chance(0.5f) ? FacadeKind::Curtain : FacadeKind::Ribbon;
          spec.crown = CrownKind::Parapet;
        }
        spec.rot += rot;
        const Rng tr = r.child(8);
        const float height = spec.floor_h * static_cast<float>(spec.floors + spec.base_floors + 4);
        for (int level = finest; level < 3; ++level) {
          const std::uint32_t first = static_cast<std::uint32_t>(sc.opaque.indices.size());
          build_tower(sc, spec, to_world(b.centre), kCurb, tr, 2 - level);
          register_lod(first, static_cast<std::uint32_t>(sc.opaque.indices.size()), level, height);
        }
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
    // standard buildings on lots (one culling range per block)
    const std::uint32_t block_first = static_cast<std::uint32_t>(sc.opaque.indices.size());
    std::vector<std::vector<Vec2>> lots;
    subdivide(b.poly, r, R.lot_area * r.range(0.8f, 1.25f), 7.0f, &lots, 0);
    for (const std::vector<Vec2>& lot : lots) {
      const std::vector<Vec2> fp_local = plan_offset(lot, -3.0f);
      if (fp_local.size() < 3 || plan_area(fp_local) < 120.0f) continue;
      const std::vector<Vec2> fp = poly_world(fp_local);
      if (!r.chance(R.fill_p)) {
        // an unbuilt lot: lawn with a hedge or a tree — the voids that make
        // small settlements read as sparse
        Emit lawn(&mesh, M_GRASS);
        lawn.polygon(plan_offset(fp, -1.0f), kCurb + 0.02f, true);
        if (r.chance(0.4f) && tree_budget > 0) { gen_tree(sc, r.child(77), P3(plan_centroid(fp), kCurb), r.range(6.0f, 9.0f)); --tree_budget; }
        continue;
      }
      // the outer districts of large cities get the cheapest standards
      const int std_detail = (size >= CitySize::Large && b.t > (size == CitySize::Metropolis ? 0.4f : 0.6f)) ? 0 : detail;
      StandardSpec s = random_standard(r, plan_area(fp_local), b.t);
      if (std_detail == 0) { s.pilasters = false; s.balconies = false; }
      build_standard(sc, s, fp, kCurb, r.child(static_cast<std::uint32_t>(st.lots)), std_detail);
      ++st.lots;
      ++st.standards;
      // occasional low wall around the lot
      if (r.chance(0.25f)) build_low_wall(sc, poly_world(lot), 0.8f, 0.5f, 0.3f, kCurb, M_CONCRETE_WHITE, 18.0f, r);
    }
    {
      const Vec2 cw = to_world(b.centre);
      sc.register_range(block_first, static_cast<std::uint32_t>(sc.opaque.indices.size()), Vec3{cw.x, 12.0f, cw.y}, inrad * 1.6f + 30.0f);
    }
  }

  // ---- overpasses between plazas across an artery ---------------------------------------
  {
    Rng orr = root.child(4);
    const int wanted = size <= CitySize::Small ? 1 : (size == CitySize::Medium ? 2 : (size == CitySize::Large ? 4 : 8));
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
void generate_showcase(Scene& sc, Rng root) {
  Mesh& mesh = sc.opaque;
  const float y = kCurb;
  // ground: a wide marble plaza on a raised plate, asphalt beyond
  {
    Emit g(&mesh, M_ASPHALT);
    g.polygon({{-900.0f, -900.0f}, {900.0f, -900.0f}, {900.0f, 900.0f}, {-900.0f, 900.0f}}, -0.02f, true);
    const std::vector<Vec2> plate = plan_rect(330.0f, 150.0f, Vec2{0, 0});
    Emit s(&mesh, M_SIDEWALK);
    s.polygon(plate, y, true);
    Emit c(&mesh, M_CURB);
    c.wall(plate, 0.0f, y, true, true);
    Emit m(&mesh, M_MARBLE_WHITE);
    m.polygon(plan_rect(320.0f, 60.0f, Vec2{0, 60.0f}), y + 0.01f, true);
  }
  // back row: the tower families, tallest in the middle, on a gentle arc
  struct Entry { TowerSpec spec; float x; };
  std::vector<Entry> row;
  TowerSpec t;
  t = spec_xframe(24.0f, 12.0f, 14); row.push_back({t, -270.0f});
  t = spec_hex(15.0f, 30); row.push_back({t, -200.0f});
  t = spec_finweave(16.0f, 28); row.push_back({t, -130.0f});
  t = spec_lens(30.0f, 12.0f, 34, radians(90.0f)); t.base = BaseKind::Lobby; row.push_back({t, -50.0f});
  t = spec_diagrid(17.5f, 42); row.push_back({t, 45.0f});
  t = spec_sail(28.0f, 12.0f, 36, radians(90.0f)); t.base = BaseKind::Podium; row.push_back({t, 135.0f});
  t = TowerSpec{}; t.plan = PlanKind::RoundedRect; t.a = 17.0f; t.b = 13.0f; t.floors = 26; t.floor_h = 4.0f; t.facade = FacadeKind::Curtain;
  t.glass = M_GLASS_BLUE; t.spandrel_h = 0.9f; t.setback_floor = 17; t.base = BaseKind::Plinth; t.crown = CrownKind::Mast; row.push_back({t, 210.0f});
  t = TowerSpec{}; t.plan = PlanKind::Circle; t.a = t.b = 14.0f; t.floors = 22; t.floor_h = 3.8f; t.facade = FacadeKind::Louvre;
  t.glass = M_GLASS_CONTEXT; t.member = M_WHITE_METAL; t.fin_depth = 0.45f; t.module_w = 2.4f; t.base = BaseKind::Colonnade; t.crown = CrownKind::Louvres; row.push_back({t, 275.0f});
  int k = 0;
  for (Entry& e : row) {
    e.spec.random = root.child(static_cast<std::uint32_t>(k)).next();
    const float z = -20.0f + 0.0006f * e.x * e.x;  // arc bulging away in the middle
    build_tower(sc, e.spec, Vec2{e.x, z}, y, root.child(static_cast<std::uint32_t>(10 + k)), 2);
    ++k;
  }
  // middle row: the five standard types with different entrances and roofs
  const StdType types[5] = {StdType::Office, StdType::Residential, StdType::Mixed, StdType::Civic, StdType::Lab};
  for (int i = 0; i < 5; ++i) {
    Rng r = root.child(static_cast<std::uint32_t>(30 + i));
    StandardSpec s = random_standard(r, 700.0f, 0.5f);
    s.type = types[i];
    s.entrance = static_cast<EntranceKind>(i % 4);
    s.roof = static_cast<RoofKind>(i % 4);
    s.storeys = 4 + (i % 3);
    if (s.type == StdType::Civic) { s.wall = M_MARBLE_WHITE; s.floor_h = 4.4f; }
    if (s.type == StdType::Residential) { s.wall = M_PANEL_WARM; s.floor_h = 3.2f; s.balconies = true; }
    if (s.type == StdType::Mixed) { s.retail_ground = true; }
    if (s.type == StdType::Lab) { s.wall = M_PANEL_DARK; }
    const float x = -240.0f + 120.0f * static_cast<float>(i);
    build_standard(sc, s, plan_rect(16.0f, 11.0f, Vec2{x, 95.0f}), y, r, 2);
  }
  // front row: the ground kit
  Rng pr = root.child(50);
  build_government(sc, Vec2{0.0f, 40.0f}, 0.0f, 26.0f, y, pr, 2);
  build_unification_ring(sc, Vec2{0.0f, 118.0f}, y, 12.0f, kPi * 0.5f, 2);
  build_fountain(sc, Vec2{-120.0f, 60.0f}, 8.0f, y, pr, 2);
  build_basin(sc, Vec2{120.0f, 60.0f}, 14.0f, 4.0f, false, y, 0.45f);
  build_basin(sc, Vec2{160.0f, 60.0f}, 6.0f, 6.0f, true, y, 0.45f);
  build_monument(sc, MonumentKind::Pillar, Vec2{-300.0f, 80.0f}, y, 1.2f, pr, 2);
  build_monument(sc, MonumentKind::Ribbon, Vec2{-300.0f, 120.0f}, y, 1.2f, pr, 2);
  build_monument(sc, MonumentKind::Weave, Vec2{300.0f, 80.0f}, y, 1.2f, pr, 2);
  build_monument(sc, MonumentKind::Obelisk, Vec2{300.0f, 120.0f}, y, 1.2f, pr, 2);
  build_landing_pad(sc, Vec2{-230.0f, 135.0f}, 14.0f, y, pr, 2);
  build_foundation(sc, plan_rect(16.0f, 12.0f, Vec2{230.0f, 130.0f}), y, 2.0f, 2, 2);
  build_monument(sc, MonumentKind::Weave, Vec2{230.0f, 130.0f}, y + 2.0f, 0.8f, pr, 2);
  build_hedge_ring(sc, plan_rect(300.0f, 25.0f, Vec2{0, 80.0f}), 0.0f, 0.9f, 0.9f, y, 22.0f, pr);
  build_low_wall(sc, plan_rect(320.0f, 60.0f, Vec2{0, 60.0f}), 0.5f, 0.5f, 0.3f, y, M_CONCRETE_WHITE, 30.0f, pr);
  build_overpass(sc, {Vec2{-90.0f, 140.0f}, Vec2{-40.0f, 128.0f}, Vec2{40.0f, 136.0f}, Vec2{90.0f, 140.0f}}, y + 6.5f, y, pr, 2);
  gen_planter(sc, pr.child(1), Vec2{-60.0f, 60.0f}, 6.0f, 2.5f, y);
  gen_planter(sc, pr.child(2), Vec2{60.0f, 60.0f}, 6.0f, 2.5f, y);
  for (int i = 0; i < 6; ++i) gen_lamp(sc, P3(Vec2{-250.0f + 100.0f * static_cast<float>(i), 145.0f}, y), kPi * 0.5f);
  for (int i = 0; i < 8; ++i) gen_tree(sc, pr.child(static_cast<std::uint32_t>(100 + i)), P3(Vec2{-280.0f + 80.0f * static_cast<float>(i), 148.0f}, y), 8.0f);
  gen_park(sc, pr.child(7), Vec2{-190.0f, 55.0f}, y);
  gen_pavilion(sc, pr.child(8), Vec2{200.0f, 90.0f}, 12.0f, 15.0f, y);
  sc.camera_position = Vec3{-40.0f, 60.0f, 330.0f};
  sc.camera_target = Vec3{0.0f, 55.0f, 20.0f};
}

}  // namespace cb
