#pragma once
#include <atomic>
#include <memory>

#include "deep_sky_render.hpp"
#include "sim/vec3.hpp"

namespace inf::app {
struct StellarCatalog {
  sim::Vec3 origin;
  std::vector<float> vertices;
  double build_ms{0};
};
// Fixed spatial cells and fixed luminosity bands: observer changes can alter
// visibility, but cannot reseed a star or change its luminosity.
StellarCatalog build_stellar_catalog(
    const gen::GalaxyOctree& galaxy, sim::Vec3 eye, sim::Vec3 velocity,
    const std::atomic<bool>* cancelled = nullptr, double magnitude_limit = 8.3);
class StellarStream {
 public:
  StellarStream(const core::Seed128& seed, const gen::GalaxyParams& galaxy);
  ~StellarStream();
  void request(sim::Vec3 eye, sim::Vec3 velocity, double magnitude_limit = 8.3);
  std::unique_ptr<StellarCatalog> take_ready();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace inf::app
