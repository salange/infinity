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

// Closed-form fade derivative: 30 t^2 (t^2 - 2t + 1) = 30 t^2 (t - 1)^2.
Real dfade(Real t) {
  const Real tm1 = t - Real(1.0);
  return Real(30.0) * t * t * tm1 * tm1;
}

// The gradient VECTOR matching grad_dot's case table exactly (the dual
// of each dot product). Any mismatch here is the invisible-sign-error
// trap the brief warns about — keep the two switches in lockstep.
void grad_vec(std::uint64_t hash, Real* gx, Real* gy, Real* gz) {
  Real x(0.0);
  Real y(0.0);
  Real z(0.0);
  switch (hash & 15U) {
    case 0: x = Real(1.0); y = Real(1.0); break;
    case 1: x = Real(-1.0); y = Real(1.0); break;
    case 2: x = Real(1.0); y = Real(-1.0); break;
    case 3: x = Real(-1.0); y = Real(-1.0); break;
    case 4: x = Real(1.0); z = Real(1.0); break;
    case 5: x = Real(-1.0); z = Real(1.0); break;
    case 6: x = Real(1.0); z = Real(-1.0); break;
    case 7: x = Real(-1.0); z = Real(-1.0); break;
    case 8: y = Real(1.0); z = Real(1.0); break;
    case 9: y = Real(-1.0); z = Real(1.0); break;
    case 10: y = Real(1.0); z = Real(-1.0); break;
    case 11: y = Real(-1.0); z = Real(-1.0); break;
    case 12: x = Real(1.0); y = Real(1.0); break;
    case 13: y = Real(-1.0); z = Real(1.0); break;
    case 14: x = Real(-1.0); y = Real(1.0); break;
    default: y = Real(-1.0); z = Real(-1.0); break;
  }
  *gx = x;
  *gy = y;
  *gz = z;
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
    if (params.normalize_octaves == 0 || octave < params.normalize_octaves) {
      total += weight;
    }
    amplitude = amplitude * params.gain;
    frequency = frequency * params.lacunarity;
  }
  return sum / total;
}

NoiseD gradient_noise3_d(std::uint64_t lattice_key, Real x, Real y, Real z) {
  const std::int64_t xi = floor_to_int(x);
  const std::int64_t yi = floor_to_int(y);
  const std::int64_t zi = floor_to_int(z);
  const Real fx = x - Real(static_cast<double>(xi));
  const Real fy = y - Real(static_cast<double>(yi));
  const Real fz = z - Real(static_cast<double>(zi));

  const Real u = fade(fx);
  const Real v = fade(fy);
  const Real w = fade(fz);
  const Real du = dfade(fx);
  const Real dv = dfade(fy);
  const Real dw = dfade(fz);

  // Corner dot products and gradient vectors, in one lattice sweep.
  Real n[2][2][2];
  Real gx[2][2][2];
  Real gy[2][2][2];
  Real gz[2][2][2];
  for (int cz = 0; cz < 2; ++cz) {
    for (int cy = 0; cy < 2; ++cy) {
      for (int cx = 0; cx < 2; ++cx) {
        const std::uint64_t hash = cell_hash(lattice_key, xi + cx, yi + cy, zi + cz);
        n[cz][cy][cx] = grad_dot(hash, fx - Real(static_cast<double>(cx)),
                                 fy - Real(static_cast<double>(cy)),
                                 fz - Real(static_cast<double>(cz)));
        grad_vec(hash, &gx[cz][cy][cx], &gy[cz][cy][cx], &gz[cz][cy][cx]);
      }
    }
  }

  const auto trilerp = [&](const Real values[2][2][2]) {
    const Real x00 = det::lerp(values[0][0][0], values[0][0][1], u);
    const Real x10 = det::lerp(values[0][1][0], values[0][1][1], u);
    const Real x01 = det::lerp(values[1][0][0], values[1][0][1], u);
    const Real x11 = det::lerp(values[1][1][0], values[1][1][1], u);
    const Real y0 = det::lerp(x00, x10, v);
    const Real y1 = det::lerp(x01, x11, v);
    return det::lerp(y0, y1, w);
  };

  // Value plus the interpolation-weight partials (against u, v, w).
  const Real x00 = det::lerp(n[0][0][0], n[0][0][1], u);
  const Real x10 = det::lerp(n[0][1][0], n[0][1][1], u);
  const Real x01 = det::lerp(n[1][0][0], n[1][0][1], u);
  const Real x11 = det::lerp(n[1][1][0], n[1][1][1], u);
  const Real y0 = det::lerp(x00, x10, v);
  const Real y1 = det::lerp(x01, x11, v);
  const Real value = det::lerp(y0, y1, w);

  const Real dVdu = det::lerp(det::lerp(n[0][0][1] - n[0][0][0], n[0][1][1] - n[0][1][0], v),
                              det::lerp(n[1][0][1] - n[1][0][0], n[1][1][1] - n[1][1][0], v),
                              w);
  const Real dVdv = det::lerp(x10 - x00, x11 - x01, w);
  const Real dVdw = y1 - y0;

  // d n_c / d fx is exactly the corner gradient's x component, so the
  // direct term is the trilinear blend of the gradient vectors.
  const Real scale(1.4);
  NoiseD out;
  out.value = value * scale;
  out.dx = (trilerp(gx) + dVdu * du) * scale;
  out.dy = (trilerp(gy) + dVdv * dv) * scale;
  out.dz = (trilerp(gz) + dVdw * dw) * scale;
  return out;
}

