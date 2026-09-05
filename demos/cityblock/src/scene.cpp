#include "scene.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "rng.hpp"

namespace cb {

namespace {

// ---------------------------------------------------------------------------
// Materials
// ---------------------------------------------------------------------------
enum Mat : std::uint32_t {
  M_ASPHALT = 0, M_LANE_WHITE, M_LANE_YELLOW, M_CURB, M_SIDEWALK, M_PLAZA, M_TERRAZZO, M_GRASS, M_SOIL,
  M_CONCRETE_WHITE, M_CONCRETE, M_CONCRETE_DARK, M_WHITE_METAL, M_SILVER, M_DARK_METAL, M_BRONZE,
  M_GLASS_BLUE, M_GLASS_SILVER, M_GLASS_DARK, M_GLASS_CLEAR, M_SPANDREL, M_ROOF, M_BARK, M_LEAF, M_LAMP,
  M_MARBLE, M_LOBBY_LIGHT, M_SIGN, M_WATER, M_GLASS_XFRAME, M_GLASS_CONTEXT, M_COUNT
};

std::vector<MaterialDesc> make_materials() {
  std::vector<MaterialDesc> m(M_COUNT);
  auto set = [&](Mat id, const char* name, Vec3 color, float rough, float metal, const char* tex, float scale, std::uint32_t flags = 0) {
    MaterialDesc& d = m[id];
    d.name = name;
    d.base_color = color;
    d.roughness = rough;
    d.metallic = metal;
    d.albedo_set = tex;
    d.uv_scale = scale;
    d.flags = flags;
  };
  set(M_ASPHALT, "asphalt", {0.8f, 0.8f, 0.8f}, 0.9f, 0, "asphalt", 5.0f, kMatPlanarXZ);
  set(M_LANE_WHITE, "lane_white", {0.85f, 0.85f, 0.82f}, 0.7f, 0, "", 1.0f);
  set(M_LANE_YELLOW, "lane_yellow", {0.85f, 0.65f, 0.15f}, 0.7f, 0, "", 1.0f);
  set(M_CURB, "curb", {0.85f, 0.85f, 0.85f}, 0.75f, 0, "concrete_smooth", 3.0f, kMatTriplanar);
  set(M_SIDEWALK, "sidewalk", {0.95f, 0.95f, 0.95f}, 0.7f, 0, "paving_slabs", 3.0f, kMatPlanarXZ);
  set(M_PLAZA, "plaza", {0.82f, 0.82f, 0.80f}, 0.65f, 0, "pavement_light", 4.0f, kMatPlanarXZ);
  set(M_TERRAZZO, "terrazzo", {1.0f, 1.0f, 1.0f}, 0.35f, 0, "terrazzo", 3.0f, kMatPlanarXZ);
  set(M_GRASS, "grass", {0.95f, 1.0f, 0.9f}, 0.9f, 0, "grass", 3.0f, kMatPlanarXZ);
  set(M_SOIL, "soil", {0.9f, 0.9f, 0.9f}, 0.95f, 0, "soil", 2.0f, kMatPlanarXZ);
  set(M_CONCRETE_WHITE, "concrete_white", {1.0f, 1.0f, 1.0f}, 0.6f, 0, "concrete_white", 3.0f, kMatTriplanar);
  set(M_CONCRETE, "concrete", {1.0f, 1.0f, 1.0f}, 0.7f, 0, "concrete_smooth", 4.0f, kMatTriplanar);
  set(M_CONCRETE_DARK, "concrete_dark", {0.9f, 0.9f, 0.9f}, 0.7f, 0, "concrete_panels", 3.0f, kMatTriplanar);
  set(M_WHITE_METAL, "white_metal", {0.92f, 0.92f, 0.90f}, 0.42f, 0.0f, "", 1.0f);
  set(M_SILVER, "silver", {1.0f, 1.0f, 1.0f}, 0.55f, 1.0f, "metal_silver", 1.0f, kMatTriplanar);
  set(M_DARK_METAL, "dark_metal", {0.25f, 0.25f, 0.26f}, 0.55f, 0.9f, "metal_black", 1.0f, kMatTriplanar);
  set(M_BRONZE, "bronze", {0.36f, 0.26f, 0.18f}, 0.4f, 1.0f, "", 1.0f);
  // Glass per building so the interior room grid matches the real floors.
  set(M_GLASS_BLUE, "glass_diagrid", {0.6f, 0.75f, 0.9f}, 0.06f, 0, "", 1.0f, kMatGlass);
  m[M_GLASS_BLUE].tint2 = Vec3{0.62f, 0.78f, 0.92f};
  m[M_GLASS_BLUE].room_w = 3.0f; m[M_GLASS_BLUE].room_h = 4.0f; m[M_GLASS_BLUE].room_d = 7.0f;
  set(M_GLASS_SILVER, "glass_lens", {0.8f, 0.85f, 0.9f}, 0.04f, 0, "", 1.0f, kMatGlass);
  m[M_GLASS_SILVER].tint2 = Vec3{0.72f, 0.80f, 0.86f};
  m[M_GLASS_SILVER].room_w = 3.6f; m[M_GLASS_SILVER].room_h = 3.9f; m[M_GLASS_SILVER].room_d = 6.0f;
  set(M_GLASS_DARK, "glass_fin", {0.3f, 0.35f, 0.4f}, 0.05f, 0, "", 1.0f, kMatGlass);
  m[M_GLASS_DARK].tint2 = Vec3{0.28f, 0.34f, 0.40f};
  m[M_GLASS_DARK].lit_probability = 0.45f;
  m[M_GLASS_DARK].room_w = 4.8f; m[M_GLASS_DARK].room_h = 3.8f; m[M_GLASS_DARK].room_d = 6.0f;
  set(M_GLASS_XFRAME, "glass_xframe", {0.6f, 0.75f, 0.9f}, 0.06f, 0, "", 1.0f, kMatGlass);
  m[M_GLASS_XFRAME].tint2 = Vec3{0.62f, 0.78f, 0.92f};
  m[M_GLASS_XFRAME].room_w = 3.0f; m[M_GLASS_XFRAME].room_h = 4.4f; m[M_GLASS_XFRAME].room_d = 8.0f;
  set(M_GLASS_CONTEXT, "glass_context", {0.5f, 0.62f, 0.75f}, 0.08f, 0, "", 1.0f, kMatGlass);
  m[M_GLASS_CONTEXT].tint2 = Vec3{0.55f, 0.66f, 0.78f};
  m[M_GLASS_CONTEXT].room_w = 4.2f; m[M_GLASS_CONTEXT].room_h = 3.8f; m[M_GLASS_CONTEXT].room_d = 7.0f;
  m[M_GLASS_CONTEXT].lit_probability = 0.5f;
  set(M_GLASS_CLEAR, "glass_clear", {0.9f, 0.95f, 0.95f}, 0.03f, 0, "", 1.0f, kMatGlass);
  m[M_GLASS_CLEAR].tint2 = Vec3{0.9f, 0.95f, 0.93f};
  m[M_GLASS_CLEAR].room_w = 12.0f; m[M_GLASS_CLEAR].room_h = 6.0f; m[M_GLASS_CLEAR].room_d = 14.0f; m[M_GLASS_CLEAR].lit_probability = 0.9f;
  set(M_SPANDREL, "spandrel", {0.16f, 0.17f, 0.19f}, 0.6f, 0.2f, "", 1.0f);
  set(M_ROOF, "roof", {0.42f, 0.42f, 0.44f}, 0.9f, 0, "concrete_smooth", 5.0f, kMatTriplanar);
  set(M_BARK, "bark", {0.9f, 0.9f, 0.9f}, 0.9f, 0, "bark", 1.2f);
  set(M_LEAF, "leaf", {1.0f, 1.0f, 1.0f}, 0.75f, 0, "", 1.0f, kMatFoliage);
  set(M_LAMP, "lamp", {0.9f, 0.9f, 0.9f}, 0.5f, 0, "", 1.0f, kMatEmissive | kMatNightOnly);
  m[M_LAMP].emissive = 6.0f; m[M_LAMP].tint2 = Vec3{1.0f, 0.86f, 0.62f};
  set(M_MARBLE, "marble", {1.0f, 1.0f, 1.0f}, 0.25f, 0, "marble", 3.0f, kMatPlanarXZ);
  set(M_LOBBY_LIGHT, "lobby_light", {0.9f, 0.9f, 0.9f}, 0.5f, 0, "", 1.0f, kMatEmissive);
  m[M_LOBBY_LIGHT].emissive = 1.6f; m[M_LOBBY_LIGHT].tint2 = Vec3{1.0f, 0.93f, 0.8f};
  set(M_SIGN, "sign", {0.2f, 0.55f, 1.0f}, 0.5f, 0, "", 1.0f, kMatEmissive | kMatNightOnly);
  m[M_SIGN].emissive = 3.0f; m[M_SIGN].tint2 = Vec3{0.25f, 0.6f, 1.0f};
  set(M_WATER, "water", {0.05f, 0.08f, 0.1f}, 0.02f, 0, "", 1.0f);
  return m;
}

// ---------------------------------------------------------------------------
// Facade helpers
// ---------------------------------------------------------------------------
struct Surface {
  Sampled s;      // sampled plan (ccw)
  float perimeter{0};
  Vec2 centre;
  // Position and outward normal at arclength u (wraps).
  void at(float u, Vec2* p, Vec2* n) const {
    u = std::fmod(std::fmod(u, perimeter) + perimeter, perimeter);
    const std::size_t count = s.points.size();
    std::size_t i = 0;
    // binary search on arclen
    std::size_t lo = 0, hi = count - 1;
    while (lo < hi) {
      const std::size_t mid = (lo + hi + 1) / 2;
      if (s.arclen[mid] <= u) lo = mid; else hi = mid - 1;
    }
    i = lo;
    const std::size_t j = (i + 1) % count;
    const float a0 = s.arclen[i];
    const float a1 = j == 0 ? perimeter : s.arclen[j];
    const float t = a1 > a0 ? (u - a0) / (a1 - a0) : 0.0f;
    *p = s.points[i] + (s.points[j] - s.points[i]) * t;
    *n = s.normals[i];
  }
};

Surface make_surface(const std::vector<Vec2>& plan, float step) {
  Surface sf;
  sf.s = plan_sample(plan, step);
  sf.perimeter = plan_perimeter(plan);
  Vec2 c{0, 0};
  for (const Vec2& p : plan) c = c + p;
  sf.centre = c * (1.0f / static_cast<float>(plan.size()));
  return sf;
}

struct Panel {
  Vec2 p0, p1;  // bottom corners (xz), left → right seen from outside
  Vec2 n;       // outward normal (xz)
  float y0, y1;
  float u0, u1;  // arclength
  int floor;
  int index;
};

// Splits the perimeter into modules of ~module_w metres for each floor.
std::vector<Panel> panelize(const Surface& sf, float base_y, float floor_h, int floors, float module_w,
                            float scale_at_floor(int, void*) = nullptr, void* ctx = nullptr) {
  (void)scale_at_floor;
  (void)ctx;
  std::vector<Panel> out;
  const int modules = std::max(3, static_cast<int>(std::round(sf.perimeter / module_w)));
  const float mw = sf.perimeter / static_cast<float>(modules);
  for (int f = 0; f < floors; ++f) {
    for (int i = 0; i < modules; ++i) {
      Panel p;
      p.u0 = static_cast<float>(i) * mw;
      p.u1 = p.u0 + mw;
      Vec2 n0, n1;
      sf.at(p.u0, &p.p0, &n0);
      sf.at(p.u1, &p.p1, &n1);
      p.n = normalize(perp(p.p1 - p.p0) * -1.0f);  // right-hand normal for ccw travel
      p.y0 = base_y + floor_h * static_cast<float>(f);
      p.y1 = p.y0 + floor_h;
      p.floor = f;
      p.index = i;
      out.push_back(p);
    }
  }
  return out;
}

Vec3 P3(Vec2 xz, float y) { return Vec3{xz.x, y, xz.y}; }

struct FacadeStyle {
  Mat glass{M_GLASS_BLUE};
  Mat mullion{M_SILVER};
  Mat spandrel{M_SPANDREL};
  float mullion_w{0.08f}, mullion_d{0.16f};
  float transom_h{0.10f};
  float spandrel_h{0.0f};     // opaque band at the floor line (0 = none)
  float glass_recess{0.06f};
  float fin_depth{0.0f};      // horizontal fin projection at each floor line
  float fin_thickness{0.12f};
  int vertical_subdiv{1};     // extra mullions inside the panel
  float random{0.0f};
};

// Emits one curtain-wall panel: recessed glass, mullions, transom, optional spandrel and fin.
void curtain_panel(Mesh& mesh, const Panel& p, const FacadeStyle& st, Vec2 origin_hint) {
  (void)origin_hint;
  const Vec3 n3{p.n.x, 0, p.n.y};
  const Vec3 a = P3(p.p0, p.y0), b = P3(p.p1, p.y0), c = P3(p.p1, p.y1), d = P3(p.p0, p.y1);
  const Vec3 rec = n3 * (-st.glass_recess);
  // glass (facade coords: u along, v up)
  {
    Emit e(&mesh, st.glass);
    e.element_random = st.random;
    const float sp = st.spandrel_h;
    const Vec3 ga = a + rec + Vec3{0, sp, 0}, gb = b + rec + Vec3{0, sp, 0}, gc = c + rec, gd = d + rec;
    QuadUV uv{{p.u0, p.y0 + sp}, {p.u1, p.y0 + sp}, {p.u1, p.y1}, {p.u0, p.y1}};
    e.quad(ga, gb, gc, gd, uv);
    if (sp > 0.0f) {
      Emit s(&mesh, st.spandrel);
      s.quad(a + rec * 0.5f, b + rec * 0.5f, b + rec * 0.5f + Vec3{0, sp, 0}, a + rec * 0.5f + Vec3{0, sp, 0},
             QuadUV{{p.u0, 0}, {p.u1, 0}, {p.u1, sp}, {p.u0, sp}});
    }
  }
  // vertical mullion at the left edge (the right edge belongs to the next panel)
  if (st.mullion_w > 0.0f) {
    Emit e(&mesh, st.mullion);
    e.occlusion = 0.9f;
    const Vec3 mid = (a + d) * 0.5f + n3 * (st.mullion_d * 0.5f - st.glass_recess);
    const Vec3 along = normalize(b - a);
    e.box(mid, Vec3{st.mullion_w * 0.5f, (p.y1 - p.y0) * 0.5f, st.mullion_d * 0.5f}, along, Vec3{0, 1, 0}, n3);
    for (int k = 1; k < st.vertical_subdiv; ++k) {
      const float t = static_cast<float>(k) / static_cast<float>(st.vertical_subdiv);
      const Vec3 base = lerp(a, b, t);
      e.box(base + Vec3{0, (p.y1 - p.y0) * 0.5f, 0} + n3 * (st.mullion_d * 0.3f - st.glass_recess),
            Vec3{st.mullion_w * 0.35f, (p.y1 - p.y0) * 0.5f, st.mullion_d * 0.3f}, along, Vec3{0, 1, 0}, n3);
    }
    // transom at the top edge
    const Vec3 tm = (c + d) * 0.5f + n3 * (st.mullion_d * 0.5f - st.glass_recess);
    e.box(tm, Vec3{length(b - a) * 0.5f, st.transom_h * 0.5f, st.mullion_d * 0.5f}, along, Vec3{0, 1, 0}, n3);
    if (st.spandrel_h > 0.0f) {
      const Vec3 sm = (a + b) * 0.5f + Vec3{0, st.spandrel_h, 0} + n3 * (st.mullion_d * 0.4f - st.glass_recess);
      e.box(sm, Vec3{length(b - a) * 0.5f, st.transom_h * 0.4f, st.mullion_d * 0.4f}, along, Vec3{0, 1, 0}, n3);
    }
  }
  if (st.fin_depth > 0.0f) {
    Emit e(&mesh, M_WHITE_METAL);
    e.occlusion = 0.95f;
    const Vec3 along = normalize(b - a);
    const Vec3 fm = (a + b) * 0.5f + n3 * (st.fin_depth * 0.5f - st.glass_recess) + Vec3{0, st.fin_thickness * 0.5f, 0};
    e.box(fm, Vec3{length(b - a) * 0.5f + 0.01f, st.fin_thickness * 0.5f, st.fin_depth * 0.5f}, along, Vec3{0, 1, 0}, n3);
  }
}

// Floor slab ring + roof cap for a plan at height y.
void slab(Mesh& mesh, const std::vector<Vec2>& plan, float y, float thickness, Mat mat) {
  Emit e(&mesh, mat);
  e.polygon(plan, y, true, Vec2{0.25f, 0.25f});
  e.polygon(plan, y - thickness, false, Vec2{0.25f, 0.25f});
  e.wall(plan, y - thickness, y, true, true);
}

void parapet(Mesh& mesh, const std::vector<Vec2>& plan, float y, float height, float thickness, Mat mat) {
  Emit e(&mesh, mat);
  const std::vector<Vec2> inner = plan_offset(plan, -thickness);
  e.wall(plan, y, y + height, true, true);
  e.wall(inner, y, y + height, true, false);
  // top ring: quads between plan and inner
  for (std::size_t i = 0; i < plan.size(); ++i) {
    const std::size_t j = (i + 1) % plan.size();
    e.quad_metric(P3(plan[i], y + height), P3(plan[j], y + height), P3(inner[j], y + height), P3(inner[i], y + height));
  }
}

// Rooftop equipment: a few boxes and cylinders scattered inside the plan.
void roof_equipment(Mesh& mesh, Rng& rng, const std::vector<Vec2>& plan, float y, int count) {
  Emit e(&mesh, M_DARK_METAL);
  Emit c(&mesh, M_CONCRETE);
  float minx = 1e9f, maxx = -1e9f, minz = 1e9f, maxz = -1e9f;
  for (const Vec2& p : plan) {
    minx = std::min(minx, p.x); maxx = std::max(maxx, p.x); minz = std::min(minz, p.y); maxz = std::max(maxz, p.y);
  }
  for (int i = 0; i < count * 6 && count > 0; ++i) {
    const Vec2 p{rng.range(minx, maxx), rng.range(minz, maxz)};
    if (!point_in_polygon(plan_offset(plan, -3.0f), p)) continue;
    const float w = rng.range(1.2f, 3.5f), h = rng.range(1.0f, 2.6f);
    if (rng.chance(0.6f)) e.box(P3(p, y + h * 0.5f), Vec3{w * 0.5f, h * 0.5f, rng.range(1.0f, 2.5f)});
    else c.tube(P3(p, y), P3(p, y + h), w * 0.35f, 16, true);
    if (--count <= 0) break;
  }
}

// ---------------------------------------------------------------------------
// Buildings
// ---------------------------------------------------------------------------

// 1. Diagrid tower: rounded-square plan, white steel diagrid over blue glass.
void gen_diagrid_tower(Scene& sc, Rng rng, Vec2 centre, float half, int floors, float floor_h) {
  Mesh& mesh = sc.opaque;
  const std::vector<Vec2> plan = plan_superellipse(half, half, 3.2f, 96, centre, 0.0f);
  const float base_y = 0.0f;
  const float lobby_h = 2.0f * floor_h;  // two-storey lobby
  const float top = base_y + lobby_h + floor_h * static_cast<float>(floors);
  Surface sf = make_surface(plan, 0.5f);
  // Podium/lobby: recessed clear glass with a canopy slab.
  {
    const std::vector<Vec2> inner = plan_offset(plan, -2.5f);
    Surface sfi = make_surface(inner, 0.5f);
    FacadeStyle st;
    st.glass = M_GLASS_CLEAR; st.mullion = M_DARK_METAL; st.mullion_w = 0.14f; st.mullion_d = 0.3f; st.transom_h = 0.14f;
    st.glass_recess = 0.1f; st.random = rng.next();
    for (const Panel& p : panelize(sfi, base_y, lobby_h, 1, 3.0f)) curtain_panel(mesh, p, st, centre);
    // lobby floor + ceiling light strip
    Emit f(&mesh, M_MARBLE);
    f.polygon(inner, base_y + 0.02f, true);
    Emit l(&mesh, M_LOBBY_LIGHT);
    l.polygon(plan_offset(inner, -1.0f), base_y + lobby_h - 0.3f, false);
    slab(mesh, plan_offset(plan, 0.8f), base_y + lobby_h, 1.2f, M_WHITE_METAL);  // canopy
    // columns
    Emit c(&mesh, M_CONCRETE_WHITE);
    for (int i = 0; i < 16; ++i) {
      Vec2 p, n;
      sf.at(sf.perimeter * static_cast<float>(i) / 16.0f, &p, &n);
      c.tube(P3(p - n * 0.9f, base_y), P3(p - n * 0.9f, base_y + lobby_h), 0.5f, 14, false);
    }
  }
  // Tower curtain wall.
  FacadeStyle st;
  st.glass = M_GLASS_BLUE; st.mullion = M_DARK_METAL; st.mullion_w = 0.07f; st.mullion_d = 0.12f; st.transom_h = 0.35f;
  st.spandrel_h = 0.0f; st.glass_recess = 0.05f; st.vertical_subdiv = 2; st.random = rng.next();
  const std::vector<Panel> panels = panelize(sf, base_y + lobby_h, floor_h, floors, 3.0f);
  for (const Panel& p : panels) curtain_panel(mesh, p, st, centre);
  // floor slabs visible as dark bands behind the glass: thin dark ring every floor
  {
    Emit s(&mesh, M_SPANDREL);
    const std::vector<Vec2> ring = plan_offset(plan, -0.08f);
    for (int f = 0; f <= floors; ++f) {
      const float y = base_y + lobby_h + floor_h * static_cast<float>(f);
      s.wall(ring, y - 0.55f, y + 0.15f, true, true);
    }
  }
  // Diagrid: nodes every 2 floors, modules ~ perimeter / 20.
  {
    Emit d(&mesh, M_WHITE_METAL);
    d.occlusion = 1.0f;
    const int modules = 20;
    const float mw = sf.perimeter / static_cast<float>(modules);
    const float node_h = 2.0f * floor_h;
    const int rows = floors / 2 + 2;  // continues above the roof as a crown
    const float r_member = 0.42f;
    const float offset = 0.75f;
    auto node = [&](int i, int j) {
      Vec2 p, n;
      sf.at(mw * static_cast<float>(i), &p, &n);
      const float y = base_y + lobby_h + node_h * static_cast<float>(j);
      return P3(p + n * offset, y);
    };
    for (int j = 0; j < rows; ++j) {
      for (int i = 0; i < modules; ++i) {
        const Vec3 a = node(i, j), b = node(i + 1, j + 1), c2 = node(i + 1, j), d2 = node(i, j + 1);
        // taper the member radius toward the crown
        const float shrink = j >= rows - 2 ? 0.75f : 1.0f;
        d.tube(a, b, r_member * shrink, 12, false);
        d.tube(c2, d2, r_member * shrink, 12, false);
      }
      for (int i = 0; i < modules; ++i) d.sphere(node(i, j), r_member * 1.25f, 8, 12);
    }
    for (int i = 0; i < modules; ++i) d.sphere(node(i, rows), r_member * 0.95f, 8, 12);
    // ring beam at the crown top
    Vec2 pp, nn;
    for (int i = 0; i < modules; ++i) {
      sf.at(mw * static_cast<float>(i), &pp, &nn);
      Vec2 pq, nq;
      sf.at(mw * static_cast<float>(i + 1), &pq, &nq);
      const float y = base_y + lobby_h + node_h * static_cast<float>(rows);
      d.tube(P3(pp + nn * offset, y), P3(pq + nq * offset, y), r_member * 0.8f, 10, false);
    }
    // base: members land on white concrete plinths
    Emit pl(&mesh, M_CONCRETE_WHITE);
    for (int i = 0; i < modules; ++i) {
      const Vec3 nd = node(i, 0);
      pl.frustum(Vec3{nd.x, base_y, nd.z}, Vec3{nd.x, base_y + lobby_h, nd.z}, 1.1f, 0.6f, 12, true);
    }
  }
  // roof
  slab(mesh, plan_offset(plan, -0.2f), top, 0.6f, M_ROOF);
  parapet(mesh, plan_offset(plan, -0.2f), top, 1.1f, 0.35f, M_WHITE_METAL);
  roof_equipment(mesh, rng, plan, top, 5);
  // crown glass lantern: a smaller superellipse rising into the lattice
  {
    const std::vector<Vec2> lantern = plan_superellipse(half * 0.55f, half * 0.55f, 3.2f, 48, centre, 0.0f);
    Surface sl = make_surface(lantern, 0.5f);
    FacadeStyle ls;
    ls.glass = M_GLASS_SILVER; ls.mullion = M_DARK_METAL; ls.mullion_w = 0.06f; ls.mullion_d = 0.1f; ls.transom_h = 0.1f;
    ls.random = rng.next();
    for (const Panel& p : panelize(sl, top, floor_h, 2, 2.5f)) curtain_panel(mesh, p, ls, centre);
    slab(mesh, lantern, top + floor_h * 2.0f, 0.5f, M_WHITE_METAL);
    Emit m(&mesh, M_DARK_METAL);
    m.tube(P3(centre, top + floor_h * 2.0f), P3(centre, top + floor_h * 2.0f + 14.0f), 0.25f, 8, true);
    Emit sgn(&mesh, M_SIGN);
    sgn.sphere(P3(centre, top + floor_h * 2.0f + 14.3f), 0.5f, 6, 10);
  }
}

// 2. Twin lens towers: two lens plans, tapering to a point, horizontal fins.
void gen_lens_towers(Scene& sc, Rng rng, Vec2 centre, float rot, int floors, float floor_h) {
  Mesh& mesh = sc.opaque;
  const float r = 46.0f, d = 34.0f;  // lens width 2*sqrt(r^2-d^2) ≈ 62, thickness 2*(r-d) = 24
  const float gap = 6.0f;
  const float lens_half_thick = r - d;
  const float base_y = 0.0f;
  const float podium_h = 5.5f;
  for (int side = 0; side < 2; ++side) {
    const float sgn = side == 0 ? 1.0f : -1.0f;
    const Vec2 off = Vec2{-std::sin(rot), std::cos(rot)} * (sgn * (lens_half_thick + gap * 0.5f));
    const Vec2 c = centre + off;
    const std::vector<Vec2> base_plan = plan_lens(r, d, 40, c, rot);
    // taper profile: scale(f) shrinks toward the top and pinches into a tip
    auto scale_at = [&](float f01) {
      const float taper = 1.0f - 0.22f * f01 * f01;
      const float tip = f01 > 0.88f ? 1.0f - 0.55f * std::pow((f01 - 0.88f) / 0.12f, 1.6f) : 1.0f;
      return taper * tip;
    };
    // podium (shared look: dark stone base with clear glass)
    {
      const std::vector<Vec2> pod = plan_offset(base_plan, 1.5f);
      Surface sp = make_surface(pod, 0.5f);
      FacadeStyle st;
      st.glass = M_GLASS_CLEAR; st.mullion = M_DARK_METAL; st.mullion_w = 0.12f; st.mullion_d = 0.25f; st.transom_h = 0.12f; st.random = rng.next();
      for (const Panel& p : panelize(sp, base_y, podium_h, 1, 2.8f)) curtain_panel(mesh, p, st, c);
      slab(mesh, plan_offset(pod, 0.6f), base_y + podium_h, 0.8f, M_CONCRETE_WHITE);
      Emit f(&mesh, M_TERRAZZO);
      f.polygon(pod, base_y + 0.02f, true);
    }
    // tower: floor by floor with its own scaled plan
    FacadeStyle st;
    st.glass = M_GLASS_SILVER; st.mullion = M_SILVER; st.mullion_w = 0.05f; st.mullion_d = 0.08f; st.transom_h = 0.06f;
    st.glass_recess = 0.04f; st.fin_depth = 0.5f; st.fin_thickness = 0.22f; st.vertical_subdiv = 1; st.random = rng.next();
    const float tower_base = base_y + podium_h;
    float u_offset = 0.0f;
    (void)u_offset;
    for (int f = 0; f < floors; ++f) {
      const float f0 = static_cast<float>(f) / static_cast<float>(floors);
      const float f1 = static_cast<float>(f + 1) / static_cast<float>(floors);
      const std::vector<Vec2> p0 = plan_scale(base_plan, scale_at(f0), c);
      const std::vector<Vec2> p1 = plan_scale(base_plan, scale_at(f1), c);
      const float y0 = tower_base + floor_h * static_cast<float>(f), y1 = y0 + floor_h;
      const std::size_t n = p0.size();
      const int per_edge = 2;
      float u = 0.0f;
      Emit g(&mesh, st.glass);
      g.element_random = st.random;
      Emit m(&mesh, st.mullion);
      Emit fin(&mesh, M_WHITE_METAL);
      for (std::size_t i = 0; i < n; ++i) {
        const std::size_t j = (i + 1) % n;
        for (int k = 0; k < per_edge; ++k) {
          const float ta = static_cast<float>(k) / per_edge, tb = static_cast<float>(k + 1) / per_edge;
          const Vec2 a0 = p0[i] + (p0[j] - p0[i]) * ta, b0 = p0[i] + (p0[j] - p0[i]) * tb;
          const Vec2 a1 = p1[i] + (p1[j] - p1[i]) * ta, b1 = p1[i] + (p1[j] - p1[i]) * tb;
          const float w = length(b0 - a0);
          const Vec3 A = P3(a0, y0), B = P3(b0, y0), C = P3(b1, y1), D = P3(a1, y1);
          const Vec3 nrm = normalize(cross(B - A, D - A));
          const Vec3 rec = nrm * (-st.glass_recess);
          g.quad(A + rec, B + rec, C + rec, D + rec, QuadUV{{u, y0}, {u + w, y0}, {u + w, y1}, {u, y1}});
          // horizontal fin at the floor line
          const Vec3 fm = (A + B) * 0.5f + nrm * (st.fin_depth * 0.5f - st.glass_recess) + Vec3{0, st.fin_thickness * 0.5f, 0};
          fin.box(fm, Vec3{w * 0.5f + 0.02f, st.fin_thickness * 0.5f, st.fin_depth * 0.5f}, normalize(B - A), Vec3{0, 1, 0}, nrm);
          u += w;
        }
      }
      if (f == floors - 1) {
        slab(mesh, p1, y1, 0.5f, M_WHITE_METAL);
        Emit cap(&mesh, M_WHITE_METAL);
        cap.polygon(plan_offset(p1, 0.3f), y1 + 0.5f, true);
      }
    }
  }
  // sky bridge between the two towers at mid height
  {
    const float y = base_y + podium_h + floor_h * static_cast<float>(floors) * 0.55f;
    const Vec2 dir{-std::sin(rot), std::cos(rot)};
    Emit g(&mesh, M_GLASS_CLEAR);
    Emit m(&mesh, M_WHITE_METAL);
    const Vec2 a = centre - dir * (gap * 0.5f + 2.0f), b = centre + dir * (gap * 0.5f + 2.0f);
    const Vec2 side = perp(dir) * 4.0f;
    m.box(P3(centre, y - 0.4f), Vec3{4.0f, 0.4f, gap * 0.5f + 2.5f}, perp(dir).x != 0 ? Vec3{perp(dir).x, 0, perp(dir).y} : Vec3{1, 0, 0}, Vec3{0, 1, 0}, Vec3{dir.x, 0, dir.y});
    m.box(P3(centre, y + 4.2f), Vec3{4.0f, 0.3f, gap * 0.5f + 2.5f}, Vec3{perp(dir).x, 0, perp(dir).y}, Vec3{0, 1, 0}, Vec3{dir.x, 0, dir.y});
    g.quad_metric(P3(a + side, y), P3(b + side, y), P3(b + side, y + 3.9f), P3(a + side, y + 3.9f));
    g.quad_metric(P3(b - side, y), P3(a - side, y), P3(a - side, y + 3.9f), P3(b - side, y + 3.9f));
  }
}

// 3. Fin tower: dark glass cylinder with a weave of vertical bronze fins.
void gen_fin_tower(Scene& sc, Rng rng, Vec2 centre, float radius, int floors, float floor_h) {
  Mesh& mesh = sc.opaque;
  const std::vector<Vec2> plan = plan_circle(radius, 72, centre);
  Surface sf = make_surface(plan, 0.5f);
  const float base_y = 0.0f, lobby_h = 6.0f;
  {
    const std::vector<Vec2> inner = plan_circle(radius - 2.0f, 48, centre);
    Surface si = make_surface(inner, 0.5f);
    FacadeStyle st;
    st.glass = M_GLASS_CLEAR; st.mullion = M_DARK_METAL; st.mullion_w = 0.12f; st.mullion_d = 0.24f; st.transom_h = 0.12f; st.random = rng.next();
    for (const Panel& p : panelize(si, base_y, lobby_h, 1, 3.2f)) curtain_panel(mesh, p, st, centre);
    Emit f(&mesh, M_TERRAZZO);
    f.polygon(inner, base_y + 0.02f, true);
    Emit c(&mesh, M_CONCRETE_DARK);
    for (int i = 0; i < 12; ++i) {
      Vec2 p, n;
      sf.at(sf.perimeter * static_cast<float>(i) / 12.0f, &p, &n);
      c.box(P3(p - n * 0.6f, base_y + lobby_h * 0.5f), Vec3{0.45f, lobby_h * 0.5f, 0.45f});
    }
    slab(mesh, plan_offset(plan, 0.5f), base_y + lobby_h, 0.9f, M_CONCRETE_DARK);
  }
  FacadeStyle st;
  st.glass = M_GLASS_DARK; st.mullion = M_DARK_METAL; st.mullion_w = 0.0f; st.mullion_d = 0.1f; st.transom_h = 0.45f;
  st.glass_recess = 0.04f; st.random = rng.next();
  const float tower_base = base_y + lobby_h;
  for (const Panel& p : panelize(sf, tower_base, floor_h, floors, 1.6f)) curtain_panel(mesh, p, st, centre);
  // fins: one per module, offset half a module every other floor, depth modulated by a wave
  {
    Emit fin(&mesh, M_BRONZE);
    fin.occlusion = 0.95f;
    const int modules = std::max(3, static_cast<int>(std::round(sf.perimeter / 1.6f)));
    const float mw = sf.perimeter / static_cast<float>(modules);
    for (int f = 0; f < floors; ++f) {
      const float y0 = tower_base + floor_h * static_cast<float>(f);
      const float shift = (f % 2) ? 0.5f : 0.0f;
      for (int i = 0; i < modules; ++i) {
        const float u = mw * (static_cast<float>(i) + shift + 0.5f);
        Vec2 p, n;
        sf.at(u, &p, &n);
        const float phase = u / sf.perimeter * 2.0f * kPi;
        const float depth = 0.55f + 0.45f * std::sin(phase * 3.0f + static_cast<float>(f) * 0.35f) * std::cos(static_cast<float>(f) * 0.11f);
        const Vec3 n3{n.x, 0, n.y};
        const Vec3 along = normalize(Vec3{-n.y, 0, n.x});
        fin.box(P3(p, y0 + floor_h * 0.5f) + n3 * (depth * 0.5f), Vec3{0.07f, floor_h * 0.5f - 0.02f, depth * 0.5f}, along, Vec3{0, 1, 0}, n3);
      }
    }
  }
  const float top = tower_base + floor_h * static_cast<float>(floors);
  slab(mesh, plan_offset(plan, 1.6f), top, 1.0f, M_CONCRETE_DARK);  // cantilevered cap
  parapet(mesh, plan_offset(plan, 1.6f), top, 0.9f, 0.3f, M_DARK_METAL);
  roof_equipment(mesh, rng, plan, top, 3);
  // louvre crown band
  {
    Emit l(&mesh, M_DARK_METAL);
    const std::vector<Vec2> ring = plan_circle(radius + 1.4f, 72, centre);
    for (int k = 0; k < 4; ++k) {
      const float y = top + 1.2f + 0.7f * static_cast<float>(k);
      Emit e(&mesh, M_DARK_METAL);
      e.wall(ring, y, y + 0.35f, true, true);
    }
  }
}

// 4. X-frame block: rectangular mid-rise with a white structural diamond
//    exoskeleton (two-floor modules) over recessed dark glass.
void gen_xframe_block(Scene& sc, Rng rng, Vec2 centre, float hx, float hz, int floors, float floor_h) {
  Mesh& mesh = sc.opaque;
  const std::vector<Vec2> plan = plan_rect(hx, hz, centre);
  Surface sf = make_surface(plan, 0.5f);
  const float base_y = 0.0f;
  FacadeStyle st;
  st.glass = M_GLASS_XFRAME; st.mullion = M_DARK_METAL; st.mullion_w = 0.06f; st.mullion_d = 0.1f; st.transom_h = 0.3f;
  st.glass_recess = 0.05f; st.vertical_subdiv = 2; st.random = rng.next();
  for (const Panel& p : panelize(sf, base_y, floor_h, floors, 3.0f)) curtain_panel(mesh, p, st, centre);
  {
    Emit s(&mesh, M_SPANDREL);
    const std::vector<Vec2> ring = plan_offset(plan, -0.08f);
    for (int f = 1; f <= floors; ++f) {
      const float y = base_y + floor_h * static_cast<float>(f);
      s.wall(ring, y - 0.5f, y + 0.12f, true, true);
    }
  }
  // exoskeleton: per face, modules of ~9 m, cells 2 floors tall; diagonals
  {
    Emit x(&mesh, M_WHITE_METAL);
    const float offset = 0.9f;
    const float cell_h = 2.0f * floor_h;
    const int rows = floors / 2;
    for (std::size_t e = 0; e < plan.size(); ++e) {
      const Vec2 a = plan[e], b = plan[(e + 1) % plan.size()];
      const float len = length(b - a);
      const int modules = std::max(1, static_cast<int>(std::round(len / 9.0f)));
      const Vec2 dir = normalize(b - a);
      const Vec2 n = Vec2{dir.y, -dir.x};
      const Vec3 n3{n.x, 0, n.y};
      for (int j = 0; j < rows; ++j) {
        const float y0 = base_y + cell_h * static_cast<float>(j), y1 = y0 + cell_h;
        for (int i = 0; i < modules; ++i) {
          const Vec2 p0 = a + dir * (len * static_cast<float>(i) / modules) + n * offset;
          const Vec2 p1 = a + dir * (len * static_cast<float>(i + 1) / modules) + n * offset;
          x.beam(P3(p0, y0), P3(p1, y1), 0.55f, 1.0f, n3);
          x.beam(P3(p1, y0), P3(p0, y1), 0.55f, 1.0f, n3);
        }
        // horizontal chord at the cell line
        x.beam(P3(a + n * offset, y0), P3(b + n * offset, y0), 0.45f, 0.9f, n3);
      }
      x.beam(P3(a + n * offset, base_y + cell_h * rows), P3(b + n * offset, base_y + cell_h * rows), 0.45f, 0.9f, n3);
      // vertical corner posts
      x.beam(P3(a + n * offset, base_y), P3(a + n * offset, base_y + cell_h * rows), 0.6f, 1.0f, n3);
    }
  }
  const float top = base_y + floor_h * static_cast<float>(floors);
  slab(mesh, plan, top, 0.6f, M_ROOF);
  parapet(mesh, plan, top, 1.0f, 0.3f, M_WHITE_METAL);
  roof_equipment(mesh, rng, plan, top, 4);
  // entrance canopy on the north face
  {
    Emit c(&mesh, M_WHITE_METAL);
    const Vec2 mid{centre.x, centre.y - hz};
    c.box(P3(mid + Vec2{0, -3.0f}, 4.6f), Vec3{7.0f, 0.2f, 3.2f});
    Emit pcol(&mesh, M_DARK_METAL);
    pcol.tube(P3(mid + Vec2{-6.0f, -5.6f}, 0), P3(mid + Vec2{-6.0f, -5.6f}, 4.4f), 0.15f, 8, false);
    pcol.tube(P3(mid + Vec2{6.0f, -5.6f}, 0), P3(mid + Vec2{6.0f, -5.6f}, 4.4f), 0.15f, 8, false);
  }
}

// 5. Folded pavilion: a faceted white shell with triangulated glazing.
void gen_pavilion(Scene& sc, Rng rng, Vec2 centre, float radius, float height) {
  Mesh& mesh = sc.opaque;
  std::vector<Vec2> base;
  const int n = 5;
  for (int i = 0; i < n; ++i) {
    const float a = static_cast<float>(i) / n * 2.0f * kPi + rng.range(-0.15f, 0.15f);
    const float r = radius * rng.range(0.8f, 1.15f);
    base.push_back(centre + Vec2{std::cos(a), std::sin(a)} * r);
  }
  const Vec3 apex = P3(centre + Vec2{rng.range(-0.25f, 0.25f), rng.range(-0.25f, 0.25f)} * radius, height);
  Emit shell(&mesh, M_WHITE_METAL);
  Emit glass(&mesh, M_GLASS_CLEAR);
  glass.element_random = rng.next();
  Emit truss(&mesh, M_WHITE_METAL);
  Emit rib(&mesh, M_WHITE_METAL);
  // each face: triangle (base_i, base_j, apex) subdivided into 4
  for (int i = 0; i < n; ++i) {
    const Vec3 A = P3(base[i], 0.0f), B = P3(base[(i + 1) % n], 0.0f), C = apex;
    const Vec3 AB = (A + B) * 0.5f, BC = (B + C) * 0.5f, CA = (C + A) * 0.5f;
    const Vec3 tris[4][3] = {{A, AB, CA}, {AB, B, BC}, {CA, BC, C}, {AB, BC, CA}};
    for (int k = 0; k < 4; ++k) {
      const Vec3 a = tris[k][0], b = tris[k][1], c = tris[k][2];
      const bool is_glass = rng.chance(0.45f) || k == 3;
      const Vec3 nrm = normalize(cross(b - a, c - a));
      if (is_glass) {
        const Vec3 rec = nrm * (-0.35f);
        // facade frame for interior mapping: u along ab, v perpendicular in-plane
        glass.facade = true;
        glass.facade_origin = a + rec;
        glass.facade_u = normalize(b - a);
        glass.facade_v = normalize(cross(nrm, glass.facade_u));
        glass.triangle(a + rec, b + rec, c + rec);
        // inner truss: three struts from the centroid to the edge midpoints
        const Vec3 cen = (a + b + c) * (1.0f / 3.0f) + rec * 0.5f;
        truss.tube(cen, (a + b) * 0.5f + rec * 0.5f, 0.12f, 8, false);
        truss.tube(cen, (b + c) * 0.5f + rec * 0.5f, 0.12f, 8, false);
        truss.tube(cen, (c + a) * 0.5f + rec * 0.5f, 0.12f, 8, false);
        truss.tube((a + b) * 0.5f + rec * 0.5f, (b + c) * 0.5f + rec * 0.5f, 0.09f, 8, false);
        truss.tube((b + c) * 0.5f + rec * 0.5f, (c + a) * 0.5f + rec * 0.5f, 0.09f, 8, false);
        truss.tube((c + a) * 0.5f + rec * 0.5f, (a + b) * 0.5f + rec * 0.5f, 0.09f, 8, false);
      } else {
        shell.triangle(a, b, c);
        // inset panel seams: a slightly recessed inner triangle reads as panels
        const Vec3 cen = (a + b + c) * (1.0f / 3.0f);
        const Vec3 ia = lerp(a, cen, 0.08f) + nrm * 0.02f, ib = lerp(b, cen, 0.08f) + nrm * 0.02f, ic = lerp(c, cen, 0.08f) + nrm * 0.02f;
        shell.triangle(ia, ib, ic);
      }
      // ribs along edges
      rib.tube(a + nrm * 0.02f, b + nrm * 0.02f, 0.2f, 8, false);
      rib.tube(b + nrm * 0.02f, c + nrm * 0.02f, 0.2f, 8, false);
      rib.tube(c + nrm * 0.02f, a + nrm * 0.02f, 0.2f, 8, false);
    }
  }
  // glass base band + floor
  Emit floor(&mesh, M_TERRAZZO);
  floor.polygon(plan_offset(base, 0.5f), 0.03f, true);
  Emit light(&mesh, M_LOBBY_LIGHT);
  light.polygon(plan_offset(base, -2.0f), 3.0f, false);
  // plinth
  Emit plinth(&mesh, M_CONCRETE_WHITE);
  plinth.wall(plan_offset(base, 1.2f), -0.3f, 0.35f, true, true);
  plinth.polygon(plan_offset(base, 1.2f), 0.35f, true);
}

// ---------------------------------------------------------------------------
// Landscape and street furniture
// ---------------------------------------------------------------------------

void gen_tree(Scene& sc, Rng rng, Vec3 base, float height) {
  Mesh& mesh = sc.opaque;
  Emit bark(&mesh, M_BARK);
  const float trunk_r = height * 0.035f;
  const Vec3 top = base + Vec3{rng.range(-0.3f, 0.3f), height * 0.45f, rng.range(-0.3f, 0.3f)};
  bark.frustum(base, top, trunk_r, trunk_r * 0.7f, 10, false);
  const int branches = 4;
  std::vector<Vec3> tips;
  for (int i = 0; i < branches; ++i) {
    const float a = static_cast<float>(i) / branches * 2.0f * kPi + rng.range(-0.4f, 0.4f);
    const Vec3 tip = top + Vec3{std::cos(a) * height * 0.22f, height * rng.range(0.18f, 0.32f), std::sin(a) * height * 0.22f};
    bark.frustum(top, tip, trunk_r * 0.6f, trunk_r * 0.2f, 7, false);
    tips.push_back(tip);
  }
  // canopy: leaf-cluster quads distributed in an ellipsoid
  Emit leaf(&sc.foliage, M_LEAF);
  const Vec3 cc = top + Vec3{0, height * 0.28f, 0};
  const float rx = height * 0.32f, ry = height * 0.28f;
  const int clusters = 26;
  for (int i = 0; i < clusters; ++i) {
    Vec3 d{rng.range(-1, 1), rng.range(-1, 1), rng.range(-1, 1)};
    d = normalize(d) * std::pow(rng.next(), 0.35f);
    const Vec3 p = cc + Vec3{d.x * rx, d.y * ry, d.z * rx};
    const float size = height * rng.range(0.16f, 0.24f);
    const float ang = rng.range(0, kPi);
    for (int q = 0; q < 2; ++q) {
      const float a = ang + static_cast<float>(q) * kPi * 0.5f;
      const Vec3 right{std::cos(a), 0, std::sin(a)};
      const Vec3 up = normalize(Vec3{rng.range(-0.3f, 0.3f), 1, rng.range(-0.3f, 0.3f)});
      const Vec3 a0 = p - right * size - up * size, b0 = p + right * size - up * size;
      const Vec3 c0 = p + right * size + up * size, d0 = p - right * size + up * size;
      leaf.quad(a0, b0, c0, d0, QuadUV{{0, 1}, {1, 1}, {1, 0}, {0, 0}});
      // canopy shading normal: radial from the canopy centre (smooth volume look)
      for (std::size_t vi = sc.foliage.vertices.size() - 4; vi < sc.foliage.vertices.size(); ++vi) {
        Vertex& v = sc.foliage.vertices[vi];
        const Vec3 rel = Vec3{(v.position.x - cc.x) / rx, (v.position.y - cc.y) / ry, (v.position.z - cc.z) / rx};
        v.normal = normalize(lerp(v.normal, normalize(rel), 0.85f));
        v.aux.w = 0.45f + 0.55f * clampf(length(rel), 0.0f, 1.0f);  // inner leaves darker
      }
    }
  }
}

void gen_lamp(Scene& sc, Vec3 base, float yaw) {
  Mesh& mesh = sc.opaque;
  Emit m(&mesh, M_DARK_METAL);
  const float h = 6.5f;
  m.frustum(base, base + Vec3{0, h, 0}, 0.11f, 0.07f, 10, true);
  const Vec3 dir{std::cos(yaw), 0, std::sin(yaw)};
  const Vec3 arm_end = base + Vec3{0, h, 0} + dir * 1.6f + Vec3{0, 0.25f, 0};
  m.tube(base + Vec3{0, h - 0.1f, 0}, arm_end, 0.05f, 8, false);
  Emit housing(&mesh, M_WHITE_METAL);
  housing.box(arm_end + Vec3{0, -0.08f, 0}, Vec3{0.28f, 0.06f, 0.55f}, dir, Vec3{0, 1, 0}, cross(dir, Vec3{0, 1, 0}));
  Emit l(&mesh, M_LAMP);
  l.box(arm_end + Vec3{0, -0.15f, 0}, Vec3{0.22f, 0.02f, 0.45f}, dir, Vec3{0, 1, 0}, cross(dir, Vec3{0, 1, 0}));
  PointLight pl;
  pl.position = arm_end + Vec3{0, -0.4f, 0};
  pl.radius = 22.0f;
  pl.color = Vec3{1.0f, 0.86f, 0.62f};
  pl.intensity = 160.0f;
  sc.lights.push_back(pl);
}

void gen_bench(Scene& sc, Vec3 pos, float yaw) {
  Mesh& mesh = sc.opaque;
  const Vec3 dir{std::cos(yaw), 0, std::sin(yaw)};
  const Vec3 side = cross(dir, Vec3{0, 1, 0});
  Emit c(&mesh, M_CONCRETE_WHITE);
  c.box(pos + Vec3{0, 0.22f, 0} - dir * 0.8f, Vec3{0.12f, 0.22f, 0.3f}, dir, Vec3{0, 1, 0}, side);
  c.box(pos + Vec3{0, 0.22f, 0} + dir * 0.8f, Vec3{0.12f, 0.22f, 0.3f}, dir, Vec3{0, 1, 0}, side);
  Emit w(&mesh, M_BRONZE);
  for (int i = 0; i < 4; ++i) {
    w.box(pos + Vec3{0, 0.47f, 0} + side * (-0.24f + 0.16f * static_cast<float>(i)), Vec3{1.0f, 0.02f, 0.06f}, dir, Vec3{0, 1, 0}, side);
  }
}

// Ribbon polygon along a polyline (for paths): returns ccw polygon.
std::vector<Vec2> ribbon(const std::vector<Vec2>& line, float half_width) {
  std::vector<Vec2> left, right;
  for (std::size_t i = 0; i < line.size(); ++i) {
    const Vec2 prev = line[i > 0 ? i - 1 : 0], next = line[std::min(i + 1, line.size() - 1)];
    const Vec2 d = normalize(next - prev);
    const Vec2 n{d.y, -d.x};
    left.push_back(line[i] - n * half_width);
    right.push_back(line[i] + n * half_width);
  }
  std::vector<Vec2> poly = right;
  for (auto it = left.rbegin(); it != left.rend(); ++it) poly.push_back(*it);
  if (plan_area(poly) < 0) std::reverse(poly.begin(), poly.end());
  return poly;
}

void gen_park(Scene& sc, Rng rng, Vec2 centre) {
  Mesh& mesh = sc.opaque;
  // Terraces: arcs around a sunken plaza, stepping up to the south.
  const Vec2 pc = centre;
  const float a0 = radians(15.0f), a1 = radians(165.0f);  // terraces on the south side, open to the north
  const int levels = 6;
  const float r0 = 21.0f, dr = 3.6f, dh = 0.5f;
  auto arc = [&](float r, int seg, bool reverse) {
    std::vector<Vec2> pts;
    for (int i = 0; i <= seg; ++i) {
      const float t = static_cast<float>(reverse ? seg - i : i) / seg;
      const float a = a0 + (a1 - a0) * t;
      pts.push_back(pc + Vec2{std::cos(a), std::sin(a)} * r);
    }
    return pts;
  };
  // plaza floor (sunken 0.0) as a fan
  {
    Emit p(&mesh, M_PLAZA);
    std::vector<Vec2> fan = arc(r0, 40, false);
    fan.push_back(pc);
    if (plan_area(fan) < 0) std::reverse(fan.begin(), fan.end());
    p.polygon(fan, 0.01f, true);
  }
  for (int k = 0; k < levels; ++k) {
    const float ri = r0 + dr * static_cast<float>(k), ro = ri + dr;
    const float y = dh * static_cast<float>(k + 1);
    std::vector<Vec2> ring = arc(ro, 48, false);
    std::vector<Vec2> inner = arc(ri, 48, true);
    ring.insert(ring.end(), inner.begin(), inner.end());
    if (plan_area(ring) < 0) std::reverse(ring.begin(), ring.end());
    Emit top(&mesh, (k % 2 == 0) ? M_GRASS : M_CONCRETE_WHITE);
    top.polygon(ring, y, true);
    // riser
    Emit riser(&mesh, M_CONCRETE_WHITE);
    riser.occlusion = 0.85f;
    std::vector<Vec2> ri_arc = arc(ri, 48, false);
    riser.wall(ri_arc, y - dh, y + 0.02f, false, false);
    // edge strip
    Emit strip(&mesh, M_CONCRETE_WHITE);
    std::vector<Vec2> outer_strip = arc(ri + 0.35f, 48, false);
    std::vector<Vec2> inner_strip = arc(ri, 48, true);
    outer_strip.insert(outer_strip.end(), inner_strip.begin(), inner_strip.end());
    if (plan_area(outer_strip) < 0) std::reverse(outer_strip.begin(), outer_strip.end());
    strip.polygon(outer_strip, y + 0.015f, true);
  }
  // Retaining wall on the outside of the top terrace
  {
    Emit w(&mesh, M_CONCRETE_WHITE);
    const float ro = r0 + dr * levels;
    w.wall(arc(ro, 48, false), 0.0f, dh * levels + 0.6f, false, true);
    w.wall(arc(ro, 48, false), dh * levels, dh * levels + 0.6f, false, false);
  }
  // Lawn around: handled by the plaza generator (ground). Trees on the terraces.
  for (int k = 0; k < levels; k += 2) {
    const float ri = r0 + dr * static_cast<float>(k) + dr * 0.5f;
    const float y = dh * static_cast<float>(k + 1);
    for (int i = 0; i < 4 + k; ++i) {
      const float a = a0 + (a1 - a0) * (static_cast<float>(i) + 0.5f) / static_cast<float>(4 + k);
      const Vec2 p = pc + Vec2{std::cos(a), std::sin(a)} * ri;
      gen_tree(sc, rng.child(100 + k * 20 + i), P3(p, y), rng.range(7.0f, 10.5f));
    }
  }
  // Benches facing the plaza on level 1
  for (int i = 0; i < 5; ++i) {
    const float a = a0 + (a1 - a0) * (static_cast<float>(i) + 0.5f) / 5.0f;
    const Vec2 p = pc + Vec2{std::cos(a), std::sin(a)} * (r0 + dr * 1.5f);
    gen_bench(sc, P3(p, dh * 2.0f), a + kPi * 0.5f);  // along the arc, facing inward
  }
  // Water feature in the plaza centre
  {
    Emit rim(&mesh, M_CONCRETE_WHITE);
    const std::vector<Vec2> pool = plan_circle(5.0f, 40, pc + Vec2{0, 4.0f});
    rim.wall(pool, 0.0f, 0.45f, true, true);
    rim.polygon(plan_offset(pool, 0.0f), 0.45f, true);
    Emit w(&mesh, M_WATER);
    w.polygon(plan_circle(4.6f, 40, pc + Vec2{0, 4.0f}), 0.38f, true);
  }
}

// Ground: plaza, sidewalks, streets, curbs, lane paint, context.
struct BlockDims {
  float hx{120.0f}, hz{100.0f};  // lot half extents (curb line)
  float road_w{16.0f};
  float walk_w{6.0f};
};

void gen_ground_and_streets(Scene& sc, Rng rng, const BlockDims& b) {
  Mesh& mesh = sc.opaque;
  const float curb_h = 0.14f;
  // Block plaza (raised by the curb) — whole lot area
  {
    Emit p(&mesh, M_PLAZA);
    p.polygon(plan_rect(b.hx, b.hz), curb_h, true);
    Emit c(&mesh, M_CURB);
    c.wall(plan_rect(b.hx, b.hz), 0.0f, curb_h, true, true);
  }
  // Streets ring: asphalt from the curb to the far sidewalk
  {
    Emit a(&mesh, M_ASPHALT);
    const float ox = b.hx + b.road_w, oz = b.hz + b.road_w;
    // four rectangles
    a.polygon(plan_rect(ox, b.road_w * 0.5f, Vec2{0, -(b.hz + b.road_w * 0.5f)}), 0.0f, true);
    a.polygon(plan_rect(ox, b.road_w * 0.5f, Vec2{0, (b.hz + b.road_w * 0.5f)}), 0.0f, true);
    a.polygon(plan_rect(b.road_w * 0.5f, oz, Vec2{-(b.hx + b.road_w * 0.5f), 0}), 0.0f, true);
    a.polygon(plan_rect(b.road_w * 0.5f, oz, Vec2{(b.hx + b.road_w * 0.5f), 0}), 0.0f, true);
    // far sidewalks and the outer ground
    Emit s(&mesh, M_SIDEWALK);
    const float sx = ox + b.walk_w, sz = oz + b.walk_w;
    s.polygon(plan_rect(sx, b.walk_w * 0.5f, Vec2{0, -(oz + b.walk_w * 0.5f)}), curb_h, true);
    s.polygon(plan_rect(sx, b.walk_w * 0.5f, Vec2{0, (oz + b.walk_w * 0.5f)}), curb_h, true);
    s.polygon(plan_rect(b.walk_w * 0.5f, sz, Vec2{-(ox + b.walk_w * 0.5f), 0}), curb_h, true);
    s.polygon(plan_rect(b.walk_w * 0.5f, sz, Vec2{(ox + b.walk_w * 0.5f), 0}), curb_h, true);
    Emit c(&mesh, M_CURB);
    c.wall(plan_rect(ox, oz), 0.0f, curb_h, true, false);
    // ground beyond (context lots), slightly below the sidewalk
    Emit g(&mesh, M_ASPHALT);
    const float far = 1600.0f;
    g.polygon({{-far, -far}, {far, -far}, {far, -sz}, {-far, -sz}}, -0.02f, true);
    g.polygon({{-far, sz}, {far, sz}, {far, far}, {-far, far}}, -0.02f, true);
    g.polygon({{-far, -sz}, {-sx, -sz}, {-sx, sz}, {-far, sz}}, -0.02f, true);
    g.polygon({{sx, -sz}, {far, -sz}, {far, sz}, {sx, sz}}, -0.02f, true);
  }
  // Lane paint: centre double yellow + dashed white per direction; crosswalks
  {
    Emit w(&mesh, M_LANE_WHITE);
    Emit yl(&mesh, M_LANE_YELLOW);
    const float y = 0.006f;
    auto road_strip = [&](Vec2 a, Vec2 bpt, float width, Emit& e, bool dashed) {
      const Vec2 d = normalize(bpt - a);
      const Vec2 n{d.y, -d.x};
      const float len = length(bpt - a);
      if (!dashed) {
        e.quad_metric(P3(a - n * width * 0.5f, y), P3(bpt - n * width * 0.5f, y), P3(bpt + n * width * 0.5f, y), P3(a + n * width * 0.5f, y));
        return;
      }
      for (float t = 0.0f; t + 3.0f <= len; t += 9.0f) {
        const Vec2 p0 = a + d * t, p1 = a + d * (t + 3.0f);
        e.quad_metric(P3(p0 - n * width * 0.5f, y), P3(p1 - n * width * 0.5f, y), P3(p1 + n * width * 0.5f, y), P3(p0 + n * width * 0.5f, y));
      }
    };
    const float ox = b.hx + b.road_w, oz = b.hz + b.road_w;
    const float cz_n = -(b.hz + b.road_w * 0.5f), cz_s = (b.hz + b.road_w * 0.5f);
    const float cx_w = -(b.hx + b.road_w * 0.5f), cx_e = (b.hx + b.road_w * 0.5f);
    for (float cz : {cz_n, cz_s}) {
      road_strip(Vec2{-ox + 6.0f, cz - 0.12f}, Vec2{ox - 6.0f, cz - 0.12f}, 0.12f, yl, false);
      road_strip(Vec2{-ox + 6.0f, cz + 0.12f}, Vec2{ox - 6.0f, cz + 0.12f}, 0.12f, yl, false);
      road_strip(Vec2{-ox + 6.0f, cz - 4.0f}, Vec2{ox - 6.0f, cz - 4.0f}, 0.12f, w, true);
      road_strip(Vec2{-ox + 6.0f, cz + 4.0f}, Vec2{ox - 6.0f, cz + 4.0f}, 0.12f, w, true);
    }
    for (float cx : {cx_w, cx_e}) {
      road_strip(Vec2{cx - 0.12f, -oz + 6.0f}, Vec2{cx - 0.12f, oz - 6.0f}, 0.12f, yl, false);
      road_strip(Vec2{cx + 0.12f, -oz + 6.0f}, Vec2{cx + 0.12f, oz - 6.0f}, 0.12f, yl, false);
      road_strip(Vec2{cx - 4.0f, -oz + 6.0f}, Vec2{cx - 4.0f, oz - 6.0f}, 0.12f, w, true);
      road_strip(Vec2{cx + 4.0f, -oz + 6.0f}, Vec2{cx + 4.0f, oz - 6.0f}, 0.12f, w, true);
    }
    // crosswalks at the block corners (across each road at the ends)
    auto zebra = [&](Vec2 start, Vec2 dir, Vec2 across, float across_len) {
      for (int i = 0; i < 12; ++i) {
        const Vec2 p = start + dir * (0.6f + 1.2f * static_cast<float>(i));
        const Vec2 q = p + across * across_len;
        const Vec2 n = dir * 0.3f;
        w.quad_metric(P3(p - n, y), P3(q - n, y), P3(q + n, y), P3(p + n, y));
      }
    };
    for (float sx : {-1.0f, 1.0f}) {
      for (float sz : {-1.0f, 1.0f}) {
        // across the east-west road near the corner
        zebra(Vec2{sx * (b.hx - 16.0f), sz * b.hz}, Vec2{sx, 0}, Vec2{0, sz}, b.road_w);
        zebra(Vec2{sx * b.hx, sz * (b.hz - 16.0f)}, Vec2{0, sz}, Vec2{sx, 0}, b.road_w);
      }
    }
  }
  // Street lamps along the block curb + far sidewalk, trees along the far sidewalk
  {
    const float lx = b.hx - 1.2f, lz = b.hz - 1.2f;
    for (float x = -lx + 12.0f; x < lx; x += 26.0f) {
      gen_lamp(sc, Vec3{x, curb_h, -lz}, -kPi * 0.5f);
      gen_lamp(sc, Vec3{x, curb_h, lz}, kPi * 0.5f);
    }
    for (float z = -lz + 14.0f; z < lz - 6.0f; z += 26.0f) {
      gen_lamp(sc, Vec3{-lx, curb_h, z}, kPi);
      gen_lamp(sc, Vec3{lx, curb_h, z}, 0.0f);
    }
    const float tx = b.hx + b.road_w + b.walk_w * 0.55f, tz = b.hz + b.road_w + b.walk_w * 0.55f;
    int i = 0;
    for (float x = -tx + 10.0f; x < tx - 6.0f; x += 13.0f, ++i) {
      gen_tree(sc, rng.child(300 + i), Vec3{x, curb_h, -tz}, rng.range(6.5f, 9.0f));
      gen_tree(sc, rng.child(400 + i), Vec3{x, curb_h, tz}, rng.range(6.5f, 9.0f));
    }
    for (float z = -tz + 10.0f; z < tz - 6.0f; z += 13.0f, ++i) {
      gen_tree(sc, rng.child(500 + i), Vec3{-tx, curb_h, z}, rng.range(6.5f, 9.0f));
      gen_tree(sc, rng.child(600 + i), Vec3{tx, curb_h, z}, rng.range(6.5f, 9.0f));
    }
  }
}

// Planting beds with shrubs (small foliage clusters) between the buildings.
void gen_planter(Scene& sc, Rng rng, Vec2 centre, float hx, float hz, float y) {
  Mesh& mesh = sc.opaque;
  const std::vector<Vec2> plan = plan_rounded_rect(hx, hz, std::min(hx, hz) * 0.5f, 6, centre);
  Emit c(&mesh, M_CONCRETE_WHITE);
  c.wall(plan, y, y + 0.6f, true, true);
  c.wall(plan_offset(plan, -0.2f), y + 0.2f, y + 0.6f, true, false);
  for (std::size_t i = 0; i < plan.size(); ++i) {
    const std::size_t j = (i + 1) % plan.size();
    const std::vector<Vec2> inner = plan_offset(plan, -0.2f);
    c.quad_metric(P3(plan[i], y + 0.6f), P3(plan[j], y + 0.6f), P3(inner[j], y + 0.6f), P3(inner[i], y + 0.6f));
  }
  Emit soil(&mesh, M_GRASS);
  soil.polygon(plan_offset(plan, -0.2f), y + 0.5f, true);
  Emit leaf(&sc.foliage, M_LEAF);
  const int shrubs = static_cast<int>(hx * hz * 0.25f);
  for (int i = 0; i < shrubs; ++i) {
    const Vec2 p = centre + Vec2{rng.range(-hx + 0.8f, hx - 0.8f), rng.range(-hz + 0.8f, hz - 0.8f)};
    const float s = rng.range(0.5f, 0.9f);
    const Vec3 b = P3(p, y + 0.5f + s * 0.6f);
    for (int q = 0; q < 3; ++q) {
      const float a = rng.range(0, kPi);
      const Vec3 right{std::cos(a), 0, std::sin(a)};
      const Vec3 up{0, 1, 0};
      leaf.quad(b - right * s - up * s * 0.7f, b + right * s - up * s * 0.7f, b + right * s + up * s * 0.7f, b - right * s + up * s * 0.7f,
                QuadUV{{0, 1}, {1, 1}, {1, 0}, {0, 0}});
    }
  }
  if (hx > 3.0f) gen_tree(sc, rng.child(9), P3(centre, y + 0.5f), rng.range(6.0f, 8.0f));
}

// Context: simple curtain-wall towers around the block so the sky has a skyline.
void gen_context(Scene& sc, Rng rng, const BlockDims& b) {
  Mesh& mesh = sc.opaque;
  const float start_x = b.hx + b.road_w + b.walk_w + 30.0f;
  const float start_z = b.hz + b.road_w + b.walk_w + 30.0f;
  struct Slot { Vec2 c; float hx, hz; };
  std::vector<Slot> slots;
  for (int ring = 0; ring < 2; ++ring) {
    const float rx = start_x + 110.0f * static_cast<float>(ring), rz = start_z + 110.0f * static_cast<float>(ring);
    for (int i = -2; i <= 2; ++i) {
      slots.push_back({Vec2{static_cast<float>(i) * 90.0f, -(rz + 30.0f)}, 24.0f, 26.0f});
      slots.push_back({Vec2{static_cast<float>(i) * 90.0f, (rz + 30.0f)}, 24.0f, 26.0f});
    }
    for (int i = -1; i <= 1; ++i) {
      slots.push_back({Vec2{-(rx + 30.0f), static_cast<float>(i) * 90.0f}, 26.0f, 24.0f});
      slots.push_back({Vec2{(rx + 30.0f), static_cast<float>(i) * 90.0f}, 26.0f, 24.0f});
    }
  }
  int idx = 0;
  for (const Slot& s : slots) {
    Rng r = rng.child(idx++);
    if (r.chance(0.18f)) continue;
    const float h = r.range(18.0f, 70.0f) * (length(s.c) > 330.0f ? r.range(1.2f, 2.4f) : 1.0f);
    const float hx = s.hx * r.range(0.6f, 1.0f), hz = s.hz * r.range(0.6f, 1.0f);
    const Mat glass = M_GLASS_CONTEXT;
    const std::vector<Vec2> plan = r.chance(0.3f) ? plan_rounded_rect(hx, hz, 6.0f, 4, s.c) : plan_rect(hx, hz, s.c);
    Emit g(&mesh, glass);
    g.element_random = r.next();
    const float floor_h = 3.8f;
    const int floors = static_cast<int>(h / floor_h);
    float u = 0.0f;
    for (std::size_t i = 0; i < plan.size(); ++i) {
      const std::size_t j = (i + 1) % plan.size();
      const float w = length(plan[j] - plan[i]);
      const float top = floor_h * static_cast<float>(floors);
      g.quad(P3(plan[i], 0.0f), P3(plan[j], 0.0f), P3(plan[j], top), P3(plan[i], top), QuadUV{{u, 0}, {u + w, 0}, {u + w, top}, {u, top}});
      u += w;
    }
    Emit sp(&mesh, M_SPANDREL);
    const std::vector<Vec2> ring = plan_offset(plan, 0.03f);
    for (int f = 1; f <= floors; ++f) {
      const float y = floor_h * static_cast<float>(f);
      sp.wall(ring, y - 0.7f, y + 0.1f, true, true);
    }
    slab(mesh, plan, floor_h * static_cast<float>(floors), 0.5f, M_ROOF);
    parapet(mesh, plan, floor_h * static_cast<float>(floors), 0.8f, 0.3f, M_DARK_METAL);
    roof_equipment(mesh, r, plan, floor_h * static_cast<float>(floors), 3);
  }
}

}  // namespace

std::vector<TextureSetSpec> Scene::texture_sets() const {
  std::vector<TextureSetSpec> sets;
  auto add = [&](const char* name, float r, float g, float b, float rough, int pattern) {
    TextureSetSpec s;
    s.name = name;
    s.fallback_rgb[0] = r; s.fallback_rgb[1] = g; s.fallback_rgb[2] = b;
    s.fallback_roughness = rough;
    s.fallback_pattern = pattern;
    sets.push_back(s);
  };
  add("flat", 1, 1, 1, 0.6f, 0);
  add("asphalt", 0.22f, 0.22f, 0.23f, 0.9f, 5);
  add("pavement_light", 0.62f, 0.60f, 0.57f, 0.7f, 3);
  add("paving_slabs", 0.55f, 0.55f, 0.54f, 0.65f, 3);
  add("terrazzo", 0.72f, 0.70f, 0.66f, 0.4f, 1);
  add("grass", 0.20f, 0.32f, 0.10f, 0.9f, 4);
  add("soil", 0.28f, 0.22f, 0.16f, 0.95f, 1);
  add("concrete_white", 0.78f, 0.77f, 0.74f, 0.6f, 1);
  add("concrete_smooth", 0.62f, 0.62f, 0.61f, 0.7f, 1);
  add("concrete_panels", 0.38f, 0.38f, 0.37f, 0.7f, 1);
  add("metal_silver", 0.80f, 0.80f, 0.82f, 0.3f, 2);
  add("metal_black", 0.08f, 0.08f, 0.09f, 0.45f, 2);
  add("bark", 0.30f, 0.24f, 0.18f, 0.9f, 1);
  add("marble", 0.80f, 0.80f, 0.80f, 0.25f, 1);
  return sets;
}

Scene generate_scene(const SceneParams& params) {
  Scene sc;
  sc.materials = make_materials();
  Rng root = root_rng(params.seed);
  BlockDims dims;
  gen_ground_and_streets(sc, root.child(1), dims);
  // Lots (x east, z south):
  gen_diagrid_tower(sc, root.child(10), Vec2{-68.0f, -52.0f}, 17.5f, 42, 4.0f);
  gen_lens_towers(sc, root.child(11), Vec2{62.0f, -48.0f}, radians(-12.0f), 34, 3.9f);
  gen_fin_tower(sc, root.child(12), Vec2{-74.0f, 52.0f}, 17.0f, 26, 3.8f);
  gen_xframe_block(sc, root.child(13), Vec2{72.0f, 58.0f}, 30.0f, 14.0f, 12, 4.4f);
  gen_pavilion(sc, root.child(14), Vec2{0.0f, 66.0f}, 16.0f, 21.0f);
  gen_park(sc, root.child(15), Vec2{0.0f, -6.0f});
  // planters between lots
  gen_planter(sc, root.child(20), Vec2{-20.0f, -75.0f}, 9.0f, 3.0f, 0.14f);
  gen_planter(sc, root.child(21), Vec2{-30.0f, -20.0f}, 4.0f, 10.0f, 0.14f);
  gen_planter(sc, root.child(22), Vec2{30.0f, 10.0f}, 3.0f, 12.0f, 0.14f);
  gen_planter(sc, root.child(23), Vec2{-40.0f, 88.0f}, 12.0f, 3.0f, 0.14f);
  gen_planter(sc, root.child(24), Vec2{40.0f, 88.0f}, 10.0f, 3.0f, 0.14f);
  gen_planter(sc, root.child(25), Vec2{100.0f, 0.0f}, 3.0f, 14.0f, 0.14f);
  gen_planter(sc, root.child(26), Vec2{-104.0f, 0.0f}, 3.0f, 14.0f, 0.14f);
  // tree groves on grass circles in the plaza corners, and paths
  {
    Rng g = root.child(40);
    const Vec2 groves[4] = {{-28.0f, -88.0f}, {28.0f, -88.0f}, {-100.0f, -6.0f}, {104.0f, 14.0f}};
    for (int k = 0; k < 4; ++k) {
      const float r = g.range(7.0f, 9.5f);
      Emit lawn(&sc.opaque, M_GRASS);
      const std::vector<Vec2> circle = plan_circle(r, 40, groves[k]);
      lawn.polygon(circle, 0.16f, true);
      Emit rim(&sc.opaque, M_CONCRETE_WHITE);
      rim.wall(circle, 0.14f, 0.34f, true, true);
      rim.polygon(plan_circle(r, 40, groves[k]), 0.34f, true);
      lawn.polygon(plan_circle(r - 0.25f, 40, groves[k]), 0.36f, true);
      for (int i = 0; i < 5; ++i) {
        const float a = g.range(0, 2 * kPi), rr = g.range(1.5f, r - 2.0f);
        gen_tree(sc, g.child(k * 10 + i), P3(groves[k] + Vec2{std::cos(a), std::sin(a)} * rr, 0.36f), g.range(7.5f, 11.0f));
      }
      for (int i = 0; i < 3; ++i) {
        const float a = static_cast<float>(i) / 3.0f * 2.0f * kPi + 0.4f;
        gen_bench(sc, P3(groves[k] + Vec2{std::cos(a), std::sin(a)} * (r + 1.2f), 0.14f), a + kPi * 0.5f);
      }
    }
    Emit path(&sc.opaque, M_SIDEWALK);
    const Vec2 corners[4] = {{-118.0f, -98.0f}, {118.0f, -98.0f}, {-118.0f, 98.0f}, {118.0f, 98.0f}};
    for (int k = 0; k < 4; ++k) {
      const std::vector<Vec2> ctrl = {corners[k], corners[k] * 0.55f + Vec2{g.range(-15.0f, 15.0f), g.range(-15.0f, 15.0f)}, Vec2{0.0f, -6.0f} + normalize(corners[k]) * 24.0f};
      path.polygon(ribbon(spline(ctrl, 10), 2.2f), 0.155f, true);
    }
    // bollards along the north edge
    Emit bol(&sc.opaque, M_SILVER);
    for (float x = -110.0f; x <= 110.0f; x += 4.0f) bol.tube(Vec3{x, 0.14f, -97.0f}, Vec3{x, 1.0f, -97.0f}, 0.09f, 8, true);
  }
  if (params.context_buildings) gen_context(sc, root.child(30), dims);
  sc.camera_position = Vec3{-150.0f, 26.0f, 165.0f};
  sc.camera_target = Vec3{-8.0f, 46.0f, -24.0f};
  return sc;
}

}  // namespace cb
