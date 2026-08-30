#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "core/det/real.hpp"
#include "core/key.hpp"

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

struct PlanetParams {
  PlanetType type{PlanetType::EarthLike};
  det::Real radius_m;         // ~40-100 km, type-dependent
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
