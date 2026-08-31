#include "gen/caves.hpp"

#include "core/det/trig.hpp"
#include "gen/names.hpp"

namespace inf::gen {

using det::Real;

namespace {

Real u01(std::uint64_t word) {
  return Real(static_cast<double>(word >> 11U) * 0x1.0p-53);
}

double type_base(PlanetType type) {
  switch (type) {
    case PlanetType::Barren: return 0.32;  // lava tubes
    case PlanetType::Desert: return 0.45;  // canyon karst
    case PlanetType::Ice: return 0.30;     // moulins
    case PlanetType::EarthLike:
    default: return 0.0;  // flooded/eroded shallow caves; karst waits for
                          // rock classes in a future caves version
  }
}

double archetype_factor(Archetype archetype) {
  switch (archetype) {
    case Archetype::Highlands: return 1.3;
    case Archetype::Mesas: return 1.4;
    case Archetype::Canyonlands: return 1.6;
    case Archetype::CrevasseField: return 1.5;
    case Archetype::RidgeField: return 1.0;
    case Archetype::Cratered: return 0.7;
    case Archetype::RegolithPlains: return 0.8;
    case Archetype::Dunes: return 0.3;
    case Archetype::GlacialShield: return 0.6;
    default: return 0.0;
  }
}

Dir3 sub(const Dir3& a, const Dir3& b) { return Dir3{a.x - b.x, a.y - b.y, a.z - b.z}; }
Dir3 add(const Dir3& a, const Dir3& b) { return Dir3{a.x + b.x, a.y + b.y, a.z + b.z}; }
Dir3 scale(const Dir3& a, Real s) { return Dir3{a.x * s, a.y * s, a.z * s}; }

// Capsule SDF with linearly varying radius along the segment.
Real capsule_sdf(const Dir3& p, const Dir3& a, const Dir3& b, Real ra, Real rb) {
  const Dir3 ab = sub(b, a);
  const Dir3 ap = sub(p, a);
  const Real len_sq = det::max(dot(ab, ab), Real(1.0e-6));
  const Real t = det::clamp(dot(ap, ab) / len_sq, Real(0.0), Real(1.0));
  const Dir3 closest = add(a, scale(ab, t));
  const Dir3 d = sub(p, closest);
  return det::sqrt(dot(d, d)) - det::lerp(ra, rb, t);
}

}  // namespace

CaveField::CaveField(const core::Key& body_key, const PlanetParams& planet)
    : caves_key_(core::derive_named(body_key, name::CavesV1)),
      provinces_(body_key, planet),
      type_(planet.type),
      cells_per_face_(planet.cells_per_face),
      radius_m_(planet.radius_m) {
  enabled_ = type_base(type_) > 0.0 && cells_per_face_ > 0;
  // Candidate probe offset: a system's whole bound stays within one cell
  // of its anchor as long as the bound is a small fraction of the SMALLEST
  // cell chord (~0.94/N near cube corners); 2.2x converts the chord reach
  // to a uv offset that survives the corner metric compression.
  const double reach_chord = (kBoundCapM + kSminM + 8.0) / planet.radius_m.to_double();
  probe_uv_ = Real(reach_chord * 2.2);
}

CellId CaveField::cell_of(const Dir3& unit_dir) const { return provinces_.cell_of(unit_dir); }

bool CaveField::hosted(const CellId& cell) const {
  if (!enabled_) {
    return false;
  }
  const double p = type_base(type_) * archetype_factor(provinces_.cell_params(cell).archetype);
  if (p <= 0.0) {
    return false;
  }
  const core::Key cell_key =
      core::derive_child(caves_key_, kind::Cave, cell.face, cell.ci, cell.cj);
  const auto draw = core::draw_point(cell_key, channel::Params, 0, 0, 0);
  return u01(draw[0]).to_double() < p;
}

Dir3 CaveField::anchor_dir(const CellId& cell) const {
  const core::Key cell_key =
      core::derive_child(caves_key_, kind::Cave, cell.face, cell.ci, cell.cj);
  const auto draw = core::draw_point(cell_key, channel::Params, 0, 1, 0);
  const Real n(static_cast<double>(cells_per_face_));
  const Real ju = (u01(draw[0]) - Real(0.5)) * Real(0.8);
  const Real jv = (u01(draw[1]) - Real(0.5)) * Real(0.8);
  const Real u = (Real(static_cast<double>(cell.ci)) + Real(0.5) + ju) / n * Real(2.0) - Real(1.0);
  const Real v = (Real(static_cast<double>(cell.cj)) + Real(0.5) + jv) / n * Real(2.0) - Real(1.0);
  return face_uv_to_dir(FaceUV{cell.face, u, v});
}

int CaveField::candidates(const Dir3& unit_dir, CellId out[kMaxCandidates]) const {
  int count = 0;
  const auto push = [&](const CellId& cell) {
    for (int c = 0; c < count; ++c) {
      if (out[c] == cell) {
        return;
      }
    }
    if (count < kMaxCandidates) {
      out[count++] = cell;
    }
  };
  Dir3 t1{};
  Dir3 t2{};
  tangent_basis(unit_dir, &t1, &t2);
  // Owner plus the 8 neighbours at one probe reach: since a system bound
  // is a small fraction of a cell, only cells touching the query's
  // immediate neighbourhood can host a reaching system, and 4 distinct
  // cells meet at any point at most.
  for (int di = -1; di <= 1; ++di) {
    for (int dj = -1; dj <= 1; ++dj) {
      const Real ou = probe_uv_ * Real(static_cast<double>(di));
      const Real ov = probe_uv_ * Real(static_cast<double>(dj));
      const Dir3 probe = Dir3{unit_dir.x + t1.x * ou + t2.x * ov,
                              unit_dir.y + t1.y * ou + t2.y * ov,
                              unit_dir.z + t1.z * ou + t2.z * ov};
      push(cell_of(probe));
    }
  }
  return count;
}

Dir3 CaveField::mouth_probe_dir(const CellId& cell, Real surface_r_anchor) const {
  const System skeleton = build_system(cell, surface_r_anchor, surface_r_anchor);
  if (skeleton.node_count == 0) {
    return anchor_dir(cell);
  }
  // The promoted node is the radially topmost — same rule as the builder.
  int top = 0;
  Real best(-1.0e30);
  for (int i = 0; i < skeleton.node_count; ++i) {
    const Real r = det::sqrt(dot(skeleton.nodes[i], skeleton.nodes[i]));
    if (r > best) {
      best = r;
      top = i;
    }
  }
  return normalize(skeleton.nodes[top]);
}

CaveField::System CaveField::build_system(const CellId& cell, Real surface_r_anchor,
                                          Real surface_r_mouth) const {
  System system;
  if (!hosted(cell)) {
    return system;
  }
  system.hosted = true;
  const core::Key cell_key =
      core::derive_child(caves_key_, kind::Cave, cell.face, cell.ci, cell.cj);
  const auto draw0 = core::draw_point(cell_key, channel::Params, 0, 0, 0);
  const auto draw1 = core::draw_point(cell_key, channel::Params, 0, 1, 0);
  const auto draw2 = core::draw_point(cell_key, channel::Params, 0, 2, 0);

  // Bounds shrink on bodies whose province cells get small (tiny moons):
  // the stencil invariant (bound << cell chord) is kept by construction.
  const double cell_chord_min_m = 0.94 / static_cast<double>(cells_per_face_) *
                                  radius_m_.to_double();
  const Real dim(det::min(Real(1.0), Real(0.1 * cell_chord_min_m / kBoundCapM)));

  system.anchor_dir = anchor_dir(cell);
  const Real depth = (Real(30.0) + Real(120.0) * u01(draw0[1])) * dim;
  const Dir3 anchor = scale(system.anchor_dir, surface_r_anchor - depth);
  system.bound_center = anchor;

  const int node_count =
      8 + static_cast<int>((draw1[2] >> 32U) % static_cast<std::uint64_t>(kMaxNodes - 8 + 1));
  system.node_count = node_count;

  // Persistent heading in the tangent plane (draws below jitter around it)
  // so systems wander instead of balling up around the anchor.
  Dir3 t1{};
  Dir3 t2{};
  tangent_basis(system.anchor_dir, &t1, &t2);
  const Real theta = u01(draw0[3]) * Real(6.28318530717958647692);
  const Real hc = det::cos(theta);
  const Real hs = det::sin(theta);
  const Dir3 heading = add(scale(t1, hc), scale(t2, hs));

  const Real walk_cap = Real(kBoundCapM) * dim * Real(0.55);
  Dir3 pos = anchor;
  for (int i = 0; i < node_count; ++i) {
    if (i > 0) {
      const auto step_draw = core::draw_point(cell_key, channel::Params, i, 3, 0);
      const auto len_draw = core::draw_point(cell_key, channel::Params, i, 4, 0);
      const Dir3 rand3{u01(step_draw[0]) * Real(2.0) - Real(1.0),
                       u01(step_draw[1]) * Real(2.0) - Real(1.0),
                       u01(step_draw[2]) * Real(2.0) - Real(1.0)};
      // Downward and outward bias per the design; jitter keeps it organic.
      const Dir3 down = scale(system.anchor_dir, Real(-0.35));
      Dir3 step_dir = add(add(scale(heading, Real(0.55)), scale(rand3, Real(0.75))), down);
      step_dir = normalize(step_dir);
      const Real step_len = (Real(15.0) + Real(20.0) * u01(len_draw[0])) * dim;
      pos = add(pos, scale(step_dir, step_len));
      // Clamp the walk inside the bound budget...
      const Dir3 rel = sub(pos, anchor);
      const Real rel_len = det::sqrt(dot(rel, rel));
      if (rel_len > walk_cap) {
        pos = add(anchor, scale(rel, walk_cap / rel_len));
      }
      // ...and keep tunnels from surfacing anywhere except the mouth.
      const Real pos_r = det::sqrt(dot(pos, pos));
      const Real ceiling = surface_r_anchor - Real(22.0) * dim;
      if (pos_r > ceiling) {
        pos = scale(pos, ceiling / pos_r);
      }
      system.node_r[i] = (Real(2.0) + Real(8.0) * u01(step_draw[3])) * dim;
    } else {
      system.node_r[i] = (Real(2.0) + Real(8.0) * u01(draw2[3])) * dim;
    }
    system.nodes[i] = pos;
  }

  // Chambers: a few nodes get room-sized radii.
  const int chambers = 1 + static_cast<int>((draw1[3] >> 32U) % 3ULL);
  for (int c = 0; c < chambers; ++c) {
    const int index =
        static_cast<int>((draw2[c] >> 24U) % static_cast<std::uint64_t>(node_count));
    system.node_r[index] = (Real(10.0) + Real(18.0) * u01(draw2[c])) * dim;
  }

  // Mouth: promote the radially topmost node with a capsule extended past
  // the surface at ITS direction — the entrance connects by construction.
  const bool wants_mouth = u01(draw0[2]) < Real(0.7);
  if (wants_mouth) {
    int top = 0;
    Real best(-1.0e30);
    for (int i = 0; i < system.node_count; ++i) {
      const Real r = det::sqrt(dot(system.nodes[i], system.nodes[i]));
      if (r > best) {
        best = r;
        top = i;
      }
    }
    const Dir3 mouth_dir = normalize(system.nodes[top]);
    const Dir3 mouth_top = scale(mouth_dir, surface_r_mouth + Real(45.0) * dim);
    const Real mouth_r = det::min(system.node_r[top] * Real(2.2) + Real(4.0) * dim,
                                  Real(16.0) * dim);
    // Keep the invariant over guaranteed break-through: a mouth that
    // cannot fit the bound cap (terrain climbed away above the anchor)
    // is dropped, deterministically on every client.
    const Dir3 rel = sub(mouth_top, anchor);
    const Real need = det::sqrt(dot(rel, rel)) + mouth_r + Real(kSminM + 8.0);
    if (need <= Real(kBoundCapM) * dim) {
      system.has_mouth = true;
      system.mouth_top = mouth_top;
      system.mouth_r = mouth_r;
    }
  }

  // Bounding sphere over every capsule plus the blend radius.
  Real bound(0.0);
  for (int i = 0; i < system.node_count; ++i) {
    const Dir3 rel = sub(system.nodes[i], anchor);
    bound = det::max(bound, det::sqrt(dot(rel, rel)) + system.node_r[i]);
  }
  if (system.has_mouth) {
    const Dir3 rel = sub(system.mouth_top, anchor);
    bound = det::max(bound, det::sqrt(dot(rel, rel)) + system.mouth_r);
  }
  system.bound_m = bound + Real(kSminM + 4.0);
  return system;
}

int CaveField::radial_intervals(const System& system, const Dir3& unit_dir, double* lo_out,
                                double* hi_out, int max_out) {
  if (!system.hosted) {
    return 0;
  }
  int count = 0;
  const auto sphere = [&](const Dir3& center, Real radius) {
    if (count >= max_out) {
      return;
    }
    const Real along = dot(center, unit_dir);
    const Dir3 off{center.x - unit_dir.x * along, center.y - unit_dir.y * along,
                   center.z - unit_dir.z * along};
    const Real cover = radius + Real(kSminM + 8.0);
    const Real lat_sq = dot(off, off);
    if (lat_sq >= cover * cover) {
      return;
    }
    const Real half = det::sqrt(cover * cover - lat_sq);
    lo_out[count] = (along - half).to_double();
    hi_out[count] = (along + half).to_double();
    ++count;
  };
  for (int i = 0; i < system.node_count; ++i) {
    sphere(system.nodes[i], system.node_r[i]);
  }
  if (system.has_mouth) {
    int top = 0;
    Real best(-1.0e30);
    for (int i = 0; i < system.node_count; ++i) {
      const Real r = det::sqrt(dot(system.nodes[i], system.nodes[i]));
      if (r > best) {
        best = r;
        top = i;
      }
    }
    // Cover the mouth capsule with a few spheres along its axis.
    for (int s = 0; s <= 3; ++s) {
      const Real t(static_cast<double>(s) / 3.0);
      const Dir3 p = add(system.nodes[top],
                         scale(sub(system.mouth_top, system.nodes[top]), t));
      sphere(p, det::max(system.node_r[top], system.mouth_r));
    }
  }
  return count;
}

Real CaveField::system_sdf(const System& system, const Dir3& position_m) {
  const Dir3 rel = sub(position_m, system.bound_center);
  if (dot(rel, rel) >= system.bound_m * system.bound_m) {
    return Real(1.0e6);
  }
  Real d(1.0e6);
  for (int i = 0; i + 1 < system.node_count; ++i) {
    const Real seg = capsule_sdf(position_m, system.nodes[i], system.nodes[i + 1],
                                 system.node_r[i], system.node_r[i + 1]);
    d = smin(d, seg, Real(kSminM));
  }
  if (system.has_mouth) {
    int top = 0;
    Real best(-1.0e30);
    for (int i = 0; i < system.node_count; ++i) {
      const Real r = det::sqrt(dot(system.nodes[i], system.nodes[i]));
      if (r > best) {
        best = r;
        top = i;
      }
    }
    const Real mouth = capsule_sdf(position_m, system.nodes[top], system.mouth_top,
                                   system.node_r[top], system.mouth_r);
    d = smin(d, mouth, Real(kSminM));
  }
  return d;
}

}  // namespace inf::gen
