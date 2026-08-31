#pragma once

#include <cstdint>

#include "core/key.hpp"
#include "gen/geo.hpp"

namespace inf::gen {

// T0017: the galaxy level. Three pieces share one file because they share
// one contract — galaxy-params/v1 (WP1) parameterizes the ANALYTIC
// density model (WP2), and the density model is the single source of
// truth for BOTH generation (the octree's per-cell star counts integrate
// it, T0017 WP3) and rendering (the Milky Way band and dust lanes are
// line integrals of it, T0018). There is deliberately no second density
// model anywhere.
//
// Scale (design/scales-and-distances.md §9, decided 2026-09-01):
// interstellar space is 1:10 with no extra compression.
inline constexpr double kLightYearM = 9.461e14;  // 1 game light-year

// --- galaxy-params/v1 (WP1) ----------------------------------------------

// Hubble-class morphology. The type drives GEOMETRY, not amplitude: an
// elliptical has no disc, no arms and almost no dust; a lenticular keeps
// the disc but loses the arms; an irregular is a lumpy disc without
// coherent arms.
enum class GalaxyType : std::uint8_t {
  Spiral = 0,      // unbarred spiral
  Barred = 1,      // barred spiral (about two thirds of spirals)
  Elliptical = 2,  // E0-E7, ellipticity drawn separately
  Lenticular = 3,  // S0: disc + bulge, no arms, little dust
  Irregular = 4,   // lumpy disc, no coherent structure
};

const char* to_string(GalaxyType type);

struct GalaxyParams {
  GalaxyType type{GalaxyType::Spiral};
  det::Real diameter_ly;            // 5 000 - 200 000
  int arm_count{0};                 // 2-6 for spirals, 0 otherwise
  det::Real pitch_deg;              // 5-30, spiral tightness
  det::Real arm_amplitude;          // arm density contrast (0 = no arms)
  det::Real bar_fraction;           // bar half-length / disc radius (0 = none)
  det::Real disc_scale_length_ly;   // ~diameter/12
  det::Real thin_scale_height_ly;
  det::Real thick_scale_height_ly;
  det::Real bulge_radius_ly;
  det::Real bulge_frac;             // stellar-mass fraction in the bulge
  det::Real ellipticity;            // c/a axis ratio (ellipticals; 1 = round)
  det::Real dust_scale_height_ly;   // thinner than the stars: the dark rift
  det::Real dust_opacity;           // overall extinction strength
  det::Real clumpiness;             // irregulars: lumpy density modulation
  det::Real metallicity_gradient;   // dex per scale length, negative outward
  det::Real age_gyr;
  det::Real total_mass_suns;        // stellar mass
};

// One draw per galaxy from derive_named(K_galaxy, "galaxy-params/v1").
GalaxyParams derive_galaxy_params(const core::Key& galaxy_entity_key);

// --- the shared density model (WP2) --------------------------------------

// Mean stellar population at a point (radial gradient).
struct ColorTemp {
  det::Real temperature_k;
  det::Real metallicity;  // [Fe/H]-ish dex, 0 = solar
};

// Pure, analytic, pointwise, cheap (target <= 20 ns per stars() call —
// the sky bake evaluates it ~1e8 times). Positions are galactocentric
// game metres; densities are solar masses per m^3. det ops throughout
// (fast_exp/fast_log/fast_sin_cos are bit-deterministic).
class GalaxyDensity {
 public:
  explicit GalaxyDensity(const GalaxyParams& params);

  // Stellar mass density.
  det::Real stars(const Dir3& p_m) const;
  // Dust density — separate field: thinner scale height, hugs the arms
  // harder. Line-integrate for extinction.
  det::Real dust(const Dir3& p_m) const;
  ColorTemp population(const Dir3& p_m) const;

  const GalaxyParams& params() const { return params_; }
  det::Real radius_m() const { return radius_m_; }

 private:
  GalaxyParams params_;
  det::Real radius_m_;
  // Precomputed reciprocals/amplitudes (all metres / per-metre).
  double inv_disc_length_{0.0};
  double inv_thin_height_{0.0};
  double inv_thick_height_{0.0};
  double inv_dust_height_{0.0};
  double inv_bulge_core_{0.0};
  double bulge_r0_{0.0};
  double rho_disc_{0.0};   // Msun/m^3 at centre, thin disc
  double rho_thick_{0.0};
  double rho_bulge_{0.0};
  double rho_dust_{0.0};
  double arm_b_{0.0};      // m / tan(pitch): phase = m*phi - b*ln(R/R0)
  double inv_arm_r0_{0.0};
  double arm_amp_{0.0};
  double bar_len_m_{0.0};
  double bar_amp_{0.0};
  double clump_amp_{0.0};
  double ellip_inv_c_{1.0};  // 1/(c/a) applied to z for ellipticals
  int thick_ratio_{3};       // h_thick / h_thin, integer by construction
  std::uint64_t clump_lattice_{0};
};

}  // namespace inf::gen
