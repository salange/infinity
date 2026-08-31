#include "gen/effective_field.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace inf::gen {

namespace {

using det::Real;
using world::SphereEdit;

// Deterministic ground search near edits: fixed step + fixed bisection
// count, no tolerance loops.
constexpr double kScanStep = 0.5;    // m; edits are meter-scale spheres
constexpr int kBisectSteps = 24;

std::vector<SphereEdit> edits_near(const world::EditStore& store, const Dir3& unit_dir,
                                   double r0) {
  det::Fixed64 center[3] = {det::Fixed64::from_double(unit_dir.x.to_double() * r0),
                            det::Fixed64::from_double(unit_dir.y.to_double() * r0),
                            det::Fixed64::from_double(unit_dir.z.to_double() * r0)};
  // Generous ball: catches every op that could move the ground here.
  return store.overlapping(center, det::Fixed64::from_double(64.0));
}

}  // namespace

det::Real EffectiveField::density(const gen::Dir3& position_m) const {
  const Real base = field_.density(position_m);
  if (edits_ == nullptr || edits_->size() == 0) {
    return base;
  }
  det::Fixed64 center[3] = {det::Fixed64::from_double(position_m.x.to_double()),
                            det::Fixed64::from_double(position_m.y.to_double()),
                            det::Fixed64::from_double(position_m.z.to_double())};
  const auto hits = edits_->overlapping(center, det::Fixed64::from_double(0.0));
  if (hits.empty()) {
    return base;
  }
  return Real(world::apply_edits(base.to_double(), hits, position_m.x.to_double(),
                                 position_m.y.to_double(), position_m.z.to_double()));
}

det::Real EffectiveField::ground_radius_m(const gen::Dir3& unit_dir) const {
  const Real base = field_.ground_radius_m(unit_dir);
  if (edits_ == nullptr || edits_->size() == 0) {
    return base;
  }
  const double r0 = base.to_double();
  const auto hits = edits_near(*edits_, unit_dir, r0);
  if (hits.empty()) {
    return base;
  }

  // Bracket the search along the radial by the ops' reach.
  double r_hi = r0;
  double r_lo = r0;
  const double dx = unit_dir.x.to_double();
  const double dy = unit_dir.y.to_double();
  const double dz = unit_dir.z.to_double();
  for (const SphereEdit& edit : hits) {
    const double proj = edit.center(0).to_double() * dx + edit.center(1).to_double() * dy +
                        edit.center(2).to_double() * dz;
    const double r = edit.radius().to_double();
    if (!edit.subtract) {
      r_hi = std::max(r_hi, proj + r);
    } else {
      r_lo = std::min(r_lo, proj - r);
    }
  }
  r_hi += kScanStep;
  r_lo -= kScanStep;

  const auto effective_at = [&](double r) {
    const gen::Dir3 p{Real(dx * r), Real(dy * r), Real(dz * r)};
    return world::apply_edits(field_.density(p).to_double(), hits, dx * r, dy * r, dz * r);
  };

  // Scan downward from above for the topmost air->solid crossing, then
  // bisect it. Fixed iteration counts keep this bit-deterministic.
  double above = r_hi;
  double value_above = effective_at(above);
  while (above > r_lo && value_above > 0.0) {
    // Started inside solid (an add op above the old ground): climb.
    above += kScanStep;
    value_above = effective_at(above);
    if (above > r_hi + 64.0) {
      return Real(above);  // degenerate; treat as ground
    }
  }
  double solid = r_lo;
  double probe = above;
  bool found = false;
  while (probe > r_lo) {
    probe -= kScanStep;
    const double value = effective_at(probe);
    if (value > 0.0) {
      solid = probe;
      found = true;
      break;
    }
    above = probe;
  }
  if (!found) {
    // Everything in the bracket is air (dug clean through): the ground is
    // the untouched base density below the bracket.
    return Real(r_lo);
  }
  double air = above;
  for (int i = 0; i < kBisectSteps; ++i) {
    const double mid = 0.5 * (solid + air);
    if (effective_at(mid) > 0.0) {
      solid = mid;
    } else {
      air = mid;
    }
  }
  return Real(0.5 * (solid + air));
}

det::Real EffectiveField::ground_radius_below_m(const gen::Dir3& unit_dir,
                                               det::Real from_r) const {
  const Real base = field_.ground_radius_below_m(unit_dir, from_r);
  if (edits_ == nullptr || edits_->size() == 0) {
    return base;
  }
  const auto hits = edits_near(*edits_, unit_dir, base.to_double());
  if (hits.empty()) {
    return base;
  }
  // Scan down from the caller for the first air->solid crossing of the
  // fully composed field (terrain + caves + edit overlay).
  const double dx = unit_dir.x.to_double();
  const double dy = unit_dir.y.to_double();
  const double dz = unit_dir.z.to_double();
  const auto effective_at = [&](double r) {
    const gen::Dir3 p{Real(dx * r), Real(dy * r), Real(dz * r)};
    return world::apply_edits(field_.density(p).to_double(), hits, dx * r, dy * r, dz * r);
  };
  double air = from_r.to_double();
  double solid = air;
  bool found = false;
  for (int step = 1; step <= 200; ++step) {
    solid = from_r.to_double() - kScanStep * static_cast<double>(step);
    if (effective_at(solid) > 0.0) {
      found = true;
      break;
    }
    air = solid;
  }
  if (!found) {
    return base;
  }
  for (int i = 0; i < kBisectSteps; ++i) {
    const double mid = 0.5 * (solid + air);
    if (effective_at(mid) > 0.0) {
      solid = mid;
    } else {
      air = mid;
    }
  }
  return Real(0.5 * (solid + air));
}

}  // namespace inf::gen
