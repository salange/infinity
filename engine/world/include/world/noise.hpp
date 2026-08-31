#pragma once

#include <cstdint>

#include "core/det/real.hpp"

namespace inf::world {

// Deterministic 3D gradient noise over an integer lattice. Lattice
// gradients come from the sanctioned cheap-mixer path (seeding spec
// section 1): mix64(lattice_key ^ packed cell coords). Quintic (C2)
// interpolation — Perlin 2002. All arithmetic in det::Real basic ops.
// Output roughly in [-1, 1].
det::Real gradient_noise3(std::uint64_t lattice_key, det::Real x, det::Real y, det::Real z);

struct FbmParams {
  int octaves = 5;
  det::Real lacunarity{2.0};
  det::Real gain{0.5};
  // Damping factor for octave 0 (Murray, GDC 2017: the first octave causes
  // the repetitive large-scale cadence — damp it, weight lower octaves).
  det::Real octave0_damp{0.5};
  // Ridge/billow blend ("sharpness"): 0 = smooth fBm, 1 = fully ridged.
  det::Real sharpness{0.0};
};

// Fractal sum with per-octave emphasis. Output roughly in [-1, 1].
det::Real fbm3(std::uint64_t lattice_key, det::Real x, det::Real y, det::Real z,
               const FbmParams& params);

// Domain-warped fBm: p' = p + warp_strength * (vector fBm of p).
det::Real warped_fbm3(std::uint64_t lattice_key, det::Real x, det::Real y, det::Real z,
                      const FbmParams& params, det::Real warp_strength);

// --- analytical derivatives (terrain-stack v2 WP0) -----------------------
// Under statelessness the analytical derivative is the only context
// available (Murray, GDC 2017): erosion end-states, talus operators,
// slope-oriented ravines and slope-driven materials all read del-h rather
// than simulating anything. The value fields below follow the exact same
// arithmetic as their value-only counterparts; the derivatives ride along
// the same lattice fetches (cost < 2x).
struct NoiseD {
  det::Real value;
  det::Real dx, dy, dz;  // partial derivatives in noise-domain units
};

NoiseD gradient_noise3_d(std::uint64_t lattice_key, det::Real x, det::Real y, det::Real z);

// Per-octave gradients accumulate scaled by that octave's frequency; the
// ridge blend differentiates through 1 - 2|n| (sign of n flips the slope).
NoiseD fbm3_d(std::uint64_t lattice_key, det::Real x, det::Real y, det::Real z,
              const FbmParams& params);

// Chain rule through the warp Jacobian: with p' = p + s*W(p),
// grad f(p') = (I + s*J_W)^T * (grad f)(p').
NoiseD warped_fbm3_d(std::uint64_t lattice_key, det::Real x, det::Real y, det::Real z,
                     const FbmParams& params, det::Real warp_strength);

}  // namespace inf::world
