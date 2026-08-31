#include "gen/drainage.hpp"

#include <array>
#include <queue>

#include "gen/names.hpp"

namespace inf::gen {

using det::Real;

namespace {

// Valley width and carve depth by Strahler order (1-based, clamped).
// Order comes free from the spanning forest; catchment-scaled widths are
// the documented upgrade path (T0015 Q3).
constexpr double kWidthM[7] = {0, 700, 1200, 1900, 2800, 3800, 5000};
constexpr double kDepthM[7] = {0, 22, 38, 60, 85, 110, 135};

Dir3 sub(const Dir3& a, const Dir3& b) { return Dir3{a.x - b.x, a.y - b.y, a.z - b.z}; }

// Chord distance from q to the GREAT-ARC segment a..b: nearest point on
// the chord, reprojected onto the sphere. Province cells span degrees of
// arc, so the raw chord sags kilometres below the surface — measuring to
// it would swallow the whole valley profile.
Real segment_chord(const Dir3& q, const Dir3& a, const Dir3& b) {
  const Dir3 ab = sub(b, a);
  const Dir3 aq = sub(q, a);
  const Real len_sq = det::max(dot(ab, ab), Real(1.0e-12));
  const Real t = det::clamp(dot(aq, ab) / len_sq, Real(0.0), Real(1.0));
  const Dir3 p{a.x + ab.x * t, a.y + ab.y * t, a.z + ab.z * t};
  const Real p_len = det::max(det::sqrt(dot(p, p)), Real(1.0e-6));
  const Dir3 d{q.x - p.x / p_len, q.y - p.y / p_len, q.z - p.z / p_len};
  return det::sqrt(dot(d, d));
}

}  // namespace

DrainageField::DrainageField(const core::Key& body_key, const PlanetParams& planet)
    : provinces_(body_key, planet),
      macro_(body_key),
      drainage_key_(core::derive_named(body_key, name::DrainageV1)),
      n_(planet.cells_per_face),
      radius_m_(planet.radius_m) {
  // v1 drains EarthLike worlds only (a solved sea to flow into); dry and
  // frozen types keep zero cost. Desert canyon networks are WP8's client.
  enabled_ = planet.type == PlanetType::EarthLike && n_ > 0;
  if (enabled_) {
    build(body_key, planet);
  }
}

void DrainageField::build(const core::Key&, const PlanetParams& planet) {
  const std::uint32_t count = 6U * n_ * n_;
  verts_.resize(count);
  cell_segments_.resize(count);

  // 1. Vertices: jittered representatives, classified sea/land against
  // the same canonical macro composition the terrain uses.
  MacroField::Cache macro_cache;
  for (std::uint8_t face = 0; face < 6; ++face) {
    for (std::uint32_t ci = 0; ci < n_; ++ci) {
      for (std::uint32_t cj = 0; cj < n_; ++cj) {
        const CellId cell{face, ci, cj};
        Vertex& vertex = verts_[index_of(cell)];
        vertex.dir = provinces_.representative(cell);
        const Real macro_m =
            macro_.canonical_value(dir_to_face_uv(vertex.dir), &macro_cache) *
            planet.macro_amplitude_m;
        vertex.sea = macro_m < planet.sea_level_m;
      }
    }
  }

  // 2. Neighbour graph: the 8 surrounding cells via tangent probes from
  // the cell centre (cross-face adjacency falls out of cell_of), then
  // symmetrized. Jittered grids make these the natural neighbours.
  constexpr int kMaxNeighbors = 12;
  std::vector<std::array<std::int32_t, kMaxNeighbors>> neighbors(count);
  std::vector<int> neighbor_count(count, 0);
  const auto add_neighbor = [&](std::uint32_t a, std::uint32_t b) {
    if (a == b) {
      return;
    }
    for (int i = 0; i < neighbor_count[a]; ++i) {
      if (neighbors[a][static_cast<std::size_t>(i)] == static_cast<std::int32_t>(b)) {
        return;
      }
    }
    if (neighbor_count[a] < kMaxNeighbors) {
      neighbors[a][static_cast<std::size_t>(neighbor_count[a]++)] =
          static_cast<std::int32_t>(b);
    }
  };
  const Real cell_step(2.0 / static_cast<double>(n_));
  for (std::uint8_t face = 0; face < 6; ++face) {
    for (std::uint32_t ci = 0; ci < n_; ++ci) {
      for (std::uint32_t cj = 0; cj < n_; ++cj) {
        const CellId cell{face, ci, cj};
        const std::uint32_t index = index_of(cell);
        const Real cu =
            (Real(static_cast<double>(ci)) + Real(0.5)) * cell_step - Real(1.0);
        const Real cv =
            (Real(static_cast<double>(cj)) + Real(0.5)) * cell_step - Real(1.0);
        const Dir3 center = face_uv_to_dir(FaceUV{face, cu, cv});
        Dir3 t1{};
        Dir3 t2{};
        tangent_basis(center, &t1, &t2);
        for (int di = -1; di <= 1; ++di) {
          for (int dj = -1; dj <= 1; ++dj) {
            if (di == 0 && dj == 0) {
              continue;
            }
            const Real ou = cell_step * Real(static_cast<double>(di)) * Real(0.75);
            const Real ov = cell_step * Real(static_cast<double>(dj)) * Real(0.75);
            const Dir3 probe{center.x + t1.x * ou + t2.x * ov,
                             center.y + t1.y * ou + t2.y * ov,
                             center.z + t1.z * ou + t2.z * ov};
            const std::uint32_t other = index_of(provinces_.cell_of(probe));
            add_neighbor(index, other);
            add_neighbor(other, index);
          }
        }
      }
    }
  }

  // 3. Mouths-first growth (Derzapf section 4.2): a deterministic
  // min-heap ordered by each vertex's drawn priority word stands in for
  // the paper's pseudo-random order. One river per mouth (sea vertex),
  // <=2 merges per land vertex; grow until the reachable continent is a
  // spanning forest. Landlocked leftovers simply stay riverless.
  std::vector<std::uint64_t> priority(count);
  for (std::uint8_t face = 0; face < 6; ++face) {
    for (std::uint32_t ci = 0; ci < n_; ++ci) {
      for (std::uint32_t cj = 0; cj < n_; ++cj) {
        const core::Key cell_key =
            core::derive_child(drainage_key_, kind::Province, face, ci, cj);
        priority[index_of(CellId{face, ci, cj})] =
            core::draw_point(cell_key, channel::Params, 0, 0, 0)[0];
      }
    }
  }
  struct Edge {
    std::uint64_t word;
    std::uint32_t v;  // land vertex to attach
    std::uint32_t u;  // network vertex it drains into
  };
  const auto edge_greater = [](const Edge& a, const Edge& b) {
    if (a.word != b.word) return a.word > b.word;
    if (a.v != b.v) return a.v > b.v;
    return a.u > b.u;
  };
  std::priority_queue<Edge, std::vector<Edge>, decltype(edge_greater)> frontier(
      edge_greater);
  std::vector<bool> in_network(count, false);
  std::vector<std::uint8_t> child_count(count, 0);
  std::vector<std::uint32_t> attach_order;
  attach_order.reserve(count);
  for (std::uint32_t v = 0; v < count; ++v) {
    if (!verts_[v].sea) {
      continue;
    }
    in_network[v] = true;
    for (int i = 0; i < neighbor_count[v]; ++i) {
      const auto w = static_cast<std::uint32_t>(neighbors[v][static_cast<std::size_t>(i)]);
      if (!verts_[w].sea) {
        frontier.push(Edge{priority[w], w, v});
      }
    }
  }
  while (!frontier.empty()) {
    const Edge edge = frontier.top();
    frontier.pop();
    if (in_network[edge.v]) {
      continue;
    }
    const std::uint8_t cap = verts_[edge.u].sea ? 1 : 2;
    if (child_count[edge.u] >= cap) {
      continue;
    }
    in_network[edge.v] = true;
    ++child_count[edge.u];
    verts_[edge.v].parent = static_cast<std::int32_t>(edge.u);
    attach_order.push_back(edge.v);
    for (int i = 0; i < neighbor_count[edge.v]; ++i) {
      const auto w =
          static_cast<std::uint32_t>(neighbors[edge.v][static_cast<std::size_t>(i)]);
      if (!in_network[w] && !verts_[w].sea) {
        frontier.push(Edge{priority[w], w, edge.v});
      }
    }
  }

  // 4. Strahler order: parents attach before children, so one reverse
  // pass sees every child before its parent.
  std::vector<std::uint8_t> best(count, 0);
  std::vector<std::uint8_t> best_dupes(count, 0);
  for (std::size_t i = attach_order.size(); i-- > 0;) {
    const std::uint32_t v = attach_order[i];
    std::uint8_t order = best[v] == 0 ? 1 : best[v];
    if (best[v] != 0 && best_dupes[v] >= 2) {
      ++order;
    }
    verts_[v].order = order;
    const auto parent = static_cast<std::uint32_t>(verts_[v].parent);
    if (!verts_[parent].sea) {
      if (order > best[parent]) {
        best[parent] = order;
        best_dupes[parent] = 1;
      } else if (order == best[parent]) {
        ++best_dupes[parent];
      }
    }
  }

  // 5. Rasterize every river segment onto the cell grid with a one-cell
  // margin: the carve query is then one owner-cell fetch.
  const auto mark = [&](const Dir3& point, std::int32_t segment) {
    Dir3 t1{};
    Dir3 t2{};
    tangent_basis(point, &t1, &t2);
    for (int di = -1; di <= 1; ++di) {
      for (int dj = -1; dj <= 1; ++dj) {
        const Real ou = cell_step * Real(static_cast<double>(di));
        const Real ov = cell_step * Real(static_cast<double>(dj));
        const Dir3 probe{point.x + t1.x * ou + t2.x * ov,
                         point.y + t1.y * ou + t2.y * ov,
                         point.z + t1.z * ou + t2.z * ov};
        std::vector<std::int32_t>& list = cell_segments_[index_of(provinces_.cell_of(probe))];
        bool seen = false;
        for (const std::int32_t s : list) {
          if (s == segment) {
            seen = true;
            break;
          }
        }
        if (!seen) {
          list.push_back(segment);
        }
      }
    }
  };
  for (std::uint32_t v = 0; v < count; ++v) {
    if (verts_[v].parent < 0) {
      continue;
    }
    const Dir3& a = verts_[v].dir;
    const Dir3& b = verts_[static_cast<std::uint32_t>(verts_[v].parent)].dir;
    constexpr int kSamples = 12;
    for (int s = 0; s <= kSamples; ++s) {
      const Real t(static_cast<double>(s) / kSamples);
      const Dir3 p{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
      mark(p, static_cast<std::int32_t>(v));
    }
  }
}

Real DrainageField::carve_m(const Dir3& unit_dir, Real above_sea_m) const {
  if (!enabled_ || above_sea_m <= Real(3.0)) {
    return Real(0.0);
  }
  const std::vector<std::int32_t>& segments =
      cell_segments_[index_of(provinces_.cell_of(unit_dir))];
  Real carve(0.0);
  for (const std::int32_t index : segments) {
    const Vertex& child = verts_[static_cast<std::uint32_t>(index)];
    const Vertex& parent = verts_[static_cast<std::uint32_t>(child.parent)];
    const int order = child.order > 6 ? 6 : child.order;
    const Real width(kWidthM[order]);
    const Real x = segment_chord(unit_dir, child.dir, parent.dir) * radius_m_ / width;
    if (x >= Real(1.0)) {
      continue;
    }
    // U-valley profile, zero value and slope at the rim.
    const Real inner = Real(1.0) - x * x;
    const Real cut = Real(kDepthM[order]) * inner * inner;
    carve = det::max(carve, cut);
  }
  return det::min(carve, above_sea_m - Real(3.0));
}

}  // namespace inf::gen
