#pragma once

#include <cstdint>

namespace inf::gen {

// biome/v1 (T0019, design/surface-texturing.md section 2): a Whittaker-
// style grid over the planet's OWN quantile-normalised climate axes
// (t01 = biotemperature rank, h01 = humidity rank), so every world spends
// its full biome range instead of collapsing into one zone. Biomes drive
// weathering on sterile worlds (soil vs. sand vs. permafrost) and the
// vegetation cover on living ones; they never decide life by themselves.
enum class Biome : std::uint8_t {
  PolarDesert = 0,
  Tundra = 1,
  BorealForest = 2,
  TemperateGrassland = 3,
  TemperateForest = 4,
  TemperateRainforest = 5,
  Shrubland = 6,
  Savanna = 7,
  HotDesert = 8,
  TropicalSeasonalForest = 9,
  TropicalRainforest = 10,
  Alpine = 11,
  Count = 12,
};

const char* to_string(Biome biome);

struct BiomeSample {
  Biome primary{Biome::PolarDesert};
  Biome secondary{Biome::PolarDesert};
  double blend{0.0};  // fraction of the secondary (0 = pure primary)
  // Continuous descriptors consumers blend materials with (0..1):
  double forest{0.0};   // canopy / forest-floor propensity
  double grass{0.0};    // grassland propensity
  double aridity{0.0};  // bare sand / dry soil propensity
  double cold{0.0};     // tundra / permafrost propensity
};

// Pure function. altitude_m above the sea datum lifts a point into the
// alpine belt; the belt lowers on colder worlds (alpine_line_m scales
// with t01).
BiomeSample classify_biome(double t01, double h01, double biotemp_c, double altitude_m);

}  // namespace inf::gen
