#include "world/cubesphere.hpp"

namespace inf::world {

using det::Real;

Dir3 normalize(const Dir3& dir) {
  const Real length = det::sqrt(dot(dir, dir));
  return Dir3{dir.x / length, dir.y / length, dir.z / length};
}

FaceUV dir_to_face_uv(const Dir3& dir) {
  const Real ax = det::abs(dir.x);
  const Real ay = det::abs(dir.y);
  const Real az = det::abs(dir.z);

  // Ties broken in axis order x, y, z — stable and deterministic.
  if (ax >= ay && ax >= az) {
    if (dir.x >= Real(0.0)) {
      return FaceUV{0, dir.y / ax, dir.z / ax};
    }
    return FaceUV{1, -dir.y / ax, dir.z / ax};
  }
  if (ay >= az) {
    if (dir.y >= Real(0.0)) {
      return FaceUV{2, -dir.x / ay, dir.z / ay};
    }
    return FaceUV{3, dir.x / ay, dir.z / ay};
  }
  if (dir.z >= Real(0.0)) {
    return FaceUV{4, dir.y / az, dir.x / az};
  }
  return FaceUV{5, dir.y / az, -dir.x / az};
}

Dir3 face_uv_to_dir(const FaceUV& face_uv) {
  const Real u = face_uv.u;
  const Real v = face_uv.v;
  const Real one(1.0);
  Dir3 dir{};
  switch (face_uv.face) {
    case 0: dir = Dir3{one, u, v}; break;
    case 1: dir = Dir3{-one, -u, v}; break;
    case 2: dir = Dir3{-u, one, v}; break;
    case 3: dir = Dir3{u, -one, v}; break;
    case 4: dir = Dir3{v, u, one}; break;
    default: dir = Dir3{-v, u, -one}; break;
  }
  return normalize(dir);
}

void tangent_basis(const Dir3& unit_dir, Dir3* t1, Dir3* t2) {
  // Pick the axis least aligned with the direction, cross twice.
  const Real ax = det::abs(unit_dir.x);
  const Real ay = det::abs(unit_dir.y);
  const Real az = det::abs(unit_dir.z);
  Dir3 helper{Real(1.0), Real(0.0), Real(0.0)};
  if (ay <= ax && ay <= az) {
    helper = Dir3{Real(0.0), Real(1.0), Real(0.0)};
  } else if (az <= ax && az <= ay) {
    helper = Dir3{Real(0.0), Real(0.0), Real(1.0)};
  }
  // t1 = normalize(dir x helper), t2 = dir x t1.
  const Dir3 cross1{unit_dir.y * helper.z - unit_dir.z * helper.y,
                    unit_dir.z * helper.x - unit_dir.x * helper.z,
                    unit_dir.x * helper.y - unit_dir.y * helper.x};
  *t1 = normalize(cross1);
  *t2 = Dir3{unit_dir.y * t1->z - unit_dir.z * t1->y, unit_dir.z * t1->x - unit_dir.x * t1->z,
             unit_dir.x * t1->y - unit_dir.y * t1->x};
}

}  // namespace inf::world
