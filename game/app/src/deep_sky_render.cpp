#include "deep_sky_render.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <thread>

#include "gen/universe.hpp"

namespace inf::app {
namespace {
struct V3 {
  double x{0.0}, y{0.0}, z{0.0};
};
V3 operator-(const V3& a, const V3& b) {
  return {a.x - b.x, a.y - b.y, a.z - b.z};
}
V3 operator+(const V3& a, const V3& b) {
  return {a.x + b.x, a.y + b.y, a.z + b.z};
}
V3 operator*(const V3& a, double s) { return {a.x * s, a.y * s, a.z * s}; }
double dot(const V3& a, const V3& b) {
  return a.x * b.x + a.y * b.y + a.z * b.z;
}
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
      {2500.0, 1.00f, 0.42f, 0.22f},  {3500.0, 1.00f, 0.60f, 0.40f},
      {4500.0, 1.00f, 0.77f, 0.56f},  {5800.0, 1.00f, 0.93f, 0.82f},
      {7000.0, 1.00f, 0.98f, 0.97f},  {8500.0, 0.83f, 0.90f, 1.00f},
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
  std::int32_t exponent =
      static_cast<std::int32_t>((bits >> 23) & 0xFF) - 127 + 15;
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
  return static_cast<std::uint16_t>(
      sign | (static_cast<std::uint32_t>(exponent) << 10) | (mantissa >> 13));
}

}  // namespace

void stellar_tint(double temperature, float rgb[3]) {
  blackbody_tint(temperature, rgb);
  saturate_tint(rgb, 1.5f);
}

