#include "gen/biome.hpp"

namespace inf::gen {

namespace {

double clamp01(double x) { return x < 0.0 ? 0.0 : (x > 1.0 ? 1.0 : x); }

double smooth(double x, double lo, double hi) {
  const double t = clamp01((x - lo) / (hi - lo));
  return t * t * (3.0 - 2.0 * t);
}

// Grid: 4 temperature rows x 3 humidity columns.
constexpr double kTempEdges[3] = {0.20, 0.45, 0.75};
constexpr double kHumEdges[2] = {0.30, 0.65};

Biome cell(int row, int col) {
  static constexpr Biome kGrid[4][3] = {
      {Biome::PolarDesert, Biome::PolarDesert, Biome::Tundra},
      {Biome::Tundra, Biome::BorealForest, Biome::BorealForest},
      {Biome::TemperateGrassland, Biome::TemperateForest, Biome::TemperateRainforest},
      {Biome::HotDesert, Biome::Savanna, Biome::TropicalSeasonalForest},
  };
  return kGrid[row][col];
}

}  // namespace

const char* to_string(Biome biome) {
  switch (biome) {
    case Biome::PolarDesert: return "PolarDesert";
    case Biome::Tundra: return "Tundra";
    case Biome::BorealForest: return "BorealForest";
    case Biome::TemperateGrassland: return "TemperateGrassland";
    case Biome::TemperateForest: return "TemperateForest";
    case Biome::TemperateRainforest: return "TemperateRainforest";
    case Biome::Shrubland: return "Shrubland";
    case Biome::Savanna: return "Savanna";
    case Biome::HotDesert: return "HotDesert";
    case Biome::TropicalSeasonalForest: return "TropicalSeasonalForest";
    case Biome::TropicalRainforest: return "TropicalRainforest";
    case Biome::Alpine: return "Alpine";
    case Biome::Count: break;
  }
  return "?";
}

BiomeSample classify_biome(double t01, double h01, double biotemp_c, double altitude_m) {
  // Axes: half the planet's own percentile (spreads every world over its
  // full range), half absolute physics (a frozen point is polar on ANY
  // world, a 25 C point is tropical on any world).
  const double t_abs = clamp01(biotemp_c / 28.0);
  const double h_abs = clamp01(h01);  // h01 arrives as the percentile; humidity
                                      // itself is folded in by the caller below
  t01 = biotemp_c <= 0.0 ? clamp01(t01) * 0.15 : 0.5 * clamp01(t01) + 0.5 * t_abs;
  h01 = h_abs;
  int row = 0;
  while (row < 3 && t01 >= kTempEdges[row]) {
    ++row;
  }
  int col = 0;
  while (col < 2 && h01 >= kHumEdges[col]) {
    ++col;
  }
  BiomeSample out;
  out.primary = cell(row, col);
  // Tropical wet corner splits into seasonal / rain forest.
  if (row == 3 && col == 2 && h01 > 0.82) {
    out.primary = Biome::TropicalRainforest;
  }
  // Shrubland: the dry edge of the temperate row.
  if (row == 2 && col == 0 && h01 > 0.18) {
    out.primary = Biome::Shrubland;
  }

  // Secondary = the neighbouring cell across the nearest boundary; blend
  // ramps to 0.5 exactly on the boundary so materials never pop.
  double best_dist = 1.0;
  int srow = row;
  int scol = col;
  for (int r = 0; r < 3; ++r) {
    const double d = t01 - kTempEdges[r];
    const double ad = d < 0.0 ? -d : d;
    if (ad < best_dist) {
      best_dist = ad;
      srow = d < 0.0 ? r + 1 : r;
      scol = col;
    }
  }
  for (int c = 0; c < 2; ++c) {
    const double d = h01 - kHumEdges[c];
    const double ad = d < 0.0 ? -d : d;
    if (ad < best_dist) {
      best_dist = ad;
      srow = row;
      scol = d < 0.0 ? c + 1 : c;
    }
  }
  out.secondary = cell(srow, scol);
  constexpr double kBlendWidth = 0.08;
  out.blend = best_dist < kBlendWidth ? 0.5 * (1.0 - best_dist / kBlendWidth) : 0.0;

  // Alpine belt: cold-by-altitude on any row; the line scales with the
  // planet's temperature rank (1:10 scale — 1500 m game = 15 km real).
  const double alpine_line = 400.0 + 1600.0 * t01;
  const double alpine = smooth(altitude_m, alpine_line, alpine_line + 500.0);
  if (alpine > 0.5) {
    out.secondary = out.primary;
    out.blend = 1.0 - alpine;
    out.primary = Biome::Alpine;
  } else if (alpine > 0.0) {
    out.secondary = Biome::Alpine;
    out.blend = alpine;
  }

  // Continuous descriptors (used for material weights; independent of the
  // discrete pick so blends stay smooth).
  const double warm = smooth(biotemp_c, 1.0, 9.0);
  out.forest = smooth(h01, 0.35, 0.70) * smooth(t01, 0.22, 0.45) * warm * (1.0 - alpine);
  out.grass = smooth(h01, 0.12, 0.40) * (1.0 - smooth(h01, 0.60, 0.85)) * smooth(t01, 0.18, 0.40) *
              warm * (1.0 - alpine);
  out.aridity = (1.0 - smooth(h01, 0.10, 0.40)) * smooth(t01, 0.30, 0.60);
  // Cold is ABSOLUTE: permafrost needs a biotemperature near zero, not
  // merely the planet's colder half.
  out.cold = (1.0 - smooth(biotemp_c, 0.5, 5.0)) + alpine * 0.6;
  out.cold = clamp01(out.cold);
  return out;
}

}  // namespace inf::gen
