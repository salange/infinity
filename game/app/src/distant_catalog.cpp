#include <algorithm>
#include <cmath>

#include "core/det/mix.hpp"
#include "distant_galaxies.hpp"
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
                       std::uint64_t index, float intensity, bool local) {
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
                    {.78f, .85f, 1.0f},
                    p,
                    local,
                    0};
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
  const auto tree = gen::make_tree(seed);
  // The surrounding cluster shell is a spatial macro level, not a route or a
  // timed list of future views. Every direction uses the same keyed objects.
  for (int cz = -1; cz <= 1; ++cz)
    for (int cy = -1; cy <= 1; ++cy)
      for (int cx = -1; cx <= 1; ++cx) {
        const bool local = cx == 0 && cy == 0 && cz == 0;
        const auto address = core::tree::Address{}.child(
            {gen::name::ClustersAxis, core::tree::Cell::grid(cx, cy, cz)});
        const auto cluster = tree->get(address)->key();
        const sim::Vec3 origin{cx * gen::kClusterCellM, cy * gen::kClusterCellM,
                               cz * gen::kClusterCellM};
        const std::uint64_t prefix =
            local ? 0
                  : static_cast<std::uint64_t>((cz + 1) * 9 + (cy + 1) * 3 + cx + 1) *
                        100000;
        for (std::uint32_t i = 0; i < gen::galaxy_count_in_cluster(cluster); ++i) {
          const auto key = tree->get(address.child({gen::name::GalaxiesAxis,
                                                    core::tree::Cell::index(i)}))
                               ->key();
          const auto p = local && i == 0 ? gen::home_galaxy_params(seed)
                                         : gen::derive_galaxy_params(key);
          const auto pos =
              origin + vector(gen::galaxy_position_in_cluster(cluster, i, p.diameter_ly));
          if (!local || i != 0) add(p, pos, prefix + i, .006f, local);
          for (std::uint32_t j = 0; j < gen::satellite_count(key); ++j) {
            const auto satellite = gen::satellite_galaxy(key, p, j);
            add(satellite.params, pos + vector(satellite.offset_m),
                local && i == 0 ? j + 60000 : prefix + i * 8ULL + j + 4096,
                local && i == 0 ? .007f : .005f, local);
          }
        }
      }
  return result;
}
}  // namespace inf::app
