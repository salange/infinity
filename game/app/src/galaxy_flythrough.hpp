#pragma once
#include "core/seed.hpp"
#include "gen/galaxy.hpp"
#include <algorithm>
namespace inf::render {
class Rhi;
}
#include "sim/player.hpp"
struct GLFWwindow;
namespace inf::app {
class Hud;
// Camera positions are galactocentric game metres. No world-scale mutation.
struct GalaxyRoute {
  static constexpr double duration_s = 120.0;
  sim::Vec3 axis = sim::normalize(sim::Vec3{1.0, 0.35, 0.12});
  double extent_m;
  explicit GalaxyRoute(const gen::GalaxyParams &galaxy)
      : extent_m(1.1 * galaxy.diameter_ly.to_double() * gen::kLightYearM *
                 0.5) {}
  sim::Vec3 position(double seconds) const {
    return axis * (extent_m *
                   (2.0 * std::clamp(seconds / duration_s, 0.0, 1.0) - 1.0));
  }
  double speed_mps() const { return 2.0 * extent_m / duration_s; }
};
void run_galaxy_flythrough(GLFWwindow *window, render::Rhi &rhi, Hud &hud,
                           std::uint32_t quad, const core::Seed128 &seed,
                           const gen::GalaxyParams &galaxy, int &width,
                           int &height, const char *profile_path,
                           const char *capture_prefix = nullptr);
} // namespace inf::app
