#pragma once

#include <cstdint>
#include <vector>

#include "core/det/real.hpp"
#include "core/key.hpp"
#include "gen/geo.hpp"
#include "gen/macro.hpp"
#include "gen/planet.hpp"

namespace inf::gen {

// climate/v1 (T0019, design/surface-texturing.md section 2.2): a pure
// pointwise climate model on Holdridge's axes. Inputs are all on
// PlanetParams (stellar flux, star temperature, obliquity, tidal lock,
// pressure) plus latitude, altitude and the height above the sea datum;
// randomness is one continent-scale keyed noise. No libm: deterministic
// trig from core/det, polynomial bumps elsewhere.
//
// Every sample also carries the planet's OWN quantile axes (t01, h01):
// the constructor samples 864 fixed directions (macro relief only) and
// ranks; a cold world therefore still spends its full biome range
// (WorldEngine's percentile trick) instead of collapsing to one zone.
struct Climate {
  double temperature_k{288.0};  // annual mean at the point
  double biotemp_c{15.0};       // Holdridge biotemperature: 0 below freezing
  double humidity{0.5};         // 0..1 precipitation proxy
  double insolation{1.0};       // annual-mean insolation, 0..1 of the max
  double t01{0.5};              // rank of biotemp among this planet's samples
  double h01{0.5};              // rank of humidity among this planet's samples
  bool frozen{false};           // temperature_k below 273.15
};

class ClimateField {
 public:
  ClimateField(const core::Key& body_entity_key, const PlanetParams& planet,
               const MacroField& macro);

  // altitude_m: elevation above the NOMINAL radius (the lapse rate input);
  // height_above_sea_m: elevation minus the sea datum (coastal moisture).
  Climate sample(const Dir3& unit_dir, double altitude_m, double height_above_sea_m) const;

  double mean_temperature_k() const { return mean_temperature_k_; }
  double mean_humidity() const { return mean_humidity_; }
  double sea_level_temperature_k() const { return sea_level_temperature_k_; }
  bool has_atmosphere() const { return pressure_rel_ > 0.0; }

 private:
  void raw(const Dir3& unit_dir, double altitude_m, double height_above_sea_m,
           double* temperature_k, double* humidity, double* insolation) const;
  static double rank(const std::vector<double>& sorted, double value);

  PlanetType type_;
  double radius_m_;
  double sea_level_m_;
  double land_fraction_;
  double flux_rel_;
  double pressure_rel_;
  double obliquity_cos_;
  bool tidally_locked_;
  bool has_sea_;               // lapse counts from the sea datum, not the nominal radius
  double base_temperature_k_;  // global mean before the local terms
  double sea_level_temperature_k_;
  double mean_temperature_k_;
  double mean_humidity_;
  std::uint64_t lattice_a_;
  std::uint64_t lattice_b_;
  std::vector<double> sorted_temp_;
  std::vector<double> sorted_hum_;
};

}  // namespace inf::gen
