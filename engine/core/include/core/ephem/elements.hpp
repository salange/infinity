#pragma once

#include <cstdint>

#include "core/det/real.hpp"

namespace inf::core {

// Epoch-Zero forever-state payloads (planetary-systems spec section 7).
// All serializable plain data; all lengths/mu already at game scale
// (1:10 rule: lengths /10 AND mu /10 => periods /10, velocities real).

enum class StellarClass : std::uint8_t {
  O = 0, B = 1, A = 2, F = 3, G = 4, K = 5, M = 6,
  WhiteDwarf = 7, NeutronStar = 8, BlackHole = 9,
};

enum class PlanetClass : std::uint8_t {
  Rocky = 0,       // < ~1.8 R_earth, inside the radius valley
  SuperEarth = 1,  // rocky side of the valley, large
  SubNeptune = 2,  // gas-enveloped side of the valley
  IceGiant = 3,
  GasGiant = 4,
};

struct StarPhys {
  // Drawn.
  det::Real mass_solar;      // in solar masses
  det::Real age_gyr;
  det::Real metallicity;     // [Fe/H]-ish, ~N(0, 0.2)
  // Derived (main-sequence relations, DSP-style power laws).
  det::Real luminosity_solar;
  det::Real radius_solar;
  det::Real temperature_k;
  det::Real mu;              // game-scale GM (m^3/s^2, already /10)
  StellarClass cls{StellarClass::G};
};

// Osculating Keplerian elements at Epoch Zero, parent frame.
struct OrbitalElements {
  det::Real a_m;             // semi-major axis, game-scale meters
  det::Real e;               // eccentricity, generation-clamped < 0.95
  det::Real i_rad;           // inclination
  det::Real raan_rad;        // longitude of ascending node
  det::Real argp_rad;        // argument of periapsis
  det::Real mean_anom_0_rad; // mean anomaly at Epoch Zero
  det::Real mu_parent;       // parent's game-scale GM
};

struct SpinState {
  det::Real obliquity_rad;
  det::Real axis_azimuth_rad;
  det::Real spin_rate_rad_s;   // signed (retrograde < 0)
  det::Real spin_phase_0_rad;  // at Epoch Zero
  bool tidally_locked{false};
};

struct AtmosphereBasics {
  det::Real height_m;   // 0 = airless
  det::Real pressure_rel;  // ~1 = Earth-like (cosmetic-ish for now)
};

struct PlanetPhys {
  PlanetClass cls{PlanetClass::Rocky};
  det::Real mass_earth;    // in Earth masses
  det::Real radius_m;      // game-scale (1:10) — ~150-800 km
  det::Real g_surface;     // authored, decoupled from mu (spec section 4)
  det::Real mu;            // game-scale GM for children (moons)
  std::uint32_t surface_type{0};  // game's planet-type id (opaque here)
  AtmosphereBasics atmosphere;
};

}  // namespace inf::core
