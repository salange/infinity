#pragma once
// HDRI environment → sun light + image-based lighting. Loads a Poly Haven
// equirectangular .hdr, extracts the sun (direction, irradiance) and
// removes it from the map, builds the background cubemap, a GGX-prefiltered
// specular cubemap (mip = roughness) and SH9 irradiance. All on the CPU at
// load, in parallel; no GPU compute needed.
#include <cstdint>
#include <string>
#include <vector>

#include "gpu.hpp"
#include "math.hpp"

namespace cb {

struct Environment {
  Texture background; // RGBA16F cube, mip chain (mip 0 = the sky as loaded)
  Texture specular;   // RGBA16F cube, 6 mips, GGX prefiltered, sun removed
  float sh[9][3]{};   // irradiance SH (already convolved with the cosine lobe)
  Vec3 sun_dir{0, 1, 0}; // toward the sun (world)
  Vec3 sun_color{1, 1,
                 1}; // irradiance (W/m^2-ish in map units) of the sun disc
  Vec3 moon_dir{0, 1, 0};
  Vec3 moon_color{}; // weak secondary irradiance, separately positioned
  Vec2 moon_phase{.60f, -.80f};
  float moon_angular_radius{.017f};
  float night_factor{-1.f}; // negative: use the interactive day/night switch
  bool moon_visible{false};
  bool sun_visible{false};
  float sky_luminance{0.2f}; // mean upper-hemisphere radiance, sun removed
  float lighting_scale{
      1.0f}; // explicit LDR light calibration; background is unchanged
  float exposure{1.0f}; // suggested linear exposure
  // An authored LDR sky has already received its display look. Geometry and
  // reflection lighting still use its separately calibrated linear radiance.
  bool display_referred_background{false};
  bool background_contains_atmosphere{false};
  bool has_sun{false};
  bool ok{false};
};

// Authored LDR maps preserve their pixels and never synthesize a sun disc.
// Explicit scene lighting remains separate from their relative radiance.
// yaw rotates the environment about +Y (radians). specular_size is the
// base mip of the prefiltered cube; background_size the sky cube.
Environment load_environment(Gpu &gpu, const std::string &hdr_path, float yaw,
                             std::uint32_t background_size,
                             std::uint32_t specular_size, bool verbose,
                             bool extract_sun = true);
// A fixed rectilinear sky plate projected into world directions. Its exterior
// blends into an optional cloud panorama above the horizon, or an analytic
// environment. Both sources contain sky only; the lower hemisphere is retained.
Environment load_perspective_environment(Gpu &gpu, const std::string &path,
                                         Vec3 forward, float fov_y,
                                         float aspect,
                                         std::uint32_t background_size,
                                         std::uint32_t specular_size,
                                         bool night = true,
                                         const std::string &cloud_path = {},
                                         float cloud_yaw = 0.f);
Environment make_overcast_environment(Gpu &gpu, std::uint32_t background_size,
                                      std::uint32_t specular_size);
// A neutral analytic sky when no HDRI file exists.
Environment make_analytic_environment(Gpu &gpu, Vec3 sun_dir,
                                      std::uint32_t background_size,
                                      std::uint32_t specular_size);

} // namespace cb
