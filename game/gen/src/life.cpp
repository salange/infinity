#include "gen/life.hpp"

#include <cstdio>

#include "gen/names.hpp"

namespace inf::gen {

namespace {

double clamp01(double x) { return x < 0.0 ? 0.0 : (x > 1.0 ? 1.0 : x); }

double smooth(double x, double lo, double hi) {
  const double t = clamp01((x - lo) / (hi - lo));
  return t * t * (3.0 - 2.0 * t);
}

double bump(double x) {
  const double a = 1.0 - x * x;
  return a > 0.0 ? a * a : 0.0;
}

double u01(std::uint64_t word) {
  return static_cast<double>(word >> 11U) * 0x1.0p-53;
}

void set3(float out[3], double r, double g, double b) {
  out[0] = static_cast<float>(r);
  out[1] = static_cast<float>(g);
  out[2] = static_cast<float>(b);
}

// Carbon photosynthetic cover colour by host star temperature (Kiang et
// al. 2007 II): pigments peak in the blue around F stars (reflect
// yellow-orange), green around G, red-orange around K, and absorb the
// whole visible band around M (near-black with a purple cast).
void carbon_pigment(double star_t, float out[3]) {
  struct Stop {
    double t;
    double r, g, b;
  };
  static constexpr Stop kStops[] = {
      {3000.0, 0.09, 0.06, 0.11},
      {3700.0, 0.26, 0.11, 0.10},
      {4500.0, 0.42, 0.24, 0.12},
      {5300.0, 0.24, 0.44, 0.16},
      {6000.0, 0.34, 0.52, 0.19},
      {7000.0, 0.72, 0.60, 0.26},
      {8000.0, 0.82, 0.70, 0.34},
  };
  constexpr int kCount = static_cast<int>(sizeof(kStops) / sizeof(kStops[0]));
  if (star_t <= kStops[0].t) {
    set3(out, kStops[0].r, kStops[0].g, kStops[0].b);
    return;
  }
  for (int i = 1; i < kCount; ++i) {
    if (star_t <= kStops[i].t) {
      const double f = (star_t - kStops[i - 1].t) / (kStops[i].t - kStops[i - 1].t);
      set3(out, kStops[i - 1].r + (kStops[i].r - kStops[i - 1].r) * f,
           kStops[i - 1].g + (kStops[i].g - kStops[i - 1].g) * f,
           kStops[i - 1].b + (kStops[i].b - kStops[i - 1].b) * f);
      return;
    }
  }
  set3(out, kStops[kCount - 1].r, kStops[kCount - 1].g, kStops[kCount - 1].b);
}

LifeStage carbon_stage_for(double age_gyr, int jitter) {
  // Thresholds tuned so the occupied-world histogram lands near the
  // accepted weights (microbial ~25 %, crusts ~15 %, full ~20 %,
  // senescent ~5 % of habitable worlds) under a 0.5-9 Gyr age draw.
  int stage;
  if (age_gyr < 1.2) {
    stage = 1;
  } else if (age_gyr < 2.6) {
    stage = 2;
  } else if (age_gyr < 3.8) {
    stage = 3;
  } else if (age_gyr < 5.2) {
    stage = 4;
  } else if (age_gyr < 8.3) {
    stage = 5;
  } else {
    stage = 6;
  }
  stage += jitter;
  if (stage < 1) stage = 1;
  if (stage > 6) stage = 6;
  return static_cast<LifeStage>(stage);
}

// Exotic chemistries skip Earth's oxygen story: mats, crusts, or full.
LifeStage exotic_stage_for(double age_gyr, int jitter) {
  const auto carbon = static_cast<int>(carbon_stage_for(age_gyr, jitter));
  if (carbon <= 2) return LifeStage::MicrobialMats;
  if (carbon <= 4) return LifeStage::CrustColonisation;
  return LifeStage::FullBiosphere;
}

}  // namespace

const char* to_string(LifeChemistry chemistry) {
  switch (chemistry) {
    case LifeChemistry::None: return "None";
    case LifeChemistry::CarbonWater: return "CarbonWater";
    case LifeChemistry::Crystalline: return "Crystalline";
    case LifeChemistry::Ammonia: return "Ammonia";
    case LifeChemistry::Sulfur: return "Sulfur";
  }
  return "?";
}

const char* to_string(LifeStage stage) {
  switch (stage) {
    case LifeStage::Sterile: return "Sterile";
    case LifeStage::PrebioticHaze: return "PrebioticHaze";
    case LifeStage::MicrobialMats: return "MicrobialMats";
    case LifeStage::Oxygenation: return "Oxygenation";
    case LifeStage::CrustColonisation: return "CrustColonisation";
    case LifeStage::FullBiosphere: return "FullBiosphere";
    case LifeStage::Senescent: return "Senescent";
  }
  return "?";
}

LifeParams derive_life(const core::Key& body_entity_key, const PlanetParams& planet,
                       const ClimateField& climate) {
  LifeParams life;
  const core::Key life_key = core::derive_named(body_entity_key, name::LifeV1);
  const auto d0 = core::draw_point(life_key, channel::Params, 0, 0, 0);
  const auto d1 = core::draw_point(life_key, channel::Params, 1, 0, 0);
  life.variant = static_cast<std::uint32_t>(d1[3] >> 40U);

  const double mean_t = climate.mean_temperature_k();
  const double flux = planet.flux_rel.to_double();
  const double age = planet.star_age_gyr.to_double();
  const bool has_sea = planet.land_fraction.to_double() < 0.999;

  // --- habitability: computed, never drawn --------------------------------
  LifeChemistry candidate = LifeChemistry::None;
  double p_base = 0.0;
  switch (planet.type) {
    case PlanetType::EarthLike:
      if (mean_t > 255.0 && mean_t < 335.0) {
        candidate = LifeChemistry::CarbonWater;
        p_base = 0.55;
      }
      break;
    case PlanetType::Ice:
      if (has_sea && flux > 0.03) {
        candidate = LifeChemistry::Ammonia;  // subglacial / ammonia oceans
        p_base = 0.30;
      } else if (flux < 0.16) {
        candidate = LifeChemistry::Crystalline;
        p_base = 0.40;
      }
      break;
    case PlanetType::Desert:
      if (flux > 1.8 && planet.pressure_rel.to_double() < 0.3) {
        candidate = LifeChemistry::Sulfur;
        p_base = 0.28;
      } else if (planet.pressure_rel.to_double() > 0.0) {
        candidate = LifeChemistry::CarbonWater;  // extremophiles only
        p_base = 0.20;
      }
      break;
    case PlanetType::Barren:
      if (flux < 0.22) {
        candidate = LifeChemistry::Crystalline;  // cryogenic silicon polymers
        p_base = 0.40;
      }
      break;
  }
  // T0020: a race home world is alive by decree (design civilization
  // section 6.4). Chemistry follows the type as usual; a type with no
  // natural candidate (a Barren machine cradle) takes the crystalline
  // table so the world still reads as inhabited.
  if (planet.forced_biosphere) {
    if (candidate == LifeChemistry::None) {
      candidate = planet.type == PlanetType::Barren || planet.type == PlanetType::Ice
                      ? LifeChemistry::Crystalline
                      : LifeChemistry::CarbonWater;
    }
    p_base = 1.0;
  }
  life.habitable = candidate != LifeChemistry::None;
  if (!life.habitable) {
    return life;
  }

  // --- occupancy: keyed Bernoulli, odds rising with star age -------------
  const double age_ramp = clamp01((age - 0.8) / 4.0) * 0.85 + 0.15;
  life.occupied = planet.forced_biosphere || u01(d0[0]) < p_base * age_ramp;
  const double jitter_roll = u01(d0[1]);
  const int jitter = jitter_roll < 0.25 ? -1 : (jitter_roll < 0.75 ? 0 : 1);

  if (!life.occupied) {
    // Sterile-habitable. Young worlds may still wear the prebiotic haze
    // (atmospheric chemistry, not life).
    life.chemistry = LifeChemistry::None;
    life.stage = (age < 1.5 && u01(d0[2]) < 0.5) ? LifeStage::PrebioticHaze
                                                 : LifeStage::Sterile;
    return life;
  }

  life.chemistry = candidate;
  if (candidate == LifeChemistry::CarbonWater) {
    life.stage = carbon_stage_for(age, jitter);
    if (planet.type == PlanetType::Desert && static_cast<int>(life.stage) > 2) {
      life.stage = LifeStage::MicrobialMats;  // thin air: extremophile mats only
    }
    carbon_pigment(planet.star_temperature_k.to_double(), life.pigment);
    // Per-planet drift of the pigment (species accident, +-10%).
    const double dr = 0.9 + 0.2 * u01(d1[0]);
    const double dg = 0.9 + 0.2 * u01(d1[1]);
    life.pigment[0] = static_cast<float>(life.pigment[0] * dr);
    life.pigment[1] = static_cast<float>(life.pigment[1] * dg);
    // Mats: purple (retinal), green (cyanobacteria), rust (iron/halophile).
    switch (static_cast<int>(u01(d1[2]) * 3.0)) {
      case 0: set3(life.pigment2, 0.46, 0.18, 0.52); break;
      case 1: set3(life.pigment2, 0.20, 0.50, 0.24); break;
      default: set3(life.pigment2, 0.66, 0.30, 0.10); break;
    }
  } else {
    life.stage = exotic_stage_for(age, jitter);
    switch (candidate) {
      case LifeChemistry::Crystalline:
        switch (static_cast<int>(u01(d1[0]) * 4.0)) {
          case 0: set3(life.pigment, 0.35, 0.80, 0.85); break;  // cyan
          case 1: set3(life.pigment, 0.60, 0.35, 0.85); break;  // violet
          case 2: set3(life.pigment, 0.90, 0.65, 0.25); break;  // amber
          default: set3(life.pigment, 0.90, 0.40, 0.55); break; // rose
        }
        set3(life.pigment2, life.pigment[0] * 0.6, life.pigment[1] * 0.6,
             life.pigment[2] * 0.6);
        life.emissive = static_cast<float>(0.4 + 0.6 * u01(d1[1]));
        break;
      case LifeChemistry::Sulfur:
        set3(life.pigment, 0.85, 0.75, 0.20);
        set3(life.pigment2, 0.90, 0.50, 0.15);
        break;
      case LifeChemistry::Ammonia:
        set3(life.pigment, 0.35, 0.55, 0.55);
        set3(life.pigment2, 0.50, 0.60, 0.70);
        break;
      default: break;
    }
  }
  if (planet.forced_biosphere) {
    life.stage = LifeStage::FullBiosphere;
  }
  return life;
}

double life_coverage(const LifeParams& life, const Climate& climate, double slope,
                     double height_above_sea_m, double patch) {
  if (!life.occupied) {
    return 0.0;
  }
  const double gentle = 1.0 - smooth(slope, 0.12, 0.30);
  const double h = height_above_sea_m;
  switch (life.chemistry) {
    case LifeChemistry::CarbonWater: {
      switch (life.stage) {
        case LifeStage::MicrobialMats:
        case LifeStage::Oxygenation: {
          // Shore band and shallows: 25 m below to 40 m above the datum.
          const double band = h < 0.0 ? bump(h / 25.0) : bump(h / 40.0);
          const double wet = smooth(climate.humidity, 0.15, 0.35);
          const double warm = smooth(climate.temperature_k, 268.0, 280.0);
          const double base = band * wet * warm * gentle * (0.55 + 0.45 * patch);
          return life.stage == LifeStage::Oxygenation ? base * 0.8 : base;
        }
        case LifeStage::CrustColonisation: {
          const double wet = smooth(climate.humidity, 0.30, 0.60);
          const double warm = smooth(climate.biotemp_c, 1.0, 10.0);
          const double low = 1.0 - smooth(h, 900.0, 1600.0);
          return wet * warm * gentle * low * (0.35 + 0.65 * patch) * (h > -5.0 ? 1.0 : 0.0);
        }
        case LifeStage::FullBiosphere: {
          const double wet = smooth(climate.humidity, 0.12, 0.38);
          const double warm = smooth(climate.biotemp_c, 0.5, 6.0);
          return wet * warm * gentle * (0.7 + 0.3 * patch) * (h > -2.0 ? 1.0 : 0.0);
        }
        case LifeStage::Senescent: {
          const double wet = smooth(climate.humidity, 0.12, 0.38);
          const double warm = smooth(climate.biotemp_c, 0.5, 6.0);
          return 0.45 * wet * warm * gentle * smooth(patch, 0.35, 0.75) *
                 (h > -2.0 ? 1.0 : 0.0);
        }
        default: return 0.0;
      }
    }
    case LifeChemistry::Ammonia: {
      // Along the ice-sea margin and slush shelves.
      const double band = bump(h / 60.0);
      const double stage_gain = life.stage == LifeStage::FullBiosphere ? 1.0
                                : life.stage == LifeStage::CrustColonisation ? 0.7
                                                                              : 0.45;
      return band * gentle * stage_gain * (0.5 + 0.5 * patch);
    }
    case LifeChemistry::Sulfur: {
      // Vent fields: hot spots from the patch noise, growing with stage.
      const double thresh = life.stage == LifeStage::FullBiosphere ? 0.45
                            : life.stage == LifeStage::CrustColonisation ? 0.58
                                                                          : 0.68;
      return smooth(patch, thresh, thresh + 0.2) * gentle;
    }
    case LifeChemistry::Crystalline: {
      // Grows on rock in patches; prefers slopes and shade, so no gentle term.
      const double thresh = life.stage == LifeStage::FullBiosphere ? 0.42
                            : life.stage == LifeStage::CrustColonisation ? 0.56
                                                                          : 0.66;
      return smooth(patch, thresh, thresh + 0.18);
    }
    case LifeChemistry::None:
    default: return 0.0;
  }
}

std::string LifeParams::to_json() const {
  char buffer[512];
  std::snprintf(buffer, sizeof(buffer),
                "{\n  \"habitable\": %s,\n  \"occupied\": %s,\n  \"chemistry\": \"%s\",\n"
                "  \"stage\": \"%s\",\n  \"pigment\": [%.3f, %.3f, %.3f],\n"
                "  \"pigment2\": [%.3f, %.3f, %.3f],\n  \"emissive\": %.3f,\n"
                "  \"variant\": %u\n}\n",
                habitable ? "true" : "false", occupied ? "true" : "false",
                gen::to_string(chemistry), gen::to_string(stage),
                static_cast<double>(pigment[0]), static_cast<double>(pigment[1]),
                static_cast<double>(pigment[2]), static_cast<double>(pigment2[0]),
                static_cast<double>(pigment2[1]), static_cast<double>(pigment2[2]),
                static_cast<double>(emissive), variant);
  return buffer;
}

}  // namespace inf::gen
