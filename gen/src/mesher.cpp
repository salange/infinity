#include "gen/mesher.hpp"

#include <array>
#include <cmath>

#include "gen/mc_tables.hpp"

namespace inf::gen {

namespace {

struct Vec3d {
  double x, y, z;
};

Vec3d operator-(const Vec3d& a, const Vec3d& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3d operator+(const Vec3d& a, const Vec3d& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3d operator*(const Vec3d& a, double s) { return {a.x * s, a.y * s, a.z * s}; }
double norm(const Vec3d& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }
Vec3d cross(const Vec3d& a, const Vec3d& b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

// Corner offsets in marching-cubes order (Bourke convention).
constexpr std::array<std::array<int, 3>, 8> kCornerOffsets = {{
    {0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0},
    {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1},
}};

// Edge -> corner pair (Bourke convention).
constexpr std::array<std::array<int, 2>, 12> kEdgeCorners = {{
    {0, 1}, {1, 2}, {2, 3}, {3, 0},
    {4, 5}, {5, 6}, {6, 7}, {7, 4},
    {0, 4}, {1, 5}, {2, 6}, {3, 7},
}};

// Cached world positions over the padded grid (chunk-relative doubles).
class PositionCache {
 public:
  PositionCache(const ChunkGrid& grid, const Vec3d& origin) : grid_(grid), origin_(origin) {
    constexpr std::size_t kCount = static_cast<std::size_t>(PaddedDensity::kPadded) *
                                   PaddedDensity::kPadded * PaddedDensity::kPadded;
    positions_.resize(kCount);
    computed_.assign(kCount, false);
  }

  const Vec3d& at(int gx, int gy, int gz) {
    const std::size_t index = ((static_cast<std::size_t>(gz + 1) * PaddedDensity::kPadded) +
                               static_cast<std::size_t>(gy + 1)) *
                                  PaddedDensity::kPadded +
                              static_cast<std::size_t>(gx + 1);
    if (!computed_[index]) {
      const Dir3 world = grid_.corner_position(gx, gy, gz);
      positions_[index] = Vec3d{world.x.to_double() - origin_.x, world.y.to_double() - origin_.y,
                                world.z.to_double() - origin_.z};
      computed_[index] = true;
    }
    return positions_[index];
  }

 private:
  ChunkGrid grid_;
  Vec3d origin_;
  std::vector<Vec3d> positions_;
  std::vector<bool> computed_;
};

}  // namespace

ChunkMesh mesh_chunk(const ChunkGrid& grid, const PaddedDensity& padded) {
  ChunkMesh mesh;
  constexpr int kCenter = static_cast<int>(ChunkGrid::kVoxels) / 2;
  const Dir3 origin_pos = grid.corner_position(kCenter, kCenter, kCenter);
  mesh.origin[0] = origin_pos.x.to_double();
  mesh.origin[1] = origin_pos.y.to_double();
  mesh.origin[2] = origin_pos.z.to_double();
  const Vec3d origin{mesh.origin[0], mesh.origin[1], mesh.origin[2]};

  PositionCache positions(grid, origin);

  const auto density = [&](int gx, int gy, int gz) {
    return padded.at(gx, gy, gz).to_double();
  };

  // Density gradient at a lattice corner, in world space: sum over the
  // three (curvilinear, near-orthogonal) grid axes of the central
  // difference along that axis divided by its world step, times the axis
  // direction. Depends only on world position + field values, so shared
  // chunk-boundary corners get (near-)identical normals in both chunks.
  const auto corner_gradient = [&](int gx, int gy, int gz) {
    Vec3d gradient{0.0, 0.0, 0.0};
    const int coords[3] = {gx, gy, gz};
    for (int axis = 0; axis < 3; ++axis) {
      int lo[3] = {coords[0], coords[1], coords[2]};
      int hi[3] = {coords[0], coords[1], coords[2]};
      lo[axis] -= 1;
      hi[axis] += 1;
      const double delta_density =
          density(hi[0], hi[1], hi[2]) - density(lo[0], lo[1], lo[2]);
      const Vec3d delta_position =
          positions.at(hi[0], hi[1], hi[2]) - positions.at(lo[0], lo[1], lo[2]);
      const double step = norm(delta_position);
      if (step > 0.0) {
        gradient = gradient + delta_position * (delta_density / (step * step));
      }
    }
    return gradient;
  };

  for (int gz = 0; gz < static_cast<int>(ChunkGrid::kVoxels); ++gz) {
    for (int gy = 0; gy < static_cast<int>(ChunkGrid::kVoxels); ++gy) {
      for (int gx = 0; gx < static_cast<int>(ChunkGrid::kVoxels); ++gx) {
        std::array<double, 8> cell_density;
        int cube_index = 0;
        for (int corner = 0; corner < 8; ++corner) {
          const auto& offset = kCornerOffsets[static_cast<std::size_t>(corner)];
          cell_density[static_cast<std::size_t>(corner)] =
              density(gx + offset[0], gy + offset[1], gz + offset[2]);
          // Bourke's tables set the bit for values below the isolevel; our
          // isolevel is 0 with solid = density > 0, so the bit marks AIR.
          // Orientation is pinned by the outward-normal unit test.
          if (cell_density[static_cast<std::size_t>(corner)] < 0.0) {
            cube_index |= 1 << corner;
          }
        }
        const std::uint16_t edge_mask = mc::kEdgeTable[static_cast<std::size_t>(cube_index)];
        if (edge_mask == 0) {
          continue;
        }

        std::array<Vec3d, 12> edge_vertex{};
        std::array<Vec3d, 12> edge_normal{};
        for (int edge = 0; edge < 12; ++edge) {
          if ((edge_mask & (1U << edge)) == 0) {
            continue;
          }
          const int c0 = kEdgeCorners[static_cast<std::size_t>(edge)][0];
          const int c1 = kEdgeCorners[static_cast<std::size_t>(edge)][1];
          const auto& o0 = kCornerOffsets[static_cast<std::size_t>(c0)];
          const auto& o1 = kCornerOffsets[static_cast<std::size_t>(c1)];
          const double d0 = cell_density[static_cast<std::size_t>(c0)];
          const double d1 = cell_density[static_cast<std::size_t>(c1)];
          const double denom = d0 - d1;
          const double t = denom != 0.0 ? d0 / denom : 0.5;

          const Vec3d& p0 = positions.at(gx + o0[0], gy + o0[1], gz + o0[2]);
          const Vec3d& p1 = positions.at(gx + o1[0], gy + o1[1], gz + o1[2]);
          edge_vertex[static_cast<std::size_t>(edge)] = p0 + (p1 - p0) * t;

          // Normal = -gradient (gradient points into solid), interpolated
          // between the edge endpoints' corner gradients.
          const Vec3d g0 = corner_gradient(gx + o0[0], gy + o0[1], gz + o0[2]);
          const Vec3d g1 = corner_gradient(gx + o1[0], gy + o1[1], gz + o1[2]);
          Vec3d normal = (g0 + (g1 - g0) * t) * -1.0;
          const double len = norm(normal);
          normal = len > 1e-12 ? normal * (1.0 / len) : Vec3d{0.0, 0.0, 1.0};
          edge_normal[static_cast<std::size_t>(edge)] = normal;
        }

        const auto& triangles = mc::kTriTable[static_cast<std::size_t>(cube_index)];
        for (int t = 0; triangles[static_cast<std::size_t>(t)] != -1; t += 3) {
          const auto e0 = static_cast<std::size_t>(triangles[static_cast<std::size_t>(t)]);
          const auto e1 = static_cast<std::size_t>(triangles[static_cast<std::size_t>(t + 1)]);
          const auto e2 = static_cast<std::size_t>(triangles[static_cast<std::size_t>(t + 2)]);
          // Drop degenerate triangles.
          const double area2 = norm(cross(edge_vertex[e1] - edge_vertex[e0],
                                          edge_vertex[e2] - edge_vertex[e0]));
          if (area2 <= 1e-12) {
            continue;
          }
          for (const std::size_t e : {e0, e1, e2}) {
            mesh.vertices.push_back(static_cast<float>(edge_vertex[e].x));
            mesh.vertices.push_back(static_cast<float>(edge_vertex[e].y));
            mesh.vertices.push_back(static_cast<float>(edge_vertex[e].z));
            mesh.vertices.push_back(static_cast<float>(edge_normal[e].x));
            mesh.vertices.push_back(static_cast<float>(edge_normal[e].y));
            mesh.vertices.push_back(static_cast<float>(edge_normal[e].z));
          }
        }
      }
    }
  }
  return mesh;
}

}  // namespace inf::gen
