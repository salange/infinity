#pragma once
// Finite point-source visibility over existing surface occupancy. This is a
// voxel approximation: one receiver cell and one source cell are excluded to
// avoid self-intersection. Unknown space stays explicitly unknown, not
// occluded.
#include "math.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <type_traits>

namespace cb::point_visibility {
struct Grid {
  Vec3 origin;
  float cell;
  std::array<int, 3> dimensions;
};
struct Result {
  bool blocked{};
  float covered{}; // fraction of the finite segment inside known grids
};
struct Surface {
  std::uint32_t
      normal{}; // octahedral signed15-bit xy, occupied30, incompatible31
  std::int16_t lower{}, upper{}; // plane interval, cell-relative /32767
};
inline Vec3 surface_normal(std::uint32_t packed) {
  auto value = [&](int shift) {
    const int bits = (packed >> shift) & 32767;
    return float(bits >= 16384 ? bits - 32768 : bits) / 16383.f;
  };
  Vec3 n{value(0), value(15), 1.f - std::abs(value(0)) - std::abs(value(15))};
  const float fold = std::max(-n.z, 0.f);
  n.x += n.x >= 0 ? -fold : fold;
  n.y += n.y >= 0 ? -fold : fold;
  return normalize(n);
}
inline void include_surface(Surface &surface, Vec3 p, Vec3 geometric_normal,
                            Vec3 centre, float cell) {
  if ((surface.normal & 0x80000000u) != 0)
    return;
  const bool first = surface.normal == 0;
  if (first) {
    Vec3 oct = geometric_normal * (1.f / (std::abs(geometric_normal.x) +
                                          std::abs(geometric_normal.y) +
                                          std::abs(geometric_normal.z)));
    if (oct.z < 0) {
      const float x = oct.x;
      oct.x = (1.f - std::abs(oct.y)) * (x < 0 ? -1.f : 1.f);
      oct.y = (1.f - std::abs(x)) * (oct.y < 0 ? -1.f : 1.f);
    }
    auto pack = [](float f) {
      return std::uint32_t(int(std::round(f * 16383.f))) & 32767u;
    };
    surface.normal = pack(oct.x) | (pack(oct.y) << 15) | 0x40000000u;
  }
  const Vec3 n = surface_normal(surface.normal);
  const float alignment = dot(n, geometric_normal);
  if (std::abs(alignment) < .99999f) {
    surface.normal |= 0x80000000u;
    return;
  }
  const Vec3 aligned_normal = geometric_normal * (alignment < 0 ? -1.f : 1.f);
  const Vec3 quantization = n - aligned_normal;
  // Cover normal quantization over the full cell and round the plane interval
  // outward. Axis-aligned surfaces retain exact normals and sub-mm precision.
  const float padding =
      .5f * (std::abs(quantization.x) + std::abs(quantization.y) +
             std::abs(quantization.z));
  const float offset = dot(aligned_normal, p - centre) / cell;
  const int low = std::clamp(int(std::floor((offset - padding) * 32767.f)) - 1,
                             -32767, 32767);
  const int high = std::clamp(int(std::ceil((offset + padding) * 32767.f)) + 1,
                              -32767, 32767);
  surface.lower = static_cast<std::int16_t>(
      first ? low : std::min<int>(surface.lower, low));
  surface.upper = static_cast<std::int16_t>(
      first ? high : std::max<int>(surface.upper, high));
}
inline bool surface_blocks(const Surface &surface, Vec3 centre, float cell,
                           Vec3 a, Vec3 b) {
  if (surface.normal == 0)
    return false;
  if ((surface.normal & 0x80000000u) != 0)
    return true;
  const Vec3 n = surface_normal(surface.normal);
  const float da = dot(n, a - centre) / cell, db = dot(n, b - centre) / cell;
  return std::max(da, db) >= float(surface.lower) / 32767.f &&
         std::min(da, db) <= float(surface.upper) / 32767.f;
}
inline float component(Vec3 v, int axis) {
  return axis == 0 ? v.x : axis == 1 ? v.y : v.z;
}
inline std::array<int, 3> cell_at(const Grid &grid, Vec3 p) {
  return {int(std::floor((p.x - grid.origin.x) / grid.cell)),
          int(std::floor((p.y - grid.origin.y) / grid.cell)),
          int(std::floor((p.z - grid.origin.z) / grid.cell))};
}
inline std::array<float, 2> interval(const Grid &grid, Vec3 start, Vec3 delta) {
  float begin = 0, end = 1;
  for (int axis = 0; axis < 3; ++axis) {
    const float low = component(grid.origin, axis),
                high = low + grid.dimensions[axis] * grid.cell,
                p = component(start, axis), d = component(delta, axis);
    if (std::abs(d) < 1e-12f) {
      if (p < low || p >= high)
        return {1, 0};
    } else {
      const float a = (low - p) / d, b = (high - p) / d;
      begin = std::max(begin, std::min(a, b));
      end = std::min(end, std::max(a, b));
    }
  }
  return {begin, end};
}
template <class Occupied>
inline bool trace_interval(const Grid &grid, Vec3 start, Vec3 delta,
                           Vec3 receiver, Vec3 source, float begin, float end,
                           Occupied occupied) {
  if (end <= begin)
    return false;
  const auto receiver_cell = cell_at(grid, receiver),
             source_cell = cell_at(grid, source);
  auto cursor = cell_at(
      grid, start + delta * (begin + std::min(1e-6f, (end - begin) * .25f)));
  std::array<int, 3> step{};
  std::array<float, 3> next{}, stride{};
  for (int axis = 0; axis < 3; ++axis) {
    cursor[axis] = std::clamp(cursor[axis], 0, grid.dimensions[axis] - 1);
    const float d = component(delta, axis);
    step[axis] = d > 0 ? 1 : d < 0 ? -1 : 0;
    stride[axis] = step[axis] ? grid.cell / std::abs(d) : 1e30f;
    next[axis] = step[axis]
                     ? (component(grid.origin, axis) +
                        (cursor[axis] + (step[axis] > 0 ? 1 : 0)) * grid.cell -
                        component(start, axis)) /
                           d
                     : 1e30f;
  }
  // Each step crosses at least one cell boundary. No fixed ray-distance or
  // sample-count cutoff can declare a still-untraversed region visible.
  const int bound =
      grid.dimensions[0] + grid.dimensions[1] + grid.dimensions[2] + 3;
  float entered = begin;
  for (int count = 0; count < bound; ++count) {
    const float crossing = std::min({next[0], next[1], next[2]});
    if (cursor != receiver_cell && cursor != source_cell) {
      bool hit = false;
      if constexpr (std::is_invocable_r_v<bool, Occupied, int, int, int, Vec3,
                                          Vec3>)
        hit = occupied(cursor[0], cursor[1], cursor[2], start + delta * entered,
                       start + delta * std::min(crossing, end));
      else
        hit = occupied(cursor[0], cursor[1], cursor[2]);
      if (hit)
        return true;
    }
    if (crossing >= end)
      return false;
    for (int axis = 0; axis < 3; ++axis)
      if (next[axis] <= crossing + 1e-7f) {
        cursor[axis] += step[axis];
        next[axis] += stride[axis];
        if (cursor[axis] < 0 || cursor[axis] >= grid.dimensions[axis])
          return false;
      }
    entered = crossing;
  }
  // Unreachable for valid finite grids, conservative if numerical corruption
  // ever violates the traversal invariant.
  return true;
}
template <class CoarseOccupied, class FineOccupied>
inline Result segment(Vec3 receiver, Vec3 normal, Vec3 source,
                      const Grid &coarse, CoarseOccupied coarse_occupied,
                      const Grid *fine, FineOccupied fine_occupied) {
  // A fixed 2 mm surface offset does not hop arbitrary numbers of voxels.
  // Source/receiver cell exclusion is separate and documented above.
  const Vec3 start = receiver + normal * .002f, delta = source - start;
  if (dot(delta, delta) < 1e-12f)
    return {};
  const auto ci = interval(coarse, start, delta);
  const auto fi =
      fine ? interval(*fine, start, delta) : std::array<float, 2>{1, 0};
  Result result;
  auto run = [&](const Grid &grid, float begin, float end, auto occupied) {
    if (end <= begin)
      return;
    result.covered += end - begin;
    result.blocked =
        result.blocked || trace_interval(grid, start, delta, receiver, source,
                                         begin, end, occupied);
  };
  if (fi[1] > fi[0]) {
    run(*fine, fi[0], fi[1], fine_occupied);
    run(coarse, ci[0], std::min(ci[1], fi[0]), coarse_occupied);
    run(coarse, std::max(ci[0], fi[1]), ci[1], coarse_occupied);
  } else {
    run(coarse, ci[0], ci[1], coarse_occupied);
  }
  result.covered = std::clamp(result.covered, 0.f, 1.f);
  return result;
}
} // namespace cb::point_visibility
