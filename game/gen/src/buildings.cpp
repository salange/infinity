#include "gen/buildings.hpp"

#include <algorithm>
#include <cmath>

#include "core/det/mix.hpp"
#include "gen/material.hpp"
#include "gen/names.hpp"

namespace inf::gen {

namespace {

constexpr double kPi = 3.14159265358979323846;

// --- keyed draws for one lot -------------------------------------------------
// A small deterministic stream seeded from ONE Philox draw of the lot
// key; never crosses the lot boundary (seeding spec section 3: local
// sequential streams inside one algorithm invocation are permitted).
class Rng {
 public:
  explicit Rng(const core::Key& lot_key) {
    const auto d = core::draw_point(lot_key, channel::Layout, 0, 0, 0);
    state_ = d[0] ^ (d[1] << 1U);
  }
  double u01() {
    state_ = det::mix64(state_ + 0x9E3779B97F4A7C15ULL);
    return static_cast<double>(state_ >> 11U) * 0x1.0p-53;
  }
  double uniform(double lo, double hi) { return lo + (hi - lo) * u01(); }
  int pick(int count) { return static_cast<int>(u01() * count) % count; }
  bool chance(double p) { return u01() < p; }

 private:
  std::uint64_t state_;
};

// --- geometry sink --------------------------------------------------------------
struct Sink {
  BuildingMesh* mesh;
  void vertex(double x, double y, double z, double nx, double ny, double nz, int slot) {
    auto& v = mesh->vertices;
    v.push_back(static_cast<float>(x));
    v.push_back(static_cast<float>(y));
    v.push_back(static_cast<float>(z));
    v.push_back(static_cast<float>(nx));
    v.push_back(static_cast<float>(ny));
    v.push_back(static_cast<float>(nz));
    v.push_back(static_cast<float>(slot));
    if (z > mesh->top_z) mesh->top_z = static_cast<float>(z);
  }
  void tri(const double a[3], const double b[3], const double c[3], int slot) {
    // Normal from the winding (counter-clockwise seen from outside).
    const double ux = b[0] - a[0], uy = b[1] - a[1], uz = b[2] - a[2];
    const double vx = c[0] - a[0], vy = c[1] - a[1], vz = c[2] - a[2];
    double nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
    const double len = std::sqrt(nx * nx + ny * ny + nz * nz);
    if (len < 1e-12) return;
    nx /= len; ny /= len; nz /= len;
    vertex(a[0], a[1], a[2], nx, ny, nz, slot);
    vertex(b[0], b[1], b[2], nx, ny, nz, slot);
    vertex(c[0], c[1], c[2], nx, ny, nz, slot);
    ++mesh->triangle_count;
  }
  void quad(const double a[3], const double b[3], const double c[3], const double d[3], int slot) {
    tri(a, b, c, slot);
    tri(a, c, d, slot);
  }
};

// A box scope: centre (cx, cy), yaw, half sizes, base z and height.
struct Scope {
  double cx{0.0}, cy{0.0};
  double yaw{0.0};
  double hx{4.0}, hy{4.0};
  double z0{0.0};
  double h{6.0};
};

// Local (u along the yaw x axis, v along y) to site coordinates.
void scope_point(const Scope& s, double u, double v, double z, double out[3]) {
  const double c = std::cos(s.yaw);
  const double sn = std::sin(s.yaw);
  out[0] = s.cx + u * c - v * sn;
  out[1] = s.cy + u * sn + v * c;
  out[2] = z;
}

// Solid box (six faces, outward). `top` false skips the roof face.
void emit_box(Sink& sink, const Scope& s, int wall_slot, int top_slot, bool top = true, bool bottom = false) {
  const double z1 = s.z0 + s.h;
  double p[8][3];
  scope_point(s, -s.hx, -s.hy, s.z0, p[0]);
  scope_point(s, s.hx, -s.hy, s.z0, p[1]);
  scope_point(s, s.hx, s.hy, s.z0, p[2]);
  scope_point(s, -s.hx, s.hy, s.z0, p[3]);
  scope_point(s, -s.hx, -s.hy, z1, p[4]);
  scope_point(s, s.hx, -s.hy, z1, p[5]);
  scope_point(s, s.hx, s.hy, z1, p[6]);
  scope_point(s, -s.hx, s.hy, z1, p[7]);
  sink.quad(p[0], p[1], p[5], p[4], wall_slot);  // -v face (front)
  sink.quad(p[1], p[2], p[6], p[5], wall_slot);  // +u
  sink.quad(p[2], p[3], p[7], p[6], wall_slot);  // +v
  sink.quad(p[3], p[0], p[4], p[7], wall_slot);  // -u
  if (top) sink.quad(p[4], p[5], p[6], p[7], top_slot);
  if (bottom) sink.quad(p[3], p[2], p[1], p[0], top_slot);
}

// A regular n-gon prism (frustum when r1 != r0), optional tilt of the axis.
void emit_prism(Sink& sink, double cx, double cy, double z0, double r0, double r1, double h,
                int n, double phase, int wall_slot, int top_slot, bool top = true,
                double tilt_x = 0.0, double tilt_y = 0.0) {
  std::vector<double> bottom(static_cast<std::size_t>(n) * 3);
  std::vector<double> upper(static_cast<std::size_t>(n) * 3);
  for (int i = 0; i < n; ++i) {
    const double a = phase + 2.0 * kPi * i / n;
    bottom[i * 3] = cx + std::cos(a) * r0;
    bottom[i * 3 + 1] = cy + std::sin(a) * r0;
    bottom[i * 3 + 2] = z0;
    upper[i * 3] = cx + std::cos(a) * r1 + tilt_x * h;
    upper[i * 3 + 1] = cy + std::sin(a) * r1 + tilt_y * h;
    upper[i * 3 + 2] = z0 + h;
  }
  for (int i = 0; i < n; ++i) {
    const int j = (i + 1) % n;
    sink.quad(&bottom[i * 3], &bottom[j * 3], &upper[j * 3], &upper[i * 3], wall_slot);
  }
  if (top && r1 > 0.01) {
    const double c[3] = {cx + tilt_x * h, cy + tilt_y * h, z0 + h};
    for (int i = 0; i < n; ++i) {
      const int j = (i + 1) % n;
      sink.tri(c, &upper[i * 3], &upper[j * 3], top_slot);
    }
  } else if (top) {
    const double c[3] = {cx + tilt_x * h, cy + tilt_y * h, z0 + h};
    for (int i = 0; i < n; ++i) {
      const int j = (i + 1) % n;
      sink.tri(c, &bottom[i * 3], &bottom[j * 3], top_slot);  // a cone
    }
  }
}

// Hemisphere / dome (latitude rings) of radius r on z0, squashed by
// `squash` in z; rings x segments.
void emit_dome(Sink& sink, double cx, double cy, double z0, double r, double squash, int rings,
               int segs, int slot) {
  for (int ring = 0; ring < rings; ++ring) {
    const double t0 = 0.5 * kPi * ring / rings;
    const double t1 = 0.5 * kPi * (ring + 1) / rings;
    const double r0 = r * std::cos(t0), z0r = z0 + r * squash * std::sin(t0);
    const double r1 = r * std::cos(t1), z1r = z0 + r * squash * std::sin(t1);
    for (int i = 0; i < segs; ++i) {
      const double a0 = 2.0 * kPi * i / segs;
      const double a1 = 2.0 * kPi * (i + 1) / segs;
      const double p0[3] = {cx + std::cos(a0) * r0, cy + std::sin(a0) * r0, z0r};
      const double p1[3] = {cx + std::cos(a1) * r0, cy + std::sin(a1) * r0, z0r};
      const double p2[3] = {cx + std::cos(a1) * r1, cy + std::sin(a1) * r1, z1r};
      const double p3[3] = {cx + std::cos(a0) * r1, cy + std::sin(a0) * r1, z1r};
      if (ring == rings - 1) {
        const double apex[3] = {cx, cy, z0 + r * squash};
        sink.tri(p0, p1, apex, slot);
      } else {
        sink.quad(p0, p1, p2, p3, slot);
      }
    }
  }
}

// --- the part library (the instanced terminal set) ---------------------------
// Parts are emitted into a facade frame: origin on the wall, `along` the
// wall direction, `out` the outward normal, z up. Each part is a pure
// function of its parameters; a loaded kit part would replace the body
// of one of these functions.
struct Frame {
  double ox, oy, oz;    // origin
  double ax, ay;        // unit along-wall direction
  double nx, ny;        // unit outward normal
  void at(double u, double d, double z, double out[3]) const {
    out[0] = ox + ax * u + nx * d;
    out[1] = oy + ay * u + ny * d;
    out[2] = oz + z;
  }
};

// Window bay: a glass pane just proud of the wall, framed by a sill and
// a lintel ledge that project 16 cm out — the wall is a solid face, so a
// recessed pane would be hidden; the ledges give the bay its shadow line.
void part_window(Sink& sink, const Frame& f, double u0, double w, double z0, double h, int glass_slot,
                 int trim_slot, bool broken) {
  const double proud = 0.04;
  const double ledge = 0.16;
  const double lip = 0.10;
  double a[3], b[3], c[3], d[3];
  if (!broken) {
    f.at(u0, proud, z0, a);
    f.at(u0 + w, proud, z0, b);
    f.at(u0 + w, proud, z0 + h, c);
    f.at(u0, proud, z0 + h, d);
    sink.quad(a, b, c, d, glass_slot);
  }
  // Sill: a ledge below the pane (top face, front face, underside).
  const double su0 = u0 - 0.12;
  const double sw = w + 0.24;
  f.at(su0, 0.0, z0, a);
  f.at(su0 + sw, 0.0, z0, b);
  f.at(su0 + sw, ledge, z0, c);
  f.at(su0, ledge, z0, d);
  sink.quad(a, b, c, d, trim_slot);
  f.at(su0, ledge, z0 - lip, a);
  f.at(su0 + sw, ledge, z0 - lip, b);
  f.at(su0 + sw, ledge, z0, c);
  f.at(su0, ledge, z0, d);
  sink.quad(a, b, c, d, trim_slot);
  f.at(su0, 0.0, z0 - lip, a);
  f.at(su0 + sw, 0.0, z0 - lip, b);
  f.at(su0 + sw, ledge, z0 - lip, c);
  f.at(su0, ledge, z0 - lip, d);
  sink.quad(d, c, b, a, trim_slot);
  // Lintel: the same ledge above the pane, lip upward.
  const double z1 = z0 + h;
  f.at(su0, 0.0, z1, a);
  f.at(su0 + sw, 0.0, z1, b);
  f.at(su0 + sw, ledge, z1, c);
  f.at(su0, ledge, z1, d);
  sink.quad(d, c, b, a, trim_slot);
  f.at(su0, ledge, z1, a);
  f.at(su0 + sw, ledge, z1, b);
  f.at(su0 + sw, ledge, z1 + lip, c);
  f.at(su0, ledge, z1 + lip, d);
  sink.quad(a, b, c, d, trim_slot);
  f.at(su0, 0.0, z1 + lip, a);
  f.at(su0 + sw, 0.0, z1 + lip, b);
  f.at(su0 + sw, ledge, z1 + lip, c);
  f.at(su0, ledge, z1 + lip, d);
  sink.quad(a, b, c, d, trim_slot);
}

// Door: a recessed dark panel with a step.
void part_door(Sink& sink, const Frame& f, double u0, double w, double h, int panel_slot, int trim_slot) {
  double a[3], b[3], c[3], d[3];
  f.at(u0, -0.3, 0.0, a);
  f.at(u0 + w, -0.3, 0.0, b);
  f.at(u0 + w, -0.3, h, c);
  f.at(u0, -0.3, h, d);
  sink.quad(a, b, c, d, panel_slot);
  f.at(u0, 0.0, h, a);
  f.at(u0 + w, 0.0, h, b);
  f.at(u0 + w, -0.3, h, c);
  f.at(u0, -0.3, h, d);
  sink.quad(d, c, b, a, trim_slot);
  f.at(u0 - 0.15, 0.0, 0.0, a);
  f.at(u0 + w + 0.15, 0.0, 0.0, b);
  f.at(u0 + w + 0.15, 0.6, 0.0, c);
  f.at(u0 - 0.15, 0.6, 0.0, d);
  double a2[3], b2[3], c2[3], d2[3];
  f.at(u0 - 0.15, 0.0, 0.18, a2);
  f.at(u0 + w + 0.15, 0.0, 0.18, b2);
  f.at(u0 + w + 0.15, 0.6, 0.18, c2);
  f.at(u0 - 0.15, 0.6, 0.18, d2);
  sink.quad(a2, b2, c2, d2, trim_slot);  // step top
  sink.quad(b, c, c2, b2, trim_slot);
  sink.quad(c, d, d2, c2, trim_slot);
  sink.quad(d, a, a2, d2, trim_slot);
}

// Balcony: a slab and a railing box.
void part_balcony(Sink& sink, const Frame& f, double u0, double w, double z0, int slab_slot, int rail_slot) {
  const double depth = 1.3;
  double a[3], b[3], c[3], d[3], a2[3], b2[3], c2[3], d2[3];
  f.at(u0, 0.0, z0, a);
  f.at(u0 + w, 0.0, z0, b);
  f.at(u0 + w, depth, z0, c);
  f.at(u0, depth, z0, d);
  f.at(u0, 0.0, z0 + 0.15, a2);
  f.at(u0 + w, 0.0, z0 + 0.15, b2);
  f.at(u0 + w, depth, z0 + 0.15, c2);
  f.at(u0, depth, z0 + 0.15, d2);
  sink.quad(a2, b2, c2, d2, slab_slot);
  sink.quad(d, c, b, a, slab_slot);
  sink.quad(b, c, c2, b2, slab_slot);
  sink.quad(c, d, d2, c2, slab_slot);
  sink.quad(d, a, a2, d2, slab_slot);
  // Railing: a thin wall along the outer edge and the two sides.
  const double rz = z0 + 0.15;
  const double rh = 1.0;
  double r0[3], r1[3], r2[3], r3[3];
  f.at(u0, depth - 0.06, rz, r0);
  f.at(u0 + w, depth - 0.06, rz, r1);
  f.at(u0 + w, depth - 0.06, rz + rh, r2);
  f.at(u0, depth - 0.06, rz + rh, r3);
  sink.quad(r0, r1, r2, r3, rail_slot);
  sink.quad(r3, r2, r1, r0, rail_slot);
}

// Conduit: a pipe (square section) along the wall at height z.
void part_conduit(Sink& sink, const Frame& f, double u0, double u1, double z, double size, int slot) {
  double p[8][3];
  f.at(u0, 0.0, z - 0.5 * size, p[0]);
  f.at(u1, 0.0, z - 0.5 * size, p[1]);
  f.at(u1, size, z - 0.5 * size, p[2]);
  f.at(u0, size, z - 0.5 * size, p[3]);
  f.at(u0, 0.0, z + 0.5 * size, p[4]);
  f.at(u1, 0.0, z + 0.5 * size, p[5]);
  f.at(u1, size, z + 0.5 * size, p[6]);
  f.at(u0, size, z + 0.5 * size, p[7]);
  sink.quad(p[1], p[2], p[6], p[5], slot);
  sink.quad(p[2], p[3], p[7], p[6], slot);
  sink.quad(p[3], p[0], p[4], p[7], slot);
  sink.quad(p[4], p[5], p[6], p[7], slot);
  sink.quad(p[3], p[2], p[1], p[0], slot);
}

// Antenna mast on a roof point: a thin tall box with a cross arm.
void part_antenna(Sink& sink, double x, double y, double z0, double h, int slot) {
  Scope mast{x, y, 0.0, 0.08, 0.08, z0, h};
  emit_box(sink, mast, slot, slot);
  Scope arm{x, y, 0.0, 0.6, 0.05, z0 + h * 0.8, 0.1};
  emit_box(sink, arm, slot, slot);
}

// --- palettes ----------------------------------------------------------------------

}  // namespace

void building_palette(const StyleVector& style, std::uint8_t out[4]) {
  const auto m = [](Material id) { return static_cast<std::uint8_t>(id); };
  switch (style.material_family) {
    case 1: out[0] = m(Material::Plating); out[1] = m(Material::RockShale); out[2] = m(Material::WindowGlass); out[3] = m(Material::Paving); break;
    case 2: out[0] = m(Material::ResinFloor); out[1] = m(Material::SoilDry); out[2] = m(Material::ResinFloor); out[3] = m(Material::RockSandstone); break;
    case 3: out[0] = m(Material::CrystalField); out[1] = m(Material::CrystalFloor); out[2] = m(Material::CrystalFloor); out[3] = m(Material::IceSheet); break;
    case 4: out[0] = m(Material::Moss); out[1] = m(Material::LichenCrust); out[2] = m(Material::MicrobialMat); out[3] = m(Material::ForestFloor); break;
    default: out[0] = m(Material::RockSandstone); out[1] = m(Material::RockShale); out[2] = m(Material::WindowGlass); out[3] = m(Material::Paving); break;
  }
  if (style.faction_type == FactionType::Outlaw) {
    out[0] = m(Material::ScrapMetal);
    out[1] = m(Material::ScrapMetal);
    out[3] = m(Material::RockShale);
  }
  if (style.faction_type == FactionType::AlignedMachine || style.faction_type == FactionType::RenegadeMachine) {
    out[0] = m(Material::Plating);
    out[1] = m(Material::Plating);
  }
  if (style.ruined) {
    out[0] = m(Material::RockShale);
    out[1] = m(Material::Scree);
    out[2] = m(Material::RockShale);  // no glass survives
  }
  // Dark windows when the site is unlit (outlaws, low light density).
  if (style.light_density < 0.25f && out[2] == m(Material::WindowGlass)) {
    out[2] = m(Material::WindowDark);
  }
}

BuildingRuleSet rule_set_for(const StyleVector& style) {
  if (style.domed) return BuildingRuleSet::Dome;
  switch (style.race_type) {
    case RaceType::Reptilian: return BuildingRuleSet::Ziggurat;
    case RaceType::Insectoid: return BuildingRuleSet::Mound;
    case RaceType::Avian: return BuildingRuleSet::Spire;
    case RaceType::Aquatic: return BuildingRuleSet::Stilt;
    case RaceType::Fungoid: return BuildingRuleSet::Cap;
    case RaceType::Machine: return BuildingRuleSet::HexLattice;
    case RaceType::Crystalline: return BuildingRuleSet::Crystal;
    case RaceType::Precursor: return BuildingRuleSet::Monolith;
    default: break;
  }
  if (style.faction_type == FactionType::AlignedMachine || style.faction_type == FactionType::RenegadeMachine) {
    return BuildingRuleSet::HexLattice;
  }
  return BuildingRuleSet::Stacked;
}

namespace {

struct Ctx {
  const Lot& lot;
  const StyleVector& style;
  const BuildingParams& params;
  Rng rng;
  Sink sink;
  double cx, cy;      // footprint centroid
  double hx, hy;      // half extents along the footprint's own axes
  double yaw;         // footprint orientation
  double base_z;      // ground - 0.8
  double height;      // target height (budget x construction)
  double stage;       // construction 0..1
  bool parts;         // instanced-part terminal set active
  bool ruined;
};

// The facade rule: split a wall into floors and bays; bays become window
// parts (or a flat glass quad without the part terminal), the ground
// floor gets a door near the middle of the front, balconies by faction.
void facade(Ctx& c, const Scope& s, int face, double floor_h, bool front, bool balconies, bool colonnade) {
  // Wall corners for the face: 0 = -v (front), 1 = +u, 2 = +v, 3 = -u.
  double p0[3], p1[3];
  const double z0 = s.z0;
  switch (face) {
    case 0: scope_point(s, -s.hx, -s.hy, z0, p0); scope_point(s, s.hx, -s.hy, z0, p1); break;
    case 1: scope_point(s, s.hx, -s.hy, z0, p0); scope_point(s, s.hx, s.hy, z0, p1); break;
    case 2: scope_point(s, s.hx, s.hy, z0, p0); scope_point(s, -s.hx, s.hy, z0, p1); break;
    default: scope_point(s, -s.hx, s.hy, z0, p0); scope_point(s, -s.hx, -s.hy, z0, p1); break;
  }
  const double wx = p1[0] - p0[0];
  const double wy = p1[1] - p0[1];
  const double width = std::sqrt(wx * wx + wy * wy);
  if (width < 1.5) return;
  Frame f;
  f.ox = p0[0]; f.oy = p0[1]; f.oz = z0;
  f.ax = wx / width; f.ay = wy / width;
  f.nx = f.ay; f.ny = -f.ax;  // outward for counter-clockwise corners
  const int floors = std::max(1, static_cast<int>(s.h / floor_h));
  const double bay_w = c.style.regularity > 0.6f ? 3.2 : 2.6 + 1.2 * c.rng.u01();
  int bays = std::max(1, static_cast<int>((width - 0.8) / bay_w));
  if (bays > 14) bays = 14;
  const double pitch = (width - 0.8) / bays;
  const double win_w = std::min(pitch * 0.55, 1.9);
  const double win_h = std::min(floor_h * 0.55, 1.8);
  const int door_bay = bays / 2;
  const bool has_floor_frame = c.stage >= 0.3;
  for (int fl = 0; fl < floors; ++fl) {
    const double fz = fl * floor_h + 0.9;
    if (fz + win_h > s.h - 0.2) break;
    for (int b = 0; b < bays; ++b) {
      const double u0 = 0.4 + b * pitch + 0.5 * (pitch - win_w);
      if (c.ruined && c.rng.chance(0.45)) continue;  // shattered bays
      if (fl == 0 && front && b == door_bay) {
        if (c.parts) part_door(c.sink, f, u0 - 0.2, win_w + 0.4, std::min(2.3, floor_h - 0.4), 3, 1);
        continue;
      }
      if (fl == 0 && colonnade) continue;  // the colonnade covers the ground floor
      if (!has_floor_frame) continue;
      if (c.parts) {
        part_window(c.sink, f, u0, win_w, fz, win_h, 2, 1, c.ruined);
        if (balconies && fl > 0 && c.rng.chance(0.35)) {
          part_balcony(c.sink, f, u0 - 0.3, win_w + 0.6, fz - 0.05, 1, 3);
        }
      } else {
        double a[3], bq[3], cq[3], d[3];
        f.at(u0, 0.03, fz, a);
        f.at(u0 + win_w, 0.03, fz, bq);
        f.at(u0 + win_w, 0.03, fz + win_h, cq);
        f.at(u0, 0.03, fz + win_h, d);
        c.sink.quad(a, bq, cq, d, 2);
      }
    }
  }
  if (colonnade && c.parts) {
    // Pillars along the face at the bay pitch, a lintel beam above.
    const double ph = std::min(floor_h + 0.5, s.h);
    for (int b = 0; b <= bays; ++b) {
      double o[3];
      f.at(0.4 + b * pitch, 1.2, 0.0, o);
      emit_prism(c.sink, o[0], o[1], o[2], 0.28, 0.28, ph, 8, 0.0, 3, 3);
    }
    double a[3], bq[3], cq[3], d[3], a2[3], b2[3], c2[3], d2[3];
    f.at(0.1, 0.0, ph, a); f.at(width - 0.1, 0.0, ph, bq); f.at(width - 0.1, 1.5, ph, cq); f.at(0.1, 1.5, ph, d);
    f.at(0.1, 0.0, ph + 0.5, a2); f.at(width - 0.1, 0.0, ph + 0.5, b2); f.at(width - 0.1, 1.5, ph + 0.5, c2); f.at(0.1, 1.5, ph + 0.5, d2);
    c.sink.quad(a2, b2, c2, d2, 1);
    c.sink.quad(d, cq, bq, a, 1);
    c.sink.quad(bq, cq, c2, b2, 1);
    c.sink.quad(cq, d, d2, c2, 1);
  }
  // Conduits for machine factions.
  if (c.parts && (c.style.faction_type == FactionType::AlignedMachine || c.style.faction_type == FactionType::RenegadeMachine)) {
    part_conduit(c.sink, f, 0.3, width - 0.3, std::min(s.h - 0.4, floor_h * 0.5 + 1.2), 0.22, 3);
  }
}

void roof(Ctx& c, const Scope& s, int kind) {
  // kind: 0 flat + parapet, 1 gable, 2 hip, 3 dome, 4 spire, 5 flat plain.
  const double z1 = s.z0 + s.h;
  switch (kind) {
    case 0: {
      // Parapet ring.
      Scope outer = s;
      outer.z0 = z1;
      outer.h = 0.6;
      emit_box(c.sink, outer, 1, 1);
      break;
    }
    case 1: {
      // Gable along the long axis.
      const bool along_x = s.hx >= s.hy;
      const double rise = std::min(along_x ? s.hy : s.hx, 3.5) * 0.9;
      double a[3], b[3], cc[3], d[3], r0[3], r1[3];
      if (along_x) {
        scope_point(s, -s.hx, -s.hy, z1, a); scope_point(s, s.hx, -s.hy, z1, b);
        scope_point(s, s.hx, s.hy, z1, cc); scope_point(s, -s.hx, s.hy, z1, d);
        scope_point(s, -s.hx - 0.3, 0.0, z1 + rise, r0); scope_point(s, s.hx + 0.3, 0.0, z1 + rise, r1);
      } else {
        scope_point(s, s.hx, -s.hy, z1, a); scope_point(s, s.hx, s.hy, z1, b);
        scope_point(s, -s.hx, s.hy, z1, cc); scope_point(s, -s.hx, -s.hy, z1, d);
        scope_point(s, 0.0, -s.hy - 0.3, z1 + rise, r0); scope_point(s, 0.0, s.hy + 0.3, z1 + rise, r1);
      }
      c.sink.quad(a, b, r1, r0, 1);
      c.sink.quad(cc, d, r0, r1, 1);
      c.sink.tri(b, cc, r1, 0);
      c.sink.tri(d, a, r0, 0);
      break;
    }
    case 2: {
      const double rise = std::min(s.hx, s.hy) * 0.7;
      double a[3], b[3], cc[3], d[3], apex[3];
      scope_point(s, -s.hx, -s.hy, z1, a); scope_point(s, s.hx, -s.hy, z1, b);
      scope_point(s, s.hx, s.hy, z1, cc); scope_point(s, -s.hx, s.hy, z1, d);
      scope_point(s, 0.0, 0.0, z1 + rise, apex);
      c.sink.tri(a, b, apex, 1); c.sink.tri(b, cc, apex, 1); c.sink.tri(cc, d, apex, 1); c.sink.tri(d, a, apex, 1);
      break;
    }
    case 3: {
      const double r = std::min(s.hx, s.hy) * 0.85;
      emit_prism(c.sink, s.cx, s.cy, z1, r + 0.4, r + 0.4, 0.7, 16, 0.0, 3, 3);
      emit_dome(c.sink, s.cx, s.cy, z1 + 0.7, r, 0.8, 4, 16, 1);
      break;
    }
    case 4: {
      const double r = std::min(s.hx, s.hy) * 0.6;
      emit_prism(c.sink, s.cx, s.cy, z1, r, 0.0, r * 3.0, 8, s.yaw, 1, 1, true);
      break;
    }
    default: break;
  }
}

void props(Ctx& c, const Scope& s) {
  if (!c.parts || c.stage < 0.9 || c.ruined) return;
  const double z1 = s.z0 + s.h + (c.style.faction_type == FactionType::Government ? 0.0 : 0.6);
  const int count = static_cast<int>(c.style.tech_tier * c.style.light_density * 2.5 * c.rng.u01());
  for (int i = 0; i < count && i < 4; ++i) {
    const double u = c.rng.uniform(-s.hx * 0.6, s.hx * 0.6);
    const double v = c.rng.uniform(-s.hy * 0.6, s.hy * 0.6);
    double o[3];
    scope_point(s, u, v, z1, o);
    if (c.rng.chance(0.6)) {
      part_antenna(c.sink, o[0], o[1], o[2], c.rng.uniform(2.0, 6.0), 3);
    } else {
      Scope unit{o[0], o[1], s.yaw, 0.7, 0.5, o[2], 0.9};  // a vent / machine unit
      emit_box(c.sink, unit, 3, 3);
    }
  }
}

void construction_frame(Ctx& c, const Scope& s, double floor_h) {
  // Frame stage: corner pillars and floor slabs, no walls.
  const int floors = std::max(1, static_cast<int>(s.h / floor_h));
  for (int i = 0; i < 4; ++i) {
    const double u = (i & 1) != 0 ? s.hx - 0.3 : -s.hx + 0.3;
    const double v = (i & 2) != 0 ? s.hy - 0.3 : -s.hy + 0.3;
    double o[3];
    scope_point(s, u, v, s.z0, o);
    Scope pillar{o[0], o[1], s.yaw, 0.3, 0.3, s.z0, s.h};
    emit_box(c.sink, pillar, 3, 3);
  }
  for (int fl = 1; fl <= floors; ++fl) {
    Scope slab = s;
    slab.z0 = s.z0 + fl * floor_h;
    slab.h = 0.25;
    emit_box(c.sink, slab, 1, 1, true, true);
  }
}

// --- rule sets ----------------------------------------------------------------------

void rules_stacked(Ctx& c) {
  const bool gov = c.style.faction_type == FactionType::Government;
  const bool indep = c.style.faction_type == FactionType::Independent;
  const bool outlaw = c.style.faction_type == FactionType::Outlaw;
  const double floor_h = outlaw ? 3.0 : (gov ? 4.2 : 3.4);
  const double total = c.height;
  if (c.stage < 0.3) {
    Scope slab{c.cx, c.cy, c.yaw, c.hx, c.hy, c.base_z, 0.8 + 0.3};
    emit_box(c.sink, slab, 1, 1);
    return;
  }
  // Masses: one, or a tower with setbacks when tall; outlaws stack
  // containers askew.
  const int floors_total = std::max(1, static_cast<int>(total / floor_h));
  if (outlaw) {
    const int stack = std::min(3, std::max(1, floors_total));
    double z = c.base_z;
    for (int i = 0; i < stack; ++i) {
      Scope box{c.cx + c.rng.uniform(-0.6, 0.6), c.cy + c.rng.uniform(-0.6, 0.6),
                c.yaw + c.rng.uniform(-0.25, 0.25), c.hx * c.rng.uniform(0.6, 1.0),
                c.hy * c.rng.uniform(0.5, 0.9), z, 2.9};
      emit_box(c.sink, box, 0, 1);
      if (c.parts && c.stage >= 0.7) facade(c, box, 0, 3.0, i == 0, false, false);
      z += 2.9;
    }
    // A lean-to shed and a wall segment.
    Scope shed{c.cx + c.hx * 0.8, c.cy - c.hy * 0.7, c.yaw + 0.4, 1.5, 1.2, c.base_z, 2.2};
    emit_box(c.sink, shed, 3, 3);
    Scope wall{c.cx - c.hx * 0.9, c.cy, c.yaw, 0.15, c.hy, c.base_z, 2.0};
    emit_box(c.sink, wall, 0, 0);
    if (c.parts && c.stage >= 0.9) {
      part_antenna(c.sink, c.cx, c.cy, z, 4.0, 3);
    }
    return;
  }
  const bool tower = floors_total >= 8;
  const int tiers = tower ? std::min(3, 1 + floors_total / 8) : 1;
  double z = c.base_z;
  double hx = c.hx;
  double hy = c.hy;
  int floors_left = floors_total;
  for (int t = 0; t < tiers; ++t) {
    const int floors = t == tiers - 1 ? floors_left : std::max(2, floors_total / tiers);
    floors_left -= floors;
    Scope mass{c.cx, c.cy, c.yaw, hx, hy, z, floors * floor_h + (t == 0 ? 0.8 : 0.0)};
    if (c.stage < 0.7) {
      construction_frame(c, mass, floor_h);
    } else {
      emit_box(c.sink, mass, 0, 1);
      for (int face = 0; face < 4; ++face) {
        facade(c, mass, face, floor_h, face == 0 && t == 0, indep, gov && t == 0 && c.lot.usage == LotUsage::Civic);
      }
    }
    z += mass.h;
    hx *= 0.75;
    hy *= 0.75;
    if (t == tiers - 1 && c.stage >= 0.7) {
      int kind = 5;
      if (c.ruined) {
        kind = 5;
      } else if (c.lot.usage == LotUsage::Civic || c.lot.usage == LotUsage::Monument) {
        kind = gov ? (c.rng.chance(0.5) ? 3 : 4) : 2;
      } else if (indep) {
        kind = c.rng.chance(0.7) ? 1 : 2;
      } else if (gov) {
        kind = tower ? 0 : (c.rng.chance(0.5) ? 2 : 0);
      } else {
        kind = c.rng.chance(0.5) ? 0 : 1;
      }
      roof(c, mass, kind);
      props(c, mass);
    }
  }
  // Monuments: an obelisk on a plinth.
  if (c.lot.usage == LotUsage::Monument && c.parts && !c.ruined) {
    emit_prism(c.sink, c.cx, c.cy, z, 1.2, 0.15, c.height * 0.8, 4, c.yaw + kPi / 4.0, 3, 3);
  }
}

void rules_ziggurat(Ctx& c) {
  const int steps = c.stage < 0.3 ? 1 : std::clamp(static_cast<int>(c.height / 4.0), 2, 6);
  double z = c.base_z;
  double hx = c.hx;
  double hy = c.hy;
  const double step_h = c.height / steps;
  for (int i = 0; i < steps; ++i) {
    Scope tier{c.cx, c.cy, c.yaw, hx, hy, z, step_h + (i == 0 ? 0.8 : 0.0)};
    emit_box(c.sink, tier, 0, 1);
    if (c.parts && i == 0) {
      // A stair ramp up the front.
      Scope ramp{c.cx, c.cy - hy - 1.5, c.yaw, 1.6, 1.5, c.base_z, step_h * 0.5};
      emit_box(c.sink, ramp, 3, 3);
    }
    z += tier.h;
    hx *= 0.72;
    hy *= 0.72;
  }
  if (c.parts && c.stage >= 0.9 && !c.ruined) {
    Scope shrine{c.cx, c.cy, c.yaw, std::max(1.2, hx * 1.2), std::max(1.2, hy * 1.2), z, 3.0};
    emit_box(c.sink, shrine, 3, 1);
    facade(c, shrine, 0, 3.0, true, false, false);
  }
}

void rules_mound(Ctx& c) {
  const double r = std::min(c.hx, c.hy) * 1.1;
  const int rings = c.stage < 0.3 ? 1 : 4;
  double z = c.base_z;
  double rr = r;
  const double ring_h = (c.height + 0.8) / rings;
  for (int i = 0; i < rings; ++i) {
    const double phase = c.yaw + i * 0.35;
    const double ox = c.rng.uniform(-0.15, 0.15) * r;
    const double oy = c.rng.uniform(-0.15, 0.15) * r;
    emit_prism(c.sink, c.cx + ox, c.cy + oy, z, rr, rr * 0.78, ring_h, 8, phase, 0, 1, i == rings - 1);
    z += ring_h;
    rr *= 0.78;
  }
  if (c.parts && c.stage >= 0.7) {
    // Tunnel mouths: dark inset octagons on the lowest ring, and ribs.
    const int mouths = 1 + c.rng.pick(3);
    for (int i = 0; i < mouths; ++i) {
      const double a = c.yaw + i * 2.1 + c.rng.uniform(-0.4, 0.4);
      const double mx = c.cx + std::cos(a) * (r - 0.3);
      const double my = c.cy + std::sin(a) * (r - 0.3);
      emit_prism(c.sink, mx, my, c.base_z + 0.8, 1.1, 1.1, 1.6, 8, 0.0, 3, 3);
    }
    for (int i = 0; i < 6; ++i) {
      const double a = c.yaw + i * kPi / 3.0;
      Scope rib{c.cx + std::cos(a) * r * 0.7, c.cy + std::sin(a) * r * 0.7, a, r * 0.45, 0.25, c.base_z + 0.8, c.height * 0.5};
      emit_box(c.sink, rib, 1, 1);
    }
  }
}

void rules_spire(Ctx& c) {
  const double r = std::min(c.hx, c.hy) * 0.7;
  const double h = c.height + 0.8;
  emit_prism(c.sink, c.cx, c.cy, c.base_z, r, r * 0.35, h, 8, c.yaw, 0, 1, true);
  if (c.parts && c.stage >= 0.7) {
    const int ledges = std::max(1, static_cast<int>(h / 6.0));
    for (int i = 1; i <= ledges; ++i) {
      const double z = c.base_z + h * i / (ledges + 1);
      const double rr = r * (1.0 - 0.65 * i / (ledges + 1)) + 1.4;
      emit_prism(c.sink, c.cx, c.cy, z, rr, rr, 0.3, 8, c.yaw + 0.4 * i, 3, 3);
    }
    if (c.stage >= 0.9 && !c.ruined) {
      emit_dome(c.sink, c.cx, c.cy, c.base_z + h, r * 0.5 + 1.2, 0.7, 3, 10, 1);  // the aerie
    }
  }
}

void rules_stilt(Ctx& c) {
  const double deck_z = c.base_z + 0.8 + 3.0;
  if (c.parts) {
    for (int i = 0; i < 4; ++i) {
      const double u = (i & 1) != 0 ? c.hx * 0.8 : -c.hx * 0.8;
      const double v = (i & 2) != 0 ? c.hy * 0.8 : -c.hy * 0.8;
      double o[3];
      Scope s{c.cx, c.cy, c.yaw, c.hx, c.hy, c.base_z, 0.0};
      scope_point(s, u, v, c.base_z, o);
      emit_prism(c.sink, o[0], o[1], c.base_z, 0.35, 0.35, deck_z - c.base_z, 6, 0.0, 3, 3);
    }
  }
  Scope deck{c.cx, c.cy, c.yaw, c.hx * 1.05, c.hy * 1.05, deck_z, 0.4};
  emit_box(c.sink, deck, 1, 1, true, true);
  if (c.stage >= 0.7) {
    const double r = std::min(c.hx, c.hy) * 0.9;
    emit_dome(c.sink, c.cx, c.cy, deck_z + 0.4, r, std::min(1.0, c.height / r), 4, 12, 0);
    if (c.parts) {
      emit_prism(c.sink, c.cx + r * 0.6, c.cy, deck_z + 0.4, 1.0, 1.0, 1.4, 8, 0.0, 2, 2);  // a port
    }
  }
}

void rules_cap(Ctx& c) {
  const double stem_r = std::min(c.hx, c.hy) * 0.35;
  const double h = c.height + 0.8;
  emit_prism(c.sink, c.cx, c.cy, c.base_z, stem_r * 1.3, stem_r, h * 0.7, 10, c.yaw, 0, 0, false);
  if (c.stage >= 0.7) {
    const double cap_r = std::min(c.hx, c.hy) * 1.15;
    emit_prism(c.sink, c.cx, c.cy, c.base_z + h * 0.55, stem_r, cap_r, h * 0.15, 12, c.yaw, 1, 1, false);
    emit_dome(c.sink, c.cx, c.cy, c.base_z + h * 0.7, cap_r, 0.45, 4, 12, 1);
  }
  if (c.parts && c.stage >= 0.9) {
    const int pods = 1 + c.rng.pick(3);
    for (int i = 0; i < pods; ++i) {
      const double a = c.yaw + i * 2.0 + c.rng.uniform(0.0, 1.0);
      const double d = std::min(c.hx, c.hy) * 0.9;
      emit_dome(c.sink, c.cx + std::cos(a) * d, c.cy + std::sin(a) * d, c.base_z + 0.8, 1.2 + c.rng.u01(), 0.9, 3, 8, 3);
    }
  }
}

void rules_hex(Ctx& c) {
  const double r = std::min(c.hx, c.hy) * 1.05;
  const int stack = c.stage < 0.3 ? 1 : std::clamp(static_cast<int>(c.height / 5.0), 1, 4);
  const double seg_h = (c.height + 0.8) / stack;
  double z = c.base_z;
  for (int i = 0; i < stack; ++i) {
    emit_prism(c.sink, c.cx, c.cy, z, r * (1.0 - 0.12 * i), r * (1.0 - 0.12 * i), seg_h, 6, c.yaw + i * kPi / 6.0, 0, 1);
    z += seg_h;
  }
  if (c.parts && c.stage >= 0.7) {
    // Conduits along the base edges and a mast.
    for (int i = 0; i < 6; ++i) {
      const double a0 = c.yaw + kPi / 3.0 * i;
      const double a1 = a0 + kPi / 3.0;
      Frame f;
      f.ox = c.cx + std::cos(a0) * r; f.oy = c.cy + std::sin(a0) * r; f.oz = c.base_z + 0.8;
      const double ex = std::cos(a1) * r - std::cos(a0) * r;
      const double ey = std::sin(a1) * r - std::sin(a0) * r;
      const double len = std::sqrt(ex * ex + ey * ey);
      f.ax = ex / len; f.ay = ey / len; f.nx = f.ay; f.ny = -f.ax;
      part_conduit(c.sink, f, 0.2, len - 0.2, 1.4, 0.25, 3);
      if (!c.ruined && c.style.light_density > 0.3f) {
        double a[3], b[3], cc[3], d[3];
        f.at(0.3, 0.03, 2.4, a); f.at(len - 0.3, 0.03, 2.4, b); f.at(len - 0.3, 0.03, 2.7, cc); f.at(0.3, 0.03, 2.7, d);
        c.sink.quad(a, b, cc, d, 2);  // a lit strip
      }
    }
    if (c.stage >= 0.9 && !c.ruined) part_antenna(c.sink, c.cx, c.cy, z, c.rng.uniform(3.0, 8.0), 3);
  }
}

void rules_crystal(Ctx& c) {
  const int count = c.stage < 0.3 ? 1 : 3 + c.rng.pick(3);
  const double r = std::min(c.hx, c.hy);
  for (int i = 0; i < count; ++i) {
    const double a = c.yaw + i * 2.4 + c.rng.uniform(-0.3, 0.3);
    const double d = i == 0 ? 0.0 : r * c.rng.uniform(0.3, 0.8);
    const double h = (c.height + 0.8) * (i == 0 ? 1.0 : c.rng.uniform(0.35, 0.8));
    const double cr = r * (i == 0 ? 0.55 : c.rng.uniform(0.2, 0.4));
    const double tilt = c.rng.uniform(-0.12, 0.12);
    emit_prism(c.sink, c.cx + std::cos(a) * d, c.cy + std::sin(a) * d, c.base_z, cr, cr * 0.15, h, 6,
               c.rng.uniform(0.0, kPi), 0, 2, true, tilt * std::cos(a), tilt * std::sin(a));
  }
  if (c.parts && c.stage >= 0.7) {
    emit_prism(c.sink, c.cx, c.cy, c.base_z, r * 1.1, r * 1.05, 0.8, 12, c.yaw, 3, 3);  // the growth plinth
  }
}

void rules_monolith(Ctx& c) {
  Scope plinth{c.cx, c.cy, c.yaw, c.hx, c.hy, c.base_z, 1.4};
  emit_box(c.sink, plinth, 3, 3);
  const int slabs = 1 + c.rng.pick(3);
  for (int i = 0; i < slabs; ++i) {
    const double u = slabs == 1 ? 0.0 : (i - 0.5 * (slabs - 1)) * c.hx * 0.9;
    double o[3];
    scope_point(plinth, u, 0.0, c.base_z + 1.4, o);
    const double h = (c.height + 0.8) * (c.ruined ? c.rng.uniform(0.2, 0.6) : c.rng.uniform(0.7, 1.0));
    Scope slab{o[0], o[1], c.yaw + (c.ruined ? c.rng.uniform(-0.2, 0.2) : 0.0), c.hx * 0.22, c.hy * 0.6, c.base_z + 1.4, h};
    emit_box(c.sink, slab, 0, 1);
  }
  if (c.parts && !c.ruined) {
    emit_prism(c.sink, c.cx, c.cy - c.hy * 1.4, c.base_z, 0.8, 0.8, 0.3, 16, 0.0, 3, 3);  // a ring marker
  }
}

void rules_dome(Ctx& c) {
  const double r = std::min(c.hx, c.hy) * 0.95;
  emit_prism(c.sink, c.cx, c.cy, c.base_z, r + 0.4, r + 0.4, 0.8 + 0.6, 16, 0.0, 3, 3);  // plinth ring
  if (c.stage >= 0.3) {
    emit_dome(c.sink, c.cx, c.cy, c.base_z + 1.4, r, std::min(1.0, std::max(0.45, c.height / r)), c.parts ? 6 : 3, c.parts ? 16 : 10, 2);
  }
  if (c.parts && c.stage >= 0.7) {
    // Airlock stub toward the site centre (negative radial direction).
    const double len = std::sqrt(c.cx * c.cx + c.cy * c.cy);
    const double dx = len > 1.0 ? -c.cx / len : 1.0;
    const double dy = len > 1.0 ? -c.cy / len : 0.0;
    Scope tube{c.cx + dx * (r + 1.5), c.cy + dy * (r + 1.5), std::atan2(dy, dx), 2.0, 1.1, c.base_z + 0.8, 2.2};
    emit_box(c.sink, tube, 0, 1);
    if (c.stage >= 0.9) {
      emit_dome(c.sink, c.cx + dy * (r + 1.0), c.cy - dx * (r + 1.0), c.base_z + 0.8, 1.5, 0.9, 3, 10, 0);  // ancillary
    }
  }
}

}  // namespace

BuildingMesh build_building(const Lot& lot, const core::Key& lot_key, const BuildingParams& params) {
  BuildingMesh mesh;
  // Footprint frame: centroid, principal axes from the first edge.
  double cx = 0.0;
  double cy = 0.0;
  for (int k = 0; k < lot.vertex_count; ++k) {
    cx += lot.footprint[k][0];
    cy += lot.footprint[k][1];
  }
  cx /= lot.vertex_count;
  cy /= lot.vertex_count;
  const double ex = lot.footprint[1][0] - lot.footprint[0][0];
  const double ey = lot.footprint[1][1] - lot.footprint[0][1];
  const double yaw = std::atan2(ey, ex);
  // Half extents along the yaw axes.
  const double c = std::cos(yaw);
  const double s = std::sin(yaw);
  double hx = 0.0;
  double hy = 0.0;
  for (int k = 0; k < lot.vertex_count; ++k) {
    const double dx = lot.footprint[k][0] - cx;
    const double dy = lot.footprint[k][1] - cy;
    hx = std::max(hx, std::fabs(dx * c + dy * s));
    hy = std::max(hy, std::fabs(-dx * s + dy * c));
  }
  hx = std::max(hx, 1.0);
  hy = std::max(hy, 1.0);
  const double stage = lot.style.construction;
  const double height = std::max(1.0, static_cast<double>(lot.height_budget_m) *
                                          (stage < 0.3 ? 0.2 : (stage < 1.0 ? 0.3 + 0.7 * (stage - 0.3) / 0.7 : 1.0)));
  Ctx ctx{lot, lot.style, params, Rng(lot_key), Sink{&mesh}, cx, cy, hx, hy, yaw,
          params.ground_z - 0.8, height, stage, params.method == BuildingMethod::GrammarParts,
          lot.style.ruined};
  if (ctx.ruined) {
    ctx.height = height * ctx.rng.uniform(0.2, 0.6);
  }
  if (params.method == BuildingMethod::Mass) {
    Scope box{cx, cy, yaw, hx, hy, ctx.base_z, 0.8 + ctx.height};
    emit_box(ctx.sink, box, 0, 1);
    return mesh;
  }
  switch (rule_set_for(lot.style)) {
    case BuildingRuleSet::Stacked: rules_stacked(ctx); break;
    case BuildingRuleSet::Ziggurat: rules_ziggurat(ctx); break;
    case BuildingRuleSet::Mound: rules_mound(ctx); break;
    case BuildingRuleSet::Spire: rules_spire(ctx); break;
    case BuildingRuleSet::Stilt: rules_stilt(ctx); break;
    case BuildingRuleSet::Cap: rules_cap(ctx); break;
    case BuildingRuleSet::HexLattice: rules_hex(ctx); break;
    case BuildingRuleSet::Crystal: rules_crystal(ctx); break;
    case BuildingRuleSet::Monolith: rules_monolith(ctx); break;
    case BuildingRuleSet::Dome: rules_dome(ctx); break;
  }
  if (ctx.ruined && ctx.parts) {
    // Rubble around the base.
    const int piles = 2 + ctx.rng.pick(4);
    for (int i = 0; i < piles; ++i) {
      const double a = ctx.rng.uniform(0.0, 2.0 * kPi);
      const double d = ctx.rng.uniform(0.6, 1.4) * std::max(hx, hy);
      Scope pile{cx + std::cos(a) * d, cy + std::sin(a) * d, ctx.rng.uniform(0.0, kPi), ctx.rng.uniform(0.6, 1.8),
                 ctx.rng.uniform(0.5, 1.2), params.ground_z - 0.3, ctx.rng.uniform(0.5, 1.3)};
      emit_box(ctx.sink, pile, 1, 1);
    }
  }
  mesh.emissive_windows = !ctx.ruined && lot.style.light_density >= 0.25f;
  return mesh;
}

}  // namespace inf::gen
