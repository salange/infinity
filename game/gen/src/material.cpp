#include "gen/material.hpp"

#include <cmath>

#include "core/det/mix.hpp"
#include "gen/names.hpp"
#include "world/noise.hpp"

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

double noise_m(std::uint64_t lattice, double px, double py, double pz, double period_m) {
  const double f = 1.0 / period_m;
  return world::gradient_noise3(lattice, det::Real(px * f), det::Real(py * f),
                                det::Real(pz * f))
      .to_double();
}

// The registry. Mean albedos approximate the CC0 tiles named in
// assets/manifest.json (measured from the 1K colour maps) or the
// procedural generators; tile sizes are metres per repeat at the fine
// scale (the shader adds a coarse scale 8x larger).
constexpr MaterialInfo kInfo[] = {
    {"none",           {0.55f, 0.52f, 0.45f}, 4.0f, 0.90f, TintGroup::None, false},
    {"rock_granite",   {0.47f, 0.44f, 0.41f}, 6.0f, 0.85f, TintGroup::Rock, false},
    {"rock_basalt",    {0.19f, 0.18f, 0.18f}, 6.0f, 0.80f, TintGroup::Rock, false},
    {"rock_sandstone", {0.60f, 0.47f, 0.35f}, 6.0f, 0.85f, TintGroup::Rock, false},
    {"rock_shale",     {0.42f, 0.41f, 0.40f}, 6.0f, 0.80f, TintGroup::Rock, false},
    {"scree",          {0.45f, 0.42f, 0.38f}, 4.0f, 0.90f, TintGroup::Rock, false},
    {"cliff_mossy",    {0.36f, 0.40f, 0.30f}, 5.0f, 0.85f, TintGroup::Rock, false},
    {"regolith_fine",  {0.46f, 0.44f, 0.41f}, 3.0f, 0.95f, TintGroup::Rock, false},
    {"regolith_rubble",{0.40f, 0.38f, 0.35f}, 3.5f, 0.95f, TintGroup::Rock, false},
    {"gravel",         {0.44f, 0.42f, 0.39f}, 2.5f, 0.90f, TintGroup::Rock, false},
    {"pebbles",        {0.50f, 0.46f, 0.41f}, 2.0f, 0.75f, TintGroup::Rock, false},
    {"sand_dune",      {0.78f, 0.66f, 0.46f}, 4.0f, 0.85f, TintGroup::Sand, false},
    {"sand_beach",     {0.74f, 0.66f, 0.50f}, 4.0f, 0.80f, TintGroup::Sand, false},
    {"sand_wet",       {0.40f, 0.36f, 0.30f}, 4.0f, 0.55f, TintGroup::Sand, false},
    {"soil_dry",       {0.58f, 0.49f, 0.38f}, 3.0f, 0.90f, TintGroup::Soil, false},
    {"soil_mud",       {0.33f, 0.27f, 0.21f}, 3.0f, 0.60f, TintGroup::Soil, false},
    {"soil_loam",      {0.40f, 0.32f, 0.24f}, 3.0f, 0.85f, TintGroup::Soil, false},
    {"forest_floor",   {0.34f, 0.28f, 0.19f}, 3.0f, 0.85f, TintGroup::Soil, false},
    {"dead_leaves",    {0.45f, 0.33f, 0.20f}, 2.5f, 0.80f, TintGroup::Life, false},
    {"grass",          {0.30f, 0.42f, 0.16f}, 2.5f, 0.75f, TintGroup::Life, false},
    {"meadow",         {0.33f, 0.40f, 0.20f}, 2.5f, 0.80f, TintGroup::Life, false},
    {"moss",           {0.28f, 0.38f, 0.16f}, 2.0f, 0.80f, TintGroup::Life, false},
    {"snow",           {0.92f, 0.93f, 0.96f}, 4.0f, 0.55f, TintGroup::None, false},
    {"snow_drift",     {0.90f, 0.92f, 0.96f}, 5.0f, 0.55f, TintGroup::None, false},
    {"snow_dirty",     {0.70f, 0.69f, 0.68f}, 3.0f, 0.65f, TintGroup::None, false},
    {"ice_sheet",      {0.70f, 0.80f, 0.88f}, 6.0f, 0.20f, TintGroup::None, false},
    {"permafrost",     {0.62f, 0.62f, 0.62f}, 3.0f, 0.60f, TintGroup::None, false},
    {"lava_rock",      {0.14f, 0.10f, 0.09f}, 5.0f, 0.70f, TintGroup::Rock, true},
    {"microbial_mat",  {0.45f, 0.35f, 0.40f}, 3.0f, 0.35f, TintGroup::Life2, false},
    {"lichen_crust",   {0.50f, 0.50f, 0.38f}, 2.5f, 0.85f, TintGroup::Life, false},
    {"crystal_field",  {0.55f, 0.65f, 0.70f}, 3.0f, 0.15f, TintGroup::Life, true},
    {"sulfur",         {0.82f, 0.72f, 0.24f}, 4.0f, 0.70f, TintGroup::Sulfur, false},
    {"tholin_dust",    {0.55f, 0.38f, 0.22f}, 4.0f, 0.95f, TintGroup::None, false},
    {"red_bed",        {0.58f, 0.30f, 0.18f}, 4.0f, 0.85f, TintGroup::None, false},
    {"salt_flat",      {0.90f, 0.88f, 0.84f}, 5.0f, 0.55f, TintGroup::None, false},
    {"ammonia_slush",  {0.55f, 0.66f, 0.72f}, 4.0f, 0.30f, TintGroup::Life, false},
    {"seabed",         {0.36f, 0.35f, 0.29f}, 4.0f, 0.60f, TintGroup::Soil, false},
    {"paving",         {0.48f, 0.47f, 0.45f}, 4.0f, 0.75f, TintGroup::None, false},
    {"plating",        {0.42f, 0.44f, 0.47f}, 4.0f, 0.45f, TintGroup::None, true},
    {"resin_floor",    {0.55f, 0.46f, 0.30f}, 4.0f, 0.35f, TintGroup::None, false},
    {"crystal_floor",  {0.60f, 0.70f, 0.78f}, 4.0f, 0.15f, TintGroup::None, true},
    {"disturbed_soil", {0.50f, 0.42f, 0.32f}, 4.0f, 0.90f, TintGroup::Soil, false},
};
static_assert(sizeof(kInfo) / sizeof(kInfo[0]) == kMaterialCount,
              "material registry and enum out of sync");

