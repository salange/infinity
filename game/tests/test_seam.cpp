#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include "core/key.hpp"
#include "gen/universe.hpp"
#include "gen/geo.hpp"
#include "gen/planet.hpp"
#include "gen/terrain.hpp"

// Transvoxel acceptance test (T0006): a fine chunk with a transition face
// and its coarser lateral neighbor must produce the SAME surface trace on
// the shared boundary plane — no cracks. We extract, from both meshes,
// all triangle edges lying on the shared plane and verify the two
// polyline sets coincide (symmetric point-coverage check).

using namespace inf;
using det::Real;

namespace {

gen::BodyHandle body_for(std::uint64_t lo) {
  return gen::default_body(core::Seed128{0, lo});
}

struct Pt {
  double x, y, z;
};

struct Segment {
  Pt a, b;
};

// Collect mesh edges whose endpoints both lie on the cube-face plane
// u == 0 of face 0 (the shared boundary chosen in the test).
std::vector<Segment> seam_segments(const gen::ChunkMesh& mesh) {
  std::vector<Segment> segments;
  const double eps = 1e-6;
  const auto on_plane = [&](const Pt& p) {
    // Face 0 (+X): u = y / x. Plane u == 0 <=> y == 0 (x > 0).
    return std::abs(p.y) < eps * p.x;
  };
  for (std::size_t v = 0; v + 18 <= mesh.vertices.size(); v += 18) {
    Pt tri[3];
    for (int k = 0; k < 3; ++k) {
      tri[k] = Pt{mesh.origin[0] + mesh.vertices[v + k * 6],
                  mesh.origin[1] + mesh.vertices[v + k * 6 + 1],
                  mesh.origin[2] + mesh.vertices[v + k * 6 + 2]};
    }
    for (int k = 0; k < 3; ++k) {
      const Pt& a = tri[k];
      const Pt& b = tri[(k + 1) % 3];
      if (on_plane(a) && on_plane(b)) {
        segments.push_back(Segment{a, b});
      }
    }
  }
  return segments;
}

double point_segment_distance(const Pt& p, const Segment& s) {
  const double dx = s.b.x - s.a.x;
  const double dy = s.b.y - s.a.y;
  const double dz = s.b.z - s.a.z;
  const double len_sq = dx * dx + dy * dy + dz * dz;
  double t = 0.0;
  if (len_sq > 0.0) {
    t = ((p.x - s.a.x) * dx + (p.y - s.a.y) * dy + (p.z - s.a.z) * dz) / len_sq;
    t = std::clamp(t, 0.0, 1.0);
  }
  const double px = s.a.x + dx * t - p.x;
  const double py = s.a.y + dy * t - p.y;
  const double pz = s.a.z + dz * t - p.z;
  return std::sqrt(px * px + py * py + pz * pz);
}

// Max distance from sampled points of set A's segments to set B.
double coverage_error(const std::vector<Segment>& a, const std::vector<Segment>& b) {
  double worst = 0.0;
  for (const Segment& segment : a) {
    for (int i = 0; i <= 4; ++i) {
      const double t = i / 4.0;
      const Pt p{segment.a.x + (segment.b.x - segment.a.x) * t,
                 segment.a.y + (segment.b.y - segment.a.y) * t,
                 segment.a.z + (segment.b.z - segment.a.z) * t};
      double best = 1e30;
      for (const Segment& other : b) {
        best = std::min(best, point_segment_distance(p, other));
      }
      worst = std::max(worst, best);
    }
  }
  return worst;
}

}  // namespace

