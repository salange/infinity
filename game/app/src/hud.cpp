#include "hud.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include <stb_easy_font.h>
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

#include "render/math.hpp"

namespace inf::app {

namespace {

using render::Mat4;
using render::Rhi;
using sim::Vec3;

constexpr double kRadarHalf = 0.21;      // radar half-size, NDC-y units
constexpr double kRadarCenterX = 0.70;   // NDC-y units from center (aspect-corrected at draw)
constexpr double kRadarCenterY = -0.68;
constexpr int kGridN = 21;               // terrain radar cells per side (odd)

struct Color {
  float r, g, b;
};

// Screen-space draw item: mesh local coords are "HUD units" (NDC-y based;
// x additionally divided by aspect at draw time).
Rhi::DrawItem hud_item(std::uint32_t mesh, double x, double y, double sx, double sy,
                       Color color, double aspect, double depth = 0.0001) {
  Rhi::DrawItem item;
  item.mesh = mesh;
  Mat4 m{};
  m.m[0] = static_cast<float>(sx / aspect);
  m.m[5] = static_cast<float>(sy);
  m.m[10] = 0.00001f;
  m.m[12] = static_cast<float>(x / aspect);
  m.m[13] = static_cast<float>(y);
  // Reversed-Z: callers still pass "smaller = closer" layering depths;
  // flip here so the HUD stack keeps its ordering under Greater.
  m.m[14] = static_cast<float>(1.0 - depth);
  m.m[15] = 1.0f;
  std::memcpy(item.mvp, m.m, sizeof(m.m));
  item.color[0] = color.r;
  item.color[1] = color.g;
  item.color[2] = color.b;
  item.color[3] = 1.0f;
  return item;
}

// A text string as an unlit mesh in local pixel coords (stb_easy_font),
// y flipped so +y is up; rebuilt only when the string changes.
class TextLine {
 public:
  void set(Rhi* rhi, const std::string& text) {
    if (text == text_) {
      return;
    }
    if (mesh_ != 0) {
      rhi->destroy_mesh(mesh_);
      mesh_ = 0;
    }
    text_ = text;
    if (text.empty()) {
      return;
    }
    static char quad_buffer[16000];
    unsigned char white[4] = {255, 255, 255, 255};
    std::string mutable_text = text;
    const int quads = stb_easy_font_print(0.0f, 0.0f, mutable_text.data(), white, quad_buffer,
                                          sizeof(quad_buffer));
    std::vector<float> vertices;
    vertices.reserve(static_cast<std::size_t>(quads) * 6 * 6);
    const auto* raw = reinterpret_cast<const float*>(quad_buffer);
    width_px_ = 0.0;
    for (int q = 0; q < quads; ++q) {
      float px[4];
      float py[4];
      for (int v = 0; v < 4; ++v) {
        px[v] = raw[(q * 4 + v) * 4 + 0];
        py[v] = -raw[(q * 4 + v) * 4 + 1];  // flip: +y up
        width_px_ = std::max(width_px_, static_cast<double>(px[v]));
      }
      const int tri[6] = {0, 1, 2, 0, 2, 3};
      for (const int v : tri) {
        vertices.push_back(px[v]);
        vertices.push_back(py[v]);
        vertices.push_back(0.0f);
        vertices.push_back(0.0f);
        vertices.push_back(0.0f);
        vertices.push_back(1.0f);
      }
    }
    if (!vertices.empty()) {
      mesh_ = rhi->create_mesh(vertices.data(), vertices.size());
    }
  }

  void destroy(Rhi* rhi) {
    if (mesh_ != 0) {
      rhi->destroy_mesh(mesh_);
      mesh_ = 0;
    }
    text_.clear();
  }

  std::uint32_t mesh() const { return mesh_; }
  double width_px() const { return width_px_; }

