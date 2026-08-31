#include "gen/material.hpp"

#include <cmath>

namespace inf::gen {

namespace {

// Smooth 0..1 ramp between lo and hi (plain doubles — classification is
// render-side cosmetics; the deterministic contract lives in the field
// itself, which this never feeds back into).
double ramp(double x, double lo, double hi) {
  const double t = (x - lo) / (hi - lo);
  const double c = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
  return c * c * (3.0 - 2.0 * c);
}

}  // namespace

MaterialField::MaterialField(const core::Key& body_entity_key, const PlanetParams& planet)
    : planet_(planet) {
  const core::Key material_key = core::derive_named(body_entity_key, name::MaterialV1);
  climate_lattice_ = core::lattice_key(material_key, channel::Lattice);
  sea_datum_m_ = planet.radius_m.to_double() + planet.sea_level_m.to_double();
  radius_m_ = planet.radius_m.to_double();
  inv_radius_ = 1.0 / planet.radius_m.to_double();
}

VertexMaterial MaterialField::classify(double px, double py, double pz, double nx,
                                       double ny, double nz) const {
  const double r = std::sqrt(px * px + py * py + pz * pz);
  if (r < 1.0) {
    return VertexMaterial{};
  }
  const double inv_r = 1.0 / r;
  const double height = r - sea_datum_m_;   // above the sea datum (shorelines)
  // Altitude for the temperature lapse is measured from the NOMINAL
  // radius: on dry worlds the sea datum is a below-minimum sentinel and
  // would freeze the whole planet.
  const double altitude = r - radius_m_;
  const double latitude = std::abs(pz) * inv_r;   // 0 equator .. 1 pole
  // Slope: 1 - dot(normal, radial); ~0 flat, ~0.3 at 45 deg, ~1 cliff.
  const double slope = 1.0 - (nx * px + ny * py + nz * pz) * inv_r;

  // Continent-scale climate wobble (~2 cells across the planet).
  const double f = 2.5 * inv_radius_;
  const double wobble =
      world::gradient_noise3(climate_lattice_, det::Real(px * f), det::Real(py * f),
                             det::Real(pz * f))
          .to_double();

  // Temperature in [0,1]-ish: type base, colder poleward and with
  // altitude, plus the wobble.
  double base_temp;
  switch (planet_.type) {
    case PlanetType::EarthLike: base_temp = 0.78; break;
    case PlanetType::Desert: base_temp = 0.98; break;
    case PlanetType::Ice: base_temp = 0.18; break;
    case PlanetType::Barren: base_temp = 0.55; break;
    default: base_temp = 0.6; break;
  }
  const double temp = base_temp - 1.35 * latitude * latitude -
                      (altitude > 0.0 ? altitude * 0.00016 : 0.0) + wobble * 0.14;

  // Base material by type/height/temperature.
  Material base;
  double snow_w = ramp(temp, 0.34, 0.22);  // 0 warm .. 1 frozen
  switch (planet_.type) {
    case PlanetType::EarthLike:
      if (height < 0.0) {
        base = Material::Seabed;
        snow_w = 0.0;  // no snow under water
      } else if (height < 45.0) {
        base = Material::Sand;  // shore band
      } else {
        base = Material::Grass;
      }
      break;
    case PlanetType::Desert:
      base = wobble < -0.35 ? Material::Regolith : Material::Sand;
      break;
    case PlanetType::Ice:
      base = Material::IceSheet;
      snow_w = ramp(temp, 0.16, 0.06);
      break;
    case PlanetType::Barren:
    default:
      base = wobble > 0.4 ? Material::Scree : Material::Regolith;
      snow_w = 0.0;  // airless: no frost caps in v1
      break;
  }

  // Slope override wins: steep faces read as bare rock everywhere.
  const double rock_w = ramp(slope, 0.10, 0.28);
  VertexMaterial out;
  if (rock_w > 0.02) {
    out.mat0 = snow_w > 0.5 ? Material::Snow : base;
    out.mat1 = Material::Rock;
    out.blend = static_cast<float>(rock_w);
    return out;
  }
  if (snow_w > 0.02) {
    out.mat0 = base;
    out.mat1 = Material::Snow;
    out.blend = static_cast<float>(snow_w);
    return out;
  }
  out.mat0 = base;
  out.mat1 = base;
  out.blend = 0.0f;
  return out;
}

}  // namespace inf::gen
