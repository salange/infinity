#include "world/noise.hpp"

#include "core/det/mix.hpp"

namespace inf::world {

using det::Real;

namespace {

// Lattice cell hash: derived key XOR mixed packed coordinates (usage rule
// in core/det/mix.hpp). Coordinates are separated into disjoint bit fields
// before mixing so (x, y, z) permutations don't collide.
std::uint64_t cell_hash(std::uint64_t lattice_key, std::int64_t x, std::int64_t y,
                        std::int64_t z) {
  const std::uint64_t packed = (static_cast<std::uint64_t>(x) * 0x9E3779B97F4A7C15ULL) ^
                               (static_cast<std::uint64_t>(y) * 0xC2B2AE3D27D4EB4FULL) ^
                               (static_cast<std::uint64_t>(z) * 0x165667B19E3779F9ULL);
  return det::mix64(lattice_key ^ det::mix64(packed));
}

// Gradient from one of 12 edge directions (Perlin 2002 distribution).
Real grad_dot(std::uint64_t hash, Real dx, Real dy, Real dz) {
  switch (hash & 15U) {
    case 0: return dx + dy;
    case 1: return -dx + dy;
    case 2: return dx - dy;
    case 3: return -dx - dy;
    case 4: return dx + dz;
    case 5: return -dx + dz;
    case 6: return dx - dz;
    case 7: return -dx - dz;
    case 8: return dy + dz;
    case 9: return -dy + dz;
    case 10: return dy - dz;
    case 11: return -dy - dz;
    case 12: return dx + dy;
    case 13: return -dy + dz;
    case 14: return -dx + dy;
    default: return -dy - dz;
  }
}

// C2-continuous quintic fade (Perlin 2002).
Real fade(Real t) {
  return t * t * t * (t * (t * Real(6.0) - Real(15.0)) + Real(10.0));
}

std::int64_t floor_to_int(Real v) {
  return static_cast<std::int64_t>(det::floor(v).to_double());
}

}  // namespace

Real gradient_noise3(std::uint64_t lattice_key, Real x, Real y, Real z) {
  const std::int64_t xi = floor_to_int(x);
  const std::int64_t yi = floor_to_int(y);
  const std::int64_t zi = floor_to_int(z);
  const Real fx = x - Real(static_cast<double>(xi));
  const Real fy = y - Real(static_cast<double>(yi));
  const Real fz = z - Real(static_cast<double>(zi));

  const Real u = fade(fx);
  const Real v = fade(fy);
  const Real w = fade(fz);

  auto corner = [&](int cx, int cy, int cz) {
    const std::uint64_t hash = cell_hash(lattice_key, xi + cx, yi + cy, zi + cz);
    return grad_dot(hash, fx - Real(static_cast<double>(cx)), fy - Real(static_cast<double>(cy)),
                    fz - Real(static_cast<double>(cz)));
  };

  const Real x00 = det::lerp(corner(0, 0, 0), corner(1, 0, 0), u);
  const Real x10 = det::lerp(corner(0, 1, 0), corner(1, 1, 0), u);
  const Real x01 = det::lerp(corner(0, 0, 1), corner(1, 0, 1), u);
  const Real x11 = det::lerp(corner(0, 1, 1), corner(1, 1, 1), u);
  const Real y0 = det::lerp(x00, x10, v);
  const Real y1 = det::lerp(x01, x11, v);
  // Scale toward [-1, 1]: gradient dot products reach ~0.7 amplitude.
  return det::lerp(y0, y1, w) * Real(1.4);
}

Real fbm3(std::uint64_t lattice_key, Real x, Real y, Real z, const FbmParams& params) {
  Real sum(0.0);
  Real amplitude(1.0);
  Real total(0.0);
  Real frequency(1.0);
  for (int octave = 0; octave < params.octaves; ++octave) {
    // Distinct lattice per octave: fold the octave index into the key.
    const std::uint64_t octave_key = det::mix64(lattice_key ^ (0xA5A5A5A5A5A5A5A5ULL +
                                                               static_cast<std::uint64_t>(octave)));
    Real value = gradient_noise3(octave_key, x * frequency, y * frequency, z * frequency);
    // Sharpness: blend toward ridged (1 - 2|n|), which peaks at lattice
    // zero-crossings — sharp crests instead of smooth bumps.
    const Real ridged = Real(1.0) - (det::abs(value) + det::abs(value));
    value = det::lerp(value, ridged, params.sharpness);
    Real weight = amplitude;
    if (octave == 0) {
      weight = weight * params.octave0_damp;
    }
    sum += value * weight;
    total += weight;
    amplitude = amplitude * params.gain;
    frequency = frequency * params.lacunarity;
  }
  return sum / total;
}

Real warped_fbm3(std::uint64_t lattice_key, Real x, Real y, Real z, const FbmParams& params,
                 Real warp_strength) {
  FbmParams warp_params = params;
  warp_params.octaves = params.octaves > 3 ? 3 : params.octaves;
  warp_params.sharpness = Real(0.0);
  const std::uint64_t warp_key_x = det::mix64(lattice_key ^ 0x57A7157A7157A71ULL);
  const std::uint64_t warp_key_y = det::mix64(lattice_key ^ 0xB0B0B0B0B0B0B0B0ULL);
  const std::uint64_t warp_key_z = det::mix64(lattice_key ^ 0x1234567890ABCDEFULL);
  const Real wx = fbm3(warp_key_x, x, y, z, warp_params) * warp_strength;
  const Real wy = fbm3(warp_key_y, x, y, z, warp_params) * warp_strength;
  const Real wz = fbm3(warp_key_z, x, y, z, warp_params) * warp_strength;
  return fbm3(lattice_key, x + wx, y + wy, z + wz, params);
}

}  // namespace inf::world
