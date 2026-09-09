#include "distant_galaxies.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "core/det/mix.hpp"
#include "gen/universe.hpp"
namespace inf::app {
namespace {
sim::Vec3 vector(const gen::Dir3& p) {
  return {p.x.to_double(), p.y.to_double(), p.z.to_double()};
}
}  // namespace
std::vector<DistantGalaxy> distant_galaxies(const core::Seed128& seed) {
  std::vector<DistantGalaxy> result;
  const auto add = [&](const gen::GalaxyParams& p, sim::Vec3 position,
                       std::uint64_t index, float intensity) {
    const auto hash = det::mix64(index + 0x9e3779b97f4a7c15ULL);
    const double az = (hash & 65535U) / 65536.0 * 6.283185307;
    const double z = ((hash >> 16) & 65535U) / 32768.0 - 1.0;
    const double xy = std::sqrt(std::max(0.0, 1.0 - z * z));
    DistantGalaxy g{position,
                    {xy * std::cos(az), xy * std::sin(az), z},
                    p.diameter_ly.to_double() * .5 * gen::kLightYearM,
                    p.ellipticity.to_double(),
                    static_cast<float>(p.type),
                    intensity,
                    static_cast<float>(hash % 8192U),
                    {.78f, .85f, 1.0f}};
    if (p.type == gen::GalaxyType::Elliptical) {
      g.tint[0] = 1;
      g.tint[1] = .84f;
      g.tint[2] = .66f;
    }
    if (p.type == gen::GalaxyType::Irregular) {
      g.tint[0] = .70f;
      g.tint[1] = .82f;
    }
    result.push_back(g);
  };
  const auto cluster = gen::home_cluster_key(seed);
  for (std::uint32_t i = 1; i < gen::galaxy_count_in_cluster(cluster); ++i) {
    const auto key = gen::galaxy_key_in_cluster(seed, 0, 0, 0, i);
    const auto p = gen::derive_galaxy_params(key);
    const auto pos =
        vector(gen::galaxy_position_in_cluster(cluster, i, p.diameter_ly));
    add(p, pos, i, .006f);
    for (std::uint32_t j = 0; j < gen::satellite_count(key); ++j) {
      const auto satellite = gen::satellite_galaxy(key, p, j);
      add(satellite.params, pos + vector(satellite.offset_m),
          i * 8ULL + j + 4096, .005f);
    }
  }
  const auto key = gen::home_galaxy_key(seed);
  for (std::uint32_t i = 0; i < gen::satellite_count(key); ++i) {
    const auto satellite =
        gen::satellite_galaxy(key, gen::home_galaxy_params(seed), i);
    add(satellite.params, vector(satellite.offset_m), i + 60000, .007f);
  }
  return result;
}
void draw_distant_galaxies(const std::vector<DistantGalaxy>& galaxies,
                           sim::Vec3 eye, const render::Mat4& vp,
                           std::uint32_t quad,
                           std::vector<render::Rhi::DrawItem>& items) {
  for (const auto& galaxy : galaxies) {
    const auto relative = galaxy.position - eye;
    const double distance = sim::length(relative);
    if (distance <= 0) continue;
    const auto direction = relative * (1.0 / distance);
    auto major = sim::cross(galaxy.spin, direction);
    if (sim::length(major) < 1e-6)
      major = sim::cross(direction, sim::Vec3{1, .3, .7});
    major = sim::normalize(major);
    const auto minor = sim::cross(direction, major);
    const double flat =
        galaxy.kind == static_cast<float>(gen::GalaxyType::Elliptical)
            ? galaxy.flattening
            : .18 + .82 * std::abs(sim::dot(galaxy.spin, direction));
    const double angle = std::atan(galaxy.radius / distance);
    // Invisible subpixel footprints have a spatially continuous visibility
    // window. There is no top-N ranking that can replace a visible neighbour.
    const double visible = std::clamp(angle / .001, 0.0, 1.0);
    const double scale = std::tan(std::min(angle, .85)) * 1e10;
    auto matrix = render::Mat4::identity();
    const sim::Vec3 axes[3] = {
        major * scale, minor * (scale * std::max(.12, flat)), direction * 1e10};
    matrix.m[0] = static_cast<float>(axes[0].x);
    matrix.m[1] = static_cast<float>(axes[0].y);
    matrix.m[2] = static_cast<float>(axes[0].z);
    matrix.m[4] = static_cast<float>(axes[1].x);
    matrix.m[5] = static_cast<float>(axes[1].y);
    matrix.m[6] = static_cast<float>(axes[1].z);
    matrix.m[12] = static_cast<float>(axes[2].x);
    matrix.m[13] = static_cast<float>(axes[2].y);
    matrix.m[14] = static_cast<float>(axes[2].z);
    const auto mvp = render::mul(vp, matrix);
    render::Rhi::DrawItem item;
    item.mesh = quad;
    item.mode = 10;
    std::memcpy(item.mvp, mvp.m, sizeof(item.mvp));
    std::copy(galaxy.tint, galaxy.tint + 3, item.color);
    item.extra[0] = galaxy.intensity *
                    static_cast<float>(visible * visible * (3 - 2 * visible));
    item.extra[1] = galaxy.kind;
    item.extra[2] = galaxy.phase;
    items.push_back(item);
  }
}
}  // namespace inf::app