NoiseD fbm3_d(std::uint64_t lattice_key, Real x, Real y, Real z, const FbmParams& params) {
  Real sum(0.0);
  Real sum_dx(0.0);
  Real sum_dy(0.0);
  Real sum_dz(0.0);
  Real amplitude(1.0);
  Real total(0.0);
  Real frequency(1.0);
  for (int octave = 0; octave < params.octaves; ++octave) {
    const std::uint64_t octave_key = det::mix64(lattice_key ^ (0xA5A5A5A5A5A5A5A5ULL +
                                                               static_cast<std::uint64_t>(octave)));
    const NoiseD noise =
        gradient_noise3_d(octave_key, x * frequency, y * frequency, z * frequency);
    Real value = noise.value;
    // Derivatives w.r.t. the ORIGINAL coordinates: chain through the
    // octave frequency.
    Real ddx = noise.dx * frequency;
    Real ddy = noise.dy * frequency;
    Real ddz = noise.dz * frequency;
    // Ridge blend 1 - 2|n|: slope flips with the sign of n.
    const Real ridged = Real(1.0) - (det::abs(value) + det::abs(value));
    const Real ridge_sign = value < Real(0.0) ? Real(2.0) : Real(-2.0);
    const Real ridged_dx = ddx * ridge_sign;
    const Real ridged_dy = ddy * ridge_sign;
    const Real ridged_dz = ddz * ridge_sign;
    value = det::lerp(value, ridged, params.sharpness);
    ddx = det::lerp(ddx, ridged_dx, params.sharpness);
    ddy = det::lerp(ddy, ridged_dy, params.sharpness);
    ddz = det::lerp(ddz, ridged_dz, params.sharpness);
    Real weight = amplitude;
    if (octave == 0) {
      weight = weight * params.octave0_damp;
    }
    sum += value * weight;
    sum_dx += ddx * weight;
    sum_dy += ddy * weight;
    sum_dz += ddz * weight;
    if (params.normalize_octaves == 0 || octave < params.normalize_octaves) {
      total += weight;
    }
    amplitude = amplitude * params.gain;
    frequency = frequency * params.lacunarity;
  }
  return NoiseD{sum / total, sum_dx / total, sum_dy / total, sum_dz / total};
}

NoiseD warped_fbm3_d(std::uint64_t lattice_key, Real x, Real y, Real z,
                     const FbmParams& params, Real warp_strength) {
  FbmParams warp_params = params;
  warp_params.octaves = params.octaves > 3 ? 3 : params.octaves;
  warp_params.sharpness = Real(0.0);
  const std::uint64_t warp_key_x = det::mix64(lattice_key ^ 0x57A7157A7157A71ULL);
  const std::uint64_t warp_key_y = det::mix64(lattice_key ^ 0xB0B0B0B0B0B0B0B0ULL);
  const std::uint64_t warp_key_z = det::mix64(lattice_key ^ 0x1234567890ABCDEFULL);
  const NoiseD wx = fbm3_d(warp_key_x, x, y, z, warp_params);
  const NoiseD wy = fbm3_d(warp_key_y, x, y, z, warp_params);
  const NoiseD wz = fbm3_d(warp_key_z, x, y, z, warp_params);
  const Real s = warp_strength;
  const NoiseD f = fbm3_d(lattice_key, x + wx.value * s, y + wy.value * s,
                          z + wz.value * s, params);
  // grad f(p + s W(p)) = (I + s J_W)^T (grad f)(p'):
  // column j of J_W holds dW_i/dx_j, so each output partial gathers the
  // warped-space partials weighted by that column.
  NoiseD out;
  out.value = f.value;
  out.dx = f.dx * (Real(1.0) + s * wx.dx) + f.dy * (s * wy.dx) + f.dz * (s * wz.dx);
  out.dy = f.dx * (s * wx.dy) + f.dy * (Real(1.0) + s * wy.dy) + f.dz * (s * wz.dy);
  out.dz = f.dx * (s * wx.dz) + f.dy * (s * wy.dz) + f.dz * (Real(1.0) + s * wz.dz);
  return out;
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
