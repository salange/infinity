#pragma once

#include <cstdint>
#include <string>

#include "core/key.hpp"
#include "gen/climate.hpp"
#include "gen/planet.hpp"

namespace inf::gen {

// life/v1 (T0019, design/surface-texturing.md section 2.3): ONE keyed
// draw per body deciding whether the world could carry life
// (habitability — computed from the climate, never drawn), whether it
// does (occupancy — a keyed Bernoulli whose odds rise with the host
// star's age), of which chemistry, and at which evolutionary stage. The
// stage is centred on the star's age with a +-1 keyed jitter, so a
// system's worlds are coherent (young systems: haze and mats; old
// systems: full or senescent biospheres). WorldTime never advances it.
//
// Pigments follow the astrobiology literature (sources/planet-surface-
// texturing.md section 4): carbon photosynthesis tunes to the star (F
// yellow-orange, G green, K red-orange, M near-black); early microbial
// mats are purple (retinal), green or rust; exotic chemistries carry
// their own tables.
enum class LifeChemistry : std::uint8_t {
  None = 0,
  CarbonWater = 1,
  Crystalline = 2,  // silicon polymers: cryogenic or hot-dry worlds
  Ammonia = 3,      // ammonia-solvent life on frozen-sea worlds
  Sulfur = 4,       // thermophile mats on hot thin-air worlds
};

enum class LifeStage : std::uint8_t {
  Sterile = 0,            // habitable (or not), nothing lives here
  PrebioticHaze = 1,      // organic haze, tholin-dusted highlands
  MicrobialMats = 2,      // coloured shorelines and shallows
  Oxygenation = 3,        // red beds, banded iron, mats persist
  CrustColonisation = 4,  // lichen / moss / algal crusts on wet lowlands
  FullBiosphere = 5,      // the whole biome grid carries vegetation
  Senescent = 6,          // relic crusts, dead cover, evaporite flats
};

const char* to_string(LifeChemistry chemistry);
const char* to_string(LifeStage stage);

struct LifeParams {
  bool habitable{false};
  bool occupied{false};
  LifeChemistry chemistry{LifeChemistry::None};
  LifeStage stage{LifeStage::Sterile};
  float pigment[3]{0.25f, 0.45f, 0.18f};   // primary cover (vegetation / crystals)
  float pigment2[3]{0.45f, 0.18f, 0.50f};  // mats / secondary cover
  float emissive{0.0f};                    // night glow of the primary cover
  std::uint32_t variant{0};

  std::string to_json() const;
};

// The draw. `climate` supplies the habitability inputs (means).
LifeParams derive_life(const core::Key& body_entity_key, const PlanetParams& planet,
                       const ClimateField& climate);

// Pointwise cover fraction of the stage's characteristic material at a
// surface point (0..1). `patch` is a 0..1 patchiness noise supplied by
// the caller; slope is 1 - dot(normal, radial).
double life_coverage(const LifeParams& life, const Climate& climate, double slope,
                     double height_above_sea_m, double patch);

}  // namespace inf::gen