class Weights {
 public:
  explicit Weights(double* storage) : w_(storage) {
    for (std::size_t i = 0; i < kMaterialCount; ++i) {
      w_[i] = 0.0;
    }
  }
  void add(Material id, double w) {
    if (w > 0.0) {
      w_[static_cast<std::size_t>(id)] += w;
    }
  }

 private:
  double* w_;
};

}  // namespace

const MaterialInfo& material_info(Material id) {
  const auto index = static_cast<std::size_t>(id);
  return kInfo[index < kMaterialCount ? index : 0];
}

MaterialField::MaterialField(const core::Key& body_entity_key, const PlanetParams& planet,
                             const LifeParams& life)
    : planet_(planet), life_(life) {
  const core::Key material_key = core::derive_named(body_entity_key, name::MaterialV2);
  lattice_patch_ = core::lattice_key(material_key, channel::Lattice);
  lattice_fine_ = det::mix64(lattice_patch_ ^ 0xF1E5A7C3D2B19E01ULL);
  lattice_vent_ = det::mix64(lattice_patch_ ^ 0x7A3C5E1F9B2D4C60ULL);

  // Mineral families from palette_id: which bedrock, which sand, and the
  // per-planet tints. Bits are consumed independently so the choices
  // are uncorrelated.
  const std::uint32_t p = planet.palette_id;
  const double a = static_cast<double>(p & 255U) / 255.0;
  const double b = static_cast<double>((p >> 8U) & 255U) / 255.0;
  const double c = static_cast<double>((p >> 16U) & 255U) / 255.0;
  switch (planet.type) {
    case PlanetType::EarthLike:
      bedrock_ = (p % 3U) == 0 ? Material::RockShale : Material::RockGranite;
      sand_ = (p % 5U) < 3 ? Material::SandBeach : Material::SandDune;
      break;
    case PlanetType::Barren:
      // Grey bedrock: crater walls and rims are steep everywhere on a
      // cratered body, and black basalt beside bright regolith read as
      // holes in the ground. Basalt is reserved for volcanic worlds.
      bedrock_ = (p % 4U) == 0 ? Material::RockGranite : Material::RockShale;
      sand_ = Material::RegolithFine;
      break;
    case PlanetType::Desert:
      bedrock_ = (p % 3U) == 0 ? Material::RockShale : Material::RockSandstone;
      sand_ = (p % 4U) == 0 ? Material::SandBeach : Material::SandDune;
      break;
    case PlanetType::Ice:
      bedrock_ = (p % 2U) == 0 ? Material::RockShale : Material::RockGranite;
      sand_ = Material::Gravel;
      break;
  }
  volcanic_ = planet.flux_rel.to_double() > 1.2 && (p % 7U) < 2 &&
              planet.type != PlanetType::EarthLike;
  if (volcanic_ && planet.type == PlanetType::Barren) {
    bedrock_ = Material::RockBasalt;
  }

  // Rock: warm-grey to cool-grey to brown; sand: yellow or ochre/red
  // (Mars-like) on Desert worlds; soil: neutral drift.
  palette_.rock[0] = static_cast<float>(1.0 + 0.16 * (a - 0.5));
  palette_.rock[1] = static_cast<float>(1.0 + 0.06 * (b - 0.5));
  palette_.rock[2] = static_cast<float>(1.0 - 0.14 * (a - 0.5));
  if (planet.type == PlanetType::Desert && (p % 3U) != 0) {
    palette_.sand[0] = static_cast<float>(1.08 + 0.10 * c);
    palette_.sand[1] = static_cast<float>(0.72 + 0.10 * c);
    palette_.sand[2] = static_cast<float>(0.50 + 0.10 * c);
  } else {
    palette_.sand[0] = static_cast<float>(1.0 + 0.08 * (c - 0.5));
    palette_.sand[1] = static_cast<float>(1.0 + 0.04 * (c - 0.5));
    palette_.sand[2] = static_cast<float>(1.0 - 0.10 * (c - 0.5));
  }
  palette_.soil[0] = static_cast<float>(1.0 + 0.10 * (b - 0.5));
  palette_.soil[1] = static_cast<float>(1.0 + 0.04 * (b - 0.5));
  palette_.soil[2] = static_cast<float>(1.0 - 0.08 * (b - 0.5));
  // Life tints are multipliers relative to a neutral green tile
  // (0.30, 0.42, 0.16) so a G-star world keeps the tile's own look.
  const float neutral[3] = {0.30f, 0.42f, 0.16f};
  const float neutral2[3] = {0.45f, 0.35f, 0.40f};
  for (int i = 0; i < 3; ++i) {
    palette_.life[i] = life.occupied ? life.pigment[i] / neutral[i] : 1.0f;
    palette_.life2[i] = life.occupied ? life.pigment2[i] / neutral2[i] : 1.0f;
  }
  palette_.emissive = life.emissive;
}

