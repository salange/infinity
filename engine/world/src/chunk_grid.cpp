#include "world/chunk_grid.hpp"

namespace inf::world {

using det::Real;

ChunkGrid ChunkGrid::from_addr(const core::ChunkAddr& addr, det::Real surface_radius_m) {
  ChunkGrid grid;
  grid.addr = addr;
  const auto cells = static_cast<double>(std::uint64_t{1} << addr.lod);
  const double u_lo = -1.0 + 2.0 * static_cast<double>(addr.i) / cells;
  const double v_lo = -1.0 + 2.0 * static_cast<double>(addr.j) / cells;
  const double span = 2.0 / cells;
  grid.u0 = Real(u_lo);
  grid.u1 = Real(u_lo + span);
  grid.v0 = Real(v_lo);
  grid.v1 = Real(v_lo + span);

  // Lateral arc length of the chunk at surface radius ~ radius * span
  // (uv is roughly angle-linear per face). Radial thickness matches it.
  // Shell boundaries sit at integer multiples of the thickness so that
  // lod L-1 boundaries coincide with every second lod L boundary (2:1
  // face alignment — required for crack-free LOD stitching).
  const Real thickness = surface_radius_m * Real(span);
  const Real shell(static_cast<double>(addr.shell));
  grid.r0 = surface_radius_m + shell * thickness;
  grid.r1 = grid.r0 + thickness;
  return grid;
}

Dir3 ChunkGrid::corner_position(int gx, int gy, int gz) const {
  return corner_position_f(static_cast<double>(gx), static_cast<double>(gy),
                           static_cast<double>(gz));
}

Dir3 ChunkGrid::corner_position_f(double gx, double gy, double gz) const {
  const Real fx(gx / kVoxels);
  const Real fy(gy / kVoxels);
  const Real fz(gz / kVoxels);
  const Real u = det::lerp(u0, u1, fx);
  const Real v = det::lerp(v0, v1, fy);
  const Real r = det::lerp(r0, r1, fz);
  const Dir3 dir = face_uv_to_dir(FaceUV{addr.face, u, v});
  return Dir3{dir.x * r, dir.y * r, dir.z * r};
}

}  // namespace inf::world