 private:
  std::string text_;
  std::uint32_t mesh_ = 0;
  double width_px_ = 0.0;
};

std::vector<float> unit_quad_vertices() {
  // Unit square in the xy plane, centered, normal +z.
  const float corners[4][2] = {{-0.5f, -0.5f}, {0.5f, -0.5f}, {0.5f, 0.5f}, {-0.5f, 0.5f}};
  const int tri[6] = {0, 1, 2, 0, 2, 3};
  std::vector<float> vertices;
  for (const int v : tri) {
    vertices.insert(vertices.end(),
                    {corners[v][0], corners[v][1], 0.0f, 0.0f, 0.0f, 1.0f});
  }
  return vertices;
}

std::vector<float> disc_vertices(int segments) {
  std::vector<float> vertices;
  for (int s = 0; s < segments; ++s) {
    const double a0 = 2.0 * 3.14159265358979323846 * s / segments;
    const double a1 = 2.0 * 3.14159265358979323846 * (s + 1) / segments;
    const float pts[3][2] = {
        {0.0f, 0.0f},
        {static_cast<float>(std::cos(a0) * 0.5), static_cast<float>(std::sin(a0) * 0.5)},
        {static_cast<float>(std::cos(a1) * 0.5), static_cast<float>(std::sin(a1) * 0.5)}};
    for (const auto& p : pts) {
      vertices.insert(vertices.end(), {p[0], p[1], 0.0f, 0.0f, 0.0f, 1.0f});
    }
  }
  return vertices;
}

std::vector<float> ring_vertices(int segments, float inner, float outer) {
  std::vector<float> vertices;
  const double tau = 2.0 * 3.14159265358979323846;
  for (int s = 0; s < segments; ++s) {
    const double a0 = tau * s / segments;
    const double a1 = tau * (s + 1) / segments;
    const float c0 = static_cast<float>(std::cos(a0));
    const float s0 = static_cast<float>(std::sin(a0));
    const float c1 = static_cast<float>(std::cos(a1));
    const float s1 = static_cast<float>(std::sin(a1));
    const float quad[4][2] = {{c0 * inner, s0 * inner},
                              {c0 * outer, s0 * outer},
                              {c1 * outer, s1 * outer},
                              {c1 * inner, s1 * inner}};
    const int tri[6] = {0, 1, 2, 0, 2, 3};
    for (const int v : tri) {
      vertices.insert(vertices.end(), {quad[v][0], quad[v][1], 0.0f, 0.0f, 0.0f, 1.0f});
    }
  }
  return vertices;
}

}  // namespace

struct Hud::Impl {
  Rhi* rhi;
  const gen::TerrainField* field;
  gen::PlanetParams planet;

  std::uint32_t quad_mesh = 0;
  std::uint32_t disc_mesh = 0;
  std::uint32_t ring_mesh = 0;
  TextLine speed_line;
  TextLine range_line;
  TextLine asl_line;
  TextLine biome_line;
  TextLine location_line;     // planet name under the radar when near it
  TextLine target_name_line;  // crosshair target readout (flight)
  TextLine target_info_line;
  TextLine letter_n, letter_s, letter_e, letter_w;
  std::vector<TextLine> card_lines;  // map-mode info card (lazy)

  // Terrain radar cache: elevation samples in a fixed east/north tangent
  // frame around a cached center, refreshed round-robin (bounded work per
  // frame, no hitches).
  std::vector<double> grid_elev;
  Vec3 grid_center_pos{};
  Vec3 grid_east{}, grid_north{};
  double grid_span = 0.0;
  int grid_cursor = 0;
  bool grid_valid = false;

  double biome_timer = 0.0;

  Impl(Rhi* rhi_in, const gen::TerrainField* field_in, const gen::PlanetParams& planet_in)
      : rhi(rhi_in), field(field_in), planet(planet_in) {
    const auto quad = unit_quad_vertices();
    quad_mesh = rhi->create_mesh(quad.data(), quad.size());
    const auto disc = disc_vertices(48);
    disc_mesh = rhi->create_mesh(disc.data(), disc.size());
    const auto ring = ring_vertices(48, 0.47f, 0.5f);
    ring_mesh = rhi->create_mesh(ring.data(), ring.size());
    grid_elev.assign(static_cast<std::size_t>(kGridN) * kGridN, 0.0);
    letter_n.set(rhi, "N");
    letter_s.set(rhi, "S");
    letter_e.set(rhi, "E");
    letter_w.set(rhi, "W");
  }