GalaxyVolume build_galaxy_volume(const core::Seed128& seed,
                                 const gen::GalaxyParams& galaxy,
                                 std::uint32_t size) {
  GalaxyVolume result;
  result.size = size;
  const gen::GalaxyDensity density(galaxy);
  result.radius_m = density.radius_m().to_double();
  const double radius = result.radius_m;
  const auto coordinate = [size](int index, double warp) {
    return 2.0 * std::sinh((2.0 * index / (size - 1.0) - 1.0) * warp) /
           std::sinh(warp);
  };
  const auto index_at = [size](double coordinate, double warp) {
    return static_cast<int>(std::floor(
        (0.5 + 0.5 * std::asinh(coordinate * 0.5 * std::sinh(warp)) / warp) *
        (size - 1)));
  };
  const V3 home = to_v3(gen::home_system_position_m(galaxy));
  const V3 inward = normalize(home * -1.0);
  const V3 tangent = normalize({-home.y, home.x, 0});
  // Reference normalization is independent of the observer. Integrate once
  // with fixed distances; moving the camera cannot change the galaxy's gain.
  constexpr int steps = 1024;
  const double dl = 4.0 * radius / steps;
  double dust_column = 0;
  for (int i = 0; i < steps; ++i) {
    const V3 p = home + inward * ((i + 0.5) * dl);
    dust_column +=
        density.dust({det::Real(p.x), det::Real(p.y), det::Real(p.z)})
            .to_double() *
        dl;
  }
  const double dust_gain =
      dust_column > 0
          ? 6.0 * (galaxy.dust_opacity.to_double() / 0.9) / dust_column
          : 0;
  double reference = 0, trans = 1;
  for (int i = 0; i < steps; ++i) {
    const V3 p = home + tangent * ((i + 0.5) * dl);
    const gen::Dir3 at{det::Real(p.x), det::Real(p.y), det::Real(p.z)};
    reference += density.stars(at).to_double() * dl * trans;
    trans *= std::exp(-density.dust(at).to_double() * dust_gain * dl);
  }
  const double emission_gain = reference > 0 ? 0.006 / reference : 0;
  const std::size_t count = static_cast<std::size_t>(size) * size * size;
  std::vector<float> field(count * 4);
  std::atomic<int> next{0};
  const auto work = [&] {
    for (;;) {
      const int z = next.fetch_add(1);
      if (z >= static_cast<int>(size)) break;
      for (std::uint32_t y = 0; y < size; ++y) {
        for (std::uint32_t x = 0; x < size; ++x) {
          const gen::Dir3 p{det::Real(coordinate(x, 4) * radius),
                            det::Real(coordinate(y, 4) * radius),
                            det::Real(coordinate(z, 7) * radius)};
          float tint[3];
          blackbody_tint(density.population(p).temperature_k.to_double(), tint);
          saturate_tint(tint, 1.7f);
          const double emission =
              density.stars(p).to_double() * emission_gain * radius;
          const auto offset =
              ((static_cast<std::size_t>(z) * size + y) * size + x) * 4;
          for (int c = 0; c < 3; ++c)
            field[offset + c] = static_cast<float>(emission * tint[c]);
          field[offset + 3] = static_cast<float>(density.dust(p).to_double() *
                                                 dust_gain * radius);
        }
      }
    }
  };
  std::vector<std::thread> pool;
  const auto workers = std::clamp(std::thread::hardware_concurrency(), 2U, 8U);
  for (unsigned i = 0; i < workers; ++i) pool.emplace_back(work);
  for (auto& thread : pool) thread.join();

  // Embed bounded nebulae in the spatial field. The profile is tied to the
  // entity's world position, so it remains present when the observer enters it.
  // Their generator and addresses are shared with the normal system sky.
  gen::NebulaField nebulae(gen::home_galaxy_key(seed), galaxy);
  std::vector<gen::Nebula> clouds;
  nebulae.nebulae_in_ball({}, det::Real(2.0 * radius), &clouds);
  for (const auto& cloud : clouds) {
    const V3 center = to_v3(cloud.center_m) * (1.0 / radius);
    const double r = cloud.radius_m.to_double() / radius;
    if (r <= 0) continue;
    const int lo[3] = {std::max(0, index_at(center.x - r * 2, 4)),
                       std::max(0, index_at(center.y - r * 2, 4)),
                       std::max(0, index_at(center.z - r * 2, 7))};
    const int hi[3] = {
        std::min(static_cast<int>(size) - 1, index_at(center.x + r * 2, 4) + 1),
        std::min(static_cast<int>(size) - 1, index_at(center.y + r * 2, 4) + 1),
        std::min(static_cast<int>(size) - 1,
                 index_at(center.z + r * 2, 7) + 1)};
    for (int z = lo[2]; z <= hi[2]; ++z)
      for (int y = lo[1]; y <= hi[1]; ++y)
        for (int x = lo[0]; x <= hi[0]; ++x) {
          const V3 local =
              (V3{coordinate(x, 4), coordinate(y, 4), coordinate(z, 7)} -
               center) *
              (1.0 / r);
          const double q = dot(local, local);
          if (q >= 4.0) continue;
          const double edge = std::clamp((4.0 - q) / 1.0, 0.0, 1.0);
          const double phase = static_cast<double>(cloud.shape_seed % 8192U);
          const double noise =
              fbm(local * 5.0 + V3{phase, phase * 1.7, phase * .6}, 4);
          double profile = std::exp(-q * 2.6) * (0.5 + 1.2 * noise);
          if (cloud.type == gen::NebulaType::Reflection)
            profile = std::exp(-q * 4.5) * (0.5 + .9 * noise);
          if (cloud.type == gen::NebulaType::Planetary)
            profile = std::exp(-std::pow(std::sqrt(q) - .72, 2) * 40) +
                      .7 * std::exp(-q * 30);
          if (cloud.type == gen::NebulaType::SupernovaRemnant)
            profile = std::exp(-std::pow(std::sqrt(q) - .85, 2) * 24) *
                      (.3 + 2.2 * noise * noise);
          const double shape = profile * edge * edge;
          const auto offset =
              ((static_cast<std::size_t>(z) * size + y) * size + x) * 4;
          const double opacity = cloud.opacity.to_double();
          if (static_cast<int>(cloud.type) == 2) {
            field[offset + 3] +=
                static_cast<float>(shape * (0.5 + 0.6 * opacity) / r);
          } else {
            for (int c = 0; c < 3; ++c)
              field[offset + c] +=
                  static_cast<float>(shape * 0.0024 * (0.4 + opacity) *
                                     cloud.color[c].to_double() / r);
          }
        }
  }
  gen::StarClusterField cluster_field(gen::home_galaxy_key(seed), galaxy);
  std::vector<gen::StarCluster> clusters;
  cluster_field.clusters_in_ball({}, det::Real(2.0 * radius), &clusters);
  for (const auto& cluster : clusters) {
    const V3 center = to_v3(cluster.center_m) * (1.0 / radius);
    const double r = cluster.radius_m.to_double() / radius;
    const int lo[3] = {std::max(0, index_at(center.x - r * 2, 4)),
                       std::max(0, index_at(center.y - r * 2, 4)),
                       std::max(0, index_at(center.z - r * 2, 7))};
    const int hi[3] = {
        std::min(static_cast<int>(size) - 1, index_at(center.x + r * 2, 4) + 1),
        std::min(static_cast<int>(size) - 1, index_at(center.y + r * 2, 4) + 1),
        std::min(static_cast<int>(size) - 1,
                 index_at(center.z + r * 2, 7) + 1)};
    const double intensity =
        6e-5 * std::sqrt(std::max(cluster.star_count.to_double(), 20.0)) / r;
    const double tint[3] = {cluster.globular ? 1.0 : .92,
                            cluster.globular ? .93 : .96,
                            cluster.globular ? .78 : 1.0};
    for (int z = lo[2]; z <= hi[2]; ++z)
      for (int y = lo[1]; y <= hi[1]; ++y)
        for (int x = lo[0]; x <= hi[0]; ++x) {
          const V3 local =
              (V3{coordinate(x, 4), coordinate(y, 4), coordinate(z, 7)} -
               center) *
              (1.0 / r);
          const double q = dot(local, local);
          if (q >= 4) continue;
          const double edge = std::clamp(4.0 - q, 0.0, 1.0);
          const double shape =
              cluster.globular ? 3 * std::exp(-q * 30) + .6 * std::exp(-q * 5)
                               : std::exp(-q * 4);
          const auto offset =
              ((static_cast<std::size_t>(z) * size + y) * size + x) * 4;
          for (int c = 0; c < 3; ++c)
            field[offset + c] +=
                static_cast<float>(shape * intensity * tint[c] * edge * edge);
        }
  }
  result.rgba_half.resize(field.size());
  for (std::size_t i = 0; i < field.size(); ++i)
    result.rgba_half[i] = to_half(field[i]);
  return result;
}
}  // namespace inf::app
