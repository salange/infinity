#include "gen/galaxy.hpp"

#include <cmath>
#include <cstring>

#include "core/det/mix.hpp"
#include "core/det/trig.hpp"
#include "gen/names.hpp"
#include "world/noise.hpp"

namespace inf::gen {

using det::Real;

namespace {

Real u01(std::uint64_t word) {
  return Real(static_cast<double>(word >> 11U) * 0x1.0p-53);
}

double uniform(std::uint64_t word, double lo, double hi) {
  return lo + (hi - lo) * u01(word).to_double();
}

// sech^2(z / (2h)) via one exponential: 4e/(1+e)^2 with e = exp(-|z|/h).
inline double sech2_half(double abs_z_over_h) {
  const double e = det::fast_exp(Real(-abs_z_over_h)).to_double();
  const double one_plus = 1.0 + e;
  return 4.0 * e / (one_plus * one_plus);
}

// sin/cos of 2*pi*f from the wrapped fraction — short Taylor polynomials,
// ~0.5% worst-case error: plenty for a density modulation, a fraction of
// the cost of the full-range kernels.
inline void wave_sin_cos(double turns, double* sine, double* cosine) {
  const double f = turns - std::floor(turns + 0.5);  // [-0.5, 0.5]
  const double u = f * f;
  *cosine = 1.0 + u * (-19.739208802178716 +
                       u * (64.93939402266829 +
                            u * (-85.45681720669373 +
                                 u * (60.24464137187666 - u * 26.42625678337438))));
  *sine = f * (6.283185307179586 +
               u * (-41.34170224039975 +
                    u * (81.60524927607504 +
                         u * (-76.70585975306136 + u * 42.05869394489765))));
}

}  // namespace

const char* to_string(GalaxyType type) {
  switch (type) {
    case GalaxyType::Spiral: return "Spiral";
    case GalaxyType::Barred: return "Barred";
    case GalaxyType::Elliptical: return "Elliptical";
    case GalaxyType::Lenticular: return "Lenticular";
    case GalaxyType::Irregular: return "Irregular";
  }
  return "?";
}

GalaxyParams derive_galaxy_params(const core::Key& galaxy_entity_key,
                                  std::optional<GalaxyType> forced_type) {
  const core::Key key = core::derive_named(galaxy_entity_key, name::GalaxyParamsV1);
  const auto draw0 = core::draw_point(key, channel::Params, 0, 0, 0);
  const auto draw1 = core::draw_point(key, channel::Params, 1, 0, 0);
  const auto draw2 = core::draw_point(key, channel::Params, 2, 0, 0);
  const auto draw3 = core::draw_point(key, channel::Params, 3, 0, 0);

  GalaxyParams params;
  // Field fractions, spirals modestly overweighted (T0017 §3): unbarred
  // 22, barred 38, elliptical 15, lenticular 10, irregular 15.
  const std::uint32_t roll = static_cast<std::uint32_t>(draw0[0] >> 40U) % 100U;
  if (roll < 22) {
    params.type = GalaxyType::Spiral;
  } else if (roll < 60) {
    params.type = GalaxyType::Barred;
  } else if (roll < 75) {
    params.type = GalaxyType::Elliptical;
  } else if (roll < 85) {
    params.type = GalaxyType::Lenticular;
  } else {
    params.type = GalaxyType::Irregular;
  }
  if (forced_type.has_value()) {
    params.type = *forced_type;
  }

  // Log-uniform diameter, 5 000 - 200 000 ly (Milky Way ~100 000).
  const double diameter =
      5000.0 * det::fast_exp(Real(u01(draw0[1]).to_double() * 3.6888794541139363))
                   .to_double();  // ln(40)
  params.diameter_ly = Real(diameter);
  params.disc_scale_length_ly = Real(diameter / 12.0 * uniform(draw0[2], 0.8, 1.25));
  params.thin_scale_height_ly = Real(diameter / 100.0 * uniform(draw0[3], 0.7, 1.4));
  // Thick disc = integer multiple of the thin height (3x or 4x): the
  // density hot path then derives exp(-z/h_thin) from exp(-z/h_thick) by
  // squaring instead of a second exponential.
  params.thick_scale_height_ly =
      Real(params.thin_scale_height_ly.to_double() * (3.0 + double(draw1[0] & 1U)));
  params.bulge_radius_ly = Real(diameter / 33.0 * uniform(draw1[1], 0.7, 1.5));
  params.dust_scale_height_ly = Real(params.thin_scale_height_ly.to_double() * 0.35);
  params.metallicity_gradient = Real(-uniform(draw1[2], 0.2, 0.5));
  params.total_mass_suns = Real(6.0e10 * (diameter / 1.0e5) * (diameter / 1.0e5) *
                                uniform(draw1[3], 0.5, 2.0));

  // Type drives GEOMETRY (the province-archetype lesson): which terms
  // exist at all, not just how loud they are.
  switch (params.type) {
    case GalaxyType::Spiral:
    case GalaxyType::Barred:
      {
        // Mostly 2-4 arms (grand designs are 2-armed); 5-6 stay rare.
        static constexpr int kArmTable[8] = {2, 2, 2, 3, 3, 4, 5, 6};
        params.arm_count = kArmTable[(draw2[0] >> 33U) % 8U];
      }
      // Open pitches read as galaxies; below ~10 deg the arms wind into
      // ring-like spirographs at our full-disc arm envelope.
      params.pitch_deg = Real(uniform(draw2[1], 11.0, 32.0));
      params.arm_amplitude = Real(uniform(draw2[2], 0.5, 1.1));
      params.bulge_frac = Real(uniform(draw2[3], 0.10, 0.30));
      params.bar_fraction =
          params.type == GalaxyType::Barred ? Real(uniform(draw3[0], 0.15, 0.40)) : Real(0.0);
      params.ellipticity = Real(1.0);
      params.dust_opacity = Real(uniform(draw3[1], 0.6, 1.2));
      params.clumpiness = Real(0.15);
      params.age_gyr = Real(uniform(draw3[2], 4.0, 10.0));
      break;
    case GalaxyType::Elliptical:
      params.arm_count = 0;
      params.pitch_deg = Real(0.0);
      params.arm_amplitude = Real(0.0);  // no arms
      params.bulge_frac = Real(1.0);     // no disc at all
      params.bar_fraction = Real(0.0);
      // E0-E7: axis ratio c/a from 1.0 down to 0.3.
      params.ellipticity = Real(1.0 - 0.7 * u01(draw2[0]).to_double());
      params.dust_opacity = Real(0.02);  // almost no dust
      params.clumpiness = Real(0.0);
      params.age_gyr = Real(uniform(draw3[2], 8.0, 13.0));
      break;
    case GalaxyType::Lenticular:
      params.arm_count = 0;
      params.pitch_deg = Real(0.0);
      params.arm_amplitude = Real(0.0);  // disc without arms
      params.bulge_frac = Real(uniform(draw2[3], 0.30, 0.60));
      params.bar_fraction = Real(0.0);
      params.ellipticity = Real(1.0);
      params.dust_opacity = Real(uniform(draw3[1], 0.05, 0.20));
      params.clumpiness = Real(0.1);
      params.age_gyr = Real(uniform(draw3[2], 7.0, 12.0));
      break;
    case GalaxyType::Irregular:
      params.arm_count = 0;
      params.pitch_deg = Real(0.0);
      params.arm_amplitude = Real(0.0);  // no coherent arms — clumps instead
      params.bulge_frac = Real(uniform(draw2[3], 0.02, 0.10));
      params.bar_fraction = Real(0.0);
      params.ellipticity = Real(1.0);
      params.dust_opacity = Real(uniform(draw3[1], 0.4, 1.0));
      params.clumpiness = Real(uniform(draw2[2], 0.8, 1.6));
      params.age_gyr = Real(uniform(draw3[2], 1.0, 6.0));
      break;
  }
  return params;
}

GalaxyDensity::GalaxyDensity(const GalaxyParams& params) : params_(params) {
  const double ly = kLightYearM;
  const double radius = params.diameter_ly.to_double() * 0.5 * ly;
  radius_m_ = Real(radius);
  const double h_r = params.disc_scale_length_ly.to_double() * ly;
  const double h_thin = params.thin_scale_height_ly.to_double() * ly;
  const double h_thick = params.thick_scale_height_ly.to_double() * ly;
  const double h_dust = params.dust_scale_height_ly.to_double() * ly;
  // Ellipticals use a larger effective core than a spiral bulge.
  const double r_core = (params.type == GalaxyType::Elliptical
                             ? params.diameter_ly.to_double() / 6.0
                             : params.bulge_radius_ly.to_double()) *
                        ly;
  inv_disc_length_ = 1.0 / h_r;
  inv_thin_height_ = 1.0 / h_thin;
  inv_thick_height_ = 1.0 / h_thick;
  thick_ratio_ = static_cast<int>(h_thick / h_thin + 0.5);  // 3 or 4 by construction
  inv_dust_height_ = 1.0 / h_dust;
  inv_bulge_core_ = 1.0 / r_core;
  bulge_r0_ = r_core;

  const double mass = params.total_mass_suns.to_double();
  const double disc_mass = mass * (1.0 - params.bulge_frac.to_double());
  const double bulge_mass = mass * params.bulge_frac.to_double();
  // Thin disc carries 90% of the disc mass, thick 10%.
  // M = 8*pi*rho0*hR^2*hz for rho0 * exp(-R/hR) * sech^2(z/2hz).
  const double eight_pi = 25.132741228718345;
  rho_disc_ = disc_mass * 0.9 / (eight_pi * h_r * h_r * h_thin);
  rho_thick_ = disc_mass * 0.1 / (eight_pi * h_r * h_r * h_thick);
  // Bulge rho = A * (rc/r) * exp(-(r/rc)^2): M = 2*pi*A*rc^3.
  rho_bulge_ = bulge_mass / (6.283185307179586 * r_core * r_core * r_core);
  rho_dust_ = params.dust_opacity.to_double() * rho_disc_ * 0.8;

  if (params.arm_amplitude.to_double() > 0.0 && params.arm_count > 0) {
    Real ps;
    Real pc;
    det::sin_cos(Real(params.pitch_deg.to_double() * det::kPi / 180.0), &ps, &pc);
    arm_b_ = static_cast<double>(params.arm_count) * pc.to_double() / ps.to_double();
    inv_arm_r0_ = 1.0 / h_r;
    arm_amp_ = params.arm_amplitude.to_double();
  }
  if (params.bar_fraction.to_double() > 0.0) {
    bar_len_m_ = params.bar_fraction.to_double() * radius;
    // The bar REDISTRIBUTES ~30% of the bulge mass along its Gaussian
    // (pi^1.5 * a*b*c volume) — an unnormalized amplitude once inflated
    // the integrated galaxy mass by 2x.
    const double volume = 5.568327996831708 * bar_len_m_ * (0.35 * bar_len_m_) *
                          (0.25 * bar_len_m_);
    bar_amp_ = 0.3 * bulge_mass / volume;
  }
  clump_amp_ = params.clumpiness.to_double();
  ellip_inv_c_ = 1.0 / params.ellipticity.to_double();
  const double mass_for_seed = params.total_mass_suns.to_double();
  std::uint64_t mass_bits;
  std::memcpy(&mass_bits, &mass_for_seed, sizeof(mass_bits));
  clump_lattice_ = det::mix64(0x9A1AC5ULL ^ mass_bits);
}

namespace {
inline double tanh_from_exp(double a) {
  // tanh(a) = (1 - e^-2a) / (1 + e^-2a), a >= 0 assumed by callers.
  const double e = det::fast_exp(Real(-2.0 * a)).to_double();
  return (1.0 - e) / (1.0 + e);
}
}  // namespace

// Planar factor of the disc at (x, y): exp radial falloff times the arm
// (or clump) modulation — the z-independent part shared by stars() and
// disc_column_mass().
double GalaxyDensity::disc_plane_factor(double x, double y, double z_for_clumps) const {
  const double radius_plane = std::sqrt(x * x + y * y);
  double factor = det::fast_exp(Real(-radius_plane * inv_disc_length_)).to_double();
  if (arm_amp_ > 0.0 && radius_plane > 1.0) {
    const double inv_r = 1.0 / radius_plane;
    const double cos1 = x * inv_r;
    const double sin1 = y * inv_r;
    double cm = cos1;
    double sm = sin1;
    for (int i = 1; i < params_.arm_count; ++i) {
      const double c_next = cm * cos1 - sm * sin1;
      sm = sm * cos1 + cm * sin1;
      cm = c_next;
    }
    const double ln_r = det::fast_log(Real(radius_plane * inv_arm_r0_)).to_double();
    double sb;
    double cb;
    wave_sin_cos(arm_b_ * ln_r * 0.15915494309189535, &sb, &cb);
    const double phase_cos = cm * cb + sm * sb;
    const double rg = radius_m_.to_double();
    double envelope = (radius_plane - bulge_r0_) / (0.6 * bulge_r0_);
    envelope = envelope < 0.0 ? 0.0 : (envelope > 1.0 ? 1.0 : envelope);
    double outer = (rg - radius_plane) / (0.35 * rg);
    outer = outer < 0.0 ? 0.0 : (outer > 1.0 ? 1.0 : outer);
    factor *= 1.0 + arm_amp_ * envelope * outer * phase_cos;
  }
  if (clump_amp_ >= 0.5) {
    const double freq = 6.0 / radius_m_.to_double();
    const Real noise = world::gradient_noise3(clump_lattice_, Real(x * freq),
                                              Real(y * freq), Real(z_for_clumps * freq));
    double clump_factor = 1.0 + clump_amp_ * noise.to_double();
    if (clump_factor < 0.05) {
      clump_factor = 0.05;
    }
    factor *= clump_factor;
  }
  return factor;
}

det::Real GalaxyDensity::disc_column_mass(det::Real x_m, det::Real y_m, det::Real z0_m,
                                          det::Real z1_m) const {
  if (rho_disc_ <= 0.0) {
    return Real(0.0);
  }
  const double factor = disc_plane_factor(x_m.to_double(), y_m.to_double(), 0.0);
  // Integral of sech^2(z / 2h) over [z0, z1] = 2h (tanh(z1/2h) - tanh(z0/2h)),
  // done via signed tanh built from one exponential each.
  const auto signed_tanh = [](double a) {
    return a < 0.0 ? -tanh_from_exp(-a) : tanh_from_exp(a);
  };
  const double z0 = z0_m.to_double();
  const double z1 = z1_m.to_double();
  const double h_thin = 1.0 / inv_thin_height_;
  const double h_thick = 1.0 / inv_thick_height_;
  const double thin = rho_disc_ * 2.0 * h_thin *
                      (signed_tanh(0.5 * z1 * inv_thin_height_) -
                       signed_tanh(0.5 * z0 * inv_thin_height_));
  const double thick = rho_thick_ * 2.0 * h_thick *
                       (signed_tanh(0.5 * z1 * inv_thick_height_) -
                        signed_tanh(0.5 * z0 * inv_thick_height_));
  return Real(factor * (thin + thick));
}

det::Real GalaxyDensity::spheroid(const Dir3& p_m) const {
  const double x = p_m.x.to_double();
  const double y = p_m.y.to_double();
  const double z = p_m.z.to_double();
  const double abs_z = z < 0.0 ? -z : z;
  const double r2_plane = x * x + y * y;
  double density = 0.0;
  const double zb = abs_z * ellip_inv_c_;
  const double r_bulge_sq = r2_plane + zb * zb;
  const double q_sq = r_bulge_sq * inv_bulge_core_ * inv_bulge_core_;
  if (q_sq < 28.0) {
    // Cored r^-1 profile: floor q so quadrature points at the exact
    // centre stay finite (the singularity is integrable, samples on it
    // are not).
    double q = std::sqrt(q_sq);
    q = q < 0.05 ? 0.05 : q;
    density += rho_bulge_ * (1.0 / q) * det::fast_exp(Real(-q_sq)).to_double();
    density += rho_bulge_ * 2.0e-4 / (q_sq * q * std::sqrt(q) + 1.0);
  }
  if (bar_amp_ > 0.0) {
    const double bx = x / bar_len_m_;
    const double by = y / (0.35 * bar_len_m_);
    const double bz = z / (0.25 * bar_len_m_);
    const double arg = bx * bx + by * by + bz * bz;
    if (arg < 28.0) {
      density += bar_amp_ * det::fast_exp(Real(-arg)).to_double();
    }
  }
  return Real(density);
}

det::Real GalaxyDensity::stars(const Dir3& p_m) const {
  const double x = p_m.x.to_double();
  const double y = p_m.y.to_double();
  const double z = p_m.z.to_double();
  const double r2_plane = x * x + y * y;
  const double abs_z = z < 0.0 ? -z : z;
  double density = 0.0;

  if (rho_disc_ > 0.0) {
    // One exponential feeds both discs: e_thin = e_thick^k (k = 3 or 4,
    // enforced at parameter time); the planar factor (radial falloff,
    // arms, clumps) is shared with disc_column_mass.
    const double e_thick = det::fast_exp(Real(-abs_z * inv_thick_height_)).to_double();
    double e_thin = e_thick * e_thick * e_thick;
    if (thick_ratio_ == 4) {
      e_thin *= e_thick;
    }
    const double thin_plus = 1.0 + e_thin;
    const double thick_plus = 1.0 + e_thick;
    const double vertical = rho_disc_ * 4.0 * e_thin / (thin_plus * thin_plus) +
                            rho_thick_ * 4.0 * e_thick / (thick_plus * thick_plus);
    density += vertical * disc_plane_factor(x, y, z);
  }

  // Bulge / elliptical body (z stretched by the ellipticity axis ratio).
  const double zb = abs_z * ellip_inv_c_;
  const double r_bulge_sq = r2_plane + zb * zb;
  const double q_sq = r_bulge_sq * inv_bulge_core_ * inv_bulge_core_;
  if (q_sq < 28.0) {  // beyond ~5.3 core radii the Gaussian is < 1e-12
    double q = std::sqrt(q_sq);
    q = q < 0.05 ? 0.05 : q;  // cored profile — see spheroid()
    density += rho_bulge_ * (1.0 / q) * det::fast_exp(Real(-q_sq)).to_double();
    // Faint halo (globular placement): ~r^-3.5 with an inner cutoff.
    density += rho_bulge_ * 2.0e-4 / (q_sq * q * std::sqrt(q) + 1.0);
  }

  if (bar_amp_ > 0.0) {
    const double bx = x / bar_len_m_;
    const double by = y / (0.35 * bar_len_m_);
    const double bz = z / (0.25 * bar_len_m_);
    const double arg = bx * bx + by * by + bz * bz;
    if (arg < 28.0) {
      density += bar_amp_ * det::fast_exp(Real(-arg)).to_double();
    }
  }
  return Real(density);
}

det::Real GalaxyDensity::dust(const Dir3& p_m) const {
  if (rho_dust_ <= 0.0) {
    return Real(0.0);
  }
  const double x = p_m.x.to_double();
  const double y = p_m.y.to_double();
  const double z = p_m.z.to_double();
  const double radius_plane = std::sqrt(x * x + y * y);
  const double abs_z = z < 0.0 ? -z : z;
  const double radial = det::fast_exp(Real(-radius_plane * inv_disc_length_)).to_double();
  double dust = rho_dust_ * radial * sech2_half(abs_z * inv_dust_height_);
  if (arm_amp_ > 0.0 && radius_plane > 1.0) {
    // Dust hugs the arms harder than the stars do.
    const double inv_r = 1.0 / radius_plane;
    const double cos1 = x * inv_r;
    const double sin1 = y * inv_r;
    double cm = cos1;
    double sm = sin1;
    for (int i = 1; i < params_.arm_count; ++i) {
      const double c_next = cm * cos1 - sm * sin1;
      sm = sm * cos1 + cm * sin1;
      cm = c_next;
    }
    const double ln_r = det::fast_log(Real(radius_plane * inv_arm_r0_)).to_double();
    double sb;
    double cb;
    wave_sin_cos(arm_b_ * ln_r * 0.15915494309189535, &sb, &cb);
    const double phase_cos = cm * cb + sm * sb;
    double boosted = 1.0 + 1.7 * arm_amp_ * phase_cos;
    if (boosted < 0.02) {
      boosted = 0.02;
    }
    dust *= boosted;
  }
  return Real(dust);
}

ColorTemp GalaxyDensity::population(const Dir3& p_m) const {
  const double x = p_m.x.to_double();
  const double y = p_m.y.to_double();
  const double z = p_m.z.to_double();
  const double radius_plane = std::sqrt(x * x + y * y);
  const double abs_z = z < 0.0 ? -z : z;
  const double q = (std::sqrt(radius_plane * radius_plane + abs_z * abs_z) + 1.0e6) *
                   inv_bulge_core_;
  // Old red population in the bulge, younger and bluer outward.
  const double bulge_weight = det::fast_exp(Real(-0.5 * q * q)).to_double();
  double disc_frac = radius_plane / radius_m_.to_double();
  disc_frac = disc_frac > 1.0 ? 1.0 : disc_frac;
  const double disc_temp = 5200.0 + 1400.0 * disc_frac;
  ColorTemp out;
  out.temperature_k = Real(4300.0 * bulge_weight + disc_temp * (1.0 - bulge_weight));
  double metallicity = 0.3 + params_.metallicity_gradient.to_double() *
                                 (radius_plane * inv_disc_length_);
  if (metallicity < -1.5) {
    metallicity = -1.5;
  }
  out.metallicity = Real(metallicity);
  return out;
}

}  // namespace inf::gen
