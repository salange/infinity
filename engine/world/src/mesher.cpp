#include "world/mesher.hpp"

#include <array>
#include <cmath>

#include "world/transvoxel_tables.hpp"

namespace inf::world {

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

// Transvoxel regular-cell corner offsets: corner index = x + 2y + 4z.
constexpr std::array<std::array<int, 3>, 8> kTvCorners = {{
    {0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0},
    {0, 0, 1}, {1, 0, 1}, {0, 1, 1}, {1, 1, 1},
}};

constexpr int kN = static_cast<int>(ChunkGrid::kVoxels);

// Emits triangles into the mesh; owns the shared position/gradient caches
// and the boundary-retreat remap.
class MeshBuilder {
 public:
  MeshBuilder(const ChunkGrid& grid, const PaddedDensity& padded, TransitionMask transitions,
              ChunkMesh* mesh)
      : grid_(grid), padded_(padded), transitions_(transitions), mesh_(mesh) {
    const Dir3 origin = grid.corner_position(kN / 2, kN / 2, kN / 2);
    mesh->origin[0] = origin.x.to_double();
    mesh->origin[1] = origin.y.to_double();
    mesh->origin[2] = origin.z.to_double();
    const std::size_t count = static_cast<std::size_t>(PaddedDensity::kPadded) *
                              PaddedDensity::kPadded * PaddedDensity::kPadded;
    raw_positions_.resize(count);
    raw_computed_.assign(count, false);
  }

  double density(int gx, int gy, int gz) const { return padded_.at(gx, gy, gz).to_double(); }

  // Unremapped world position (chunk-relative) of an integer lattice corner.
  const Vec3d& raw_position(int gx, int gy, int gz) {
    const std::size_t index = ((static_cast<std::size_t>(gz + 1) * PaddedDensity::kPadded) +
                               static_cast<std::size_t>(gy + 1)) *
                                  PaddedDensity::kPadded +
                              static_cast<std::size_t>(gx + 1);
    if (!raw_computed_[index]) {
      raw_positions_[index] = world_at(static_cast<double>(gx), static_cast<double>(gy),
                                       static_cast<double>(gz));
      raw_computed_[index] = true;
    }
    return raw_positions_[index];
  }

  Vec3d world_at(double gx, double gy, double gz) const {
    const Dir3 world = grid_.corner_position_f(gx, gy, gz);
    return Vec3d{world.x.to_double() - mesh_->origin[0], world.y.to_double() - mesh_->origin[1],
                 world.z.to_double() - mesh_->origin[2]};
  }

  // Boundary retreat (Lengyel section 4.4, applied as a lattice remap):
  // on a transition face, the boundary sample layer moves half a cell
  // inward; the freed half-cell slab is filled by transition cells.
  // skip_axis exempts one axis (used for a transition cell's own low-res
  // corners, which stay on the true shared plane of that face while still
  // following the other axes' retreats).
  Vec3d remapped_position(int gx, int gy, int gz, int skip_axis = -1) {
    double fx = static_cast<double>(gx);
    double fy = static_cast<double>(gy);
    double fz = static_cast<double>(gz);
    bool remapped = false;
    if (skip_axis != 0) {
      if ((transitions_ & kTransitionUMinus) != 0 && gx == 0) {
        fx = 0.5;
        remapped = true;
      } else if ((transitions_ & kTransitionUPlus) != 0 && gx == kN) {
        fx = kN - 0.5;
        remapped = true;
      }
    }
    if (skip_axis != 1) {
      if ((transitions_ & kTransitionVMinus) != 0 && gy == 0) {
        fy = 0.5;
        remapped = true;
      } else if ((transitions_ & kTransitionVPlus) != 0 && gy == kN) {
        fy = kN - 0.5;
        remapped = true;
      }
    }
    if (!remapped) {
      return raw_position(gx, gy, gz);
    }
    return world_at(fx, fy, fz);
  }