TEST_CASE("transvoxel: fine/coarse seam traces coincide (crack-free)") {
  const gen::BodyHandle body = body_for(0xBEEF);
  const gen::PlanetParams planet = gen::derive_planet_params(body.params, gen::PlanetType::EarthLike);
  const gen::TerrainField field(body.entity, planet);

  // Shared boundary: face 0, plane u = 0. Fine column (lod 8, i = 127)
  // has its u+ side there; coarse column (lod 7, i = 64) its u- side.
  const std::uint8_t fine_lod = 8;
  const std::uint8_t coarse_lod = 7;
  const std::uint32_t fine_i = 127;
  const std::uint32_t coarse_i = 64;

  int seams_checked = 0;
  // Scan several j positions to find seams that actually carry surface.
  for (std::uint32_t fine_j : {80U, 100U, 128U, 160U, 200U}) {
    const std::uint32_t coarse_j = fine_j / 2;

    // Shell containing the surface at the seam.
    const double u_seam = 0.0;
    const double cells_f = static_cast<double>(std::uint64_t{1} << fine_lod);
    const double v_mid = -1.0 + 2.0 * (fine_j + 0.5) / cells_f;
    const gen::Dir3 dir = gen::face_uv_to_dir(gen::FaceUV{0, Real(u_seam), Real(v_mid)});
    const double elevation = field.elevation_m(dir).to_double();
    const double radius = planet.radius_m.to_double();
    const double t_fine = 2.0 * radius / cells_f;
    const int fine_shell = static_cast<int>(std::floor(elevation / t_fine));
    const int coarse_shell = static_cast<int>(std::floor(elevation / (2.0 * t_fine)));

    const core::ChunkAddr fine_addr{0, fine_lod, fine_i, fine_j,
                                    static_cast<std::int16_t>(fine_shell)};
    const core::ChunkAddr coarse_addr{0, coarse_lod, coarse_i, coarse_j,
                                      static_cast<std::int16_t>(coarse_shell)};

    const gen::ChunkGrid fine_grid = gen::ChunkGrid::from_addr(fine_addr, planet.radius_m);
    const gen::ChunkGrid coarse_grid = gen::ChunkGrid::from_addr(coarse_addr, planet.radius_m);
    const auto fine_padded = gen::sample_chunk_density_padded(field, fine_grid);
    const auto coarse_padded = gen::sample_chunk_density_padded(field, coarse_grid);
    const gen::ChunkMesh fine_mesh =
        gen::mesh_chunk(fine_grid, fine_padded, gen::kTransitionUPlus);
    const gen::ChunkMesh coarse_mesh = gen::mesh_chunk(coarse_grid, coarse_padded, 0);

    auto fine_seam = seam_segments(fine_mesh);
    auto coarse_seam = seam_segments(coarse_mesh);
    // Restrict the coarse trace to the fine chunk's v/r window (the coarse
    // face also borders the sibling fine chunk and spans two shells).
    const double v_lo = -1.0 + 2.0 * fine_j / cells_f;
    const double v_hi = v_lo + 2.0 / cells_f;
    const double r_lo = fine_grid.r0.to_double() - 0.01;
    const double r_hi = fine_grid.r1.to_double() + 0.01;
    auto in_window = [&](const Segment& s) {
      for (const Pt& p : {s.a, s.b}) {
        const double r = std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z);
        const double v = p.z / p.x;  // face 0: v = z / x
        if (r < r_lo || r > r_hi || v < v_lo - 1e-9 || v > v_hi + 1e-9) {
          return false;
        }
      }
      return true;
    };
    coarse_seam.erase(std::remove_if(coarse_seam.begin(), coarse_seam.end(),
                                     [&](const Segment& s) { return !in_window(s); }),
                      coarse_seam.end());

    if (fine_seam.size() < 4 || coarse_seam.size() < 2) {
      continue;  // no surface at this seam location
    }
    ++seams_checked;
    CAPTURE(fine_j);
    CAPTURE(fine_seam.size());
    CAPTURE(coarse_seam.size());
    // Both directions: fine trace on coarse trace and vice versa. The
    // voxel scale here is ~500 m (lod 8); tolerance is a few millimeters.
    CHECK(coverage_error(fine_seam, coarse_seam) < 0.01);
    CHECK(coverage_error(coarse_seam, fine_seam) < 0.01);
  }
  // The test must have found real seams to be meaningful.
  REQUIRE(seams_checked >= 2);
}
