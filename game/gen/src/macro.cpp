#include "gen/macro.hpp"

#include <algorithm>

#include "core/det/trig.hpp"

namespace inf::gen {

using det::Real;

namespace {

Real u01(std::uint64_t word) {
  return Real(static_cast<double>(word >> 11U) * 0x1.0p-53);
}

Real uniform(std::uint64_t word, double lo, double hi) {
  return Real(lo) + Real(hi - lo) * u01(word);
}

// Smooth sigmoid without transcendentals: x / sqrt(1 + x^2). Same shape
// family as tanh, deterministic under the det:: contract.
Real sigmoid(Real x) { return x / det::sqrt(Real(1.0) + x * x); }

// Per-pattern fault spectrum. beta is realized through sqrt chains (libm
// power functions are banned in game/gen by the ci grep gate):
//   Supercontinent  ~ k^-1.25   1/(k*sqrt(sqrt(k)))
//   FewContinents   ~ k^-1.0    1/k
//   Archipelago     ~ k^-0.75   1/(sqrt(k)*sqrt(sqrt(k)))
//   Fractured       ~ k^-0.5    1/sqrt(k)
struct PatternSpec {
  int fault_count;
  double k_lo, k_hi;
  double amplitude_fraction;  // macro sigma / planet radius
};

constexpr PatternSpec kPatternSpecs[4] = {
    /* Supercontinent */ {220, 1.2, 5.0, 0.0028},
    /* FewContinents  */ {300, 2.0, 10.0, 0.0030},
    /* Archipelago    */ {380, 6.0, 28.0, 0.0026},
    /* Fractured      */ {320, 2.0, 14.0, 0.0050},
};

Real spectral_amplitude(MacroPattern pattern, Real k) {
  const Real root = det::sqrt(k);
  const Real quarter = det::sqrt(root);
  switch (pattern) {
    case MacroPattern::Supercontinent: return Real(1.0) / (k * quarter);
    case MacroPattern::FewContinents: return Real(1.0) / k;
    // Flat-ish spectrum: energy spread across many narrow faults is what
    // fragments the land into arcs and island groups.
    case MacroPattern::Archipelago: return Real(1.0) / quarter;
    case MacroPattern::Fractured: return Real(1.0) / root;
  }
  return Real(1.0) / k;
}

// Pattern weights: the section 4.3 table with the water-coupled rows
// removed and renormalized (21/43/29/7) — water is an orthogonal draw.
constexpr int kPatternWeights[4] = {21, 43, 29, 7};

constexpr double kTwoPi = 6.28318530717958647692;

// Fixed solve-direction set: 6 faces x 16 x 16 cell centres. Enough for
// a quantile within well under a percent of land fraction, and cheap
// enough to run inside derive_planet_params.
constexpr std::uint32_t kSolveGrid = 16;

}  // namespace

const char* to_string(MacroPattern pattern) {
  switch (pattern) {
    case MacroPattern::Supercontinent: return "Supercontinent";
    case MacroPattern::FewContinents: return "FewContinents";
    case MacroPattern::Archipelago: return "Archipelago";
    case MacroPattern::Fractured: return "Fractured";
  }
  return "?";
}

det::Real macro_amplitude_fraction(MacroPattern pattern) {
  return Real(kPatternSpecs[static_cast<std::size_t>(pattern)].amplitude_fraction);
}

MacroField::MacroField(const core::Key& body_entity_key) {
  const core::Key macro_key = core::derive_named(body_entity_key, name::MacroV1);

  // Draw 0: the continent pattern.
  const auto d0 = core::draw_point(macro_key, channel::Params, 0, 0, 0);
  {
    int total = 0;
    for (const int weight : kPatternWeights) total += weight;
    int roll = static_cast<int>((d0[0] >> 32U) % static_cast<std::uint32_t>(total));
    for (int i = 0; i < 4; ++i) {
      if (roll < kPatternWeights[i]) {
        pattern_ = static_cast<MacroPattern>(i);
        break;
      }
      roll -= kPatternWeights[i];
    }
  }

  // Draws 1..N: one fault per counter index.
  const PatternSpec& spec = kPatternSpecs[static_cast<std::size_t>(pattern_)];
  faults_.reserve(static_cast<std::size_t>(spec.fault_count));
  for (int i = 0; i < spec.fault_count; ++i) {
    const auto d = core::draw_point(macro_key, channel::Params, i + 1, 0, 0);
    Fault fault;
    // Uniform direction on the sphere: z uniform, azimuth uniform.
    const Real z = uniform(d[0], -1.0, 1.0);
    const Real azimuth = u01(d[1]) * Real(kTwoPi);
    const Real ring = det::sqrt(det::max(Real(0.0), Real(1.0) - z * z));
    Real sin_a(0.0);
    Real cos_a(0.0);
    det::sin_cos(azimuth, &sin_a, &cos_a);
    fault.normal = Dir3{ring * cos_a, ring * sin_a, z};
    fault.sharpness = uniform(d[2], spec.k_lo, spec.k_hi);
    fault.offset = uniform(d[3], -0.55, 0.55);
    const Real magnitude = spectral_amplitude(pattern_, fault.sharpness);
    fault.amplitude = (d[3] & 1U) != 0U ? magnitude : Real(0.0) - magnitude;
    faults_.push_back(fault);
  }

  // Standardize over the fixed solve set, and keep the sorted values for
  // the sea-level quantile.
  std::vector<Real> values;
  values.reserve(6U * kSolveGrid * kSolveGrid);
  for (std::uint8_t face = 0; face < 6; ++face) {
    for (std::uint32_t j = 0; j < kSolveGrid; ++j) {
      for (std::uint32_t i = 0; i < kSolveGrid; ++i) {
        const Real u(-1.0 + 2.0 * (static_cast<double>(i) + 0.5) / kSolveGrid);
        const Real v(-1.0 + 2.0 * (static_cast<double>(j) + 0.5) / kSolveGrid);
        values.push_back(raw_value(face_uv_to_dir(FaceUV{face, u, v})));
      }
    }
  }
  Real sum(0.0);
  for (const Real value : values) sum += value;
  mean_ = sum / Real(static_cast<double>(values.size()));
  Real variance(0.0);
  for (const Real value : values) {
    const Real d = value - mean_;
    variance += d * d;
  }
  variance = variance / Real(static_cast<double>(values.size()));
  const Real sigma = det::sqrt(det::max(variance, Real(1.0e-12)));
  inv_sigma_ = Real(1.0) / sigma;

  solve_sorted_.reserve(values.size());
  for (const Real value : values) {
    solve_sorted_.push_back((value - mean_) * inv_sigma_);
  }
  std::sort(solve_sorted_.begin(), solve_sorted_.end(),
            [](const Real& a, const Real& b) { return a.to_double() < b.to_double(); });
}

Real MacroField::raw_value(const Dir3& unit_dir) const {
  Real sum(0.0);
  for (const Fault& fault : faults_) {
    const Real distance = dot(unit_dir, fault.normal) - fault.offset;
    sum += fault.amplitude * sigmoid(fault.sharpness * distance);
  }
  return sum;
}

Real MacroField::value(const Dir3& unit_dir) const {
  return (raw_value(unit_dir) - mean_) * inv_sigma_;
}

Real MacroField::lattice_value(std::uint8_t face, std::uint32_t ci, std::uint32_t cj) const {
  const auto cells = static_cast<double>(kMacroLatticeCells);
  const Real u(-1.0 + 2.0 * static_cast<double>(ci) / cells);
  const Real v(-1.0 + 2.0 * static_cast<double>(cj) / cells);
  return value(face_uv_to_dir(FaceUV{face, u, v}));
}

Real MacroField::canonical_value(const FaceUV& face_uv, Cache* cache) const {
  const auto lattice = [&](std::uint32_t ci, std::uint32_t cj) {
    if (cache == nullptr) {
      return lattice_value(face_uv.face, ci, cj);
    }
    const std::uint64_t key = (static_cast<std::uint64_t>(face_uv.face) << 40U) |
                              (static_cast<std::uint64_t>(ci) << 20U) | cj;
    const auto it = cache->find(key);
    if (it != cache->end()) {
      return it->second;
    }
    const Real value = lattice_value(face_uv.face, ci, cj);
    cache->emplace(key, value);
    return value;
  };
  const auto cells = static_cast<double>(kMacroLatticeCells);
  const auto locate = [&](Real coord, std::uint32_t* cell, Real* frac) {
    const double scaled = (coord.to_double() + 1.0) * 0.5 * cells;
    double base = scaled < 0.0 ? 0.0 : scaled;
    base = base > cells - 1.0 ? cells - 1.0 : base;
    base = det::floor(Real(base)).to_double();
    *cell = static_cast<std::uint32_t>(base);
    *frac = Real(scaled - base);
  };
  std::uint32_t ci = 0;
  std::uint32_t cj = 0;
  Real fu(0.0);
  Real fv(0.0);
  locate(face_uv.u, &ci, &fu);
  locate(face_uv.v, &cj, &fv);
  const Real p00 = lattice(ci, cj);
  const Real p10 = lattice(ci + 1, cj);
  const Real p01 = lattice(ci, cj + 1);
  const Real p11 = lattice(ci + 1, cj + 1);
  return det::lerp(det::lerp(p00, p10, fu), det::lerp(p01, p11, fu), fv);
}

Real MacroField::solve_sea_level(det::Real land_fraction) const {
  const auto count = solve_sorted_.size();
  if (land_fraction.to_double() >= 0.999) {
    return solve_sorted_.front() - Real(3.0);  // dry: below the global minimum
  }
  if (land_fraction.to_double() <= 0.001) {
    return solve_sorted_.back() + Real(3.0);  // global ocean: above everything
  }
  const double quantile = (1.0 - land_fraction.to_double()) * static_cast<double>(count - 1);
  const double base = det::floor(Real(quantile)).to_double();
  const auto index = static_cast<std::size_t>(base);
  const Real frac(quantile - base);
  const std::size_t next = index + 1 < count ? index + 1 : index;
  return det::lerp(solve_sorted_[index], solve_sorted_[next], frac);
}

}  // namespace inf::gen
