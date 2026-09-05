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
  Texture background;  // RGBA16F cube, mip chain (mip 0 = the sky as loaded)
  Texture specular;    // RGBA16F cube, 6 mips, GGX prefiltered, sun removed
  float sh[9][3]{};    // irradiance SH (already convolved with the cosine lobe)
  Vec3 sun_dir{0, 1, 0};   // toward the sun (world)
  Vec3 sun_color{1, 1, 1}; // irradiance (W/m^2-ish in map units) of the sun disc
  float sky_luminance{0.2f}; // mean upper-hemisphere radiance, sun removed
  float exposure{1.0f};      // suggested linear exposure
  bool has_sun{false};
  bool ok{false};
};

// yaw rotates the environment about +Y (radians). specular_size is the
// base mip of the prefiltered cube; background_size the sky cube.
Environment load_environment(Gpu& gpu, const std::string& hdr_path, float yaw,
                             std::uint32_t background_size, std::uint32_t specular_size,
                             bool verbose);
// A neutral analytic sky when no HDRI file exists.
Environment make_analytic_environment(Gpu& gpu, Vec3 sun_dir, std::uint32_t background_size,
                                      std::uint32_t specular_size);

}  // namespace cb