  // Outward normal from the density gradient at a lattice corner
  // (unremapped physical field — cosmetic output).
  Vec3d corner_normal(int gx, int gy, int gz) {
    Vec3d gradient{0.0, 0.0, 0.0};
    const int c[3] = {gx, gy, gz};
    for (int axis = 0; axis < 3; ++axis) {
      int lo[3] = {c[0], c[1], c[2]};
      int hi[3] = {c[0], c[1], c[2]};
      lo[axis] -= 1;
      hi[axis] += 1;
      const double delta = density(hi[0], hi[1], hi[2]) - density(lo[0], lo[1], lo[2]);
      const Vec3d step = raw_position(hi[0], hi[1], hi[2]) - raw_position(lo[0], lo[1], lo[2]);
      const double len = norm(step);
      if (len > 0.0) {
        gradient = gradient + step * (delta / (len * len));
      }
    }
    return gradient * -1.0;  // gradient points into solid; normal points out
  }

  void emit_triangle(const Vec3d* p0, const Vec3d* n0, const Vec3d* p1, const Vec3d* n1,
                     const Vec3d* p2, const Vec3d* n2, bool reverse) {
    if (reverse) {
      std::swap(p1, p2);
      std::swap(n1, n2);
    }
    const double area = norm(cross(*p1 - *p0, *p2 - *p0));
    if (area <= 1e-12) {
      return;
    }
    for (const auto& [p, n] : {std::pair{p0, n0}, std::pair{p1, n1}, std::pair{p2, n2}}) {
      Vec3d unit = *n;
      const double len = norm(unit);
      unit = len > 1e-12 ? unit * (1.0 / len) : Vec3d{0.0, 0.0, 1.0};
      mesh_->vertices.push_back(static_cast<float>(p->x));
      mesh_->vertices.push_back(static_cast<float>(p->y));
      mesh_->vertices.push_back(static_cast<float>(p->z));
      mesh_->vertices.push_back(static_cast<float>(unit.x));
      mesh_->vertices.push_back(static_cast<float>(unit.y));
      mesh_->vertices.push_back(static_cast<float>(unit.z));
    }
  }

  // --- regular cells (Transvoxel modified marching cubes) ---------------
  void mesh_regular_cells() {
    for (int gz = 0; gz < kN; ++gz) {
      for (int gy = 0; gy < kN; ++gy) {
        for (int gx = 0; gx < kN; ++gx) {
          mesh_regular_cell(gx, gy, gz);
        }
      }
    }
  }

  void mesh_regular_cell(int gx, int gy, int gz) {
    // Lengyel's convention: solid = negative sample. Our solid is
    // density > 0, so the table value is the negated density.
    std::array<double, 8> value;
    int case_code = 0;
    for (int corner = 0; corner < 8; ++corner) {
      const auto& offset = kTvCorners[static_cast<std::size_t>(corner)];
      value[static_cast<std::size_t>(corner)] =
          -density(gx + offset[0], gy + offset[1], gz + offset[2]);
      if (value[static_cast<std::size_t>(corner)] < 0.0) {
        case_code |= 1 << corner;
      }
    }
    if (case_code == 0 || case_code == 255) {
      return;
    }
    const unsigned char cell_class = tv::regularCellClass[static_cast<std::size_t>(case_code)];
    const auto& cell_data = tv::regularCellData[cell_class];
    const auto* vertex_data = tv::regularVertexData[static_cast<std::size_t>(case_code)];

    std::array<Vec3d, 12> positions;
    std::array<Vec3d, 12> normals;
    const long vertex_count = cell_data.GetVertexCount();
    for (long v = 0; v < vertex_count; ++v) {
      const int edge = vertex_data[v] & 0xFF;
      const int corner_a = (edge >> 4) & 0x0F;
      const int corner_b = edge & 0x0F;
      const double value_a = value[static_cast<std::size_t>(corner_a)];
      const double value_b = value[static_cast<std::size_t>(corner_b)];
      const double denom = value_a - value_b;
      const double t = denom != 0.0 ? value_a / denom : 0.5;
      const auto& oa = kTvCorners[static_cast<std::size_t>(corner_a)];
      const auto& ob = kTvCorners[static_cast<std::size_t>(corner_b)];
      const Vec3d pa = remapped_position(gx + oa[0], gy + oa[1], gz + oa[2]);
      const Vec3d pb = remapped_position(gx + ob[0], gy + ob[1], gz + ob[2]);
      positions[static_cast<std::size_t>(v)] = pa + (pb - pa) * t;
      const Vec3d na = corner_normal(gx + oa[0], gy + oa[1], gz + oa[2]);
      const Vec3d nb = corner_normal(gx + ob[0], gy + ob[1], gz + ob[2]);
      normals[static_cast<std::size_t>(v)] = na + (nb - na) * t;
    }
    const long triangle_count = cell_data.GetTriangleCount();
    for (long t = 0; t < triangle_count; ++t) {
      const auto i0 = static_cast<std::size_t>(cell_data.vertexIndex[t * 3]);
      const auto i1 = static_cast<std::size_t>(cell_data.vertexIndex[t * 3 + 1]);
      const auto i2 = static_cast<std::size_t>(cell_data.vertexIndex[t * 3 + 2]);
      emit_triangle(&positions[i0], &normals[i0], &positions[i1], &normals[i1], &positions[i2],
                    &normals[i2], kRegularWindingReverse);
    }
  }

