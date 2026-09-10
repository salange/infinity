#pragma once
// Conservative coverage of a finite triangle by closed voxel boxes. Cells are
// emitted from geometric clipping, never from a surface-sampling density.
#include "math.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace cb::voxel_coverage {
struct Grid {
  Vec3 origin;
  float cell;
  std::array<int, 3> dimensions;
};
namespace detail {
using Point = std::array<double, 3>;
inline Point subtract(Point a, Point b) {
  return {a[0]-b[0], a[1]-b[1], a[2]-b[2]};
}
inline Point cross_product(Point a, Point b) {
  return {a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2],
          a[0]*b[1]-a[1]*b[0]};
}
struct Polygon {
  // Starting with three vertices, twelve half-space clips add at most twelve
  // vertices. Extra capacity also accommodates coincident boundary vertices.
  std::array<Point, 24> vertices{};
  std::size_t size{};
};
inline Polygon clip(const Polygon& input, int axis, double bound, bool greater) {
  Polygon output;
  if (input.size == 0) return output;
  Point previous = input.vertices[input.size-1];
  bool previous_inside = greater ? previous[axis] >= bound : previous[axis] <= bound;
  for (std::size_t i=0; i<input.size; ++i) {
    const Point current = input.vertices[i];
    const bool current_inside = greater ? current[axis] >= bound : current[axis] <= bound;
    if (previous_inside != current_inside) {
      const double t = (bound-previous[axis])/(current[axis]-previous[axis]);
      Point intersection;
      for (int a=0; a<3; ++a)
        intersection[a] = previous[a]+t*(current[a]-previous[a]);
      intersection[axis] = bound;
      output.vertices[output.size++] = intersection;
    }
    if (current_inside) output.vertices[output.size++] = current;
    previous = current;
    previous_inside = current_inside;
  }
  return output;
}
inline std::array<double, 2> extent(const Polygon& polygon, int axis) {
  double low = polygon.vertices[0][axis], high = low;
  for (std::size_t i=1; i<polygon.size; ++i) {
    low = std::min(low, polygon.vertices[i][axis]);
    high = std::max(high, polygon.vertices[i][axis]);
  }
  return {low, high};
}
inline std::array<int, 2> cells(const Polygon& polygon, int axis,
                              const Point& origin, double cell,
                              const std::array<int, 3>& dimensions) {
  const auto range = extent(polygon, axis);
  // Closed boxes on both sides of an exactly coincident face touch the surface.
  // This is boundary ownership, not an inflated triangle or plane interval.
  const int low = int(std::ceil((range[0]-origin[axis])/cell))-1;
  const int high = int(std::floor((range[1]-origin[axis])/cell));
  return {std::clamp(low, 0, dimensions[axis]-1),
          std::clamp(high, 0, dimensions[axis]-1)};
}
} // namespace detail

// Visit(int x,int y,int z,Vec3 point_on_triangle) is called exactly once for
// every intersected cell. The supplied point lies on the clipped triangle and
// in that cell, suitable for surface-plane metadata. Zero-area input triangles
// are rejected; arbitrarily thin nonzero-area triangles remain eligible.
template<class Visit>
inline std::size_t triangle_cells(const Grid& grid, Vec3 a, Vec3 b, Vec3 c,
                                  Visit visit) {
  using namespace detail;
  if (!(grid.cell > 0) || !std::isfinite(grid.cell)) return 0;
  for (int size : grid.dimensions) if (size <= 0) return 0;
  const Point origin{grid.origin.x, grid.origin.y, grid.origin.z};
  Polygon polygon;
  polygon.vertices[0] = {a.x, a.y, a.z};
  polygon.vertices[1] = {b.x, b.y, b.z};
  polygon.vertices[2] = {c.x, c.y, c.z};
  polygon.size = 3;
  for (int axis=0; axis<3; ++axis) {
    if (!std::isfinite(origin[axis])) return 0;
    for (std::size_t i=0; i<3; ++i)
      if (!std::isfinite(polygon.vertices[i][axis])) return 0;
  }
  const Point normal = cross_product(subtract(polygon.vertices[1], polygon.vertices[0]),
                                     subtract(polygon.vertices[2], polygon.vertices[0]));
  int major = 0;
  for (int axis=1; axis<3; ++axis)
    if (std::abs(normal[axis]) > std::abs(normal[major])) major = axis;
  if (normal[major] == 0) return 0;
  const double cell = grid.cell;
  for (int axis=0; axis<3; ++axis) {
    polygon = clip(polygon, axis, origin[axis], true);
    polygon = clip(polygon, axis, origin[axis]+grid.dimensions[axis]*cell, false);
    if (polygon.size == 0) return 0;
  }
  const int u = (major+1)%3, v = (major+2)%3;
  const auto us = cells(polygon, u, origin, cell, grid.dimensions);
  std::size_t count = 0;
  for (int x=us[0]; x<=us[1]; ++x) {
    Polygon strip = clip(polygon, u, origin[u]+x*cell, true);
    strip = clip(strip, u, origin[u]+(x+1)*cell, false);
    if (strip.size == 0) continue;
    const auto vs = cells(strip, v, origin, cell, grid.dimensions);
    for (int y=vs[0]; y<=vs[1]; ++y) {
      Polygon column = clip(strip, v, origin[v]+y*cell, true);
      column = clip(column, v, origin[v]+(y+1)*cell, false);
      if (column.size == 0) continue;
      const auto depths = cells(column, major, origin, cell, grid.dimensions);
      for (int z=depths[0]; z<=depths[1]; ++z) {
        Polygon intersection = clip(column, major, origin[major]+z*cell, true);
        intersection = clip(intersection, major, origin[major]+(z+1)*cell, false);
        if (intersection.size == 0) continue;
        Point centre{};
        for (std::size_t i=0; i<intersection.size; ++i)
          for (int axis=0; axis<3; ++axis) centre[axis] += intersection.vertices[i][axis];
        for (double& coordinate : centre) coordinate /= double(intersection.size);
        std::array<int, 3> index{};
        index[u]=x; index[v]=y; index[major]=z;
        visit(index[0], index[1], index[2],
              Vec3{float(centre[0]), float(centre[1]), float(centre[2])});
        ++count;
      }
    }
  }
  return count;
}
} // namespace cb::voxel_coverage
