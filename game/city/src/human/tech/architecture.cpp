#include "city/human/tech/architecture.hpp"

#include <algorithm>
#include <cmath>

#include "city/flora.hpp"
#include "city/materials.hpp"
#include "city/props.hpp"
#include "city/standards.hpp"
#include "city/towers.hpp"

namespace inf::city::human_tech {

namespace {

constexpr float kStoreyH = 3.6f;
constexpr float kTowerMinHeight = 34.0f;  // budget above which a lot gets a tower
constexpr float kTowerMinInradius = 8.0f;

// The setback footprint of a lot: inset from the lot line, at least a
// minimal buildable area.
std::vector<Vec2> setback(const std::vector<Vec2>& footprint, float inset) {
  std::vector<Vec2> fp = plan_offset(footprint, -inset);
  if (fp.size() < 3 || plan_area(fp) < 30.0f) {
    fp = plan_offset(footprint, -0.5f * inset);
  }
  if (fp.size() < 3 || plan_area(fp) < 30.0f) {
    return footprint;
  }
  return fp;
}

}  // namespace

FactionMaterials faction_materials(const gen::StyleVector& style) {
  FactionMaterials m{M_WALL_LIGHT, M_CONCRETE_WHITE, M_GLASS_STD, M_GLASS_BLUE, M_DARK_METAL, M_WHITE_METAL, M_PLAZA};
  switch (style.faction_type) {
    case gen::FactionType::Government:
      m = {M_MARBLE_WHITE, M_WALL_LIGHT, M_GLASS_STD, M_GLASS_SILVER, M_WHITE_METAL, M_WHITE_METAL, M_MARBLE_WHITE};
      break;
    case gen::FactionType::Independent:
      m = {M_PANEL_WARM, M_WALL_LIGHT, M_GLASS_STD, M_GLASS_BRONZE, M_BRONZE, M_WHITE_METAL, M_PLAZA};
      break;
    case gen::FactionType::Outlaw:
      m = {M_PANEL_DARK, M_CONCRETE_DARK, M_GLASS_DARK, M_GLASS_DARK, M_DARK_METAL, M_DARK_METAL, M_CONCRETE_DARK};
      break;
    case gen::FactionType::AlignedMachine:
      m = {M_SILVER, M_CHROME, M_GLASS_SILVER, M_GLASS_SILVER, M_CHROME, M_CHROME, M_TERRAZZO};
      break;
    case gen::FactionType::RenegadeMachine:
      m = {M_DARK_METAL, M_PANEL_DARK, M_GLASS_DARK, M_GLASS_XFRAME, M_DARK_METAL, M_CHROME, M_CONCRETE_DARK};
      break;
    case gen::FactionType::Count:
      break;
  }
  if (style.ruined) {
    m.wall = M_CONCRETE_DARK;
    m.wall_alt = M_CONCRETE_DARK;
    m.glass = M_GLASS_DARK;
    m.glass_tower = M_GLASS_DARK;
  }
  return m;
}

std::vector<MaterialDesc> TechArchitecture::materials() const { return make_materials(); }

namespace {

// The tower itself: three levels in a group at full detail (switched by
// the camera at 200 m and 500 m), one level otherwise.
void emit_tower(Scene& sc, const TowerSpec& spec, Vec2 centre, float ground_y, float inradius, Rng tr, int detail) {
  if (detail >= 2) {
    const int group = sc.lod_groups++;
    const float height = spec.floor_h * static_cast<float>(spec.floors + spec.base_floors + 4);
    const float switch_m[3] = {200.0f, 500.0f, 1e30f};
    for (int level = 0; level < 3; ++level) {
      const std::uint32_t first = static_cast<std::uint32_t>(sc.opaque.indices.size());
      build_tower(sc, spec, centre, ground_y, tr, 2 - level);
      sc.register_range(first, static_cast<std::uint32_t>(sc.opaque.indices.size()),
                        Vec3{centre.x, ground_y + height * 0.5f, centre.y}, height * 0.5f + inradius + 2.0f, group,
                        level, switch_m[level]);
    }
  } else {
    build_tower(sc, spec, centre, ground_y, tr, detail);
  }
}

// Faction materials and facade rules on a tower spec.
void style_tower(TowerSpec* spec, const FactionMaterials& fm, const gen::StyleVector& style, bool heroes, Rng& rng) {
  const bool machine = style.faction_type == gen::FactionType::AlignedMachine ||
                       style.faction_type == gen::FactionType::RenegadeMachine;
  spec->glass = fm.glass_tower;
  spec->frame = fm.frame;
  spec->member = fm.member;
  const bool hero_facade = spec->facade == FacadeKind::Diagrid || spec->facade == FacadeKind::HexLattice ||
                           spec->facade == FacadeKind::XFrame;
  if (machine && !hero_facade) {
    spec->facade = rng.chance(0.5f) ? FacadeKind::HexLattice : FacadeKind::Diagrid;
  } else if (!heroes && hero_facade) {
    spec->facade = rng.chance(0.5f) ? FacadeKind::Curtain : FacadeKind::Ribbon;
    spec->crown = CrownKind::Parapet;
  }
  if (style.faction_type == gen::FactionType::Outlaw && spec->crown == CrownKind::Lantern) {
    spec->crown = CrownKind::Parapet;
  }
}

}  // namespace

LotBuildResult TechArchitecture::build_lot(Scene& sc, const LotInput& lot, Rng rng, int detail) const {
  LotBuildResult out;
  if (lot.footprint.size() < 3) return out;
  const FactionMaterials fm = faction_materials(lot.style);
  const float inradius = plan_inradius(lot.footprint);
  const float area = std::fabs(plan_area(lot.footprint));
  const float construction = clampf(lot.style.construction, 0.0f, 1.0f);
  const float height = lot.height_budget * (0.25f + 0.75f * construction);
  switch (lot.usage) {
    case gen::LotUsage::Pad: {
      build_landing_pad(sc, lot.centre, std::max(6.0f, inradius * 0.9f), lot.ground_y, rng, detail);
      out.built = true;
      return out;
    }
    case gen::LotUsage::Monument: {
      const MonumentKind kind = static_cast<MonumentKind>(rng.irange(0, 3));
      const float scale = clampf(lot.height_budget / 25.0f, 0.6f, 3.0f);
      build_monument(sc, kind, lot.centre, lot.ground_y, scale, rng, detail);
      Emit floor(&sc.opaque, fm.floor);
      floor.polygon(lot.footprint, lot.ground_y + 0.01f, true);
      build_hedge_ring(sc, lot.footprint, 1.5f, 0.8f, 0.8f, lot.ground_y, 12.0f, rng);
      out.built = true;
      return out;
    }
    case gen::LotUsage::Agricultural: {
      // A low shed and planters over the rest of the lot.
      const std::vector<Vec2> fp = setback(lot.footprint, 3.0f);
      StandardSpec s;
      s.type = StdType::Lab;
      s.storeys = 1;
      s.floor_h = 4.0f;
      s.roof = RoofKind::Monopitch;
      s.entrance = EntranceKind::Canopy;
      s.wall = fm.wall_alt;
      s.glass = fm.glass;
      s.pilasters = false;
      s.random = rng.next();
      std::vector<Vec2> shed = plan_scale(fp, 0.55f, plan_centroid(fp));
      build_standard(sc, s, shed, lot.ground_y, rng.child(1), detail);
      const Vec2 c = plan_centroid(lot.footprint);
      gen_planter(sc, rng.child(2), c + Vec2{inradius * 0.55f, 0.0f}, inradius * 0.3f, inradius * 0.8f, lot.ground_y);
      gen_planter(sc, rng.child(3), c - Vec2{inradius * 0.55f, 0.0f}, inradius * 0.3f, inradius * 0.8f, lot.ground_y);
      out.built = true;
      return out;
    }
    default:
      break;
  }
  const bool tower = height >= kTowerMinHeight && inradius >= kTowerMinInradius && construction > 0.5f &&
                     lot.usage != gen::LotUsage::Industrial;
  if (tower) {
    const float half = std::min(inradius - 1.5f, 20.0f);
    const int max_floors = std::clamp(static_cast<int>(height / 4.0f), 8, 60);
    TowerSpec spec = random_tower(rng, half, max_floors);
    spec.floors = std::min(spec.floors, max_floors);
    style_tower(&spec, fm, lot.style, false, rng);
    spec.rot += lot.rotation;
    // A plaza floor over the lot, then the tower.
    Emit floor(&sc.opaque, fm.floor);
    floor.polygon(lot.footprint, lot.ground_y + 0.01f, true);
    if (detail >= 1 && lot.style.ornament > 0.2f) {
      build_hedge_ring(sc, lot.footprint, 1.5f, 0.8f, 0.8f, lot.ground_y, 20.0f, rng);
    }
    emit_tower(sc, spec, lot.centre, lot.ground_y, inradius, rng.child(8), detail);
    out.tower = true;
    out.built = true;
    return out;
  }
  // Standard building on the setback footprint.
  const std::vector<Vec2> fp = setback(lot.footprint, 2.0f);
  if (plan_area(fp) < 30.0f) return out;
  StandardSpec s = random_standard(rng, area, lot.t);
  s.storeys = std::clamp(static_cast<int>(std::lround(height / kStoreyH)), 1, 8);
  if (construction < 1.0f) s.storeys = std::max(1, static_cast<int>(s.storeys * construction));
  s.floor_h = kStoreyH;
  switch (lot.usage) {
    case gen::LotUsage::Civic:
      s.type = StdType::Civic;
      s.entrance = EntranceKind::Portal;
      s.wall = fm.wall;
      break;
    case gen::LotUsage::Industrial:
      s.type = StdType::Lab;
      s.roof = rng.chance(0.6f) ? RoofKind::Monopitch : RoofKind::Flat;
      s.wall = fm.wall_alt;
      s.balconies = false;
      s.retail_ground = false;
      break;
    default:
      s.wall = rng.chance(0.7f) ? fm.wall : fm.wall_alt;
      break;
  }
  s.glass = fm.glass;
  if (lot.style.regularity > 0.7f) s.pilasters = true;
  if (lot.style.ruined) {
    s.storeys = std::max(1, s.storeys - 1);
    s.roof = RoofKind::Flat;
  }
  build_standard(sc, s, fp, lot.ground_y, rng.child(9), detail);
  if (detail >= 1 && rng.chance(0.25f * lot.style.ornament + 0.05f)) {
    build_low_wall(sc, lot.footprint, 0.6f, 0.5f, 0.3f, lot.ground_y, fm.wall_alt, 18.0f, rng);
  }
  out.built = true;
  return out;
}

void TechArchitecture::build_key(Scene& sc, KeyRole role, Vec2 centre, float rot, float half, float y, Rng rng,
                                 int detail) const {
  switch (role) {
    case KeyRole::Government:
      build_government(sc, centre, rot, half, y, rng, detail);
      break;
    case KeyRole::UnificationRing:
      build_unification_ring(sc, centre, y, half, rot, detail);
      break;
    case KeyRole::LandingPad:
      build_landing_pad(sc, centre, half, y, rng, detail);
      break;
    case KeyRole::Monument:
      build_monument(sc, static_cast<MonumentKind>(rng.irange(0, 3)), centre, y, half / 12.0f, rng, detail);
      break;
  }
}

void TechArchitecture::build_tower_block(Scene& sc, const TowerBlockInput& in, Rng rng, int detail) const {
  const LotInput& b = in.block;
  if (b.footprint.size() < 3) return;
  const FactionMaterials fm = faction_materials(b.style);
  const float inrad = plan_inradius(b.footprint);
  const float half = std::min(inrad - 9.0f, 20.0f);
  if (half < 8.0f) return;
  const int max_floors = std::max(10, in.max_floors);
  TowerSpec spec = random_tower(rng, half, max_floors);
  if (in.forced_family >= 0) {
    // The core of a metropolis shows every family, in a fixed order.
    switch (in.forced_family) {
      case 0: spec = spec_diagrid(half, max_floors); break;
      case 1: spec = spec_lens(half * 1.35f, half * 0.55f, max_floors - 4, rng.range(0, kPi)); spec.base = BaseKind::Lobby; break;
      case 2: spec = spec_finweave(half * 0.95f, max_floors - 8); break;
      case 3: spec = spec_xframe(half * 1.3f, half * 0.7f, 16); break;
      case 4: spec = spec_hex(half, max_floors - 6); break;
      default: spec = spec_sail(half * 1.3f, half * 0.6f, max_floors - 2, rng.range(0, kPi)); spec.base = BaseKind::Podium; break;
    }
    spec.random = rng.next();
  }
  style_tower(&spec, fm, b.style, in.heroes, rng);
  spec.rot += b.rotation;
  // The plaza floor over the plate, hedges around it, then the tower.
  Emit floor(&sc.opaque, M_PLAZA);
  floor.polygon(plan_offset(b.footprint, -1.0f), b.ground_y + 0.01f, true);
  build_hedge_ring(sc, b.footprint, 2.5f, 0.9f, 0.8f, b.ground_y, 20.0f, rng);
  emit_tower(sc, spec, b.centre, b.ground_y, inrad, rng.child(8), detail);
}

const TechArchitecture& instance() {
  static const TechArchitecture arch;
  return arch;
}

}  // namespace inf::city::human_tech
