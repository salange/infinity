#include "gen/mesher.hpp"

#include <array>
#include <cmath>

#include "gen/mc_tables.hpp"

namespace inf::gen {

namespace {

struct Vec3d {
  double x, y, z;
};

// Corner offsets in marching-cubes order (Bourke convention).
constexpr std::array<std::array<std::uint32_t, 3>, 8> kCornerOffsets = {{
    {0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0},
    {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1},
}};

// Edge -> corner pair (Bourke convention).
constexpr std::array<std::array<int, 2>, 12> kEdgeCorners = {{
    {0, 1}, {1, 2}, {2, 3}, {3, 0},
    {4, 5}, {5, 6}, {6, 7}, {7, 4},
    {0, 4}, {1, 5}, {2, 6}, {3, 7},
}};

std::size_t corner_index(std::uint32_t gx, std::uint32_t gy, std::uint32_t gz) {
  return (static_cast<std::size_t>(gz) * ChunkGrid::kCorners + gy) * ChunkGrid::kCorners + gx;
}

Vec3d interpolate_edge(const Vec3d& a, const Vec3d& b, double da, double db) {
  // Zero crossing between densities of opposite sign; linear.
  const double denom = da - db;
  const double t = denom != 0.0 ? da / denom : 0.5;
  return Vec3d{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
}

}  // namespace

ChunkMesh mesh_chunk(const ChunkGrid& grid, const std::vector<det::Real>& densities) {
  ChunkMesh mesh;
  const std::uint32_t center = ChunkGrid::kVoxels / 2;
  const Dir3 origin_pos = grid.corner_position(center, center, center);
  mesh.origin[0] = origin_pos.x.to_double();
  mesh.origin[1] = origin_pos.y.to_double();
  mesh.origin[2] = origin_pos.z.to_double();

  // Cache all corner world positions once (chunk-relative doubles).
  std::vector<Vec3d> positions(densities.size());
  for (std::uint32_t gz = 0; gz < ChunkGrid::kCorners; ++gz) {
    for (std::uint32_t gy = 0; gy < ChunkGrid::kCorners; ++gy) {
      for (std::uint32_t gx = 0; gx < ChunkGrid::kCorners; ++gx) {
        const Dir3 world = grid.corner_position(gx, gy, gz);
        positions[corner_index(gx, gy, gz)] =
            Vec3d{world.x.to_double() - mesh.origin[0], world.y.to_double() - mesh.origin[1],
                  world.z.to_double() - mesh.origin[2]};
      }
    }
  }

  for (std::uint32_t gz = 0; gz < ChunkGrid::kVoxels; ++gz) {
    for (std::uint32_t gy = 0; gy < ChunkGrid::kVoxels; ++gy) {
      for (std::uint32_t gx = 0; gx < ChunkGrid::kVoxels; ++gx) {
        std::array<double, 8> cell_density;
        std::array<Vec3d, 8> cell_position;
        int cube_index = 0;
        for (int corner = 0; corner < 8; ++corner) {
          const auto& offset = kCornerOffsets[static_cast<std::size_t>(corner)];
          const std::size_t index =
              corner_index(gx + offset[0], gy + offset[1], gz + offset[2]);
          cell_density[static_cast<std::size_t>(corner)] = densities[index].to_double();
          cell_position[static_cast<std::size_t>(corner)] = positions[index];
          // Bourke's tables set the bit for values below the isolevel; our
          // isolevel is 0 with solid = density > 0, so the bit marks AIR.
          // Winding/orientation is pinned by the outward-normal unit test.
          if (cell_density[static_cast<std::size_t>(corner)] < 0.0) {
            cube_index |= 1 << corner;
          }
        }
        const std::uint16_t edge_mask = mc::kEdgeTable[static_cast<std::size_t>(cube_index)];
        if (edge_mask == 0) {
          continue;
        }

        std::array<Vec3d, 12> edge_vertex{};
        for (int edge = 0; edge < 12; ++edge) {
          if ((edge_mask & (1U << edge)) != 0) {
            const int c0 = kEdgeCorners[static_cast<std::size_t>(edge)][0];
            const int c1 = kEdgeCorners[static_cast<std::size_t>(edge)][1];
            edge_vertex[static_cast<std::size_t>(edge)] = interpolate_edge(
                cell_position[static_cast<std::size_t>(c0)],
                cell_position[static_cast<std::size_t>(c1)],
                cell_density[static_cast<std::size_t>(c0)],
                cell_density[static_cast<std::size_t>(c1)]);
          }
        }

        const auto& triangles = mc::kTriTable[static_cast<std::size_t>(cube_index)];
        for (int t = 0; triangles[static_cast<std::size_t>(t)] != -1; t += 3) {
          const Vec3d& v0 = edge_vertex[static_cast<std::size_t>(triangles[static_cast<std::size_t>(t)])];
          const Vec3d& v1 = edge_vertex[static_cast<std::size_t>(triangles[static_cast<std::size_t>(t + 1)])];
          const Vec3d& v2 = edge_vertex[static_cast<std::size_t>(triangles[static_cast<std::size_t>(t + 2)])];
          // Face normal; degenerate triangles are dropped.
          const Vec3d e1{v1.x - v0.x, v1.y - v0.y, v1.z - v0.z};
          const Vec3d e2{v2.x - v0.x, v2.y - v0.y, v2.z - v0.z};
          Vec3d normal{e1.y * e2.z - e1.z * e2.y, e1.z * e2.x - e1.x * e2.z,
                       e1.x * e2.y - e1.y * e2.x};
          const double length =
              std::sqrt(normal.x * normal.x + normal.y * normal.y + normal.z * normal.z);
          if (length <= 1e-12) {
            continue;
          }
          normal = Vec3d{normal.x / length, normal.y / length, normal.z / length};
          for (const Vec3d* vertex : {&v0, &v1, &v2}) {
            mesh.vertices.push_back(static_cast<float>(vertex->x));
            mesh.vertices.push_back(static_cast<float>(vertex->y));
            mesh.vertices.push_back(static_cast<float>(vertex->z));
            mesh.vertices.push_back(static_cast<float>(normal.x));
            mesh.vertices.push_back(static_cast<float>(normal.y));
            mesh.vertices.push_back(static_cast<float>(normal.z));
          }
        }
      }
    }
  }
  return mesh;
}

}  // namespace inf::gen