  // --- transition cells --------------------------------------------------
  void mesh_transition_faces() {
    struct FaceSpec {
      TransitionMask bit;
      int axis;        // 0 = u (gx), 1 = v (gy)
      int plane;       // boundary lattice coordinate on that axis
      bool flip;       // face-frame handedness relative to the base face
    };
    // Handedness: (e_s x e_t) . inward differs per face; see T0006 notes.
    const FaceSpec faces[4] = {
        {kTransitionUMinus, 0, 0, false},
        {kTransitionUPlus, 0, kN, true},
        {kTransitionVMinus, 1, 0, true},
        {kTransitionVPlus, 1, kN, false},
    };
    for (const FaceSpec& face : faces) {
      if ((transitions_ & face.bit) == 0) {
        continue;
      }
      for (int b = 0; b < kN; b += 2) {
        for (int a = 0; a < kN; a += 2) {
          mesh_transition_cell(face.axis, face.plane, face.flip, a, b);
        }
      }
    }
  }

  void lattice_of(int axis, int plane, int s, int t, int* gx, int* gy, int* gz) const {
    if (axis == 0) {
      *gx = plane;
      *gy = s;
      *gz = t;
    } else {
      *gx = s;
      *gy = plane;
      *gz = t;
    }
  }

  void mesh_transition_cell(int axis, int plane, bool face_flip, int a, int b) {
    // 3x3 full-resolution samples, Lengyel's numbering: 0 1 2 / 3 4 5 /
    // 6 7 8 over (s, t).
    std::array<double, 13> value;
    std::array<Vec3d, 13> position;
    std::array<Vec3d, 13> normal;
    for (int dt = 0; dt < 3; ++dt) {
      for (int ds = 0; ds < 3; ++ds) {
        const int index = dt * 3 + ds;
        int gx = 0;
        int gy = 0;
        int gz = 0;
        lattice_of(axis, plane, a + ds, b + dt, &gx, &gy, &gz);
        value[static_cast<std::size_t>(index)] = -density(gx, gy, gz);
        // Full-res side sits on the RETREATED plane (remapped along the
        // face axis; other axes' retreats apply too).
        position[static_cast<std::size_t>(index)] = remapped_position(gx, gy, gz);
        normal[static_cast<std::size_t>(index)] = corner_normal(gx, gy, gz);
      }
    }
    // Low-res corners 9..C on the TRUE shared plane, aligned with face
    // samples 0, 2, 6, 8 (values duplicated).
    const int corner_map[4] = {0, 2, 6, 8};
    for (int c = 0; c < 4; ++c) {
      const int face_index = corner_map[c];
      const int ds = (face_index % 3);
      const int dt = (face_index / 3);
      int gx = 0;
      int gy = 0;
      int gz = 0;
      lattice_of(axis, plane, a + ds, b + dt, &gx, &gy, &gz);
      value[static_cast<std::size_t>(9 + c)] = value[static_cast<std::size_t>(face_index)];
      position[static_cast<std::size_t>(9 + c)] =
          remapped_position(gx, gy, gz, /*skip_axis=*/axis);
      normal[static_cast<std::size_t>(9 + c)] = normal[static_cast<std::size_t>(face_index)];
    }

    // Case code: perimeter clockwise then center (Lengyel section 4.3).
    int case_code = 0;
    const int bit_order[9] = {0, 1, 2, 5, 8, 7, 6, 3, 4};
    for (int bit = 0; bit < 9; ++bit) {
      if (value[static_cast<std::size_t>(bit_order[bit])] < 0.0) {
        case_code |= 1 << bit;
      }
    }
    if (case_code == 0 || case_code == 511) {
      return;
    }
    const unsigned char cell_class =
        tv::transitionCellClass[static_cast<std::size_t>(case_code)];
    const bool class_reverse = (cell_class & 0x80) != 0;
    const auto& cell_data = tv::transitionCellData[cell_class & 0x7F];
    const auto* vertex_data = tv::transitionVertexData[static_cast<std::size_t>(case_code)];

    std::array<Vec3d, 12> out_position;
    std::array<Vec3d, 12> out_normal;
    const long vertex_count = cell_data.GetVertexCount();
    for (long v = 0; v < vertex_count; ++v) {
      const int edge = vertex_data[v] & 0xFF;
      const auto corner_a = static_cast<std::size_t>((edge >> 4) & 0x0F);
      const auto corner_b = static_cast<std::size_t>(edge & 0x0F);
      const double value_a = value[corner_a];
      const double value_b = value[corner_b];
      const double denom = value_a - value_b;
      const double t = denom != 0.0 ? value_a / denom : 0.5;
      out_position[static_cast<std::size_t>(v)] =
          position[corner_a] + (position[corner_b] - position[corner_a]) * t;
      out_normal[static_cast<std::size_t>(v)] =
          normal[corner_a] + (normal[corner_b] - normal[corner_a]) * t;
    }
    const long triangle_count = cell_data.GetTriangleCount();
    const bool reverse = kTransitionWindingReverse ^ face_flip ^ class_reverse;
    for (long t = 0; t < triangle_count; ++t) {
      const auto i0 = static_cast<std::size_t>(cell_data.vertexIndex[t * 3]);
      const auto i1 = static_cast<std::size_t>(cell_data.vertexIndex[t * 3 + 1]);
      const auto i2 = static_cast<std::size_t>(cell_data.vertexIndex[t * 3 + 2]);
      emit_triangle(&out_position[i0], &out_normal[i0], &out_position[i1], &out_normal[i1],
                    &out_position[i2], &out_normal[i2], reverse);
    }
  }

  // Winding parities validated by the outward-normal unit test.
  static constexpr bool kRegularWindingReverse = false;
  static constexpr bool kTransitionWindingReverse = false;

 private:
  const ChunkGrid& grid_;
  const PaddedDensity& padded_;
  TransitionMask transitions_;
  ChunkMesh* mesh_;
  std::vector<Vec3d> raw_positions_;
  std::vector<bool> raw_computed_;
};

}  // namespace

ChunkMesh mesh_chunk(const ChunkGrid& grid, const PaddedDensity& padded,
                     TransitionMask transitions) {
  ChunkMesh mesh;
  MeshBuilder builder(grid, padded, transitions, &mesh);
  builder.mesh_regular_cells();
  builder.mesh_transition_faces();
  return mesh;
}

}  // namespace inf::world
