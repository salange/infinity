#include "gen/climate.hpp"

#include <algorithm>

#include "core/det/mix.hpp"
#include "core/det/trig.hpp"
#include "gen/names.hpp"
#include "world/noise.hpp"

namespace inf::gen {

namespace {

double clamp01(double x) { return x < 0.0 ? 0.0 : (x > 1.0 ? 1.0 : x); }

double smooth(double x, double lo, double hi) {
  const double t = clamp01((x - lo) / (hi - lo));
  return t * t * (3.0 - 2.0 * t);
}

// Compact bump: 1 at x = 0, 0 at |x| >= 1, C1.
double bump(double x) {
  const double a = 1.0 - x * x;
  return a > 0.0 ? a * a : 0.0;
}

double noise_at(std::uint64_t lattice, const Dir3& dir, double frequency) {
  return world::gradient_noise3(lattice, det::Real(dir.x.to_double() * frequency),
                                det::Real(dir.y.to_double() * frequency),
                                det::Real(dir.z.to_double() * frequency))
      .to_double();
}

}  // namespace

ClimateField::ClimateField(const core::Key& body_entity_key, const PlanetParams& planet,
                           const MacroField& macro)
    : type_(planet.type),
      radius_m_(planet.radius_m.to_double()),
      sea_level_m_(planet.sea_level_m.to_double()),
      land_fraction_(planet.land_fraction.to_double()),
      flux_rel_(planet.flux_rel.to_double()),
      pressure_rel_(planet.pressure_rel.to_double()),
      obliquity_cos_(det::cos(planet.obliquity_rad).to_double()),
      tidally_locked_(planet.tidally_locked),
      has_sea_(planet.land_fraction.to_double() < 0.999) {
  const core::Key climate_key = core::derive_named(body_entity_key, name::ClimateV1);
  lattice_a_ = core::lattice_key(climate_key, channel::Lattice);
  lattice_b_ = det::mix64(lattice_a_ ^ 0x5C11A7E0C1B0A7ULL);

  // Equilibrium temperature (Earth: 255 K at flux 1) plus a greenhouse
  // term by type and pressure (Earth's 33 K at pressure 1).
  const double flux_q = det::sqrt(det::sqrt(det::Real(flux_rel_ < 0.0 ? 0.0 : flux_rel_)))
                            .to_double();
  const double pressure_q = det::sqrt(det::Real(pressure_rel_ < 0.0 ? 0.0 : pressure_rel_))
                                .to_double();
  double greenhouse = 0.0;
  switch (type_) {
    case PlanetType::EarthLike: greenhouse = 33.0 * pressure_q; break;
    case PlanetType::Desert: greenhouse = 25.0 * pressure_q; break;
    case PlanetType::Ice: greenhouse = 25.0 * pressure_q; break;
    case PlanetType::Barren: greenhouse = 0.0; break;
  }
  base_temperature_k_ = 255.0 * flux_q + greenhouse;
  sea_level_temperature_k_ = base_temperature_k_;

  // Percentile axes: 6 x 12 x 12 directions, macro relief only (the
  // province blend would cost 40x more and adds nothing at this scale).
  constexpr int kN = 12;
  const double macro_amp = planet.macro_amplitude_m.to_double();
  sorted_temp_.reserve(6 * kN * kN);
  sorted_hum_.reserve(6 * kN * kN);
  double sum_t = 0.0;
  double sum_h = 0.0;
  for (std::uint8_t face = 0; face < 6; ++face) {
    for (int j = 0; j < kN; ++j) {
      for (int i = 0; i < kN; ++i) {
        const det::Real u((i + 0.5) / kN * 2.0 - 1.0);
        const det::Real v((j + 0.5) / kN * 2.0 - 1.0);
        const Dir3 dir = face_uv_to_dir(FaceUV{face, u, v});
        const double elevation = macro.value(dir).to_double() * macro_amp;
        double t;
        double h;
        double ins;
        raw(dir, elevation > 0.0 ? elevation : 0.0, elevation - sea_level_m_, &t, &h, &ins);
        sorted_temp_.push_back(t);
        sorted_hum_.push_back(h);
        sum_t += t;
        sum_h += h;
      }
    }
  }
  std::sort(sorted_temp_.begin(), sorted_temp_.end());
  std::sort(sorted_hum_.begin(), sorted_hum_.end());
  mean_temperature_k_ = sum_t / static_cast<double>(sorted_temp_.size());
  mean_humidity_ = sum_h / static_cast<double>(sorted_hum_.size());
}

double ClimateField::rank(const std::vector<double>& sorted, double value) {
  const auto it = std::lower_bound(sorted.begin(), sorted.end(), value);
  const auto index = static_cast<double>(it - sorted.begin());
  const auto n = static_cast<double>(sorted.size());
  if (it == sorted.end()) {
    return 1.0;
  }
  if (it == sorted.begin()) {
    return 0.0;
  }
  // Linear interpolation between the bracketing samples for a smooth axis.
  const double lo = *(it - 1);
  const double hi = *it;
  const double f = hi > lo ? (value - lo) / (hi - lo) : 0.0;
  return (index - 1.0 + f) / (n - 1.0);
}

void ClimateField::raw(const Dir3& unit_dir, double altitude_m, double height_above_sea_m,
                       double* temperature_k, double* humidity, double* insolation) const {
  const double z = unit_dir.z.to_double();  // sin(latitude), +Z = north
  const double x = unit_dir.x.to_double();  // substellar meridian when locked
  const bool atmosphere = pressure_rel_ > 0.0;
  // Heat transport damping of the latitude spread: thick air evens out.
  const double transport = 1.0 / (1.0 + 0.6 * pressure_rel_);

  // --- insolation ---------------------------------------------------------
  double ins_raw;
  double ins_avg;
  if (tidally_locked_) {
    ins_raw = x > 0.0 ? x : 0.0;
    ins_avg = 0.25;  // mean of max(0, cos) over the sphere
  } else {
    // Annual mean ~ 1 - c(obliquity) * P2(sin lat); c = 0.482 at 23.5 deg,
    // zero at 54.7 deg, negative (poles warmer) beyond (North 1975).
    const double p2_obl = (3.0 * obliquity_cos_ * obliquity_cos_ - 1.0) * 0.5;
    const double c = 0.633 * p2_obl;
    const double p2_lat = (3.0 * z * z - 1.0) * 0.5;
    ins_raw = 1.0 - c * p2_lat;
    ins_avg = 1.0;
  }
  *insolation = clamp01(ins_raw / (tidally_locked_ ? 1.0 : 1.25));

  // --- temperature --------------------------------------------------------
  const double spread = (tidally_locked_ ? 160.0 : 80.0) * transport;
  double t = base_temperature_k_ + spread * (ins_raw - ins_avg);
  // Lapse: 8 K per 1000 m (game metres) above the datum the air rests
  // on — the sea on wet worlds, the nominal radius on dry ones. Measured
  // from the nominal radius, every continent on a deep-ocean world read
  // as a frozen plateau.
  const double above = has_sea_ ? height_above_sea_m : altitude_m;
  if (above > 0.0) {
    t -= above * 0.008;
  }
  t += 7.0 * noise_at(lattice_a_, unit_dir, 2.5) + 3.0 * noise_at(lattice_a_, unit_dir, 7.0);
  if (t < 3.0) {
    t = 3.0;
  }
  *temperature_k = t;

  // --- humidity -----------------------------------------------------------
  double water = 0.0;
  double base;
  switch (type_) {
    case PlanetType::EarthLike:
      water = 1.0 - land_fraction_;
      base = 0.12 + 0.80 * water;
      break;
    case PlanetType::Ice:
      water = land_fraction_ < 0.999 ? 0.5 * (1.0 - land_fraction_) : 0.0;
      base = 0.15 + 0.40 * water;
      break;
    case PlanetType::Desert:
    case PlanetType::Barren:
    default:
      base = 0.04 + 0.03 * (1.0 + noise_at(lattice_b_, unit_dir, 3.0));
      break;
  }
  double h = base;
  if (atmosphere) {
    if (tidally_locked_) {
      // Convection under the substellar point, dry night side.
      h *= 0.45 + 0.75 * bump((x - 0.25) / 0.6);
    } else {
      // Hadley cells: wet equator, dry subtropics (|sin lat| ~ 0.45), a
      // second wet band at the polar front (~0.85).
      const double az = z < 0.0 ? -z : z;
      h *= 1.0 + 0.25 * bump(az / 0.20) - 0.50 * bump((az - 0.45) / 0.15) +
           0.35 * bump((az - 0.85) / 0.12);
    }
  }
  if (water > 0.0) {
    // Coastal lowlands stay moist; plateaus dry out (400 m game = 4 km).
    const double above = height_above_sea_m > 0.0 ? height_above_sea_m : 0.0;
    const double coast = 1.0 / (1.0 + above / 400.0);
    h *= 0.45 + 0.55 * coast;
  }
  // Province-scale rain shadow stand-in: directional-ish keyed noise.
  h *= 0.75 + 0.35 * noise_at(lattice_b_, unit_dir, 12.0);
  // Cold air holds little water.
  h *= 0.30 + 0.70 * smooth(t, 245.0, 285.0);
  *humidity = clamp01(h);
}

Climate ClimateField::sample(const Dir3& unit_dir, double altitude_m,
                             double height_above_sea_m) const {
  Climate out;
  raw(unit_dir, altitude_m, height_above_sea_m, &out.temperature_k, &out.humidity,
      &out.insolation);
  out.frozen = out.temperature_k < 273.15;
  out.biotemp_c = out.temperature_k > 273.15 ? out.temperature_k - 273.15 : 0.0;
  out.t01 = rank(sorted_temp_, out.temperature_k);
  out.h01 = rank(sorted_hum_, out.humidity);
  return out;
}

}  // namespace inf::gen