void MaterialField::tint(Material id, float rgb[3]) const {
  const float* t = nullptr;
  switch (material_info(id).tint) {
    case TintGroup::Rock: t = palette_.rock; break;
    case TintGroup::Sand: t = palette_.sand; break;
    case TintGroup::Soil: t = palette_.soil; break;
    case TintGroup::Life: t = palette_.life; break;
    case TintGroup::Life2: t = palette_.life2; break;
    case TintGroup::Sulfur:
    case TintGroup::None:
    default: break;
  }
  for (int i = 0; i < 3; ++i) {
    rgb[i] = t != nullptr ? t[i] : 1.0f;
  }
}

void MaterialField::mean_albedo(Material id, float rgb[3]) const {
  float t[3];
  tint(id, t);
  const MaterialInfo& info = material_info(id);
  for (int i = 0; i < 3; ++i) {
    rgb[i] = info.albedo[i] * t[i];
  }
}

VertexMaterial MaterialField::pick(const double w[kMaterialCount]) {
  std::size_t best = 0;
  std::size_t second = 0;
  double wb = -1.0;
  double ws = -1.0;
  for (std::size_t i = 1; i < kMaterialCount; ++i) {
    if (w[i] > wb) {
      second = best;
      ws = wb;
      best = i;
      wb = w[i];
    } else if (w[i] > ws) {
      second = i;
      ws = w[i];
    }
  }
  VertexMaterial out;
  if (wb <= 0.0) {
    return out;
  }
  out.mat0 = static_cast<Material>(best);
  if (ws <= 0.05 * wb) {
    out.mat1 = out.mat0;
    out.blend = 0.0f;
    return out;
  }
  out.mat1 = static_cast<Material>(second);
  out.blend = static_cast<float>(ws / (wb + ws));
  return out;
}

