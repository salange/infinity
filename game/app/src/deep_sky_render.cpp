#include "deep_sky_render.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <thread>

#include "gen/universe.hpp"

namespace inf::app {
namespace {

constexpr double kParsecM = 3.2615638 * gen::kLightYearM;

// HDR flux of an apparent-magnitude-0 star at the billboard's peak texel.
// Calibrated against the night exposure ceiling (~60-80): m=7 lands just
// above the visibility floor, m=0 blooms.
constexpr double kStarGain = 0.5;

struct V3 {
  double x{0.0}, y{0.0}, z{0.0};
};
V3 operator-(const V3& a, const V3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator+(const V3& a, const V3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator*(const V3& a, double s) { return {a.x * s, a.y * s, a.z * s}; }
double dot(const V3& a, const V3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
double length(const V3& a) { return std::sqrt(dot(a, a)); }
V3 normalize(const V3& a) {
  const double l = length(a);
  return l > 0.0 ? a * (1.0 / l) : V3{1.0, 0.0, 0.0};
}
V3 to_v3(const gen::Dir3& d) {
  return {d.x.to_double(), d.y.to_double(), d.z.to_double()};
}

// Same piecewise blackbody ramp the local suns use (main.cpp star_tint).
void blackbody_tint(double temp_k, float out[3]) {
  struct Stop {
    double temp;
    float r, g, b;
  };
  static constexpr Stop kStops[] = {
      {2500.0, 1.00f, 0.42f, 0.22f}, {3500.0, 1.00f, 0.60f, 0.40f},
      {4500.0, 1.00f, 0.77f, 0.56f}, {5800.0, 1.00f, 0.93f, 0.82f},
      {7000.0, 1.00f, 0.98f, 0.97f}, {8500.0, 0.83f, 0.90f, 1.00f},
      {12000.0, 0.72f, 0.82f, 1.00f}, {30000.0, 0.60f, 0.74f, 1.00f},
  };
  constexpr int kCount = static_cast<int>(sizeof(kStops) / sizeof(kStops[0]));
  if (temp_k <= kStops[0].temp) {
    out[0] = kStops[0].r;
    out[1] = kStops[0].g;
    out[2] = kStops[0].b;
    return;
  }
  for (int i = 1; i < kCount; ++i) {
    if (temp_k <= kStops[i].temp) {
      const float t = static_cast<float>((temp_k - kStops[i - 1].temp) /
                                         (kStops[i].temp - kStops[i - 1].temp));
      out[0] = kStops[i - 1].r + t * (kStops[i].r - kStops[i - 1].r);
      out[1] = kStops[i - 1].g + t * (kStops[i].g - kStops[i - 1].g);
      out[2] = kStops[i - 1].b + t * (kStops[i].b - kStops[i - 1].b);
      return;
    }
  }
  out[0] = kStops[kCount - 1].r;
  out[1] = kStops[kCount - 1].g;
  out[2] = kStops[kCount - 1].b;
}

// Astrophoto direction (Sascha, 2026-09-01): push saturation rather than
// muting it — the sky should read like the long exposure, not the eye.
void saturate_tint(float c[3], float amount) {
  const float grey = 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
  for (int i = 0; i < 3; ++i) {
    c[i] = std::clamp(grey + (c[i] - grey) * amount, 0.0f, 1.0f);
  }
}

// --- WP2: the star catalog ------------------------------------------------

struct CatalogStar {
  V3 dir;
  double apparent_mag;
  double temperature_k;
  float phase;
};

struct CatalogBuild {
  const gen::GalaxyOctree* octree{nullptr};
  V3 eye;
  double mag_limit{6.5};
  std::vector<CatalogStar> stars;
  std::size_t cells{0};
};

double apparent_from_absolute(double abs_mag, double dist_m) {
  const double d_pc = std::max(dist_m / kParsecM, 1.0e-6);
  return abs_mag + 5.0 * std::log10(d_pc) - 5.0;
}

void collect_cell(CatalogBuild& build, const gen::GalaxyOctree::CellId& cell) {
  ++build.cells;
  const double half = 0.5 * build.octree->cell_size_m(cell.level);
  const V3 center = to_v3(build.octree->cell_center_m(cell));
  const double dist = length(center - build.eye);
  const double half_diag = half * 1.7320508;
  // Refine while the cell subtends a large angle from the eye: distant
  // cells are counted whole (luminous_count), near cells are enumerated.
  if (half_diag > 0.35 * dist && cell.level < gen::GalaxyOctree::kMaxLevel) {
    for (int c = 0; c < 8; ++c) {
      collect_cell(build, {cell.x * 2 + (c & 1), cell.y * 2 + ((c >> 1) & 1),
                           cell.z * 2 + ((c >> 2) & 1), cell.level + 1});
    }
    return;
  }
  const double near_dist = std::max(dist - half_diag, 0.05 * gen::kLightYearM);
  const double abs_limit =
      build.mag_limit - 5.0 * std::log10(near_dist / kParsecM) + 5.0;
  if (abs_limit < -12.0) {
    return;  // nothing in the luminosity function is that bright
  }
  const std::uint32_t count =
      build.octree->luminous_count(cell, det::Real(abs_limit));
  const std::uint32_t capped = count > 4096U ? 4096U : count;
  for (std::uint32_t i = 0; i < capped; ++i) {
    const gen::GalaxyOctree::StarSummary star =
        build.octree->luminous_star(cell, det::Real(abs_limit), i);
    const V3 rel = to_v3(star.position_m) - build.eye;
    const double d = length(rel);
    if (d < 0.1 * gen::kLightYearM) {
      continue;  // the local system's own sun is rendered live
    }
    const double m_app = apparent_from_absolute(star.abs_mag.to_double(), d);
    if (m_app > build.mag_limit) {
      continue;
    }
    CatalogStar out;
    out.dir = rel * (1.0 / d);
    out.apparent_mag = m_app;
    out.temperature_k = star.temperature_k.to_double();
    // Cheap decorrelated phase for atmospheric twinkle.
    const std::uint64_t h =
        (static_cast<std::uint64_t>(cell.x * 73856093 ^ cell.y * 19349663 ^
                                    cell.z * 83492791) +
         i * 2654435761ULL);
    out.phase = static_cast<float>((h % 4096) / 4096.0);
    build.stars.push_back(out);
  }
}

}  // namespace

std::vector<float> build_star_field_mesh(const gen::GalaxyOctree& octree,
                                         const gen::Dir3& eye_m,
                                         double apparent_mag_limit,
                                         std::size_t max_stars,
                                         StarCatalogStats* stats) {
  CatalogBuild build;
  build.octree = &octree;
  build.eye = to_v3(eye_m);
  build.mag_limit = apparent_mag_limit;
  build.stars.reserve(max_stars);
  collect_cell(build, {0, 0, 0, 0});
  std::sort(build.stars.begin(), build.stars.end(),
            [](const CatalogStar& a, const CatalogStar& b) {
              return a.apparent_mag < b.apparent_mag;
            });
  if (build.stars.size() > max_stars) {
    build.stars.resize(max_stars);
  }
  if (stats != nullptr) {
    stats->star_count = build.stars.size();
    stats->cells_visited = build.cells;
    stats->brightest_apparent_mag =
        build.stars.empty() ? 99.0 : build.stars.front().apparent_mag;
  }

  std::vector<float> mesh;
  mesh.reserve(build.stars.size() * 6 * 8);
  static constexpr float kCorners[6][2] = {{-1, -1}, {1, -1}, {1, 1},
                                           {-1, -1}, {1, 1},  {-1, 1}};
  for (const CatalogStar& star : build.stars) {
    const double flux = kStarGain * std::pow(10.0, -0.4 * star.apparent_mag);
    // Brighter stars get slightly larger quads; the shader's gaussian and
    // the bloom pass do the rest.
    const float rel_size = static_cast<float>(
        std::clamp(1.55 - 0.11 * star.apparent_mag, 0.7, 2.4));
    float tint[3];
    blackbody_tint(star.temperature_k, tint);
    saturate_tint(tint, 1.5f);
    const float packed =
        static_cast<float>(static_cast<int>(tint[0] * 255.0f) * 65536 +
                           static_cast<int>(tint[1] * 255.0f) * 256 +
                           static_cast<int>(tint[2] * 255.0f));
    for (const auto& corner : kCorners) {
      mesh.insert(mesh.end(),
                  {static_cast<float>(star.dir.x), static_cast<float>(star.dir.y),
                   static_cast<float>(star.dir.z), corner[0] * rel_size,
                   corner[1] * rel_size, static_cast<float>(flux), packed,
                   star.phase});
    }
  }
  return mesh;
}

// --- WP3: the cube-map bake -----------------------------------------------

namespace {

// Visual-only value noise/fbm on ray directions (nebula and cluster
// internal structure; never feeds generation).
double hash3(double x, double y, double z) {
  double f = std::sin(x * 127.1 + y * 311.7 + z * 74.7) * 43758.5453;
  return f - std::floor(f);
}
double vnoise(const V3& p) {
  const double ix = std::floor(p.x), iy = std::floor(p.y), iz = std::floor(p.z);
  const double fx = p.x - ix, fy = p.y - iy, fz = p.z - iz;
  const double wx = fx * fx * (3.0 - 2.0 * fx);
  const double wy = fy * fy * (3.0 - 2.0 * fy);
  const double wz = fz * fz * (3.0 - 2.0 * fz);
  auto n = [&](double dx, double dy, double dz) {
    return hash3(ix + dx, iy + dy, iz + dz);
  };
  const double x00 = n(0, 0, 0) + wx * (n(1, 0, 0) - n(0, 0, 0));
  const double x10 = n(0, 1, 0) + wx * (n(1, 1, 0) - n(0, 1, 0));
  const double x01 = n(0, 0, 1) + wx * (n(1, 0, 1) - n(0, 0, 1));
  const double x11 = n(0, 1, 1) + wx * (n(1, 1, 1) - n(0, 1, 1));
  const double y0 = x00 + wy * (x10 - x00);
  const double y1 = x01 + wy * (x11 - x01);
  return y0 + wz * (y1 - y0);
}
double fbm(V3 p, int octaves) {
  double value = 0.0;
  double amp = 0.5;
  for (int i = 0; i < octaves; ++i) {
    value += amp * vnoise(p);
    p = p * 2.03 + V3{17.3, 9.1, 4.7};
    amp *= 0.5;
  }
  return value;
}

std::uint16_t to_half(float value) {
  std::uint32_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  const std::uint32_t sign = (bits >> 16) & 0x8000U;
  std::int32_t exponent = static_cast<std::int32_t>((bits >> 23) & 0xFF) - 127 + 15;
  std::uint32_t mantissa = bits & 0x7FFFFFU;
  if (exponent <= 0) {
    if (exponent < -10) {
      return static_cast<std::uint16_t>(sign);
    }
    mantissa |= 0x800000U;
    const std::uint32_t shift = static_cast<std::uint32_t>(14 - exponent);
    return static_cast<std::uint16_t>(sign | (mantissa >> shift));
  }
  if (exponent >= 31) {
    return static_cast<std::uint16_t>(sign | 0x7BFFU);  // clamp to max half
  }
  return static_cast<std::uint16_t>(sign | (static_cast<std::uint32_t>(exponent) << 10) |
                                    (mantissa >> 13));
}

// Texel (face, x, y) -> unit direction: the exact inverse of the
// shader's cube_face_uv.
V3 face_dir(int face, double u, double v) {
  switch (face) {
    case 0: return normalize({1.0, u, v});
    case 1: return normalize({-1.0, -u, v});
    case 2: return normalize({-u, 1.0, v});
    case 3: return normalize({u, -1.0, v});
    case 4: return normalize({v, u, 1.0});
    default: return normalize({-v, u, -1.0});
  }
}

// One bounded deep-sky splat: a nebula, cluster, or (kind 7) an external
// galaxy impostor projected to a cone.
struct Splat {
  V3 dir;
  double cos_bound{1.0};   // cone half-angle (with margin)
  double ang_radius{0.0};  // radians
  float color[3]{1.0f, 1.0f, 1.0f};
  double intensity{0.0};
  double noise_seed{0.0};
  int kind{0};  // 0 emission, 1 reflection, 2 dark, 3 planetary, 4 snr,
                // 5 open cluster, 6 globular, 7 external galaxy
  // kind 7 only: apparent ellipse (tangent-plane axes in radians) plus
  // the bulge tint; `sub` is the GalaxyType.
  V3 axis_a, axis_b;
  double inv_ang_a{0.0}, inv_ang_b{0.0};
  float color2[3]{1.0f, 1.0f, 1.0f};
  int sub{0};
};

double splat_profile(const Splat& s, const V3& dir, double x) {
  // x = angle / angular radius in [0, 1]; per-kind radial profile times
  // per-kind structure noise on the ray direction.
  const V3 np = dir * (14.0 / std::max(s.ang_radius, 0.02)) * 0.35 +
                V3{s.noise_seed, s.noise_seed * 1.7, s.noise_seed * 0.6};
  switch (s.kind) {
    case 0: {  // emission: filamentary wisps
      const double n = fbm(np, 4);
      const double body = std::exp(-x * x * 3.2);
      return body * (0.35 + 1.9 * std::pow(std::max(n * 1.5 - 0.35, 0.0), 1.6));
    }
    case 1: {  // reflection: soft, hugging the source
      const double n = fbm(np, 3);
      return std::exp(-x * x * 4.5) * (0.5 + 0.9 * n);
    }
    case 2: {  // dark: dense blotting core, ragged edge
      const double n = fbm(np, 4);
      return std::exp(-x * x * 2.6) * std::clamp(0.55 + 0.8 * n, 0.0, 1.3);
    }
    case 3: {  // planetary: a thin teal ring
      const double ring = std::exp(-(x - 0.72) * (x - 0.72) * 40.0);
      const double core = std::exp(-x * x * 30.0) * 0.7;
      return ring + core;
    }
    case 4: {  // supernova remnant: shell + filaments
      const double n = fbm(np * 2.0, 4);
      const double shell = std::exp(-(x - 0.85) * (x - 0.85) * 24.0);
      return shell * (0.3 + 2.2 * std::pow(std::max(n * 1.6 - 0.5, 0.0), 1.8));
    }
    case 5: {  // open cluster: soft glow + unresolved speckle
      const double n = vnoise(np * 4.0);
      return std::exp(-x * x * 4.0) * (0.6 + 0.8 * std::pow(n, 6.0) * 4.0);
    }
    default: {  // globular: steep core over a broad halo
      return std::exp(-x * x * 30.0) * 3.0 + std::exp(-x * x * 5.0) * 0.6;
    }
  }
}

}  // namespace

SkyBakeResult bake_deep_sky(const gen::GalaxyDensity& density,
                            const gen::NebulaField& nebulae,
                            const gen::StarClusterField& clusters,
                            const core::Seed128& seed, const SkyView& view,
                            std::uint32_t face_size, int thread_count) {
  SkyBakeResult result;
  result.face_size = face_size;
  const std::size_t texels = static_cast<std::size_t>(face_size) * face_size;
  for (int face = 0; face < 6; ++face) {
    result.luminance_half[face].assign(texels, 0);
    result.chroma_rgba[face].assign(texels * 4, 0);
  }

  const gen::Dir3& eye_m = view.eye_m;
  const V3 eye = to_v3(eye_m);
  const V3 sun_dir = normalize(to_v3(view.sun_dir));
  const V3 ecl_normal = normalize(to_v3(view.ecliptic_normal));
  const double galaxy_r = density.radius_m().to_double();

  // --- gather the bounded splats around the eye -------------------------
  const double gather_r = std::min(0.35 * galaxy_r, 18.0e3 * gen::kLightYearM);
  std::vector<Splat> splats;
  {
    std::vector<gen::Nebula> found;
    nebulae.nebulae_in_ball(eye_m, det::Real(gather_r), &found);
    for (const gen::Nebula& nebula : found) {
      const V3 rel = to_v3(nebula.center_m) - eye;
      const double d = length(rel);
      if (d < nebula.radius_m.to_double() * 1.2 || d <= 0.0) {
        continue;  // inside/too close: no far-field projection
      }
      Splat s;
      s.dir = rel * (1.0 / d);
      s.ang_radius = std::asin(std::min(nebula.radius_m.to_double() / d, 0.999));
      if (s.ang_radius < 0.004) {
        continue;  // sub-texel at 512^2
      }
      s.cos_bound = std::cos(std::min(s.ang_radius * 1.15 + 0.01, 1.5));
      s.kind = static_cast<int>(nebula.type);
      for (int c = 0; c < 3; ++c) {
        s.color[c] = static_cast<float>(nebula.color[c].to_double());
      }
      saturate_tint(s.color, 1.6f);
      // Fixed surface-brightness scale: nearer nebulae are LARGER, not
      // brighter (extended sources), so intensity is per-solid-angle.
      const double op = nebula.opacity.to_double();
      s.intensity = s.kind == 2 ? std::min(0.85, 0.5 + 0.6 * op) : 2.4e-3 * (0.4 + op);
      s.noise_seed = static_cast<double>(nebula.shape_seed % 8192U);
      splats.push_back(s);
    }
    std::vector<gen::StarCluster> found_clusters;
    clusters.clusters_in_ball(eye_m, det::Real(gather_r), &found_clusters);
    const int n_globular = clusters.globular_count();
    for (int gi = 0; gi < n_globular; ++gi) {
      found_clusters.push_back(clusters.globular(gi));
    }
    for (const gen::StarCluster& cluster : found_clusters) {
      const V3 rel = to_v3(cluster.center_m) - eye;
      const double d = length(rel);
      if (d < cluster.radius_m.to_double() * 2.0 || d <= 0.0) {
        continue;
      }
      Splat s;
      s.dir = rel * (1.0 / d);
      s.ang_radius = std::asin(std::min(cluster.radius_m.to_double() / d, 0.999));
      if (s.ang_radius < 0.003) {
        continue;
      }
      s.cos_bound = std::cos(std::min(s.ang_radius * 1.1 + 0.008, 1.5));
      s.kind = cluster.globular ? 6 : 5;
      const float warm = cluster.globular ? 1.0f : 0.92f;
      s.color[0] = warm;
      s.color[1] = cluster.globular ? 0.93f : 0.96f;
      s.color[2] = cluster.globular ? 0.78f : 1.0f;
      // Total light scales with membership; spread over the solid angle.
      const double stars_total = std::max(cluster.star_count.to_double(), 20.0);
      s.intensity = 6.0e-5 * std::sqrt(stars_total);
      s.noise_seed = static_cast<double>(cluster.seed % 8192U);
      splats.push_back(s);
    }

    // WP4: the home cluster's neighbour galaxies as impostor splats —
    // type-driven shape from galaxy-params/v1 macros only, no stars, no
    // structure (the M31 deal: a small, faint, extended smudge, with a
    // handful of close ones as showpieces). Surface brightness is
    // distance-independent (extended sources), so intensity is constant
    // per solid angle and distance only sets the apparent size.
    {
      const core::Key cluster_key = gen::home_cluster_key(seed);
      const std::uint32_t n_gal = gen::galaxy_count_in_cluster(cluster_key);
      std::vector<Splat> gals;
      for (std::uint32_t gi = 1; gi < n_gal; ++gi) {
        const V3 rel = to_v3(gen::galaxy_position_in_cluster(cluster_key, gi)) - eye;
        const double d = length(rel);
        if (d <= 0.0) {
          continue;
        }
        const gen::GalaxyParams gp =
            gen::derive_galaxy_params(gen::galaxy_key_in_cluster(seed, 0, 0, 0, gi));
        const double radius_m = gp.diameter_ly.to_double() * 0.5 * gen::kLightYearM;
        const double ang = std::asin(std::min(radius_m / d, 0.85));
        if (ang < 0.005) {
          continue;  // sub-texel smudge
        }
        Splat s;
        s.dir = rel * (1.0 / d);
        s.kind = 7;
        s.sub = static_cast<int>(gp.type);
        s.ang_radius = ang;
        s.cos_bound = std::cos(std::min(ang * 1.4 + 0.01, 1.5));
        // Deterministic random orientation from the index.
        std::uint64_t h = (static_cast<std::uint64_t>(gi) + 0x9e3779b97f4a7c15ULL);
        h ^= h >> 30;
        h *= 0xbf58476d1ce4e5b9ULL;
        h ^= h >> 27;
        const double az = static_cast<double>(h & 0xFFFFU) / 65536.0 * 6.2831853;
        const double cz = static_cast<double>((h >> 16) & 0xFFFFU) / 32768.0 - 1.0;
        const double sz = std::sqrt(std::max(0.0, 1.0 - cz * cz));
        const V3 spin{sz * std::cos(az), sz * std::sin(az), cz};
        V3 a{spin.y * s.dir.z - spin.z * s.dir.y, spin.z * s.dir.x - spin.x * s.dir.z,
             spin.x * s.dir.y - spin.y * s.dir.x};
        const double al = length(a);
        a = al > 1.0e-6 ? a * (1.0 / al) : V3{-s.dir.y, s.dir.x, 0.0};
        const V3 b{s.dir.y * a.z - s.dir.z * a.y, s.dir.z * a.x - s.dir.x * a.z,
                   s.dir.x * a.y - s.dir.y * a.x};
        // Apparent minor axis: discs flatten with inclination (|spin.dir|
        // face-on = round), ellipticals keep their intrinsic c/a.
        const double face_on = std::abs(dot(spin, s.dir));
        double flat = 0.18 + 0.82 * face_on;
        if (gp.type == gen::GalaxyType::Elliptical) {
          flat = gp.ellipticity.to_double();
        }
        s.axis_a = a;
        s.axis_b = b;
        s.inv_ang_a = 1.0 / ang;
        s.inv_ang_b = 1.0 / (ang * std::max(flat, 0.12));
        // Disc tint cool, bulge/elliptical tint warm; irregulars blue.
        const bool disc_type = gp.type == gen::GalaxyType::Spiral ||
                               gp.type == gen::GalaxyType::Barred ||
                               gp.type == gen::GalaxyType::Lenticular;
        if (gp.type == gen::GalaxyType::Elliptical) {
          s.color[0] = 1.0f; s.color[1] = 0.84f; s.color[2] = 0.66f;
        } else if (disc_type) {
          s.color[0] = 0.78f; s.color[1] = 0.85f; s.color[2] = 1.0f;
        } else {
          s.color[0] = 0.70f; s.color[1] = 0.82f; s.color[2] = 1.0f;
        }
        s.color2[0] = 1.0f; s.color2[1] = 0.87f; s.color2[2] = 0.70f;
        s.intensity = 2.2e-3;
        s.noise_seed = static_cast<double>(h % 8192U);
        gals.push_back(s);
      }
      // Cap the impostor count on apparent size — the far tail is texel
      // noise that costs cone tests without reading as anything.
      std::sort(gals.begin(), gals.end(), [](const Splat& x, const Splat& y) {
        return x.ang_radius > y.ang_radius;
      });
      if (gals.size() > 48) {
        gals.resize(48);
      }
      splats.insert(splats.end(), gals.begin(), gals.end());
    }
  }

  // --- the ray march ----------------------------------------------------
  // Log-spaced steps from half a light-year out to the far side of the
  // galaxy: near dust dominates extinction, the far core dominates light.
  constexpr int kSteps = 64;
  const double t0 = 0.5 * gen::kLightYearM;
  const double eye_r = length(eye);
  // Exit distance of a bounding sphere at 1.15x the galaxy radius.
  const double bound = 1.15 * galaxy_r + eye_r;
  const double log_ratio = std::log(bound / t0) / kSteps;
  const double step_ratio = std::exp(log_ratio);

  // Emission/extinction normalization: the model's units are Msun per
  // game-metre^3, so absolute gains would be meaningless. Instead march
  // ONE reference ray — from this galaxy's home vantage toward (and
  // through) the core — and scale so that unextincted column reads as a
  // fixed HDR brightness and a fixed optical depth. Per-galaxy constant:
  // every system in a galaxy shares it, the band genuinely brightens as
  // you travel coreward, and different galaxies differ naturally.
  double star_column_ref = 0.0;
  double dust_column_ref = 0.0;
  {
    const V3 home = to_v3(gen::home_system_position_m(density.params()));
    const V3 toward_core = normalize(home * -1.0);
    double t = t0;
    for (int i = 0; i < kSteps; ++i) {
      const double t_next = t * step_ratio;
      const double dl = t_next - t;
      const V3 p = home + toward_core * (t * std::sqrt(step_ratio));
      const gen::Dir3 pd{det::Real(p.x), det::Real(p.y), det::Real(p.z)};
      star_column_ref += density.stars(pd).to_double() * dl;
      dust_column_ref += density.dust(pd).to_double() * dl;
      t = t_next;
    }
  }
  // Dust: normalize the through-core optical depth to 4.5 at a NOMINAL
  // spiral opacity of 0.9, scaled by this galaxy's own opacity. The
  // opacity in the numerator undoes the cancellation from dividing by
  // the galaxy's own dust column, so type identity survives: a spiral
  // rift carves tau 3-6 (hiding the bulge glare the way the real Great
  // Rift does), a lenticular's thin dust barely smudges (tau < 1).
  const double opacity = density.params().dust_opacity.to_double();
  const double dust_gain =
      dust_column_ref > 0.0 ? 6.0 * (opacity / 0.9) / dust_column_ref : 0.0;
  // Emission normalizes on the EXTINGUISHED reference — what a sky ray
  // actually delivers with that dust in the way. (Normalizing on the raw
  // column buried the whole band under e^-tau: in-plane rays all carry
  // tau >> 1, so the visible band is the near-field light in front of
  // the rift, an order of magnitude below the raw integral.)
  // Reference ray for brightness: TANGENT to the core direction, in the
  // disc plane — the band's typical brightness, not its core maximum.
  // (Normalizing on the core ray buried the rest of the band: the core's
  // extinguished column is still ~10x the in-plane average, so pegging
  // it left everything else near black.)
  double extinguished_ref = 0.0;
  {
    const V3 home = to_v3(gen::home_system_position_m(density.params()));
    const V3 tangent = normalize({-home.y, home.x, 0.0});
    double t = t0;
    double trans = 1.0;
    for (int i = 0; i < kSteps; ++i) {
      const double t_next = t * step_ratio;
      const double dl = t_next - t;
      const V3 p = home + tangent * (t * std::sqrt(step_ratio));
      const gen::Dir3 pd{det::Real(p.x), det::Real(p.y), det::Real(p.z)};
      extinguished_ref += density.stars(pd).to_double() * dl * trans;
      trans *= std::exp(-density.dust(pd).to_double() * dust_gain * dl);
      t = t_next;
    }
  }
  // The typical in-plane band lands ~2 decades above the WP1 space floor
  // — deliberately dramatic (the astrophoto direction, Sascha
  // 2026-09-01); at the night exposure ceiling it reads as a vivid
  // luminous river, and the core climbs another decade above it.
  const double emission_gain =
      extinguished_ref > 0.0 ? 6.0e-3 / extinguished_ref : 0.0;
  // Chromatic extinction: blue scatters out first, so deep dust lanes
  // go rusty — the astrophoto look.
  const double kDustRGB[3] = {0.72, 1.0, 1.42};

  std::atomic<int> next_row{0};
  const int total_rows = 6 * static_cast<int>(face_size);

  auto worker = [&] {
    std::vector<float> row_rgb(static_cast<std::size_t>(face_size) * 3);
    for (;;) {
      const int row = next_row.fetch_add(1);
      if (row >= total_rows) {
        return;
      }
      const int face = row / static_cast<int>(face_size);
      const int y = row % static_cast<int>(face_size);
      const double v = 2.0 * (y + 0.5) / face_size - 1.0;
      for (std::uint32_t x = 0; x < face_size; ++x) {
        const double u = 2.0 * (x + 0.5) / face_size - 1.0;
        const V3 dir = face_dir(face, u, v);
        double emission[3] = {0.0, 0.0, 0.0};
        double trans[3] = {1.0, 1.0, 1.0};
        double t = t0;
        float pop_tint[3] = {1.0f, 0.95f, 0.9f};
        for (int i = 0; i < kSteps; ++i) {
          const double t_next = t * step_ratio;
          const double dl = t_next - t;
          const double tm = t * std::sqrt(step_ratio);  // log-midpoint
          const V3 p = eye + dir * tm;
          const gen::Dir3 pd{det::Real(p.x), det::Real(p.y), det::Real(p.z)};
          const double rho = density.stars(pd).to_double();
          if (rho > 0.0) {
            if ((i & 3) == 0) {
              const gen::ColorTemp pop = density.population(pd);
              blackbody_tint(pop.temperature_k.to_double(), pop_tint);
              saturate_tint(pop_tint, 1.7f);
            }
            const double tau_l = density.dust(pd).to_double() * dust_gain * dl;
            const double e = rho * dl * emission_gain;
            for (int c = 0; c < 3; ++c) {
              emission[c] += e * pop_tint[c] * trans[c];
              trans[c] *= std::exp(-tau_l * kDustRGB[c]);
            }
          }
          t = t_next;
          if (trans[1] < 1.0e-3 && tm > 0.5 * galaxy_r) {
            break;
          }
        }
        // Bounded splats: dark nebulae eat the band, everything else adds.
        for (const Splat& s : splats) {
          const double cos_a = dot(dir, s.dir);
          if (cos_a < s.cos_bound) {
            continue;
          }
          if (s.kind == 7) {
            // External galaxy (WP4): apparent ellipse in the tangent
            // plane (small-angle projection), exponential disc + compact
            // warm bulge; ellipticals get a rounder Sersic-ish falloff,
            // irregulars a clumpy blob. No stars, no arms — an impostor.
            const V3 off{dir.x - s.dir.x * cos_a, dir.y - s.dir.y * cos_a,
                         dir.z - s.dir.z * cos_a};
            const double xa = dot(off, s.axis_a) * s.inv_ang_a;
            const double xb = dot(off, s.axis_b) * s.inv_ang_b;
            const double e2 = xa * xa + xb * xb;
            if (e2 > 2.0) {
              continue;
            }
            const double e = std::sqrt(e2);
            double body = 0.0;
            double bulge = 0.0;
            if (s.sub == static_cast<int>(gen::GalaxyType::Elliptical)) {
              body = std::exp(-std::pow(e, 0.7) * 3.4);
            } else if (s.sub == static_cast<int>(gen::GalaxyType::Irregular)) {
              const V3 np = dir * (10.0 * s.inv_ang_a * 0.35) +
                            V3{s.noise_seed, s.noise_seed * 1.7, s.noise_seed * 0.6};
              body = std::exp(-e2 * 3.0) * (0.35 + 1.5 * fbm(np, 3));
            } else {
              body = std::exp(-e * 2.8);
              bulge = std::exp(-e2 * 16.0) * 1.4;
            }
            for (int c = 0; c < 3; ++c) {
              emission[c] +=
                  s.intensity * (body * s.color[c] + bulge * s.color2[c]);
            }
            continue;
          }
          const double ang = std::acos(std::min(cos_a, 1.0));
          const double xr = ang / s.ang_radius;
          if (xr > 1.15) {
            continue;
          }
          const double profile = splat_profile(s, dir, xr);
          if (s.kind == 2) {
            const double block = std::clamp(profile * s.intensity, 0.0, 0.96);
            for (int c = 0; c < 3; ++c) {
              // Dust reddens what it does not block outright.
              emission[c] *= 1.0 - block * (c == 2 ? 1.0 : (c == 1 ? 0.92 : 0.78));
            }
          } else {
            for (int c = 0; c < 3; ++c) {
              emission[c] += s.intensity * profile * s.color[c];
            }
          }
        }
        // WP5: zodiacal light — sunlight on interplanetary dust, a warm
        // wedge hugging the arrival planet's orbital plane, brightening
        // toward the sun — and the gegenschein, its faint antisolar
        // counterglow. Both analytic in (elongation, ecliptic latitude).
        {
          const double cs = dot(dir, sun_dir);
          const double sin_beta = std::abs(dot(dir, ecl_normal));
          const double plane = std::exp(-5.0 * sin_beta);
          const double g = 0.5 * (1.0 + cs);
          const double zodiacal =
              5.0e-3 * (0.12 * g * g + 1.6 * std::pow(g, 7.0)) * plane;
          const double gegenschein =
              4.0e-4 * std::pow(std::max(-cs, 0.0), 14.0) * plane;
          const double glow = zodiacal + gegenschein;
          emission[0] += glow * 1.00;
          emission[1] += glow * 0.94;
          emission[2] += glow * 0.82;
        }
        // Contrast curve pivoted at the band reference (chroma
        // preserved): the model's off-plane/in-plane column ratio is
        // only ~5-8x, which reads as a grey wash; ^1.4 stretches it to
        // ~15-20x — black sky, luminous river, blazing core — the
        // astrophoto tone curve, applied once at bake time.
        {
          const double m0 = std::max({emission[0], emission[1], emission[2]});
          if (m0 > 0.0) {
            const double pivot = 6.0e-3;
            const double m1 = pivot * std::pow(m0 / pivot, 1.55);
            const double s = m1 / m0;
            emission[0] *= s;
            emission[1] *= s;
            emission[2] *= s;
          }
        }
        const float r = static_cast<float>(emission[0]);
        const float g = static_cast<float>(emission[1]);
        const float b = static_cast<float>(emission[2]);
        const float m = std::max({r, g, b, 1.0e-9f});
        const std::size_t idx = static_cast<std::size_t>(y) * face_size + x;
        result.luminance_half[face][idx] = to_half(m);
        std::uint8_t* px = &result.chroma_rgba[face][idx * 4];
        px[0] = static_cast<std::uint8_t>(std::clamp(r / m, 0.0f, 1.0f) * 255.0f);
        px[1] = static_cast<std::uint8_t>(std::clamp(g / m, 0.0f, 1.0f) * 255.0f);
        px[2] = static_cast<std::uint8_t>(std::clamp(b / m, 0.0f, 1.0f) * 255.0f);
        px[3] = 255;
      }
    }
  };
  const int n_threads = std::max(1, thread_count);
  std::vector<std::thread> pool;
  pool.reserve(static_cast<std::size_t>(n_threads));
  for (int i = 0; i < n_threads; ++i) {
    pool.emplace_back(worker);
  }
  for (std::thread& th : pool) {
    th.join();
  }
  // Bring-up diagnostics (cheap, once per bake).
  {
    float max_lum = 0.0f;
    double sum = 0.0;
    for (int face = 0; face < 6; ++face) {
      for (std::size_t i = 0; i < texels; ++i) {
        const std::uint16_t h = result.luminance_half[face][i];
        const std::uint32_t bits = (static_cast<std::uint32_t>(h & 0x8000U) << 16) |
                                   (((h >> 10) & 0x1FU) + 112U) << 23 |
                                   (static_cast<std::uint32_t>(h & 0x3FFU) << 13);
        float v;
        std::memcpy(&v, &bits, sizeof(v));
        if ((h & 0x7C00U) == 0) {
          v = 0.0f;  // subnormal halves: near-zero, ignore for stats
        }
        max_lum = std::max(max_lum, v);
        sum += v;
      }
    }
    std::printf(
        "sky bake: %zu splats, emission_gain %.3g dust_gain %.3g "
        "(refs %.3g / ext %.3g), lum max %.4g mean %.4g\n",
        splats.size(), emission_gain, dust_gain, star_column_ref, extinguished_ref,
        max_lum, sum / (6.0 * static_cast<double>(texels)));
  }
  return result;
}

}  // namespace inf::app
