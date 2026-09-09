#pragma once
#include <vector>

#include "core/seed.hpp"
#include "gen/galaxy.hpp"
#include "render/math.hpp"
#include "render/rhi.hpp"
#include "sim/vec3.hpp"
namespace inf::app {
struct DistantGalaxy {
  sim::Vec3 position, spin;
  double radius, flattening;
  float kind, intensity, phase;
  float tint[3];
  // Persistent object fields, not precomputed views along a flight path.
  gen::GalaxyParams params;
  bool local_cluster{false};
  std::uint32_t volume{0};
};
std::vector<DistantGalaxy> distant_galaxies(const core::Seed128& seed);
void prepare_distant_galaxies(std::vector<DistantGalaxy>& galaxies, render::Rhi& rhi);
void draw_distant_galaxies(const std::vector<DistantGalaxy>& galaxies, sim::Vec3 eye,
                           const render::Mat4& view_projection, std::uint32_t quad,
                           std::vector<render::Rhi::DrawItem>& items);
}  // namespace inf::app