VertexMaterial MaterialField::classify(const MaterialInputs& in) const {
  double storage[kMaterialCount];
  weights(in, storage);
  return pick(storage);
}

void MaterialField::weights(const MaterialInputs& in, double out[kMaterialCount]) const {
  Weights w(out);
  const Climate& cl = in.climate;
  const BiomeSample& bi = in.biome;
  const double s = in.slope;
  const double cliff = smooth(s, 0.16, 0.34);
  const double steep = smooth(s, 0.08, 0.20);
  const double ground = 1.0 - cliff;
  const double h = in.height_above_sea_m;
  const bool wet_world = planet_.type == PlanetType::EarthLike ||
                         (planet_.type == PlanetType::Ice &&
                          planet_.land_fraction.to_double() < 0.999);
  const bool atmosphere = planet_.pressure_rel.to_double() > 0.0;
  const double patch = 0.5 + 0.5 * noise_m(lattice_patch_, in.px, in.py, in.pz, 90.0);
  const double fine = 0.5 + 0.5 * noise_m(lattice_fine_, in.px, in.py, in.pz, 14.0);
  const double vent = 0.5 + 0.5 * noise_m(lattice_vent_, in.px, in.py, in.pz, 700.0);

  // --- civil/v1 (T0020): paving inside settlements and along roads ------
  // A strong weight where the settlement plateau is: the surface of a
  // town is its paving, not its soil. Falls to disturbed soil at the rim.
  if (in.urban > 0.001) {
    Material surface = Material::Paving;
    switch (in.urban_family) {
      case 1: surface = Material::Plating; break;
      case 2: surface = Material::ResinFloor; break;
      case 3: surface = Material::CrystalFloor; break;
      case 4: surface = Material::DisturbedSoil; break;
      default: surface = Material::Paving; break;
    }
    w.add(surface, in.urban * 6.0);
    w.add(Material::DisturbedSoil, (1.0 - in.urban) * in.urban * 4.0);
  }

  // --- bedrock: dug faces, cave walls, cliffs ------------------------------
  const double depth = in.depth_below_surface_m;
  const double buried = smooth(depth, 0.8, 3.0);
  w.add(bedrock_, buried * 4.0);
  w.add(bedrock_, cliff * 3.0);
  w.add(Material::Scree, steep * ground * (0.8 + 0.6 * in.ruggedness) * (1.0 - buried));

  // --- base cover by planet type and province archetype ------------------
  const double cover_scale = ground * (1.0 - buried);
  switch (planet_.type) {
    case PlanetType::EarthLike: {
      const double warm = 1.0 - bi.cold;
      w.add(Material::SoilLoam, cover_scale * 0.9 * (1.0 - bi.aridity) * warm);
      w.add(Material::SoilDry, cover_scale * 0.9 * bi.aridity * warm);
      w.add(Material::SoilMud, cover_scale * 0.5 * smooth(cl.h01, 0.70, 0.95) * warm);
      w.add(Material::Permafrost, cover_scale * 0.8 * bi.cold * (1.0 - cliff));
      w.add(sand_, cover_scale * 0.6 * bi.aridity * smooth(patch, 0.4, 0.8));
      switch (in.archetype) {
        case Archetype::Alpine:
          w.add(Material::Scree, cover_scale * 0.6 * in.ruggedness);
          w.add(Material::Gravel, cover_scale * 0.3);
          break;
        case Archetype::Canyon:
        case Archetype::HighlandPlateau:
          w.add(Material::Gravel, cover_scale * 0.35);
          w.add(Material::RockSandstone, cover_scale * 0.5 * in.terrace_amount);
          break;
        default: break;
      }
      break;
    }
    case PlanetType::Barren: {
      w.add(Material::RegolithFine, cover_scale * (1.0 - 0.5 * in.ruggedness));
      w.add(Material::RegolithRubble, cover_scale * (0.25 + 0.6 * in.ruggedness) * fine);
      if (in.archetype == Archetype::Cratered) {
        w.add(Material::RegolithRubble, cover_scale * 0.5);
      }
      if (in.archetype == Archetype::Highlands) {
        w.add(Material::Gravel, cover_scale * 0.5);
      }
      w.add(Material::Pebbles, cover_scale * 0.3 * smooth(patch, 0.55, 0.85));
      break;
    }
    case PlanetType::Desert: {
      // Dune seas are sand; mesa and canyon country is dry soil, gravel
      // and exposed strata with sand only pooling in the basins.
      const double basin = 1.0 - smooth(in.altitude_m, 60.0, 260.0);
      w.add(sand_, cover_scale * (0.20 + 1.3 * in.dune_amount + 0.45 * basin * smooth(patch, 0.3, 0.7)));
      w.add(Material::SoilDry, cover_scale * 0.62 * (1.0 - in.dune_amount));
      w.add(Material::Gravel, cover_scale * (0.30 + 0.6 * in.ruggedness) * (0.4 + 0.6 * fine));
      w.add(Material::Pebbles, cover_scale * 0.35 * smooth(patch, 0.55, 0.9) * (1.0 - in.dune_amount));
      if (in.archetype == Archetype::Mesas || in.archetype == Archetype::Canyonlands) {
        w.add(bedrock_, cover_scale * 0.7 * in.terrace_amount * smooth(fine, 0.35, 0.75));
      }
      break;
    }
    case PlanetType::Ice: {
      // Frozen worlds hold little vapour, so moisture is read on the
      // planet's own scale (h01): its wetter half snows, its drier half
      // is bare ice and frozen ground.
      const double moist = cl.h01;
      const double shield = in.archetype == Archetype::GlacialShield ? 1.0 : 0.0;
      w.add(Material::IceSheet, cover_scale * (0.45 + 0.7 * shield) * (1.0 - 0.5 * moist));
      w.add(Material::Snow, cover_scale * (0.35 + 0.9 * smooth(moist, 0.35, 0.8)));
      w.add(Material::SnowDrift, cover_scale * 0.7 * smooth(moist, 0.3, 0.7) * smooth(patch, 0.45, 0.8));
      w.add(Material::Permafrost, cover_scale * (0.25 + 0.6 * (in.archetype == Archetype::CrevasseField ? 1.0 : 0.0)) * (1.0 - moist));
      if (in.archetype == Archetype::RidgeField) {
        w.add(bedrock_, cover_scale * 0.5 * in.ruggedness);
        w.add(Material::Scree, cover_scale * 0.4);
      }
      break;
    }
  }

  // --- water: seabed, shallows, beaches ------------------------------------
  if (wet_world) {
    if (planet_.type == PlanetType::Ice) {
      w.add(Material::IceSheet, smooth(-h, 0.0, 10.0) * 3.0);
    } else {
      w.add(Material::Seabed, smooth(-h, 0.0, 12.0) * 2.5 * (1.0 - cliff));
      w.add(Material::SoilMud, smooth(-h, 40.0, 300.0) * 3.0);
      w.add(Material::SandWet, bump(h / 8.0) * 2.0 * (1.0 - cliff));
      w.add(Material::SandBeach, bump((h - 18.0) / 22.0) * 1.6 * (1.0 - cliff) * (1.0 - buried));
    }
  }

  // --- climate: snow where it is cold AND there is moisture ---------------
  if (atmosphere || planet_.type == PlanetType::Ice) {
    const double snow = smooth(273.15 - cl.temperature_k, 0.0, 12.0) *
                        smooth(cl.humidity, 0.06, 0.30) * (1.0 - cliff) * (1.0 - buried);
    w.add(Material::Snow, snow * 2.2 * (h > -1.0 || planet_.type == PlanetType::Ice ? 1.0 : 0.0));
    w.add(Material::SnowDrift, snow * 0.9 * smooth(patch, 0.45, 0.8));
    w.add(Material::SnowDirty, snow * 0.5 * smooth(cl.temperature_k, 262.0, 272.0));
    w.add(Material::Permafrost, smooth(273.15 - cl.temperature_k, 4.0, 25.0) *
                                    (1.0 - smooth(cl.humidity, 0.06, 0.30)) * 0.9 * ground);
  } else {
    // Airless: cold traps hold frost; nothing else changes with latitude.
    w.add(Material::Permafrost, smooth(190.0 - cl.temperature_k, 0.0, 40.0) * 0.8 * ground);
  }

  // --- volcanism ------------------------------------------------------------
  if (volcanic_) {
    w.add(Material::LavaRock, smooth(vent, 0.58, 0.80) * 1.8 * (1.0 - buried));
  }

  // --- life -----------------------------------------------------------------
  const double cover = life_coverage(life_, cl, s, h, patch) * (1.0 - buried);
  switch (life_.stage) {
    case LifeStage::PrebioticHaze:
      w.add(Material::TholinDust, 0.55 * smooth(in.altitude_m, 150.0, 600.0) * ground);
      break;
    case LifeStage::MicrobialMats:
      w.add(Material::MicrobialMat, cover * 2.2);
      break;
    case LifeStage::Oxygenation:
      w.add(Material::MicrobialMat, cover * 2.0);
      if (life_.chemistry == LifeChemistry::CarbonWater) {
        w.add(Material::RedBed, 0.9 * (1.0 - bi.cold) * ground * (1.0 - buried) *
                                    (h > 0.0 ? 1.0 : 0.0) * smooth(patch, 0.2, 0.6));
      }
      break;
    default: break;
  }
  if (cover > 0.0) {
    switch (life_.chemistry) {
      case LifeChemistry::CarbonWater:
        if (life_.stage == LifeStage::CrustColonisation) {
          w.add(Material::LichenCrust, cover * 1.7);
          w.add(Material::Moss, cover * 0.9 * smooth(cl.h01, 0.55, 0.85));
        } else if (life_.stage == LifeStage::FullBiosphere) {
          w.add(Material::ForestFloor, cover * 2.2 * bi.forest);
          w.add(Material::Grass, cover * 2.2 * bi.grass * (1.0 - smooth(cl.h01, 0.6, 0.9)));
          w.add(Material::Meadow, cover * 2.0 * bi.grass * smooth(cl.h01, 0.5, 0.85));
          w.add(Material::Moss, cover * (1.6 * bi.cold * smooth(cl.humidity, 0.25, 0.5) +
                                         1.0 * smooth(cl.h01, 0.80, 0.98) * bi.forest));
          w.add(Material::Meadow, cover * 1.2 * (bi.primary == Biome::Savanna ||
                                                 bi.primary == Biome::Shrubland
                                                     ? 1.0
                                                     : 0.0));
          // Wet cliffs green over.
          w.add(Material::CliffMossy, cliff * 2.5 * smooth(cl.humidity, 0.45, 0.7) *
                                          smooth(cl.biotemp_c, 2.0, 8.0));
        } else if (life_.stage == LifeStage::Senescent) {
          w.add(Material::DeadLeaves, cover * 1.8);
          w.add(Material::SaltFlat, 1.0 * bi.aridity * bump(h / 20.0) * ground);
        }
        break;
      case LifeChemistry::Crystalline: w.add(Material::CrystalField, cover * 2.4); break;
      case LifeChemistry::Sulfur: w.add(Material::Sulfur, cover * 2.4); break;
      case LifeChemistry::Ammonia: w.add(Material::AmmoniaSlush, cover * 2.2); break;
      case LifeChemistry::None:
      default: break;
    }
  }

}

}  // namespace inf::gen
