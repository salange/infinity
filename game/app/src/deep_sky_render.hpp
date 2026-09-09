#pragma once
#include <cstdint>
#include <vector>

#include "core/seed.hpp"
#include "gen/deep_sky.hpp"
#include "gen/galaxy_octree.hpp"

namespace inf::app {
void stellar_tint(double temperature, float rgb[3]);
struct GalaxyVolume {
  std::uint32_t size{0};
  double radius_m{0};
  std::vector<std::uint16_t> rgba_half;
};
// Disposable spatial sampling of the shared density, nebula and cluster fields.
// No observer, route, speed or future-view dependency. Rebuilt from the seed.
GalaxyVolume build_galaxy_volume(const core::Seed128& seed,
                                 const gen::GalaxyParams& galaxy,
                                 std::uint32_t size = 192);
}  // namespace inf::app
