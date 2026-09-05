#include "tex/tiles.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

namespace inf::tex {

namespace {

constexpr double kPi = 3.14159265358979323846;

// --- hashing + periodic noise ----------------------------------------------

std::uint64_t mix64(std::uint64_t x) {
  x ^= x >> 30U;
  x *= 0xBF58476D1CE4E5B9ULL;
  x ^= x >> 27U;
  x *= 0x94D049BB133111EBULL;
  x ^= x >> 31U;
  return x;
}

double hash01(std::uint64_t seed, std::int64_t x, std::int64_t y, std::uint64_t salt = 0) {
  const std::uint64_t h = mix64(seed ^ mix64(static_cast<std::uint64_t>(x) * 0x9E3779B97F4A7C15ULL ^
                                             mix64(static_cast<std::uint64_t>(y) + salt * 0x632BE59BD9B4E019ULL)));
  return static_cast<double>(h >> 11U) * 0x1.0p-53;
}

std::int64_t wrap(std::int64_t v, std::int64_t period) {
  return ((v % period) + period) % period;
}

double fade(double t) { return t * t * t * (t * (t * 6.0 - 15.0) + 10.0); }

// Periodic gradient noise on a torus of `period` cells; x, y in cells.
double pnoise(std::uint64_t seed, double x, double y, std::int64_t period) {
  const double fx = std::floor(x);
  const double fy = std::floor(y);
  const std::int64_t ix = static_cast<std::int64_t>(fx);
  const std::int64_t iy = static_cast<std::int64_t>(fy);
  const double tx = x - fx;
  const double ty = y - fy;
  const auto grad = [&](std::int64_t gx, std::int64_t gy, double dx, double dy) {
    const double a = hash01(seed, wrap(gx, period), wrap(gy, period)) * 2.0 * kPi;
    return std::cos(a) * dx + std::sin(a) * dy;
  };
  const double n00 = grad(ix, iy, tx, ty);
  const double n10 = grad(ix + 1, iy, tx - 1.0, ty);
  const double n01 = grad(ix, iy + 1, tx, ty - 1.0);
  const double n11 = grad(ix + 1, iy + 1, tx - 1.0, ty - 1.0);
  const double u = fade(tx);
  const double v = fade(ty);
  const double a = n00 + (n10 - n00) * u;
  const double b = n01 + (n11 - n01) * u;
  return (a + (b - a) * v) * 1.4142;  // ~[-1, 1]
}

// Periodic fBm; x, y in [0, 1) tile space, base frequency `cells`.
double pfbm(std::uint64_t seed, double x, double y, int cells, int octaves, double gain = 0.5,
            double ridged = 0.0) {
  double sum = 0.0;
  double amp = 1.0;
  double norm = 0.0;
  int period = cells;
  for (int o = 0; o < octaves; ++o) {
    double n = pnoise(seed + static_cast<std::uint64_t>(o) * 0x1234567ULL, x * period,
                      y * period, period);
    if (ridged > 0.0) {
      const double r = 1.0 - std::fabs(n);
      n = n * (1.0 - ridged) + (r * r * 2.0 - 1.0) * ridged;
    }
    sum += n * amp;
    norm += amp;
    amp *= gain;
    period *= 2;
  }
  return sum / norm;
}

struct Worley {
  double f1, f2;      // distances (in cells)
  double id;          // 0..1 random id of the nearest cell
  double cx, cy;      // nearest feature point (cells)
};

// Periodic Worley / cellular noise; x, y in cells.
Worley pworley(std::uint64_t seed, double x, double y, std::int64_t period, double jitter = 1.0) {
  const std::int64_t ix = static_cast<std::int64_t>(std::floor(x));
  const std::int64_t iy = static_cast<std::int64_t>(std::floor(y));
  Worley out{1e9, 1e9, 0.0, 0.0, 0.0};
  for (std::int64_t dy = -1; dy <= 1; ++dy) {
    for (std::int64_t dx = -1; dx <= 1; ++dx) {
      const std::int64_t cx = ix + dx;
      const std::int64_t cy = iy + dy;
      const std::int64_t wx = wrap(cx, period);
      const std::int64_t wy = wrap(cy, period);
      const double px = static_cast<double>(cx) + 0.5 + (hash01(seed, wx, wy, 1) - 0.5) * jitter;
      const double py = static_cast<double>(cy) + 0.5 + (hash01(seed, wx, wy, 2) - 0.5) * jitter;
      const double d = std::sqrt((px - x) * (px - x) + (py - y) * (py - y));
      if (d < out.f1) {
        out.f2 = out.f1;
        out.f1 = d;
        out.id = hash01(seed, wx, wy, 3);
        out.cx = px;
        out.cy = py;
      } else if (d < out.f2) {
        out.f2 = d;
      }
    }
  }
  return out;
}

double clamp01(double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }
double smooth(double x, double lo, double hi) {
  const double t = clamp01((x - lo) / (hi - lo));
  return t * t * (3.0 - 2.0 * t);
}
std::uint8_t to_byte(double v) {
  return static_cast<std::uint8_t>(clamp01(v) * 255.0 + 0.5);
}

struct Rgb {
  double r, g, b;
};
Rgb mix(const Rgb& a, const Rgb& b, double t) {
  return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
}
Rgb scale(const Rgb& a, double s) { return {a.r * s, a.g * s, a.b * s}; }

// Per-texel output of a generator: colour, height (0..1), roughness,
// optional emissive mask.
struct Texel {
  Rgb color;
  double height;
  double roughness;
  double emissive;
};

// The generator kinds. Each is a function of (u, v) in [0, 1) tile space.
enum class Kind {
  Rock, Basalt, Sandstone, Shale, Scree, Rubble, Gravel, Pebbles, SandDune, SandBeach, SandWet,
  SoilDry, SoilMud, Loam, ForestFloor, DeadLeaves, Grass, Meadow, Moss, Snow, SnowDrift,
  SnowDirty, Ice, Permafrost, Lava, MicrobialMat, LichenCrust, Crystal, Sulfur, Tholin, RedBed,
  SaltFlat, Slush, Seabed, MossyCliff,
  Paving, Plating, Resin, CrystalFloor, Disturbed
};

struct NameKind {
  const char* name;
  Kind kind;
};

constexpr NameKind kNames[] = {
    {"rock_granite", Kind::Rock},       {"rock_basalt", Kind::Basalt},
    {"rock_sandstone", Kind::Sandstone}, {"rock_shale", Kind::Shale},
    {"scree", Kind::Scree},             {"cliff_mossy", Kind::MossyCliff},
    {"regolith_fine", Kind::Gravel},    {"regolith_rubble", Kind::Rubble},
    {"gravel", Kind::Gravel},           {"pebbles", Kind::Pebbles},
    {"sand_dune", Kind::SandDune},      {"sand_beach", Kind::SandBeach},
    {"sand_wet", Kind::SandWet},        {"soil_dry", Kind::SoilDry},
    {"soil_mud", Kind::SoilMud},        {"soil_loam", Kind::Loam},
    {"forest_floor", Kind::ForestFloor}, {"dead_leaves", Kind::DeadLeaves},
    {"grass", Kind::Grass},             {"meadow", Kind::Meadow},
    {"moss", Kind::Moss},               {"snow", Kind::Snow},
    {"snow_drift", Kind::SnowDrift},    {"snow_dirty", Kind::SnowDirty},
    {"ice_sheet", Kind::Ice},           {"permafrost", Kind::Permafrost},
    {"lava_rock", Kind::Lava},          {"microbial_mat", Kind::MicrobialMat},
    {"lichen_crust", Kind::LichenCrust}, {"crystal_field", Kind::Crystal},
    {"sulfur", Kind::Sulfur},           {"tholin_dust", Kind::Tholin},
    {"red_bed", Kind::RedBed},          {"salt_flat", Kind::SaltFlat},
    {"ammonia_slush", Kind::Slush},     {"seabed", Kind::Seabed},
    {"paving", Kind::Paving},           {"plating", Kind::Plating},
    {"resin_floor", Kind::Resin},       {"crystal_floor", Kind::CrystalFloor},
    {"disturbed_soil", Kind::Disturbed},
};

const char* kNameList[sizeof(kNames) / sizeof(kNames[0])] = {};

Kind kind_for(const std::string& name) {
  for (const NameKind& nk : kNames) {
    if (name == nk.name) {
      return nk.kind;
    }
  }
  return Kind::Rock;
}

// Stones: Worley cells with rounded caps, per-stone shade; `cells` per tile.
Texel stones(std::uint64_t seed, double u, double v, int cells, Rgb base, double shade_var,
             double gap, double rough) {
  const Worley w = pworley(seed, u * cells, v * cells, cells, 0.9);
  // Rounded cap: height falls from the centre; the gap between cells is a
  // groove (f2 - f1 small).
  const double edge = clamp01((w.f2 - w.f1) / gap);
  const double cap = std::sqrt(clamp01(1.0 - w.f1 * w.f1 * 1.6)) * edge;
  const double grain = pfbm(seed ^ 0x77, u, v, cells * 6, 3) * 0.06;
  const double tone = 1.0 + (w.id - 0.5) * shade_var + grain;
  Texel t;
  t.color = scale(base, tone * (0.55 + 0.45 * cap));
  t.height = 0.25 + 0.75 * cap;
  t.roughness = rough - 0.15 * cap;
  t.emissive = 0.0;
  return t;
}

Texel rock_like(std::uint64_t seed, double u, double v, Rgb base, double strata, double crack,
                double rough, double contrast) {
  const double big = pfbm(seed, u, v, 3, 5, 0.55, 0.6);
  const double fine = pfbm(seed ^ 0x5, u, v, 24, 3);
  double h = 0.5 + 0.30 * big + 0.10 * fine;
  // Strata: horizontal bands warped by the low frequency.
  if (strata > 0.0) {
    const double band = std::sin((v + 0.12 * big) * 2.0 * kPi * 9.0);
    h += 0.08 * strata * band;
  }
  // Cracks: cellular ridges as dark grooves.
  if (crack > 0.0) {
    const Worley w = pworley(seed ^ 0x9, u * 6, v * 6, 6);
    const double groove = 1.0 - smooth(w.f2 - w.f1, 0.0, 0.10);
    h -= 0.22 * crack * groove;
  }
  h = clamp01(h);
  Texel t;
  const double tone = 0.72 + contrast * (h - 0.5) + 0.06 * fine;
  t.color = scale(base, tone);
  if (strata > 0.0) {
    const double band = 0.5 + 0.5 * std::sin((v + 0.12 * big) * 2.0 * kPi * 9.0 + 1.3);
    t.color = mix(t.color, scale(base, 0.55), 0.35 * strata * band);
  }
  t.height = h;
  t.roughness = rough;
  t.emissive = 0.0;
  return t;
}

Texel sand_like(std::uint64_t seed, double u, double v, Rgb base, double ripple, double wet) {
  const double warp = pfbm(seed, u, v, 4, 3) * 0.15;
  const double r = std::sin((u * 14.0 + warp * 3.0 + 0.4 * v) * 2.0 * kPi);
  const double grain = pfbm(seed ^ 0x3, u, v, 64, 2);
  const double h = clamp01(0.5 + 0.30 * ripple * r + 0.08 * grain + 0.1 * warp);
  Texel t;
  t.color = scale(base, (0.88 + 0.14 * (h - 0.5) * 2.0 + 0.05 * grain) * (1.0 - 0.35 * wet));
  t.height = h;
  t.roughness = 0.9 - 0.5 * wet;
  t.emissive = 0.0;
  return t;
}

Texel cracked(std::uint64_t seed, double u, double v, Rgb base, int cells, double depth,
              double dome, Rgb crack_color, double rough) {
  const Worley w = pworley(seed, u * cells, v * cells, cells, 0.8);
  const double groove = 1.0 - smooth(w.f2 - w.f1, 0.0, 0.14);
  const double plate = smooth(w.f1, 0.0, 0.7);
  const double grain = pfbm(seed ^ 0x11, u, v, 40, 2) * 0.05;
  double h = 0.65 - dome * 0.25 * plate - depth * groove + grain;
  Texel t;
  t.color = mix(scale(base, 0.95 + grain * 2.0 + (w.id - 0.5) * 0.12), crack_color, groove);
  t.height = clamp01(h);
  t.roughness = rough;
  t.emissive = 0.0;
  return t;
}

Texel generic(Kind kind, std::uint64_t seed, double u, double v) {
  switch (kind) {
    case Kind::Rock:
      return rock_like(seed, u, v, {0.50, 0.47, 0.44}, 0.0, 0.5, 0.85, 0.55);
    case Kind::Basalt:
      return rock_like(seed, u, v, {0.20, 0.19, 0.19}, 0.0, 0.8, 0.80, 0.45);
    case Kind::Sandstone:
      return rock_like(seed, u, v, {0.64, 0.50, 0.37}, 1.0, 0.2, 0.85, 0.5);
    case Kind::Shale:
      return rock_like(seed, u, v, {0.44, 0.43, 0.42}, 0.7, 0.9, 0.80, 0.5);
    case Kind::RedBed:
      return rock_like(seed, u, v, {0.60, 0.31, 0.19}, 1.0, 0.3, 0.85, 0.5);
    case Kind::MossyCliff: {
      Texel t = rock_like(seed, u, v, {0.42, 0.40, 0.36}, 0.0, 0.6, 0.85, 0.5);
      const double moss = smooth(pfbm(seed ^ 0x21, u, v, 5, 4), 0.05, 0.5) * (1.0 - t.height);
      t.color = mix(t.color, {0.30, 0.42, 0.18}, moss);
      t.roughness = 0.85;
      return t;
    }
    case Kind::Scree: return stones(seed, u, v, 7, {0.46, 0.43, 0.39}, 0.5, 0.25, 0.9);
    case Kind::Rubble: return stones(seed, u, v, 10, {0.42, 0.40, 0.37}, 0.6, 0.30, 0.95);
    case Kind::Gravel: {
      Texel t = stones(seed, u, v, 26, {0.46, 0.44, 0.41}, 0.7, 0.4, 0.92);
      t.height = 0.35 + 0.35 * t.height;
      return t;
    }
    case Kind::Pebbles: return stones(seed, u, v, 16, {0.52, 0.47, 0.42}, 0.8, 0.35, 0.7);
    case Kind::SandDune: return sand_like(seed, u, v, {0.80, 0.68, 0.47}, 1.0, 0.0);
    case Kind::SandBeach: return sand_like(seed, u, v, {0.76, 0.68, 0.52}, 0.35, 0.0);
    case Kind::SandWet: return sand_like(seed, u, v, {0.46, 0.41, 0.34}, 0.3, 0.8);
    case Kind::Seabed: {
      Texel t = sand_like(seed, u, v, {0.40, 0.39, 0.32}, 0.6, 0.6);
      const Texel p = stones(seed ^ 0x8, u, v, 22, {0.38, 0.36, 0.30}, 0.5, 0.4, 0.7);
      const double k = smooth(pfbm(seed ^ 0x31, u, v, 3, 3), 0.2, 0.6);
      t.color = mix(t.color, p.color, k);
      t.height = t.height * (1.0 - k) + p.height * k;
      return t;
    }
    case Kind::SoilDry:
      return cracked(seed, u, v, {0.60, 0.50, 0.38}, 8, 0.35, 1.0, {0.30, 0.24, 0.18}, 0.9);
    case Kind::SaltFlat:
      return cracked(seed, u, v, {0.92, 0.90, 0.86}, 6, -0.18, 0.4, {0.80, 0.78, 0.74}, 0.55);
    case Kind::SoilMud: {
      const double h = clamp01(0.5 + 0.25 * pfbm(seed, u, v, 5, 4, 0.55) + 0.06 * pfbm(seed ^ 0x2, u, v, 30, 2));
      Texel t;
      t.color = scale({0.35, 0.28, 0.21}, 0.8 + 0.5 * (h - 0.5));
      t.height = h;
      t.roughness = 0.45 + 0.3 * h;
      t.emissive = 0.0;
      return t;
    }
    case Kind::Loam:
    case Kind::ForestFloor: {
      const double clump = pfbm(seed, u, v, 9, 4, 0.55);
      const Texel p = stones(seed ^ 0x8, u, v, 30, {0.38, 0.31, 0.24}, 0.5, 0.4, 0.85);
      const double k = smooth(clump, 0.1, 0.6);
      Texel t;
      const Rgb base = kind == Kind::Loam ? Rgb{0.42, 0.34, 0.25} : Rgb{0.36, 0.29, 0.20};
      t.color = mix(scale(base, 0.9 + 0.2 * clump), p.color, k * 0.6);
      t.height = clamp01(0.5 + 0.2 * clump) * (1.0 - k * 0.5) + p.height * k * 0.5;
      t.roughness = 0.88;
      t.emissive = 0.0;
      if (kind == Kind::ForestFloor) {
        // Leaf litter: flat elliptical blobs.
        const Worley w = pworley(seed ^ 0x55, u * 20, v * 20, 20, 1.0);
        const double leaf = smooth(0.45 - w.f1, 0.0, 0.12);
        const Rgb leaf_color = mix({0.50, 0.36, 0.20}, {0.30, 0.26, 0.15}, w.id);
        t.color = mix(t.color, leaf_color, leaf * 0.85);
        t.height = t.height + 0.12 * leaf;
      }
      return t;
    }
    case Kind::DeadLeaves: {
      const Worley w = pworley(seed, u * 18, v * 18, 18, 1.0);
      const double leaf = smooth(0.5 - w.f1, 0.0, 0.10);
      const Rgb leaf_color = mix({0.55, 0.38, 0.18}, {0.35, 0.24, 0.12}, w.id);
      Texel t;
      t.color = mix({0.30, 0.24, 0.16}, leaf_color, leaf);
      t.height = clamp01(0.4 + 0.3 * leaf + 0.1 * pfbm(seed ^ 0x3, u, v, 12, 2));
      t.roughness = 0.8;
      t.emissive = 0.0;
      return t;
    }
    case Kind::Grass:
    case Kind::Meadow: {
      // Blades: strongly anisotropic fine noise + clump variation.
      const double blades = pfbm(seed, u * 1.0, v * 6.0, 48, 2);
      const double clump = pfbm(seed ^ 0x7, u, v, 6, 3);
      const double h = clamp01(0.5 + 0.35 * blades + 0.12 * clump);
      const Rgb dark{0.20, 0.32, 0.10};
      const Rgb light{0.38, 0.50, 0.19};
      Texel t;
      t.color = mix(dark, light, h);
      if (kind == Kind::Meadow) {
        t.color = mix(t.color, {0.42, 0.36, 0.22}, smooth(clump, 0.25, 0.6) * 0.5);
      }
      t.height = h;
      t.roughness = 0.8;
      t.emissive = 0.0;
      return t;
    }
    case Kind::Moss: {
      const Worley w = pworley(seed, u * 24, v * 24, 24, 1.0);
      const double clump = smooth(0.6 - w.f1, 0.0, 0.5);
      const double fine = pfbm(seed ^ 0x4, u, v, 60, 2);
      Texel t;
      t.color = mix({0.22, 0.30, 0.12}, {0.34, 0.44, 0.17}, clump + 0.1 * fine);
      t.height = clamp01(0.35 + 0.5 * clump + 0.08 * fine);
      t.roughness = 0.85;
      t.emissive = 0.0;
      return t;
    }
    case Kind::Snow:
    case Kind::SnowDrift:
    case Kind::SnowDirty: {
      const double low = pfbm(seed, u, v, 3, 3, 0.5);
      const double ripple = kind == Kind::SnowDrift
                                ? std::sin((u * 8.0 + 0.6 * low + 0.3 * v) * 2.0 * kPi) * 0.25
                                : 0.0;
      const double h = clamp01(0.55 + 0.2 * low + ripple);
      const double sparkle = hash01(seed, static_cast<std::int64_t>(u * 4096.0),
                                    static_cast<std::int64_t>(v * 4096.0)) > 0.985 ? 0.12 : 0.0;
      Texel t;
      t.color = scale({0.93, 0.94, 0.97}, 0.92 + 0.08 * h + sparkle);
      if (kind == Kind::SnowDirty) {
        const double dirt = smooth(pfbm(seed ^ 0x9, u, v, 10, 3), 0.15, 0.6);
        t.color = mix(t.color, {0.40, 0.37, 0.33}, dirt * 0.7);
      }
      t.height = h;
      t.roughness = 0.6 - sparkle * 2.0;
      t.emissive = 0.0;
      return t;
    }
    case Kind::Ice: {
      const Worley w = pworley(seed, u * 5, v * 5, 5, 0.9);
      const double crack = 1.0 - smooth(w.f2 - w.f1, 0.0, 0.06);
      const double bubbles = smooth(pfbm(seed ^ 0x6, u, v, 40, 2), 0.3, 0.7);
      Texel t;
      t.color = mix({0.68, 0.79, 0.88}, {0.90, 0.94, 0.98}, crack * 0.8 + bubbles * 0.3);
      t.height = clamp01(0.6 - 0.15 * crack + 0.05 * pfbm(seed ^ 0x2, u, v, 4, 3));
      t.roughness = 0.18 + 0.3 * bubbles;
      t.emissive = 0.0;
      return t;
    }
    case Kind::Permafrost: {
      Texel t = stones(seed, u, v, 20, {0.50, 0.49, 0.48}, 0.5, 0.4, 0.8);
      const double frost = smooth(pfbm(seed ^ 0x8, u, v, 8, 3), -0.1, 0.5);
      t.color = mix(t.color, {0.85, 0.87, 0.90}, frost * 0.7);
      t.roughness = 0.75 - 0.2 * frost;
      return t;
    }
    case Kind::Lava: {
      const Worley w = pworley(seed, u * 5, v * 5, 5, 0.9);
      const double crack = 1.0 - smooth(w.f2 - w.f1, 0.0, 0.12);
      const double crust = pfbm(seed ^ 0x3, u, v, 12, 3);
      Texel t;
      t.color = mix({0.12, 0.09, 0.08}, {0.95, 0.35, 0.05}, crack);
      t.color = scale(t.color, 0.85 + 0.3 * crust);
      t.height = clamp01(0.6 - 0.3 * crack + 0.1 * crust);
      t.roughness = 0.75;
      t.emissive = crack;
      return t;
    }
    case Kind::MicrobialMat: {
      const double blob = pfbm(seed, u, v, 5, 4, 0.55);
      const Worley w = pworley(seed ^ 0x5, u * 4, v * 4, 4, 1.0);
      const double rings = 0.5 + 0.5 * std::sin(w.f1 * 22.0);
      const double h = clamp01(0.5 + 0.25 * blob + 0.06 * rings);
      Texel t;
      t.color = mix({0.42, 0.30, 0.38}, {0.55, 0.42, 0.46}, h + 0.2 * rings);
      t.height = h;
      t.roughness = 0.3 + 0.2 * rings;
      t.emissive = 0.0;
      return t;
    }
    case Kind::LichenCrust: {
      Texel t = rock_like(seed, u, v, {0.48, 0.46, 0.42}, 0.0, 0.3, 0.9, 0.4);
      const Worley w = pworley(seed ^ 0x6, u * 9, v * 9, 9, 1.0);
      const double lobe = smooth(0.62 + 0.15 * pfbm(seed ^ 0x2, u, v, 30, 2) - w.f1, 0.0, 0.2);
      const Rgb lichen = mix({0.62, 0.62, 0.42}, {0.42, 0.50, 0.30}, w.id);
      t.color = mix(t.color, lichen, lobe * 0.9);
      t.height = clamp01(t.height + 0.08 * lobe);
      return t;
    }
    case Kind::Crystal: {
      // Faceted pyramids on a cellular lattice; emissive at the tips.
      const Worley w = pworley(seed, u * 7, v * 7, 7, 0.95);
      const double dx = std::fabs(w.cx - u * 7);
      const double dy = std::fabs(w.cy - v * 7);
      const double facet = 1.0 - std::max(dx, dy) * 1.9;  // square pyramid
      const double h = clamp01(0.25 + 0.75 * clamp01(facet));
      Texel t;
      t.color = mix({0.30, 0.42, 0.50}, {0.80, 0.90, 0.95}, h * (0.6 + 0.4 * w.id));
      t.height = h;
      t.roughness = 0.12;
      t.emissive = smooth(h, 0.55, 0.95);
      return t;
    }
    case Kind::Sulfur: {
      const double blob = pfbm(seed, u, v, 6, 4, 0.55, 0.3);
      const double vents = smooth(pfbm(seed ^ 0x8, u, v, 3, 3), 0.45, 0.7);
      Texel t;
      t.color = mix({0.86, 0.76, 0.24}, {0.92, 0.55, 0.16}, smooth(blob, -0.2, 0.5));
      t.color = mix(t.color, {0.25, 0.20, 0.15}, vents);
      t.height = clamp01(0.5 + 0.25 * blob - 0.3 * vents);
      t.roughness = 0.8;
      t.emissive = 0.0;
      return t;
    }
    case Kind::Tholin: {
      Texel t = sand_like(seed, u, v, {0.58, 0.40, 0.23}, 0.2, 0.0);
      t.roughness = 0.97;
      return t;
    }
    case Kind::Slush: {
      const double blob = pfbm(seed, u, v, 6, 4, 0.5);
      const double h = clamp01(0.5 + 0.3 * blob);
      Texel t;
      t.color = mix({0.45, 0.58, 0.66}, {0.70, 0.80, 0.86}, h);
      t.height = h;
      t.roughness = 0.25;
      t.emissive = 0.0;
      return t;
    }
    // --- T0020 urban surfaces ---------------------------------------------
    case Kind::Paving: {
      // Flagstones: an 8x8 grid of slabs with bevelled joints, per-slab
      // tone, a little grime along the joints.
      const int n = 8;
      const double gx = u * n;
      const double gy = v * n;
      const double fx = gx - std::floor(gx);
      const double fy = gy - std::floor(gy);
      const double joint = 0.045;
      const double edge = std::min(std::min(fx, 1.0 - fx), std::min(fy, 1.0 - fy));
      const double bevel = smooth(edge, 0.0, joint * 2.0);
      const double slab = hash01(seed, static_cast<std::int64_t>(std::floor(gx)),
                                 static_cast<std::int64_t>(std::floor(gy)), 0x77);
      const double grain = pfbm(seed ^ 0x3, u, v, 40, 3, 0.5);
      Texel t;
      t.color = mix({0.42, 0.41, 0.39}, {0.56, 0.54, 0.51}, 0.3 + 0.7 * slab);
      t.color = scale(t.color, 0.92 + 0.08 * grain);
      t.color = mix(t.color, {0.22, 0.21, 0.19}, 1.0 - bevel);
      t.height = clamp01(0.55 + 0.35 * bevel + 0.04 * grain);
      t.roughness = 0.72 + 0.15 * (1.0 - bevel);
      t.emissive = 0.0;
      return t;
    }
    case Kind::Plating: {
      // Hex panels with recessed seams and a faint lit seam.
      const Worley w = pworley(seed, u * 6, v * 6, 6, 0.0);
      const double seam = smooth(w.f2 - w.f1, 0.0, 0.035);
      const double brushed = pfbm(seed ^ 0x9, u * 3.0, v, 60, 2, 0.4);
      Texel t;
      t.color = mix({0.36, 0.38, 0.41}, {0.50, 0.53, 0.57}, 0.5 + 0.5 * w.id);
      t.color = scale(t.color, 0.95 + 0.05 * brushed);
      t.color = mix(t.color, {0.10, 0.14, 0.18}, 1.0 - seam);
      t.height = clamp01(0.6 * seam + 0.35);
      t.roughness = 0.35 + 0.2 * (1.0 - seam);
      // A faint lit seam only: at distance the seams average into the
      // panel, so anything stronger reads as a glowing wall.
      t.emissive = (1.0 - seam) * 0.12;
      return t;
    }
    case Kind::Resin: {
      // Smooth amber resin with darker veins and bubbles.
      const double veins = smooth(std::fabs(pfbm(seed, u, v, 5, 4, 0.55)), 0.0, 0.08);
      const Worley w = pworley(seed ^ 0x4, u * 12, v * 12, 12, 1.0);
      const double bubble = smooth(0.18 - w.f1, 0.0, 0.1);
      Texel t;
      t.color = mix({0.52, 0.40, 0.22}, {0.64, 0.52, 0.30}, veins);
      t.color = mix(t.color, {0.30, 0.22, 0.12}, bubble * 0.6);
      t.height = clamp01(0.6 + 0.1 * veins - 0.15 * bubble);
      t.roughness = 0.3;
      t.emissive = 0.0;
      return t;
    }
    case Kind::CrystalFloor: {
      // Low facets on a cellular lattice, faintly lit along the edges.
      const Worley w = pworley(seed, u * 9, v * 9, 9, 0.9);
      const double facet = clamp01(1.0 - (w.f2 - w.f1) * 6.0);
      Texel t;
      t.color = mix({0.42, 0.52, 0.60}, {0.78, 0.86, 0.92}, 0.4 + 0.6 * w.id);
      t.height = clamp01(0.4 + 0.4 * (1.0 - facet));
      t.roughness = 0.15;
      t.emissive = facet * 0.5;
      return t;
    }
    case Kind::Disturbed: {
      Texel t = sand_like(seed, u, v, {0.50, 0.42, 0.32}, 0.35, 0.0);
      const double tracks = smooth(std::fabs(std::sin(v * 6.283185307179586 * 6.0 + 2.0 * pfbm(seed ^ 0x5, u, v, 4, 2))), 0.8, 1.0);
      t.color = mix(t.color, {0.36, 0.30, 0.22}, tracks * 0.5);
      t.height = clamp01(t.height - 0.1 * tracks);
      t.roughness = 0.9;
      return t;
    }
  }
  return rock_like(seed, u, v, {0.5, 0.47, 0.44}, 0.0, 0.5, 0.85, 0.5);
}

}  // namespace

void finish_tile_from_height(Tile& tile, float normal_strength, float roughness_base,
                             float roughness_variation) {
  const std::uint32_t n = tile.size;
  tile.normal.assign(static_cast<std::size_t>(n) * n * 4, 0);
  const auto height_at = [&](std::int64_t x, std::int64_t y) {
    x = ((x % n) + n) % n;
    y = ((y % n) + n) % n;
    return static_cast<double>(tile.albedo[(static_cast<std::size_t>(y) * n + x) * 4 + 3]) /
           255.0;
  };
  // Cheap ambient occlusion: how much the 5-texel neighbourhood rises
  // above the texel.
  for (std::uint32_t y = 0; y < n; ++y) {
    for (std::uint32_t x = 0; x < n; ++x) {
      const double h = height_at(x, y);
      const double dx = (height_at(x + 1, y) - height_at(x - 1, y)) * normal_strength;
      const double dy = (height_at(x, y + 1) - height_at(x, y - 1)) * normal_strength;
      const double len = std::sqrt(dx * dx + dy * dy + 1.0);
      double occl = 0.0;
      for (int r = 2; r <= 10; r += 4) {
        occl += std::max(0.0, height_at(x + r, y) - h) + std::max(0.0, height_at(x - r, y) - h) +
                std::max(0.0, height_at(x, y + r) - h) + std::max(0.0, height_at(x, y - r) - h);
      }
      const double ao = clamp01(1.0 - occl * 0.9);
      const std::size_t i = (static_cast<std::size_t>(y) * n + x) * 4;
      tile.normal[i + 0] = to_byte(0.5 - 0.5 * dx / len);
      tile.normal[i + 1] = to_byte(0.5 - 0.5 * dy / len);
      tile.normal[i + 2] = to_byte(roughness_base + roughness_variation * (0.5 - h));
      tile.normal[i + 3] = tile.emissive ? tile.normal[i + 3] : to_byte(ao);
    }
  }
}

Tile generate_tile(const std::string& material_name, std::uint32_t size, std::uint64_t seed) {
  Tile tile;
  tile.size = size;
  const Kind kind = kind_for(material_name);
  tile.emissive = kind == Kind::Lava || kind == Kind::Crystal || kind == Kind::Plating ||
                  kind == Kind::CrystalFloor;
  const std::size_t count = static_cast<std::size_t>(size) * size;
  tile.albedo.resize(count * 4);
  tile.normal.assign(count * 4, 0);
  std::vector<float> roughness(count);
  double sum[3] = {0.0, 0.0, 0.0};
  const double inv = 1.0 / static_cast<double>(size);
  for (std::uint32_t y = 0; y < size; ++y) {
    for (std::uint32_t x = 0; x < size; ++x) {
      const Texel t = generic(kind, seed, (x + 0.5) * inv, (y + 0.5) * inv);
      const std::size_t i = (static_cast<std::size_t>(y) * size + x);
      tile.albedo[i * 4 + 0] = to_byte(t.color.r);
      tile.albedo[i * 4 + 1] = to_byte(t.color.g);
      tile.albedo[i * 4 + 2] = to_byte(t.color.b);
      tile.albedo[i * 4 + 3] = to_byte(t.height);
      roughness[i] = static_cast<float>(t.roughness);
      tile.normal[i * 4 + 3] = to_byte(t.emissive);
      sum[0] += clamp01(t.color.r);
      sum[1] += clamp01(t.color.g);
      sum[2] += clamp01(t.color.b);
    }
  }
  for (int c = 0; c < 3; ++c) {
    tile.mean_albedo[c] = static_cast<float>(sum[c] / static_cast<double>(count));
  }
  finish_tile_from_height(tile, static_cast<float>(size) * 0.012f, 0.0f, 0.0f);
  // Roughness came per texel from the generator; write it over the
  // placeholder finish_tile wrote.
  for (std::size_t i = 0; i < count; ++i) {
    tile.normal[i * 4 + 2] = to_byte(roughness[i]);
  }
  return tile;
}

const char* const* known_tile_names(std::size_t* count) {
  constexpr std::size_t n = sizeof(kNames) / sizeof(kNames[0]);
  for (std::size_t i = 0; i < n; ++i) {
    kNameList[i] = kNames[i].name;
  }
  *count = n;
  return kNameList;
}

}  // namespace inf::tex
