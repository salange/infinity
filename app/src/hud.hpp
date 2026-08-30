#pragma once

#include <memory>
#include <string>
#include <vector>

#include "gen/planet.hpp"
#include "gen/terrain.hpp"
#include "render/rhi.hpp"
#include "sim/player.hpp"

namespace inf::app {

// In-game HUD (Sascha 2026-08-30):
// - lower left: velocity, plus altitude when near the planet (inside the
//   atmosphere band) or distance-to-body with switched label/units when
//   out in space;
// - lower right: radar. In space an NMS-style body view (planet disc
//   positioned in the ship frame, N/S pole markers). In atmosphere a
//   top-down terrain view around the player, rotated so up = heading,
//   with N/S/E/W cardinal letters derived from the planet axis
//   (+Z = north pole). Below the radar: the current biome name.
// Identical in ship and walking mode (rocket backpack later).
class Hud {
 public:
  Hud(render::Rhi* rhi, const gen::TerrainField* field, const gen::PlanetParams& planet);
  ~Hud();

  Hud(const Hud&) = delete;
  Hud& operator=(const Hud&) = delete;

  // Appends the HUD draw items for this frame (called after the 3D scene
  // items so the overlay draws on top).
  void build(std::vector<render::Rhi::DrawItem>* items, const sim::Player& player,
             double measured_speed_mps, double aspect, int height_px, double dt);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace inf::app
