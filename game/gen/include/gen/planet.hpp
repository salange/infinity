#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "core/det/real.hpp"
#include "core/ephem/elements.hpp"
#include "core/key.hpp"
#include "gen/names.hpp"

namespace inf::gen {

// The four v0 planet types (prototype-v0 spec section 5). Type changes
// DATA, never pipeline shape.
enum class PlanetType : std::uint8_t {
  EarthLike = 0,
  Barren = 1,
  Desert = 2,
  Ice = 3,
};

const char* to_string(PlanetType type);

// Earth's radius at the global 1:10 scale (6371 km / 10). The reference
// length for every body: radii, masses and surface gravity are all
// expressed relative to it.
inline constexpr double kEarthRadiusGame = 637'100.0;

// Smallest radius that holds a breathable atmosphere. Shared by the
// system layer's surface-type mapping and the standalone draw so both
// paths agree on where EarthLike worlds start.
inline constexpr double kAtmosphereMinRadiusM = 420'000.0;

// SINGLE SOURCE OF TRUTH for body radii (2026-08-31). Every class range
// is the real range divided by ten, per the global 1:10 rule
// (design/planetary-systems.md section 4) — Earth 637 km, Neptune
// 2462 km, Jupiter 6991 km. Both the system layer (planets/v1) and the
// standalone surface-generator draw go through this; there is no second
// radius table.
struct RadiusRange {
  double lo_m, hi_m;
};
RadiusRange radius_range_m(core::PlanetClass cls);

// Mass in Earth masses and surface gravity for a body of this class and
// radius. Mass follows the class's density family; gravity then follows
// from mass and radius the physical way (g = 9.81 * M / r^2 in Earth
// units), which is what decouples it from the /10 game-scale mu.
det::Real mass_earth_for(core::PlanetClass cls, det::Real radius_m);
det::Real surface_gravity(det::Real mass_earth, det::Real radius_m, double jitter = 1.0);

// The class a standalone (system-less) draw uses for a surface type.
// Giants only ever get a surface through the system path, where
// planets/v1 supplies the radius directly.
core::PlanetClass class_for_surface_type(PlanetType type, std::uint64_t word);

// Planet axis convention: the rotation axis is the planet-local +Z axis.
// +Z = north pole, -Z = south pole; east = north x up (right-handed spin).
// HUD/radar cardinal directions and later day/night all derive from this.
inline constexpr double kNorthAxis[3] = {0.0, 0.0, 1.0};

struct PlanetParams {
  PlanetType type{PlanetType::EarthLike};
  det::Real radius_m;         // from radius_range_m() — 1:10 of real
  det::Real core_radius_m;    // impenetrable core (fraction of radius)
  det::Real gravity;          // m/s^2, cosmetic-ish in v0
  det::Real sea_level_m;      // EarthLike only; offset above radius_m, else 0
  det::Real atmosphere_height_m;  // 0 for Barren
  std::uint32_t sky_palette{0};
  std::uint32_t cells_per_face{0};  // province grid resolution N (per face edge)
  std::uint32_t palette_id{0};

  // Serializable inter-stage payload (NMS lesson, T0004): stable,
  // human-readable, byte-reproducible.
  std::string to_json() const;
};

// Derives all planet parameters from K_body via the "planet-params/v1"
// layer. forced_type overrides the seeded type pick (CLI flag) but keeps
// every other draw identical.
PlanetParams derive_planet_params(const core::Key& body_key,
                                  std::optional<PlanetType> forced_type = std::nullopt);

}  // namespace inf::gen
