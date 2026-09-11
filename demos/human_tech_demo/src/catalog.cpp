#include "catalog.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <sstream>
#include <vector>

#include "city/flora.hpp"
#include "city/materials.hpp"
#include "city/props.hpp"
#include "city/standards.hpp"
#include "city/streets.hpp"
#include "city/towers.hpp"

namespace cb {

namespace {

struct Spec {
  std::string kind;
  std::string name;
  std::map<std::string, std::string> keys;
  float f(const char* key, float def) const {
    const auto it = keys.find(key);
    return it == keys.end() ? def : static_cast<float>(std::atof(it->second.c_str()));
  }
  int i(const char* key, int def) const {
    const auto it = keys.find(key);
    return it == keys.end() ? def : std::atoi(it->second.c_str());
  }
  std::string s(const char* key, const char* def) const {
    const auto it = keys.find(key);
    return it == keys.end() ? def : it->second;
  }
  bool has(const char* key) const { return keys.count(key) != 0; }
};

Spec parse_spec(const std::string& text) {
  Spec s;
  std::stringstream ss(text);
  std::string part;
  bool first = true;
  while (std::getline(ss, part, ';')) {
    if (first) {
      first = false;
      const std::size_t colon = part.find(':');
      s.kind = part.substr(0, colon);
      if (colon != std::string::npos) s.name = part.substr(colon + 1);
      continue;
    }
    const std::size_t eq = part.find('=');
    if (eq == std::string::npos) s.keys[part] = "1";
    else s.keys[part.substr(0, eq)] = part.substr(eq + 1);
  }
  return s;
}

template <typename T>
bool pick(const std::string& text, const std::vector<std::pair<const char*, T>>& table, T* out) {
  for (const auto& e : table) {
    if (text == e.first) {
      *out = e.second;
      return true;
    }
  }
  return false;
}

const std::vector<std::pair<const char*, FacadeKind>> kFacades = {
    {"curtain", FacadeKind::Curtain}, {"sail", FacadeKind::Sail},       {"ribbon", FacadeKind::Ribbon},
    {"finweave", FacadeKind::FinWeave}, {"louvre", FacadeKind::Louvre}, {"diagrid", FacadeKind::Diagrid},
    {"xframe", FacadeKind::XFrame},   {"hex", FacadeKind::HexLattice}};
const std::vector<std::pair<const char*, BaseKind>> kBases = {
    {"lobby", BaseKind::Lobby}, {"podium", BaseKind::Podium}, {"colonnade", BaseKind::Colonnade}, {"plinth", BaseKind::Plinth}, {"legs", BaseKind::Legs}};
const std::vector<std::pair<const char*, CrownKind>> kCrowns = {
    {"parapet", CrownKind::Parapet}, {"lattice", CrownKind::Lattice}, {"mast", CrownKind::Mast}, {"lantern", CrownKind::Lantern}, {"louvres", CrownKind::Louvres}};
const std::vector<std::pair<const char*, PlanKind>> kPlans = {
    {"superellipse", PlanKind::Superellipse}, {"lens", PlanKind::Lens}, {"circle", PlanKind::Circle}, {"roundedrect", PlanKind::RoundedRect}, {"polygon", PlanKind::Polygon}};
const std::vector<std::pair<const char*, StdType>> kStdTypes = {
    {"office", StdType::Office}, {"residential", StdType::Residential}, {"mixed", StdType::Mixed}, {"civic", StdType::Civic}, {"lab", StdType::Lab}};
const std::vector<std::pair<const char*, EntranceKind>> kEntrances = {
    {"canopy", EntranceKind::Canopy}, {"portal", EntranceKind::Portal}, {"stairs", EntranceKind::Stairs}, {"vestibule", EntranceKind::Vestibule}};
const std::vector<std::pair<const char*, RoofKind>> kRoofs = {
    {"flat", RoofKind::Flat}, {"parapet", RoofKind::Parapet}, {"green", RoofKind::Green}, {"monopitch", RoofKind::Monopitch}};
const std::vector<std::pair<const char*, MonumentKind>> kMonuments = {
    {"pillar", MonumentKind::Pillar}, {"ribbon", MonumentKind::Ribbon}, {"weave", MonumentKind::Weave}, {"obelisk", MonumentKind::Obelisk}};
const std::vector<std::pair<const char*, PlazaKind>> kPlazas = {
    {"fountain", PlazaKind::Fountain}, {"formal", PlazaKind::Formal}, {"terraced", PlazaKind::Terraced},
    {"monument", PlazaKind::Monument}, {"garden", PlazaKind::Garden}, {"landing", PlazaKind::Landing}};

// A tower from the spec: a named family (its parameters exposed), a random
// variant, or a plain curtain-wall tower; then every key that overrides
// one axis.
bool tower_from_spec(const Spec& sp, Rng& rng, TowerSpec* out, std::string* error) {
  const float half = sp.f("half", 16.0f);
  const int floors = sp.i("floors", 30);
  TowerSpec s;
  if (sp.name == "diagrid") s = spec_diagrid(half, floors);
  else if (sp.name == "lens") s = spec_lens(half * 1.4f, half * 0.55f, floors, radians(sp.f("rot_deg", 90.0f)));
  else if (sp.name == "sail") s = spec_sail(half * 1.3f, half * 0.6f, floors, radians(sp.f("rot_deg", 90.0f)));
  else if (sp.name == "finweave") s = spec_finweave(half * 0.95f, floors);
  else if (sp.name == "xframe") s = spec_xframe(half * 1.3f, half * 0.7f, std::min(floors, 18));
  else if (sp.name == "hex") s = spec_hex(half, floors);
  else if (sp.name == "random") s = random_tower(rng, half, floors);
  else if (sp.name == "curtain" || sp.name == "louvre" || sp.name == "ribbon" || sp.name.empty()) {
    s.plan = PlanKind::Superellipse;
    s.a = half;
    s.b = half * 0.85f;
    s.exponent = 3.0f;
    s.floors = floors;
    s.floor_h = 4.0f;
    s.facade = FacadeKind::Curtain;
    s.glass = M_GLASS_BLUE;
    s.base = BaseKind::Lobby;
    s.crown = CrownKind::Parapet;
    if (sp.name == "louvre") {
      s.plan = PlanKind::Circle;
      s.a = s.b = half * 0.9f;
      s.floor_h = 3.8f;
      s.facade = FacadeKind::Louvre;
      s.glass = M_GLASS_CONTEXT;
      s.member = M_WHITE_METAL;
      s.fin_depth = 0.45f;
      s.module_w = 2.4f;
      s.base = BaseKind::Colonnade;
      s.crown = CrownKind::Louvres;
    } else if (sp.name == "ribbon") {
      s.facade = FacadeKind::Ribbon;
      s.glass = M_GLASS_SILVER;
      s.member = M_WHITE_METAL;
      s.module_w = 3.6f;
      s.fin_depth = 0.5f;
      s.floor_bands = false;
    }
  } else {
    *error = "unknown tower family '" + sp.name + "'";
    return false;
  }
  // Axis overrides.
  if (sp.has("facade") && !pick(sp.s("facade", ""), kFacades, &s.facade)) { *error = "unknown facade"; return false; }
  if (sp.has("base") && !pick(sp.s("base", ""), kBases, &s.base)) { *error = "unknown base"; return false; }
  if (sp.has("crown") && !pick(sp.s("crown", ""), kCrowns, &s.crown)) { *error = "unknown crown"; return false; }
  if (sp.has("plan") && !pick(sp.s("plan", ""), kPlans, &s.plan)) { *error = "unknown plan"; return false; }
  if (sp.has("exponent")) s.exponent = sp.f("exponent", s.exponent);
  if (sp.has("sides")) s.sides = sp.i("sides", s.sides);
  if (sp.has("taper")) s.taper = sp.f("taper", s.taper);
  if (sp.has("tip")) s.tip = sp.f("tip", s.tip);
  if (sp.has("twist")) s.twist = sp.f("twist", s.twist);
  if (sp.has("setback")) s.setback_floor = sp.i("setback", s.setback_floor);
  if (sp.has("setback_scale")) s.setback_scale = sp.f("setback_scale", s.setback_scale);
  if (sp.has("floor_h")) s.floor_h = sp.f("floor_h", s.floor_h);
  if (sp.has("module")) s.module_w = sp.f("module", s.module_w);
  if (sp.has("spandrel")) s.spandrel_h = sp.f("spandrel", s.spandrel_h);
  if (sp.has("fin_depth")) s.fin_depth = sp.f("fin_depth", s.fin_depth);
  if (sp.has("member_r")) s.member_r = sp.f("member_r", s.member_r);
  if (sp.has("lattice_rows")) s.lattice_rows = sp.i("lattice_rows", s.lattice_rows);
  if (sp.has("bands")) s.floor_bands = sp.i("bands", 1) != 0;
  if (sp.has("base_floors")) s.base_floors = sp.i("base_floors", s.base_floors);
  if (sp.has("base_scale")) s.base_scale = sp.f("base_scale", s.base_scale);
  if (sp.has("a")) s.a = sp.f("a", s.a);
  if (sp.has("b")) s.b = sp.f("b", s.b);
  if (sp.has("rot_deg")) s.rot = radians(sp.f("rot_deg", 0.0f));
  s.random = rng.next();
  *out = s;
  return true;
}

// The plaza plate under the asset and the camera around it, from the
// bounds of what was built (foliage included).
void frame_asset(Scene& sc, float y_ground, float elevation_deg, float azimuth_deg, float margin, const Vec3* lo_in = nullptr,
                 const Vec3* hi_in = nullptr) {
  Vec3 lo = lo_in != nullptr ? *lo_in : vmin(sc.opaque.bounds_min, sc.foliage.bounds_min);
  Vec3 hi = hi_in != nullptr ? *hi_in : vmax(sc.opaque.bounds_max, sc.foliage.bounds_max);
  if (lo.x > hi.x) {  // nothing built
    lo = Vec3{-5, 0, -5};
    hi = Vec3{5, 5, 5};
  }
  const Vec3 centre = (lo + hi) * 0.5f;
  const float radius = std::max(length(hi - lo) * 0.5f, 2.0f);
  // The plate: a plaza disc on asphalt, well beyond the asset.
  const float plate_r = std::max(radius * 1.8f, 18.0f);
  Emit a(&sc.opaque, M_GRASS);
  a.polygon(plan_rect(plate_r * 6.0f, plate_r * 6.0f, Vec2{centre.x, centre.z}), y_ground - 0.05f, true);
  Emit p(&sc.opaque, M_PLAZA);
  p.polygon(plan_circle(plate_r, 64, Vec2{centre.x, centre.z}), y_ground - 0.02f, true);
  Emit c(&sc.opaque, M_CURB);
  c.wall(plan_circle(plate_r, 64, Vec2{centre.x, centre.z}), y_ground - 0.2f, y_ground - 0.02f, true, true);
  // Camera: fit the bounding sphere into the 55 degree vertical field with a margin.
  const float fov = radians(55.0f);
  const float dist = radius * margin / std::sin(fov * 0.5f);
  const float el = radians(elevation_deg), az = radians(azimuth_deg);
  const Vec3 dir{std::sin(az) * std::cos(el), std::sin(el), std::cos(az) * std::cos(el)};
  sc.camera_position = centre + dir * dist;
  sc.camera_target = centre;
}

}  // namespace

bool generate_asset(Scene& sc, const std::string& text, Rng root, int detail, std::string* error, bool studio_frame) {
  const Spec sp = parse_spec(text);
  Rng rng = root.child(1);
  const float y = 0.0f;
  const Vec2 o{0.0f, 0.0f};
  float elevation = sp.f("elevation", 18.0f);
  float azimuth = sp.f("azimuth", 35.0f);
  float margin = sp.f("margin", 1.02f);
  int trees = 40;
  if (sp.kind == "tower") {
    TowerSpec s;
    if (!tower_from_spec(sp, rng, &s, error)) return false;
    build_tower(sc, s, o, y, rng.child(2), sp.i("detail", detail));
    elevation = sp.f("elevation", 14.0f);
  } else if (sp.kind == "group") {
    build_tower_group(sc, rng.child(sp.i("variant", 0)), o, radians(sp.f("rot_deg", 0.0f)), y, sp.i("detail", detail));
    elevation = sp.f("elevation", 14.0f);
  } else if (sp.kind == "standard") {
    StandardSpec s = random_standard(rng, sp.f("area", 700.0f), sp.f("t", 0.5f));
    StdType type = s.type;
    if (!sp.name.empty() && !pick(sp.name, kStdTypes, &type)) { *error = "unknown standard type '" + sp.name + "'"; return false; }
    s.type = type;
    switch (type) {  // the type's own materials and heights, as the showcase sets them
      case StdType::Office: s.wall = M_WALL_LIGHT; s.floor_h = 3.8f; break;
      case StdType::Residential: s.wall = M_PANEL_WARM; s.floor_h = 3.2f; s.balconies = true; break;
      case StdType::Mixed: s.wall = M_WALL_LIGHT; s.floor_h = 3.6f; s.retail_ground = true; break;
      case StdType::Civic: s.wall = M_MARBLE_WHITE; s.floor_h = 4.4f; s.pilasters = true; break;
      case StdType::Lab: s.wall = M_PANEL_DARK; s.floor_h = 4.0f; break;
    }
    if (sp.has("entrance") && !pick(sp.s("entrance", ""), kEntrances, &s.entrance)) { *error = "unknown entrance"; return false; }
    if (sp.has("roof") && !pick(sp.s("roof", ""), kRoofs, &s.roof)) { *error = "unknown roof"; return false; }
    if (sp.has("storeys")) s.storeys = std::max(1, sp.i("storeys", s.storeys));
    if (sp.has("floor_h")) s.floor_h = sp.f("floor_h", s.floor_h);
    if (sp.has("pilasters")) s.pilasters = sp.i("pilasters", 1) != 0;
    if (sp.has("balconies")) s.balconies = sp.i("balconies", 1) != 0;
    if (sp.has("retail")) s.retail_ground = sp.i("retail", 1) != 0;
    if (sp.has("glass_lo")) s.glass_lo = sp.f("glass_lo", s.glass_lo);
    if (sp.has("glass_hi")) s.glass_hi = sp.f("glass_hi", s.glass_hi);
    const float hx = sp.f("hx", 16.0f), hz = sp.f("hz", 11.0f);
    build_standard(sc, s, plan_rect(hx, hz, o), y, rng.child(3), sp.i("detail", detail));
    azimuth = sp.f("azimuth", 150.0f);  // the entrance sits on the first (-z) edge; the sun comes from this side
    elevation = sp.f("elevation", 16.0f);
  } else if (sp.kind == "government") {
    const int stage = std::clamp(std::atoi(sp.name.c_str()), 0, 5);
    const float half = sp.f("half", 26.0f);
    build_government(sc, o, radians(sp.f("rot_deg", 0.0f)), half, y, rng, sp.i("detail", detail), stage);
    // frame the building, not the square under it
    const Vec3 lo = vmin(sc.opaque.bounds_min, sc.foliage.bounds_min), hi = vmax(sc.opaque.bounds_max, sc.foliage.bounds_max);
    if (studio_frame && stage >= 2) {
      Emit f(&sc.opaque, M_MARBLE_WHITE);
      f.polygon(plan_rect(half * 1.9f, half * 1.9f, o), y + 0.01f, true);
    }
    if(studio_frame)frame_asset(sc, y, sp.f("elevation", 18.0f), sp.f("azimuth", 30.0f), margin, &lo, &hi);  // the stairs face +z
    if (sc.lights.size() > 64) sc.lights.resize(64);
    sc.finalize_draws();
    sc.city_size = "asset " + text;
    return true;
  } else if (sp.kind == "settler") {
    build_settler_house(sc, o, radians(sp.f("rot_deg", 0.0f)), y, rng, sp.i("detail", detail), sp.name == "deck");
    elevation = sp.f("elevation", 22.0f);
  } else if (sp.kind == "pod") {
    build_pod_wreck(sc, o, y, rng, sp.i("detail", detail), sp.name == "memorial");
    elevation = sp.f("elevation", 26.0f);
  } else if (sp.kind == "basin") {
    const float r = sp.f("r", 6.0f);
    build_basin(sc, o, r, sp.f("rz", sp.name == "square" ? r * 0.5f : r), sp.name != "square", y, sp.f("rim", 0.45f));
    elevation = sp.f("elevation", 30.0f);
  } else if (sp.kind == "fountain") {
    build_fountain(sc, o, sp.f("r", 8.0f), y, rng, sp.i("detail", detail));
    elevation = sp.f("elevation", 22.0f);
  } else if (sp.kind == "hedge") {
    if (sp.name == "ring") {
      build_hedge_ring(sc, plan_rect(sp.f("hx", 14.0f), sp.f("hz", 10.0f), o), 0.0f, sp.f("width", 0.9f), sp.f("height", 0.9f), y, sp.f("gap", 12.0f), rng);
      elevation = sp.f("elevation", 30.0f);
    } else {
      build_hedge(sc, Vec2{-sp.f("len", 12.0f) * 0.5f, 0}, Vec2{sp.f("len", 12.0f) * 0.5f, 0}, sp.f("width", 0.9f), sp.f("height", 0.9f), y);
      elevation = sp.f("elevation", 24.0f);
    }
  } else if (sp.kind == "lowwall") {
    build_low_wall(sc, plan_rect(sp.f("hx", 14.0f), sp.f("hz", 10.0f), o), 0.0f, sp.f("height", 0.6f), sp.f("thickness", 0.35f), y,
                   sp.s("mat", "marble") == "concrete" ? M_CONCRETE_WHITE : M_MARBLE_WHITE, sp.f("gap", 18.0f), rng);
    elevation = sp.f("elevation", 30.0f);
  } else if (sp.kind == "foundation") {
    build_foundation(sc, plan_rect(sp.f("hx", 14.0f), sp.f("hz", 11.0f), o), y, sp.f("height", 2.4f), sp.i("edge", 2), sp.i("detail", detail));
    azimuth = sp.f("azimuth", 30.0f);  // the stairs are on edge 2 (+z)
    elevation = sp.f("elevation", 24.0f);
  } else if (sp.kind == "monument") {
    MonumentKind kind = MonumentKind::Pillar;
    if (!pick(sp.name, kMonuments, &kind)) { *error = "unknown monument '" + sp.name + "'"; return false; }
    build_monument(sc, kind, o, y, sp.f("scale", 1.2f), rng, sp.i("detail", detail));
    elevation = sp.f("elevation", 14.0f);
  } else if (sp.kind == "ring") {
    build_unification_ring(sc, o, y, sp.f("r", 12.0f), radians(sp.f("facing_deg", 90.0f)), sp.i("detail", detail));
    elevation = sp.f("elevation", 14.0f);
  } else if (sp.kind == "pad") {
    build_landing_pad(sc, o, sp.f("r", 14.0f), y, rng, sp.i("detail", detail));
    elevation = sp.f("elevation", 30.0f);
  } else if (sp.kind == "overpass") {
    const float L = sp.f("len", 90.0f);
    build_overpass(sc, {Vec2{-L * 0.5f, 0.0f}, Vec2{-L * 0.2f, -8.0f}, Vec2{L * 0.2f, 8.0f}, Vec2{L * 0.5f, 0.0f}}, y + sp.f("deck", 6.5f), y, rng,
                   sp.i("detail", detail));
    elevation = sp.f("elevation", 18.0f);
  } else if (sp.kind == "tree") {
    gen_tree(sc, rng.child(sp.i("variant", 0)), P3(o, y), sp.f("height", 8.0f));
    elevation = sp.f("elevation", 10.0f);
  } else if (sp.kind == "lamp") {
    gen_lamp(sc, P3(o, y), radians(sp.f("yaw_deg", 90.0f)));
    elevation = sp.f("elevation", 10.0f);
  } else if (sp.kind == "bench") {
    gen_bench(sc, P3(o, y), radians(sp.f("yaw_deg", 0.0f)));
    elevation = sp.f("elevation", 22.0f);
  } else if (sp.kind == "planter") {
    gen_planter(sc, rng.child(sp.i("variant", 0)), o, sp.f("hx", 6.0f), sp.f("hz", 2.5f), y);
    elevation = sp.f("elevation", 26.0f);
  } else if (sp.kind == "park") {
    gen_park(sc, rng.child(sp.i("variant", 0)), o, y);
    elevation = sp.f("elevation", 28.0f);
    azimuth = sp.f("azimuth", 20.0f);
  } else if (sp.kind == "pavilion") {
    gen_pavilion(sc, rng.child(sp.i("variant", 0)), o, sp.f("r", 12.0f), sp.f("height", 15.0f), y);
    elevation = sp.f("elevation", 18.0f);
  } else if (sp.kind == "plaza") {
    PlazaKind kind = PlazaKind::Garden;
    if (!pick(sp.name, kPlazas, &kind)) { *error = "unknown plaza '" + sp.name + "'"; return false; }
    const std::vector<Vec2> court = plan_rect(sp.f("hx", 36.0f), sp.f("hz", 30.0f), o);
    Emit s(&sc.opaque, M_SIDEWALK);
    s.polygon(plan_offset(court, 3.0f), y, true);
    build_plaza(sc, kind, court, y, rng.child(5), sp.i("detail", detail), &trees);
    elevation = sp.f("elevation", 32.0f);
  } else if (sp.kind == "median") {
    const float L = sp.f("len", 56.0f);
    Emit a(&sc.opaque, M_ASPHALT);
    a.polygon(plan_rect(L * 0.5f + 6.0f, 13.0f, o), y - 0.01f, true);
    build_median(sc, Vec2{-L * 0.5f, 0}, Vec2{L * 0.5f, 0}, sp.f("width", 3.2f), 0.15f, y);
    road_paint(sc, Vec2{-L * 0.5f - 6.0f, 0}, Vec2{L * 0.5f + 6.0f, 0}, 26.0f, true, y + 0.02f);
    elevation = sp.f("elevation", 30.0f);
  } else if (sp.kind == "street") {
    // A crossing of two secondary streets with lane paint and crosswalks.
    const float w = sp.f("width", 14.0f);
    Emit a(&sc.opaque, M_ASPHALT);
    a.polygon(plan_rect(40.0f, 40.0f, o), y - 0.01f, true);
    road_paint(sc, Vec2{-40.0f, 0}, Vec2{40.0f, 0}, w, false, y + 0.02f);
    road_paint(sc, Vec2{0, -40.0f}, Vec2{0, 40.0f}, w, false, y + 0.02f);
    crosswalk(sc, Vec2{0, -(w * 0.5f + 2.0f)}, Vec2{1, 0}, w, y + 0.025f);
    crosswalk(sc, Vec2{-(w * 0.5f + 2.0f), 0}, Vec2{0, 1}, w, y + 0.025f);
    // the sidewalk plates of the four corners
    for (float sx : {-1.0f, 1.0f}) {
      for (float sz : {-1.0f, 1.0f}) {
        const std::vector<Vec2> plate = plan_rect(12.0f, 12.0f, Vec2{sx * (w * 0.5f + 12.0f), sz * (w * 0.5f + 12.0f)});
        Emit s(&sc.opaque, M_SIDEWALK);
        s.polygon(plate, y + 0.15f, true);
        Emit c(&sc.opaque, M_CURB);
        c.wall(plate, y, y + 0.15f, true, true);
      }
    }
    gen_lamp(sc, P3(Vec2{w * 0.5f + 1.2f, w * 0.5f + 1.2f}, y + 0.15f), radians(225.0f));
    elevation = sp.f("elevation", 40.0f);
  } else if (sp.kind == "lawn") {
    Emit lawn(&sc.opaque, M_GRASS);
    lawn.polygon(plan_rect(sp.f("hx", 9.0f), sp.f("hz", 7.0f), o), y + 0.02f, true);
    gen_tree(sc, rng.child(77), P3(o, y), 7.5f);
    elevation = sp.f("elevation", 24.0f);
  } else {
    *error = "unknown asset kind '" + sp.kind + "' (try --asset list)";
    return false;
  }
  if(studio_frame)frame_asset(sc, y, elevation, azimuth, margin);
  if (sc.lights.size() > 64) sc.lights.resize(64);
  sc.finalize_draws();
  sc.city_size = "asset " + text;
  return true;
}

std::string asset_catalog_text() {
  return
      "asset kinds (cityblock --asset KIND:NAME[;key=value...]):\n"
      "  arrival-six-towers             six Blender-authored Arrival towers and their lots\n"
      "  tower:diagrid|lens|sail|finweave|xframe|hex|curtain|louvre|ribbon|random\n"
      "      keys: half floors facade base crown plan exponent sides taper tip twist setback setback_scale\n"
      "            floor_h module spandrel fin_depth member_r lattice_rows bands base_floors base_scale a b rot_deg detail\n"
      "  group                          keys: variant rot_deg detail\n"
      "  standard:office|residential|mixed|civic|lab\n"
      "      keys: entrance roof storeys floor_h pilasters balconies retail glass_lo glass_hi hx hz area t detail\n"
      "  government:0..5                keys: half rot_deg detail\n"
      "  settler:house|deck   pod:wreck|memorial\n"
      "  basin:round|square (r rz rim)  fountain (r)  hedge:line|ring (len width height hx hz gap)\n"
      "  lowwall (hx hz height thickness mat gap)  foundation (hx hz height edge)\n"
      "  monument:pillar|ribbon|weave|obelisk (scale)  ring (r facing_deg)  pad (r)  overpass (len deck)\n"
      "  tree (height variant)  lamp (yaw_deg)  bench (yaw_deg)  planter (hx hz variant)  park (variant)\n"
      "  pavilion (r height variant)  plaza:fountain|formal|terraced|monument|garden|landing (hx hz)\n"
      "  median (len width)  street (width)  lawn (hx hz)\n"
      "  every kind: elevation azimuth margin (camera framing, degrees)\n";
}

}  // namespace cb
