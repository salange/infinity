#include "distant_galaxies.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "deep_sky_render.hpp"
namespace inf::app {
void prepare_distant_galaxies(std::vector<DistantGalaxy>& galaxies, render::Rhi& rhi) {
  std::size_t near_count = 0, far_count = 0;
  for (const auto& galaxy : galaxies) {
    if (!galaxy.local_cluster) continue;
    if (sim::length(galaxy.position) < galaxy.radius * 30)
      ++near_count;
    else
      ++far_count;
  }
  // Include the RHI's companion height texture (10 bytes per voxel total).
  // More populous seeds share a smaller, fixed macro sampling grid rather
  // than allocating gigabytes or switching quality as the observer moves.
  constexpr double budget = 64.0 * 1024 * 1024;
  const double remaining = std::max(0.0, budget - near_count * 96.0 * 96 * 96 * 10);
  const auto macro_size = static_cast<std::uint32_t>(std::clamp(
      std::floor(std::cbrt(remaining / (std::max<std::size_t>(1, far_count) * 10))), 8.0,
      48.0));
  for (auto& galaxy : galaxies) {
    if (!galaxy.local_cluster) continue;
    // Tiny cluster neighbours remain an integrated macro image. All galaxies
    // in the home cluster have their actual density field resident, including
    // the large companions; approaching cannot expose a flat image.
    const double range = sim::length(galaxy.position) / galaxy.radius;
    const auto field =
        build_macro_galaxy_volume(galaxy.params, range < 30 ? 96 : macro_size);
    galaxy.volume = rhi.create_sky_volume(field.size);
    for (std::uint32_t z = 0; z < field.size; ++z)
      rhi.update_sky_slice(galaxy.volume, z,
                           field.rgba_half.data() +
                               static_cast<std::size_t>(z) * field.size * field.size * 4);
  }
}
void draw_distant_galaxies(const std::vector<DistantGalaxy>& galaxies, sim::Vec3 eye,
                           const render::Mat4& vp, std::uint32_t quad,
                           std::vector<render::Rhi::DrawItem>& items) {
  for (const auto& galaxy : galaxies) {
    const auto relative = galaxy.position - eye;
    const double distance = sim::length(relative);
    if (distance <= 0) continue;
    const auto direction = relative * (1.0 / distance);
    auto major = sim::cross(galaxy.spin, direction);
    if (sim::length(major) < 1e-6) major = sim::cross(direction, sim::Vec3{1, .3, .7});
    major = sim::normalize(major);
    const auto minor = sim::cross(direction, major);
    const double flat = galaxy.volume != 0 ? 1.0
                        : galaxy.kind == static_cast<float>(gen::GalaxyType::Elliptical)
                            ? galaxy.flattening
                            : .18 + .82 * std::abs(sim::dot(galaxy.spin, direction));
    // The complete sampled cube fits in this sphere. A view inside it uses a
    // full-screen ray march, retaining near material and parallax.
    const double bound = galaxy.radius * (galaxy.volume != 0 ? std::sqrt(12.0) : 1.0);
    const bool inside = distance <= bound;
    const double angle = std::asin(std::min(1.0, bound / distance));
    // Invisible subpixel footprints have a spatially continuous visibility
    // window. There is no top-N ranking that can replace a visible neighbour.
    const double visible =
        galaxy.volume != 0
            ? 1.0
            : std::clamp(std::atan(galaxy.radius / distance) / .001, 0.0, 1.0);
    const double scale = std::tan(std::min(angle, 1.55)) * 1e10;
    auto matrix = render::Mat4::identity();
    const sim::Vec3 axes[3] = {major * scale, minor * (scale * std::max(.12, flat)),
                               direction * 1e10};
    matrix.m[0] = static_cast<float>(axes[0].x);
    matrix.m[1] = static_cast<float>(axes[0].y);
    matrix.m[2] = static_cast<float>(axes[0].z);
    matrix.m[4] = static_cast<float>(axes[1].x);
    matrix.m[5] = static_cast<float>(axes[1].y);
    matrix.m[6] = static_cast<float>(axes[1].z);
    matrix.m[12] = static_cast<float>(axes[2].x);
    matrix.m[13] = static_cast<float>(axes[2].y);
    matrix.m[14] = static_cast<float>(axes[2].z);
    auto mvp = render::mul(vp, matrix);
    if (inside && galaxy.volume != 0) {
      mvp = render::Mat4::identity();
      mvp.m[10] = 0;
      mvp.m[14] = 2e-22f;
    } else {
      // Conservative clip bounds, including all four corners. Invisible
      // objects never consume the renderer's finite draw-item budget.
      const double ex = std::abs(mvp.m[0]) + std::abs(mvp.m[4]);
      const double ey = std::abs(mvp.m[1]) + std::abs(mvp.m[5]);
      const double ew = std::abs(mvp.m[3]) + std::abs(mvp.m[7]);
      const double w = mvp.m[15] + ew;
      if (w <= 0 || std::abs(mvp.m[12]) > w + ex || std::abs(mvp.m[13]) > w + ey)
        continue;
    }
    render::Rhi::DrawItem item;
    item.mesh = quad;
    item.mode = 10;
    std::memcpy(item.mvp, mvp.m, sizeof(item.mvp));
    std::copy(galaxy.tint, galaxy.tint + 3, item.color);
    item.extra[0] =
        galaxy.intensity * static_cast<float>(visible * visible * (3 - 2 * visible));
    item.extra[1] = galaxy.kind;
    item.extra[2] = galaxy.phase;
    if (galaxy.volume != 0) {
      const auto x = sim::normalize(sim::cross(galaxy.spin, sim::Vec3{1, .3, .7}));
      const auto y = sim::cross(galaxy.spin, x);
      const sim::Vec3 rows[3] = {x, y, galaxy.spin};
      const auto observer = (eye - galaxy.position) * (1.0 / galaxy.radius);
      for (int i = 0; i < 3; ++i) {
        item.volume_rotation[i * 4] = static_cast<float>(rows[i].x);
        item.volume_rotation[i * 4 + 1] = static_cast<float>(rows[i].y);
        item.volume_rotation[i * 4 + 2] = static_cast<float>(rows[i].z);
        item.aux[i] = static_cast<float>(sim::dot(observer, rows[i]));
      }
      item.color[3] = 1;
      item.extra[0] = 1;
      item.planet_texture = galaxy.volume;
    }
    items.push_back(item);
  }
}
}  // namespace inf::app