  ~Impl() {
    rhi->destroy_mesh(quad_mesh);
    rhi->destroy_mesh(disc_mesh);
    rhi->destroy_mesh(ring_mesh);
    for (TextLine* line : {&speed_line, &range_line, &asl_line, &biome_line, &location_line,
                           &target_name_line, &target_info_line, &letter_n, &letter_s,
                           &letter_e, &letter_w}) {
      line->destroy(rhi);
    }
    for (TextLine& line : card_lines) {
      line.destroy(rhi);
    }
  }

  double elevation_at(const Vec3& dir) const {
    return field->elevation_m(gen::Dir3{det::Real(dir.x), det::Real(dir.y), det::Real(dir.z)})
        .to_double();
  }

  void text_item(std::vector<Rhi::DrawItem>* items, TextLine& line, double x, double y,
                 double px_scale, Color color, double aspect, bool center = false) {
    if (line.mesh() == 0) {
      return;
    }
    const double offset = center ? -line.width_px() * px_scale * 0.5 : 0.0;
    items->push_back(hud_item(line.mesh(), x + offset, y, px_scale, px_scale, color, aspect,
                              0.00005));
  }

  // --- terrain radar -----------------------------------------------------
  void refresh_grid(const Vec3& position, double altitude) {
    const double span = std::clamp(std::abs(altitude) * 8.0, 800.0, 16'000.0);
    const Vec3 up = sim::normalize(position);
    const Vec3 axis{0.0, 0.0, 1.0};  // gen::kNorthAxis
    Vec3 north = axis - up * sim::dot(axis, up);
    if (sim::length(north) < 1e-6) {
      north = Vec3{1.0, 0.0, 0.0};
    }
    north = sim::normalize(north);
    const Vec3 east = sim::cross(north, up);

    const bool recenter = !grid_valid || span != grid_span ||
                          sim::length(position - grid_center_pos) > span * 0.08;
    if (recenter) {
      grid_center_pos = position;
      grid_east = east;
      grid_north = north;
      grid_span = span;
      grid_cursor = 0;
      grid_valid = true;
    }
    // Round-robin: a bounded number of samples per frame.
    const double planet_r = planet.radius_m.to_double();
    const int budget = 48;
    for (int n = 0; n < budget; ++n) {
      const int index = grid_cursor;
      grid_cursor = (grid_cursor + 1) % (kGridN * kGridN);
      const int gi = index % kGridN;
      const int gj = index / kGridN;
      const double ox = (gi - kGridN / 2) * (grid_span / kGridN);
      const double oy = (gj - kGridN / 2) * (grid_span / kGridN);
      const Vec3 sample_pos =
          grid_center_pos + grid_east * ox + grid_north * oy;
      const Vec3 dir = sim::normalize(sample_pos);
      grid_elev[static_cast<std::size_t>(index)] = elevation_at(dir);
      (void)planet_r;
    }
  }

  // Circular radar chrome: dark disc, outer ring, inner range ring,
  // center cross lines and edge ticks ("common games" look).
  void radar_chrome(std::vector<Rhi::DrawItem>* items, double cx, double aspect) {
    const Color line{0.35f, 0.75f, 0.65f};
    items->push_back(hud_item(disc_mesh, cx, kRadarCenterY, kRadarHalf * 2.2, kRadarHalf * 2.2,
                              Color{0.03f, 0.07f, 0.08f}, aspect, 0.0002));
    items->push_back(hud_item(ring_mesh, cx, kRadarCenterY, kRadarHalf * 2.2, kRadarHalf * 2.2,
                              line, aspect, 0.00012));
    items->push_back(hud_item(ring_mesh, cx, kRadarCenterY, kRadarHalf * 1.1, kRadarHalf * 1.1,
                              Color{0.2f, 0.45f, 0.4f}, aspect, 0.00012));
    const double px = 0.0028;
    items->push_back(hud_item(quad_mesh, cx, kRadarCenterY, kRadarHalf * 2.05, px,
                              Color{0.16f, 0.36f, 0.32f}, aspect, 0.00013));
    items->push_back(hud_item(quad_mesh, cx, kRadarCenterY, px, kRadarHalf * 2.05,
                              Color{0.16f, 0.36f, 0.32f}, aspect, 0.00013));
    for (int t = 0; t < 4; ++t) {
      const double angle = t * 3.14159265358979323846 * 0.5;
      const double tx = std::cos(angle) * kRadarHalf * 1.04;
      const double ty = std::sin(angle) * kRadarHalf * 1.04;
      items->push_back(hud_item(quad_mesh, cx + tx, kRadarCenterY + ty, px * 3.0, px * 3.0,
                                line, aspect, 0.00012));
    }
  }

  void radar_atmosphere(std::vector<Rhi::DrawItem>* items, const sim::Player& player,
                        double aspect) {
    const double cx = kRadarCenterX * aspect;
    const Vec3 position = player.position();
    const Vec3 up = sim::normalize(position);
    refresh_grid(position, player.altitude());

    // Heading frame: radar up = the player's forward projected to the
    // tangent plane; radar right = heading x ... (right-handed on screen).
    Vec3 heading = player.forward() - up * sim::dot(player.forward(), up);
    if (sim::length(heading) < 1e-6) {
      heading = grid_north;
    }
    heading = sim::normalize(heading);
    const Vec3 screen_right = sim::cross(heading, up);

    radar_chrome(items, cx, aspect);

    // Elevation range for coloring.
    double lo = 1e30;
    double hi = -1e30;
    for (const double e : grid_elev) {
      lo = std::min(lo, e);
      hi = std::max(hi, e);
    }
    const double range = std::max(1.0, hi - lo);
    const double sea = planet.sea_level_m.to_double();
    const bool has_sea = planet.type == gen::PlanetType::EarthLike;

    const double cell = (kRadarHalf * 2.0) / kGridN;
    for (int index = 0; index < kGridN * kGridN; ++index) {
      const int gi = index % kGridN;
      const int gj = index / kGridN;
      const double ox = (gi - kGridN / 2) * (grid_span / kGridN);
      const double oy = (gj - kGridN / 2) * (grid_span / kGridN);
      const Vec3 world_offset = grid_east * ox + grid_north * oy;
      const double sx = sim::dot(world_offset, screen_right) / grid_span * (kRadarHalf * 2.0);
      const double sy = sim::dot(world_offset, heading) / grid_span * (kRadarHalf * 2.0);
      if (sx * sx + sy * sy > (kRadarHalf - cell) * (kRadarHalf - cell)) {
        continue;  // circular radar: clip to the ring
      }
      const double elev = grid_elev[static_cast<std::size_t>(index)];
      Color color{};
      if (has_sea && elev < sea) {
        color = Color{0.10f, 0.25f, 0.55f};
      } else {
        const auto t = static_cast<float>((elev - lo) / range);
        color = Color{0.15f + 0.55f * t, 0.35f + 0.30f * t, 0.15f + 0.15f * t};
      }
      items->push_back(hud_item(quad_mesh, cx + sx, kRadarCenterY + sy, cell * 1.45,
                                cell * 1.45, color, aspect, 0.00015));
    }

    // Player marker.
    items->push_back(hud_item(disc_mesh, cx, kRadarCenterY, cell * 1.6, cell * 1.6,
                              Color{1.0f, 1.0f, 1.0f}, aspect, 0.0001));

    // Cardinal letters on the panel edge (rotate with heading).
    const struct {
      TextLine* line;
      Vec3 dir;
    } cardinals[4] = {{&letter_n, grid_north},
                      {&letter_s, grid_north * -1.0},
                      {&letter_e, grid_east},
                      {&letter_w, grid_east * -1.0}};
    for (const auto& cardinal : cardinals) {
      const double sx = sim::dot(cardinal.dir, screen_right);
      const double sy = sim::dot(cardinal.dir, heading);
      const double edge = kRadarHalf * 0.9;
      text_item(items, *cardinal.line, cx + sx * edge,
                kRadarCenterY + sy * edge - 0.012, 0.0035, Color{1.0f, 0.85f, 0.3f}, aspect,
                true);
    }
  }

  // Ship-plane system radar (2026-08-31): the disc IS the plane spanned
  // by the camera axis and the ship's left/right axis — top of the disc
  // = straight ahead along the camera, bottom = behind, left/right =
  // ship left/right. Bodies project into that plane; the camera-up
  // component becomes the elevation bar (above the plane = stem up,
  // below = stem down; the dim tick marks the in-plane point, the ring
  // marks the current anchor body). Distance is linear out to the inner
  // (half) ring, then log-compressed so the outermost body sits at the
  // rim.
  void radar_system(std::vector<Rhi::DrawItem>* items, const sim::Player& player,
                    const std::vector<RadarBody>& bodies, double aspect) {
    const double cx = kRadarCenterX * aspect;
    radar_chrome(items, cx, aspect);
    if (bodies.empty()) {
      return;
    }

    // Radar basis straight from the camera: no horizon projection.
    const Vec3 plane_fwd = sim::normalize(player.forward());
    const Vec3 up = sim::normalize(player.up());
    const Vec3 plane_right = sim::normalize(sim::cross(plane_fwd, up));

    double max_dist = 1.0;
    for (const RadarBody& body : bodies) {
      max_dist = std::max(max_dist, sim::length(body.rel));
    }
    // Two-zone range mapping: linear inside d_half (drawn out to the
    // inner ring), log-compressed from there to the rim, calibrated so
    // the farthest body lands exactly on the rim.
    const double rim = kRadarHalf * 0.95;
    const double r_half = kRadarHalf * 0.55;  // matches the inner chrome ring
    const double d_half = std::max(1.0, max_dist * 0.15);
    const double outer_denom = std::log1p(std::max(1e-9, (max_dist - d_half) / d_half));
    const auto range_map = [&](double d) {
      if (d <= d_half) {
        return (d / d_half) * r_half;
      }
      return r_half + (rim - r_half) * std::log1p((d - d_half) / d_half) / outer_denom;
    };

    for (const RadarBody& body : bodies) {
      const double dist = sim::length(body.rel);
      const double fx = sim::dot(body.rel, plane_right);
      const double fy = sim::dot(body.rel, plane_fwd);
      const double fz = sim::dot(body.rel, up);  // above/below the radar plane
      const double planar = std::max(1.0, std::hypot(fx, fy));
      const double rho = range_map(dist);
      const double base_x = cx + (fx / planar) * rho;
      const double base_y = kRadarCenterY + (fy / planar) * rho;

      // Elevation bar: signed, compressed with the same two-zone map.
      const double bar_span = kRadarHalf * 0.30;
      double bar = range_map(std::abs(fz)) / rim * bar_span * (fz < 0.0 ? -1.0 : 1.0);
      bar = std::clamp(bar, -bar_span, bar_span);

      const double icon = kRadarHalf * 0.085 * body.scale;
      const Color color{body.color[0], body.color[1], body.color[2]};
      // Base tick on the plane.
      items->push_back(hud_item(quad_mesh, base_x, base_y, icon * 0.9, kRadarHalf * 0.014,
                                Color{color.r * 0.55f, color.g * 0.55f, color.b * 0.55f},
                                aspect, 0.00014));
      // Stem from the plane to the icon.
      if (std::abs(bar) > kRadarHalf * 0.02) {
        items->push_back(hud_item(quad_mesh, base_x, base_y + bar * 0.5, kRadarHalf * 0.012,
                                  std::abs(bar),
                                  Color{color.r * 0.7f, color.g * 0.7f, color.b * 0.7f},
                                  aspect, 0.00014));
      }
      // The body icon, constant size.
      items->push_back(
          hud_item(disc_mesh, base_x, base_y + bar, icon, icon, color, aspect, 0.00013));
      if (body.anchor) {
        // Ring around the current anchor body.
        items->push_back(hud_item(ring_mesh, base_x, base_y + bar, icon * 2.0, icon * 2.0,
                                  Color{1.0f, 0.85f, 0.3f}, aspect, 0.00013));
      }
    }
  }
};

Hud::Hud(Rhi* rhi, const gen::TerrainField* field, const gen::PlanetParams& planet)
    : impl_(std::make_unique<Impl>(rhi, field, planet)) {}

Hud::~Hud() = default;

void Hud::build(std::vector<Rhi::DrawItem>* items, const sim::Player& player,
                const std::vector<RadarBody>& bodies, double measured_speed_mps,
                double aspect, int height_px, double dt, const std::string& location_name,
                const TargetInfo& target) {
  Impl& impl = *impl_;
  (void)height_px;

  const double altitude = player.altitude();
  double atmosphere = impl.planet.atmosphere_height_m.to_double();
  if (atmosphere <= 0.0) {
    atmosphere = 6000.0;  // display band for airless bodies
  }
  const bool near_planet = altitude < atmosphere;

  // --- lower left: velocity + altitude/distance ------------------------
  char buffer[64];
  if (measured_speed_mps >= 1000.0) {
    std::snprintf(buffer, sizeof(buffer), "SPD %7.2f km/s", measured_speed_mps / 1000.0);
  } else {
    std::snprintf(buffer, sizeof(buffer), "SPD %7.1f m/s", measured_speed_mps);
  }
  impl.speed_line.set(impl.rhi, buffer);
  if (near_planet) {
    std::snprintf(buffer, sizeof(buffer), "AGL %7.0f m", altitude);
  } else if (altitude >= 100'000.0) {
    std::snprintf(buffer, sizeof(buffer), "DST %7.0f km", altitude / 1000.0);
  } else {
    std::snprintf(buffer, sizeof(buffer), "DST %7.1f km", altitude / 1000.0);
  }
  impl.range_line.set(impl.rhi, buffer);
  if (near_planet) {
    // Height above sea level (sea = radius + sea offset; 0 for airless
    // types, still a useful datum above the nominal radius).
    const double sea_r =
        impl.planet.radius_m.to_double() + impl.planet.sea_level_m.to_double();
    const double asl = sim::length(player.position()) - sea_r;
    std::snprintf(buffer, sizeof(buffer), "ASL %7.0f m", asl);
    impl.asl_line.set(impl.rhi, buffer);
  } else {
    impl.asl_line.set(impl.rhi, "");
  }
  const Color text_color{0.85f, 0.95f, 1.0f};
  impl.text_item(items, impl.speed_line, -0.96 * aspect, -0.78, 0.0042, text_color, aspect);
  impl.text_item(items, impl.range_line, -0.96 * aspect, -0.85, 0.0042, text_color, aspect);
  impl.text_item(items, impl.asl_line, -0.96 * aspect, -0.92, 0.0042, text_color, aspect);

  // --- lower right: radar + location/biome ------------------------------
  if (near_planet) {
    impl.radar_atmosphere(items, player, aspect);
    impl.biome_timer -= dt;
    if (impl.biome_timer <= 0.0) {
      impl.biome_timer = 1.0;
      const sim::Vec3 dir = sim::normalize(player.position());
      const gen::BlendedParams blended = impl.field->provinces().sample(
          gen::Dir3{det::Real(dir.x), det::Real(dir.y), det::Real(dir.z)});
      impl.biome_line.set(impl.rhi, gen::to_string(blended.dominant_archetype));
    }
    // Planet name first, biome underneath.
    impl.location_line.set(impl.rhi, location_name);
    impl.text_item(items, impl.location_line, kRadarCenterX * aspect,
                   kRadarCenterY - kRadarHalf - 0.06, 0.0042, Color{1.0f, 0.85f, 0.45f},
                   aspect, true);
    impl.text_item(items, impl.biome_line, kRadarCenterX * aspect,
                   kRadarCenterY - kRadarHalf - 0.105, 0.0042, Color{0.9f, 0.9f, 0.7f},
                   aspect, true);
  } else {
    impl.radar_system(items, player, bodies, aspect);
    impl.biome_line.set(impl.rhi, "");
    impl.location_line.set(impl.rhi, "");
  }

  // --- crosshair target readout (flight) --------------------------------
  if (target.valid) {
    impl.target_name_line.set(impl.rhi, target.name);
    char info[96];
    const double km = target.distance_m / 1000.0;
    char dist_text[32];
    if (km >= 1.0e6) {
      std::snprintf(dist_text, sizeof(dist_text), "%.2f Gm", target.distance_m / 1.0e9);
    } else if (km >= 1000.0) {
      std::snprintf(dist_text, sizeof(dist_text), "%.1f Mm", target.distance_m / 1.0e6);
    } else {
      std::snprintf(dist_text, sizeof(dist_text), "%.0f km", km);
    }
    if (target.eta_s >= 0.0 && target.eta_s < 99.0 * 3600.0) {
      const int total = static_cast<int>(target.eta_s);
      std::snprintf(info, sizeof(info), "DST %s   ETA %02d:%02d:%02d", dist_text,
                    total / 3600, (total / 60) % 60, total % 60);
    } else {
      std::snprintf(info, sizeof(info), "DST %s", dist_text);
    }
    impl.target_info_line.set(impl.rhi, info);
    impl.text_item(items, impl.target_name_line, 0.0, -0.085, 0.0042,
                   Color{1.0f, 0.85f, 0.45f}, aspect, true);
    impl.text_item(items, impl.target_info_line, 0.0, -0.13, 0.0038,
                   Color{0.85f, 0.95f, 1.0f}, aspect, true);
  } else {
    impl.target_name_line.set(impl.rhi, "");
    impl.target_info_line.set(impl.rhi, "");
  }
}

void Hud::build_map_card(std::vector<Rhi::DrawItem>* items,
                         const std::vector<std::string>& lines, double x_ndc, double y_ndc,
                         double aspect, int height_px) {
  Impl& impl = *impl_;
  if (lines.empty()) {
    return;
  }
  while (impl.card_lines.size() < lines.size()) {
    impl.card_lines.emplace_back();
  }
  const double px = 2.0 / height_px;              // one pixel in NDC-y
  const double text_scale = px * 1.6;             // stb_easy_font glyph scale
  const double line_h = 14.0 * text_scale;
  const double pad = 8.0 * px;
  double width_px = 0.0;
  for (std::size_t i = 0; i < lines.size(); ++i) {
    impl.card_lines[i].set(impl.rhi, lines[i]);
    width_px = std::max(width_px, impl.card_lines[i].width_px());
  }
  for (std::size_t i = lines.size(); i < impl.card_lines.size(); ++i) {
    impl.card_lines[i].set(impl.rhi, "");
  }
  const double card_w = width_px * text_scale + 2.0 * pad;
  const double card_h = static_cast<double>(lines.size()) * line_h + 2.0 * pad;
  // Anchor to the upper right of the pointer, kept on screen.
  double left = x_ndc + 14.0 * px;
  double top = y_ndc + card_h * 0.5;
  left = std::min(left, 1.0 - card_w - 4.0 * px);
  top = std::clamp(top, -1.0 + card_h + 4.0 * px, 1.0 - 4.0 * px);
  {
    Rhi::DrawItem bg = hud_item(impl.quad_mesh, left + card_w * 0.5, top - card_h * 0.5,
                                card_w, card_h, Color{0.04f, 0.07f, 0.11f}, aspect, 0.00008);
    bg.color[3] = 0.85f;
    bg.translucent = true;
    items->push_back(bg);
  }
  for (std::size_t i = 0; i < lines.size(); ++i) {
    const Color color = i == 0 ? Color{1.0f, 0.85f, 0.45f} : Color{0.85f, 0.92f, 1.0f};
    impl.text_item(items, impl.card_lines[i], left + pad,
                   top - pad - line_h * static_cast<double>(i) - 2.0 * px, text_scale, color,
                   aspect);
  }
}

}  // namespace inf::app
