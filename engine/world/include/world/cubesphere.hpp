#pragma once

#include <cstdint>

#include "core/det/real.hpp"

namespace inf::world {

// Cube-sphere mapping (prototype-v0 spec sections 4/6): six faces, each
// parameterized by (u, v) in [-1, 1]. Face selection by largest absolute
// component. All math in det::Real (DET-REAL class; migrate-by-evidence).
//
// Face axis convention (right-handed, stable — feeds keys, never reorder):
//   face 0: +X   face 1: -X   face 2: +Y   face 3: -Y   face 4: +Z   face 5: -Z

struct Dir3 {
  det::Real x;
  det::Real y;
  det::Real z;
};

struct FaceUV {
  std::uint8_t face{0};
  det::Real u;  // [-1, 1]
  det::Real v;  // [-1, 1]
};

// dir must be nonzero; result is the unit direction.
Dir3 normalize(const Dir3& dir);

// Maps a (not necessarily unit) direction to its cube face and (u, v).
FaceUV dir_to_face_uv(const Dir3& dir);

// Maps (face, u, v) to the unit direction through that cube point.
Dir3 face_uv_to_dir(const FaceUV& face_uv);

// Orthonormal-ish tangent basis at a direction (exact orthonormality is not
// required by callers — used for neighborhood stencils only).
void tangent_basis(const Dir3& unit_dir, Dir3* t1, Dir3* t2);

inline det::Real dot(const Dir3& a, const Dir3& b) {
  return a.x * b.x + a.y * b.y + a.z * b.z;
}

// Squared chord distance between two unit directions.
inline det::Real chord_sq(const Dir3& a, const Dir3& b) {
  const det::Real dx = a.x - b.x;
  const det::Real dy = a.y - b.y;
  const det::Real dz = a.z - b.z;
  return dx * dx + dy * dy + dz * dz;
}

}  // namespace inf::world
