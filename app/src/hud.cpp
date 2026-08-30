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
  m.m[14] = static_cast<float>(depth);
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

}  // namespace

struct Hud::Impl {
  Rhi* rhi;
  const gen::TerrainField* field;
  gen::PlanetParams planet;

  std::uint32_t quad_mesh = 0;
  std::uint32_t disc_mesh = 0;
  TextLine speed_line;
  TextLine range_line;
  TextLine biome_line;
  TextLine letter_n, letter_s, letter_e, letter_w;

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
    const auto disc = disc_vertices(24);
    disc_mesh = rhi->create_mesh(disc.data(), disc.size());
    grid_elev.assign(static_cast<std::size_t>(kGridN) * kGridN, 0.0);
    letter_n.set(rhi, "N");
    letter_s.set(rhi, "S");
    letter_e.set(rhi, "E");
    letter_w.set(rhi, "W");
  }

  ~Impl() {
    rhi->destroy_mesh(quad_mesh);
    rhi->destroy_mesh(disc_mesh);
    for (TextLine* line : {&speed_line, &range_line, &biome_line, &letter_n, &letter_s,
                           &letter_e, &letter_w}) {
      line->destroy(rhi);
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

    // Background panel.
    items->push_back(hud_item(quad_mesh, cx, kRadarCenterY, kRadarHalf * 2.15,
                              kRadarHalf * 2.15, Color{0.05f, 0.08f, 0.10f}, aspect, 0.0002));

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
      if (std::abs(sx) > kRadarHalf - cell * 0.5 || std::abs(sy) > kRadarHalf - cell * 0.5) {
        continue;  // keep the rotated grid inside the square panel
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

  void radar_space(std::vector<Rhi::DrawItem>* items, const sim::Player& player,
                   double aspect) {
    const double cx = kRadarCenterX * aspect;
    // Background panel.
    items->push_back(hud_item(quad_mesh, cx, kRadarCenterY, kRadarHalf * 2.15,
                              kRadarHalf * 2.15, Color{0.05f, 0.08f, 0.10f}, aspect, 0.0002));

    const Vec3 position = player.position();
    const Vec3 forward = player.forward();
    const Vec3 up = player.up();
    const Vec3 right = sim::cross(forward, up);

    const Vec3 to_body = position * -1.0;  // planet center is the origin
    const double distance = sim::length(to_body);
    const Vec3 dir = sim::normalize(to_body);
    const double fwd_component = sim::dot(dir, forward);
    double sx = sim::dot(dir, right);
    double sy = sim::dot(dir, up);
    if (fwd_component < 0.0) {
      // Behind: pin to the panel edge in the correct direction.
      const double len = std::max(1e-6, std::hypot(sx, sy));
      sx = sx / len;
      sy = sy / len;
    }
    const double planet_r = planet.radius_m.to_double();
    const double angular = std::asin(std::clamp(planet_r / std::max(distance, planet_r + 1.0),
                                                0.0, 1.0));
    const double disc_size =
        std::clamp(angular / 1.2, 0.06, 1.0) * kRadarHalf * 1.5;
    const double px = cx + std::clamp(sx, -0.85, 0.85) * kRadarHalf;
    const double py = kRadarCenterY + std::clamp(sy, -0.85, 0.85) * kRadarHalf;
    const float dim = fwd_component < 0.0 ? 0.45f : 1.0f;
    items->push_back(hud_item(disc_mesh, px, py, disc_size, disc_size,
                              Color{0.35f * dim, 0.65f * dim, 0.45f * dim}, aspect, 0.00015));
    // Axis marker through the disc (planet axis = +Z north).
    const Vec3 axis{0.0, 0.0, 1.0};
    const double ax = sim::dot(axis, right);
    const double ay = sim::dot(axis, up);
    const double axis_len = std::max(1e-6, std::hypot(ax, ay));
    const double nx = ax / axis_len;
    const double ny = ay / axis_len;
    // N and S letters at the disc's poles.
    text_item(items, letter_n, px + nx * disc_size * 0.62, py + ny * disc_size * 0.62 - 0.012,
              0.0035, Color{1.0f, 0.85f, 0.3f}, aspect, true);
    text_item(items, letter_s, px - nx * disc_size * 0.62, py - ny * disc_size * 0.62 - 0.012,
              0.0035, Color{1.0f, 0.85f, 0.3f}, aspect, true);
  }
};

Hud::Hud(Rhi* rhi, const gen::TerrainField* field, const gen::PlanetParams& planet)
    : impl_(std::make_unique<Impl>(rhi, field, planet)) {}

Hud::~Hud() = default;

void Hud::build(std::vector<Rhi::DrawItem>* items, const sim::Player& player,
                double measured_speed_mps, double aspect, int height_px, double dt) {
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
    std::snprintf(buffer, sizeof(buffer), "ALT %7.0f m", altitude);
  } else if (altitude >= 100'000.0) {
    std::snprintf(buffer, sizeof(buffer), "DST %7.0f km", altitude / 1000.0);
  } else {
    std::snprintf(buffer, sizeof(buffer), "DST %7.1f km", altitude / 1000.0);
  }
  impl.range_line.set(impl.rhi, buffer);
  const Color text_color{0.85f, 0.95f, 1.0f};
  impl.text_item(items, impl.speed_line, -0.96 * aspect, -0.84, 0.0042, text_color, aspect);
  impl.text_item(items, impl.range_line, -0.96 * aspect, -0.91, 0.0042, text_color, aspect);

  // --- lower right: radar + biome --------------------------------------
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
    impl.text_item(items, impl.biome_line, kRadarCenterX * aspect, kRadarCenterY - kRadarHalf - 0.06,
                   0.0042, Color{0.9f, 0.9f, 0.7f}, aspect, true);
  } else {
    impl.radar_space(items, player, aspect);
    impl.biome_line.set(impl.rhi, "");
  }
}

}  // namespace inf::app
