#pragma once
// Conservative camera-specific light lists. All sphere-reaching sources
// survive; the fragment's actual world position performs the final
// finite-radius test.
#include "city/scene.hpp"
#include "renderer_memory.hpp"
#include <array>
#include <cmath>
#include <limits>
#include <span>
#include <vector>

namespace cb::lighting {
inline constexpr std::uint32_t tile_size = 16;
inline constexpr std::uint32_t view_count = 4;

struct Bounds {
  std::uint32_t x0{}, x1{}, y0{}, y1{};
  bool visible{};
};

inline Bounds sphere_bounds(inf::city::Vec3 position, float radius,
                            const inf::city::Mat4 &view,
                            const inf::city::Mat4 &projection,
                            std::uint32_t width, std::uint32_t height,
                            float near_clip) {
  if (!width || !height || !(near_clip > 0))
    throw std::length_error(
        "light projection requires valid viewport and near clip");
  if (!(radius > 0) || !std::isfinite(radius))
    return {};
  const auto v = inf::city::mul(view, inf::city::Vec4{position, 1});
  const double x = v.x, y = v.y, z = -v.z, r = radius;
  if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
    throw std::length_error("light has a non-finite camera position");
  if (z + r < near_clip)
    return {};
  const double fx = projection.at(0, 0), fy = projection.at(1, 1);
  const double shift_x = -projection.at(0, 2), shift_y = -projection.at(1, 2);
  // Test the actual jittered side planes before the near-plane fallback.
  for (const auto axis : {std::array<double, 3>{x, fx, shift_x},
                          std::array<double, 3>{y, fy, shift_y}})
    for (const double sign : {-1., 1.}) {
      const double a = sign * axis[1], b = 1 + sign * axis[2];
      if (a * axis[0] + b * z < -r * std::sqrt(a * a + b * b))
        return {};
    }
  const auto tx = (std::uint64_t(width) + tile_size - 1) / tile_size;
  const auto ty = (std::uint64_t(height) + tile_size - 1) / tile_size;
  if (z - r <= near_clip)
    return {0, static_cast<std::uint32_t>(tx - 1), 0,
            static_cast<std::uint32_t>(ty - 1), true};
  // Tangents to the sphere in each axis/depth section give the exact projected
  // extrema. Symmetric radius expansion about the projected centre is not
  // sufficient for strongly off-axis spheres.
  auto interval = [&](double c, double focal, double shift) {
    const double root = std::sqrt(std::max(0., c * c + z * z - r * r));
    const double denom = z * z - r * r;
    return std::array<double, 2>{(c * z - r * root) * focal / denom + shift,
                                 (c * z + r * root) * focal / denom + shift};
  };
  auto bx = interval(x, fx, shift_x), by = interval(y, fy, shift_y);
  // One pixel protects float view transforms and multisample boundary coverage.
  bx[0] -= 2. / width;
  bx[1] += 2. / width;
  by[0] -= 2. / height;
  by[1] += 2. / height;
  if (bx[1] < -1 || bx[0] > 1 || by[1] < -1 || by[0] > 1)
    return {};
  auto tile = [](double pixel, std::uint64_t count) {
    return static_cast<std::uint32_t>(
        std::clamp(std::floor(pixel / tile_size), 0., double(count - 1)));
  };
  return {tile((bx[0] + 1) * .5 * width, tx),
          tile((bx[1] + 1) * .5 * width, tx),
          tile((1 - by[1]) * .5 * height, ty),
          tile((1 - by[0]) * .5 * height, ty), true};
}

struct List {
  // Header per tile: absolute word offset, count. Then packed source indices.
  std::vector<std::uint32_t> words;
  std::uint32_t max_count{};
  std::uint32_t
      tiles_over_48{}; // Diagnostic only; never a selection threshold.
};

inline List build(std::span<const inf::city::PointLight> lights,
                  const inf::city::Mat4 &view,
                  const inf::city::Mat4 &projection, std::uint32_t width,
                  std::uint32_t height, float near_clip,
                  std::uint64_t maximum_bytes) {
  if (!width || !height ||
      lights.size() > std::numeric_limits<std::uint32_t>::max())
    throw std::length_error(
        "light list dimensions/count exceed supported range");
  const auto tx = (std::uint64_t(width) + tile_size - 1) / tile_size;
  const auto ty = (std::uint64_t(height) + tile_size - 1) / tile_size;
  const auto tile_count = memory::bytes(tx, ty);
  const auto header_words = memory::bytes(tile_count, 2);
  (void)memory::buffer_size(memory::bytes(header_words, 4), maximum_bytes);
  if (header_words > std::numeric_limits<std::uint32_t>::max())
    throw std::length_error("light list header offsets exceed uint32");
  std::vector<std::uint32_t> counts(static_cast<std::size_t>(tile_count));
  std::vector<Bounds> bounds;
  bounds.reserve(lights.size());
  for (const auto &light : lights) {
    const auto b = sphere_bounds(light.position, light.radius, view, projection,
                                 width, height, near_clip);
    bounds.push_back(b);
    if (b.visible)
      for (auto y = b.y0; y <= b.y1; ++y)
        for (auto x = b.x0; x <= b.x1; ++x)
          ++counts[y * tx + x];
  }
  List result;
  std::uint64_t words = header_words;
  for (const auto count : counts) {
    words += count;
    result.max_count = std::max(result.max_count, count);
    result.tiles_over_48 += count > 48;
    if (words > std::numeric_limits<std::uint32_t>::max())
      throw std::length_error("packed light list offsets exceed uint32");
  }
  (void)memory::buffer_size(memory::bytes(words, 4), maximum_bytes);
  result.words.resize(static_cast<std::size_t>(words));
  std::uint32_t offset = static_cast<std::uint32_t>(header_words);
  for (std::size_t tile = 0; tile < counts.size(); ++tile) {
    result.words[2 * tile] = offset;
    result.words[2 * tile + 1] = counts[tile];
    offset += counts[tile];
    counts[tile] = 0;
  }
  for (std::uint32_t index = 0; index < bounds.size(); ++index) {
    const auto b = bounds[index];
    if (b.visible)
      for (auto y = b.y0; y <= b.y1; ++y)
        for (auto x = b.x0; x <= b.x1; ++x) {
          const auto tile = y * tx + x;
          result.words[result.words[2 * tile] + counts[tile]++] = index;
        }
  }
  return result;
}
} // namespace cb::lighting
