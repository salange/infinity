#pragma once

#include <memory>
#include <string>
#include <vector>

#include "gen/planet.hpp"
#include "gen/terrain.hpp"
#include "render/rhi.hpp"
#include "sim/player.hpp"

namespace inf::app {

// One body on the system radar (NMS-style space view): position relative
// to the player in planet-local axes; icon color/scale are view
// properties (constant size, never distance-scaled).
struct RadarBody {
  sim::Vec3 rel;      // body center - player position
  float color[3]{1.0f, 1.0f, 1.0f};
  float scale{1.0f};  // 1 = planet; star bigger, moons smaller
  bool anchor{false}; // the body currently anchoring the world frame
};

// Crosshair target readout (flight mode): the planet under/near the
// crosshair, with surface distance and — when actually closing in on
// it — an ETA at the current speed.
struct TargetInfo {
  bool valid{false};
  std::string name;
  double distance_m{0.0};
  double eta_s{-1.0};  // < 0: not closing on the target
};

// In-game HUD (Sascha 2026-08-30):
// - lower left: velocity, plus altitude when near the planet (inside the
//   atmosphere band) or distance-to-body with switched label/units when
//   out in space;
// - lower right: radar. In space an NMS-style SYSTEM view: every body in
//   the planetary system as a constant-size icon projected onto the
//   ship's horizontal plane (up on the radar = ship forward), radial
//   distance log-compressed so the whole system fits, and a vertical bar
//   from each icon's plane point showing above/below the ship plane. In
//   atmosphere a top-down terrain view around the player, rotated so
//   up = heading, with N/S/E/W cardinal letters derived from the planet
//   axis (+Z = north pole). Below the radar: the current biome name.
// Identical in ship and walking mode (rocket backpack later).
class Hud {
 public:
  Hud(render::Rhi* rhi, const gen::TerrainField* field, const gen::PlanetParams& planet);
  ~Hud();

  Hud(const Hud&) = delete;
  Hud& operator=(const Hud&) = delete;

  // Appends the HUD draw items for this frame (called after the 3D scene
  // items so the overlay draws on top). bodies feeds the space radar's
  // system view; ignored while the atmosphere radar is showing.
  // location_name is the current planet's display name, shown under the
  // radar while near/landed on it; target is the crosshair readout
  // (TargetInfo.valid = false hides it).
  void build(std::vector<render::Rhi::DrawItem>* items, const sim::Player& player,
             const std::vector<RadarBody>& bodies, double measured_speed_mps, double aspect,
             int height_px, double dt, const std::string& location_name,
             const TargetInfo& target);

  // Map-mode overlay (design/map-mode.md section 3): the hover info card,
  // anchored at the pointer (NDC), lines top to bottom. The flight/walk
  // HUD is hidden in map mode — the app calls this INSTEAD of build().
  // Implemented on the existing stb_easy_font text path rather than an
  // ImGui panel (decision logged in T0013).
  void build_map_card(std::vector<render::Rhi::DrawItem>* items,
                      const std::vector<std::string>& lines, double x_ndc, double y_ndc,
                      double aspect, int height_px);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace inf::app
