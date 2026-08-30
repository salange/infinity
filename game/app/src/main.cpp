#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <GLFW/glfw3.h>

#include "core/ephem/ephemeris.hpp"
#include "core/key.hpp"
#include "gen/version.hpp"
#include "core/time/world_clock.hpp"
#include "gen/planet.hpp"
#include "gen/system.hpp"
#include "gen/terrain.hpp"
#include "gen/terrain_sampler.hpp"
#include "gen/universe.hpp"
#include "hud.hpp"
#include "render/math.hpp"
#include "render/rhi.hpp"
#include "sim/map_camera.hpp"
#include "sim/player.hpp"
#include "world/chunk_manager.hpp"
#include "world/edit_store.hpp"
#include "gen/effective_field.hpp"

namespace {

using inf::render::Mat4;
using RVec3 = inf::render::Vec3;
using SVec3 = inf::sim::Vec3;

constexpr double kFovY = 1.1;

struct AppState {
  inf::render::Rhi* rhi = nullptr;
  int width = 1280;
  int height = 720;
};

void framebuffer_size_callback(GLFWwindow* window, int width, int height) {
  auto* state = static_cast<AppState*>(glfwGetWindowUserPointer(window));
  if (state != nullptr && state->rhi != nullptr && width > 0 && height > 0) {
    state->width = width;
    state->height = height;
    state->rhi->resize(static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height));
  }
}

struct AddrHash {
  std::size_t operator()(const inf::core::ChunkAddr& a) const {
    std::uint64_t packed = (static_cast<std::uint64_t>(a.face) << 56U) ^
                           (static_cast<std::uint64_t>(a.lod) << 48U) ^
                           (static_cast<std::uint64_t>(static_cast<std::uint16_t>(a.shell))
                            << 32U) ^
                           (static_cast<std::uint64_t>(a.i) << 16U) ^ a.j;
    packed ^= packed >> 33U;
    packed *= 0xFF51AFD7ED558CCDULL;
    packed ^= packed >> 33U;
    return static_cast<std::size_t>(packed);
  }
};

struct LoadedChunk {
  std::uint32_t mesh_id = 0;
  RVec3 origin;
};

RVec3 to_render(const SVec3& v) { return RVec3{v.x, v.y, v.z}; }

// Unit cube (side 1, centered) as a lit-format triangle soup; used scaled
// for beams and HUD quads (unlit color path ignores the normals).
std::vector<float> unit_cube_vertices() {
  static constexpr float kFaces[6][7] = {
      // normal xyz, then axis selectors handled below per face
      {1, 0, 0, 0, 0, 0, 0},  {-1, 0, 0, 0, 0, 0, 0}, {0, 1, 0, 0, 0, 0, 0},
      {0, -1, 0, 0, 0, 0, 0}, {0, 0, 1, 0, 0, 0, 0},  {0, 0, -1, 0, 0, 0, 0},
  };
  std::vector<float> vertices;
  vertices.reserve(36 * 6);
  for (const auto& face : kFaces) {
    const float nx = face[0];
    const float ny = face[1];
    const float nz = face[2];
    // Build tangent axes for the face.
    const float ux = ny != 0 ? 1.0f : 0.0f;
    const float uy = ny != 0 ? 0.0f : (nz != 0 ? 1.0f : 0.0f);
    const float uz = (nx != 0) ? 1.0f : 0.0f;
    const float vx = ny * uz - nz * uy;
    const float vy = nz * ux - nx * uz;
    const float vz = nx * uy - ny * ux;
    const float corners[4][3] = {
        {(nx - ux - vx) * 0.5f, (ny - uy - vy) * 0.5f, (nz - uz - vz) * 0.5f},
        {(nx + ux - vx) * 0.5f, (ny + uy - vy) * 0.5f, (nz + uz - vz) * 0.5f},
        {(nx + ux + vx) * 0.5f, (ny + uy + vy) * 0.5f, (nz + uz + vz) * 0.5f},
        {(nx - ux + vx) * 0.5f, (ny - uy + vy) * 0.5f, (nz - uz + vz) * 0.5f},
    };
    const int tri[6] = {0, 1, 2, 0, 2, 3};
    for (const int index : tri) {
      vertices.push_back(corners[index][0]);
      vertices.push_back(corners[index][1]);
      vertices.push_back(corners[index][2]);
      vertices.push_back(nx);
      vertices.push_back(ny);
      vertices.push_back(nz);
    }
  }
  return vertices;
}

// Screen-space quad draw item: position/size in NDC, drawn at near depth
// over the scene (unlit color path).
inf::render::Rhi::DrawItem hud_quad(std::uint32_t mesh, double ndc_x, double ndc_y,
                                    double width_ndc, double height_ndc, float r, float g,
                                    float b) {
  inf::render::Rhi::DrawItem item;
  item.mesh = mesh;
  Mat4 m{};
  m.m[0] = static_cast<float>(width_ndc);
  m.m[5] = static_cast<float>(height_ndc);
  m.m[10] = 0.00001f;
  m.m[12] = static_cast<float>(ndc_x);
  m.m[13] = static_cast<float>(ndc_y);
  m.m[14] = 0.0001f;  // near depth: passes the Less test over everything
  m.m[15] = 1.0f;
  std::memcpy(item.mvp, m.m, sizeof(m.m));
  item.color[0] = r;
  item.color[1] = g;
  item.color[2] = b;
  item.color[3] = 1.0f;
  return item;
}

// Analytic orbit-line ribbon (map mode, design/map-mode.md section 2):
// a flat strip in the orbital plane straight from the OrbitalElements,
// parameterized by eccentric anomaly from phase_start over arc radians.
// Vertices are SYSTEM-frame meters relative to the parent (f32 rounding
// at outer-system magnitudes is sub-pixel at map framing distance).
std::vector<float> orbit_ribbon_vertices(const inf::core::OrbitalElements& elements,
                                         double phase_start, double arc, int segments,
                                         double width_m) {
  const double a = elements.a_m.to_double();
  const double e = elements.e.to_double();
  const double b = a * std::sqrt(std::max(0.0, 1.0 - e * e));
  const double ci = std::cos(elements.i_rad.to_double());
  const double si = std::sin(elements.i_rad.to_double());
  const double co = std::cos(elements.raan_rad.to_double());
  const double so = std::sin(elements.raan_rad.to_double());
  const double cw = std::cos(elements.argp_rad.to_double());
  const double sw = std::sin(elements.argp_rad.to_double());
  // r_parent = Rz(raan) * Rx(i) * Rz(argp) * r_perifocal (matches the
  // ephemeris evaluator).
  const auto to_parent = [&](double px, double py) {
    const double x1 = cw * px - sw * py;
    const double y1 = sw * px + cw * py;
    const double y2 = ci * y1;
    const double z2 = si * y1;
    return RVec3{co * x1 - so * y2, so * x1 + co * y2, z2};
  };
  const RVec3 plane_normal = inf::render::normalize(
      RVec3{so * si, -co * si, ci});  // Rz(raan)*Rx(i) applied to +z
  std::vector<float> vertices;
  vertices.reserve(static_cast<std::size_t>(segments) * 6 * 6);
  auto point = [&](int idx) {
    const double E = phase_start + arc * idx / segments;
    return to_parent(a * (std::cos(E) - e), b * std::sin(E));
  };
  for (int s = 0; s < segments; ++s) {
    const RVec3 p0 = point(s);
    const RVec3 p1 = point(s + 1);
    const RVec3 tangent = inf::render::normalize(p1 - p0);
    const RVec3 side = inf::render::normalize(inf::render::cross(plane_normal, tangent)) *
                       (width_m * 0.5);
    const RVec3 quad[4] = {p0 - side, p0 + side, p1 + side, p1 - side};
    const int tri[6] = {0, 1, 2, 0, 2, 3};
    for (const int v : tri) {
      vertices.insert(vertices.end(),
                      {static_cast<float>(quad[v].x), static_cast<float>(quad[v].y),
                       static_cast<float>(quad[v].z), static_cast<float>(plane_normal.x),
                       static_cast<float>(plane_normal.y), static_cast<float>(plane_normal.z)});
    }
  }
  return vertices;
}

// The anchor body: the planet whose planet-local frame the world lives in
// — terrain field, diff overlay, chunk streaming, player physics. Flying
// between planets re-anchors to the nearest landable body (T0014); each
// body keeps its own diff file (persistence stays a per-body diff).
struct Anchor {
  int slot = 0;
  inf::gen::BodyHandle keys;
  inf::gen::PlanetParams planet;
  double radius = 0.0;
  std::string diff_path;
  std::unique_ptr<inf::world::CsgEditStore> edits;
  std::unique_ptr<inf::gen::TerrainField> field;
  std::unique_ptr<inf::gen::TerrainSampler> sampler;
  std::unique_ptr<inf::world::ChunkManager> manager;
  std::unique_ptr<inf::gen::EffectiveField> effective;
};

std::unique_ptr<Anchor> make_anchor(const inf::core::Seed128& seed, const char* seed_text,
                                    const inf::gen::StarSystemParams& system, int slot,
                                    std::optional<inf::gen::PlanetType> forced,
                                    const char* diff_override) {
  auto anchor = std::make_unique<Anchor>();
  anchor->slot = slot;
  anchor->keys = inf::gen::body_for_slot(seed, slot);
  anchor->planet =
      forced.has_value()
          ? inf::gen::derive_planet_params(anchor->keys.params, forced)
          : inf::gen::planet_params_for_slot(system, slot, anchor->keys.params);
  anchor->radius = anchor->planet.radius_m.to_double();

  anchor->diff_path = diff_override != nullptr
                          ? std::string(diff_override)
                          : std::string("infinity-") + seed_text + "-s" +
                                std::to_string(slot) + ".edits";
  anchor->edits = std::make_unique<inf::world::CsgEditStore>();
  if (anchor->edits->load(anchor->diff_path)) {
    std::printf("diff: loaded %zu edits from %s\n", anchor->edits->size(),
                anchor->diff_path.c_str());
  }

  anchor->field = std::make_unique<inf::gen::TerrainField>(anchor->keys.entity, anchor->planet);
  anchor->sampler =
      std::make_unique<inf::gen::TerrainSampler>(*anchor->field, anchor->edits.get());

  inf::world::ChunkManagerConfig config;
  const unsigned hardware = std::thread::hardware_concurrency();
  config.worker_count = hardware > 4 ? (hardware - 2 > 8 ? 8 : hardware - 2) : 2;
  config.split_factor = 1.5;
  std::uint8_t max_lod = 8;
  while ((2.0 * anchor->radius) / static_cast<double>(std::uint64_t{1} << max_lod) > 32.0 &&
         max_lod < 16) {
    ++max_lod;
  }
  config.max_lod = max_lod;
  anchor->manager = std::make_unique<inf::world::ChunkManager>(*anchor->sampler, config);
  anchor->effective =
      std::make_unique<inf::gen::EffectiveField>(*anchor->field, anchor->edits.get());
  return anchor;
}

void save_anchor_edits(const Anchor& anchor) {
  if (anchor.edits->size() == 0) {
    return;
  }
  if (anchor.edits->save(anchor.diff_path)) {
    std::printf("diff: saved %zu edits to %s\n", anchor.edits->size(),
                anchor.diff_path.c_str());
  } else {
    std::fprintf(stderr, "diff: FAILED to save %s\n", anchor.diff_path.c_str());
  }
}

// Column-major Mat4 * (x, y, z, 1) -> clip space (picking projections).
std::array<double, 4> project_point(const Mat4& m, const RVec3& v) {
  std::array<double, 4> clip{};
  for (int row = 0; row < 4; ++row) {
    clip[row] = m.m[row] * v.x + m.m[4 + row] * v.y + m.m[8 + row] * v.z + m.m[12 + row];
  }
  return clip;
}

std::vector<float> unit_sphere_vertices(int slices, int stacks) {
  std::vector<float> vertices;
  const double pi = 3.14159265358979323846;
  auto point = [&](int slice, int stack) {
    const double phi = pi * stack / stacks - pi * 0.5;
    const double theta = 2.0 * pi * slice / slices;
    return std::array<float, 3>{static_cast<float>(std::cos(phi) * std::cos(theta)),
                                static_cast<float>(std::cos(phi) * std::sin(theta)),
                                static_cast<float>(std::sin(phi))};
  };
  for (int stack = 0; stack < stacks; ++stack) {
    for (int slice = 0; slice < slices; ++slice) {
      const auto p00 = point(slice, stack);
      const auto p10 = point(slice + 1, stack);
      const auto p01 = point(slice, stack + 1);
      const auto p11 = point(slice + 1, stack + 1);
      for (const auto& p : {p00, p10, p11, p00, p11, p01}) {
        vertices.insert(vertices.end(), {p[0], p[1], p[2], p[0], p[1], p[2]});
      }
    }
  }
  return vertices;
}

}  // namespace

int main(int argc, char** argv) {
  long max_frames = 0;
  double spawn_altitude = -1.0;  // <0: default orbit spawn
  const char* seed_text = "7";
  const char* type_text = nullptr;
  const char* diff_text = nullptr;
  bool map_demo = false;  // scripted M/Esc for headless smoke + captures
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
      max_frames = std::strtol(argv[++i], nullptr, 10);
    } else if (std::strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
      seed_text = argv[++i];
    } else if (std::strcmp(argv[i], "--type") == 0 && i + 1 < argc) {
      type_text = argv[++i];
    } else if (std::strcmp(argv[i], "--spawn-alt") == 0 && i + 1 < argc) {
      spawn_altitude = std::strtod(argv[++i], nullptr);
    } else if (std::strcmp(argv[i], "--diff-file") == 0 && i + 1 < argc) {
      diff_text = argv[++i];
    } else if (std::strcmp(argv[i], "--map-demo") == 0) {
      map_demo = true;
    }
  }

  const auto seed = inf::core::parse_seed(seed_text);
  if (!seed.has_value()) {
    std::fprintf(stderr, "invalid seed: %s\n", seed_text);
    return EXIT_FAILURE;
  }

  // The home world comes from the generated system (T0012/T0013): the
  // first landable slot. --type still forces a standalone planet draw for
  // debugging; the system layout stays authoritative for the map.
  const inf::gen::StarSystemParams system =
      inf::gen::generate_system(inf::gen::default_system_key(*seed));
  const int home_slot = inf::gen::default_landable_slot(system);

  std::optional<inf::gen::PlanetType> forced;
  if (type_text != nullptr) {
    for (std::uint32_t t = 0; t < 4; ++t) {
      if (std::strcmp(type_text, inf::gen::to_string(static_cast<inf::gen::PlanetType>(t))) ==
          0) {
        forced = static_cast<inf::gen::PlanetType>(t);
      }
    }
  }
  // Player-diff overlay (M7): the world files are ONLY per-body diffs —
  // the procedural planets are never stored.
  std::unique_ptr<Anchor> anchor =
      make_anchor(*seed, seed_text, system, home_slot, forced, diff_text);
  const double spawn_r =
      spawn_altitude >= 0.0 ? anchor->radius + spawn_altitude : anchor->radius * 2.2;
  inf::sim::Player player(*anchor->effective,
                          inf::sim::normalize(SVec3{1.0, 0.15, 0.3}) * spawn_r);

  std::printf("infinity %s (%s) — %s planet (slot %d), radius %.0f m\n",
              inf::gen::kVersion, inf::gen::kGitHash,
              inf::gen::to_string(anchor->planet.type), home_slot, anchor->radius);

  if (glfwInit() != GLFW_TRUE) {
    std::fprintf(stderr, "glfwInit failed\n");
    return EXIT_FAILURE;
  }
  glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
  GLFWwindow* window = glfwCreateWindow(1280, 720, "infinity", nullptr, nullptr);
  if (window == nullptr) {
    std::fprintf(stderr, "glfwCreateWindow failed\n");
    glfwTerminate();
    return EXIT_FAILURE;
  }
  std::string error;
  std::unique_ptr<inf::render::Rhi> rhi = inf::render::Rhi::create(window, &error);
  if (rhi == nullptr) {
    std::fprintf(stderr, "RHI init failed: %s\n", error.c_str());
    glfwDestroyWindow(window);
    glfwTerminate();
    return EXIT_FAILURE;
  }
  std::printf("adapter: %s\n", rhi->adapter_info().c_str());

  const std::vector<float> cube = unit_cube_vertices();
  const std::uint32_t cube_mesh = rhi->create_mesh(cube.data(), cube.size());
  const std::vector<float> ball = unit_sphere_vertices(32, 16);
  const std::uint32_t body_mesh = rhi->create_mesh(ball.data(), ball.size());
  // Sea shell (spec section 5): one translucent sphere at sea level,
  // EarthLike only. Zero shading effort by design. Rebuilt per anchor.
  std::uint32_t sea_mesh = 0;
  double sea_radius = 0.0;
  const auto rebuild_sea = [&] {
    if (sea_mesh != 0) {
      rhi->destroy_mesh(sea_mesh);
      sea_mesh = 0;
    }
    sea_radius = 0.0;
    if (anchor->planet.type == inf::gen::PlanetType::EarthLike) {
      const std::vector<float> sphere = unit_sphere_vertices(64, 32);
      sea_mesh = rhi->create_mesh(sphere.data(), sphere.size());
      sea_radius = anchor->radius + anchor->planet.sea_level_m.to_double();
    }
  };
  rebuild_sea();
  {  // HUD scope: must destruct before the RHI is torn down.
  auto hud = std::make_unique<inf::app::Hud>(rhi.get(), anchor->field.get(), anchor->planet);
  SVec3 last_player_pos = player.position();
  double measured_speed = 0.0;

  AppState state{rhi.get(), 1280, 720};
  glfwSetWindowUserPointer(window, &state);
  glfwSetFramebufferSizeCallback(window, framebuffer_size_callback);
  glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
  if (glfwRawMouseMotionSupported() == GLFW_TRUE) {
    glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
  }
  double last_mx = 0.0;
  double last_my = 0.0;
  glfwGetCursorPos(window, &last_mx, &last_my);
  const inf::core::LocalClock world_clock;
  inf::core::WorldTime last_time = world_clock.now();
  double fps_accum = 0.0;
  int fps_frames = 0;
  bool e_was_down = false;
  bool m_was_down = false;
  bool esc_was_down = false;
  double edit_cooldown = 0.0;

  // --- map mode state (T0013, design/map-mode.md) -----------------------
  enum class MapPhase { Off, Entering, On, Exiting };
  MapPhase map_phase = MapPhase::Off;
  double map_timer = 0.0;
  inf::sim::MapCameraParams map_params;  // exposed tuning knobs
  inf::sim::Pose map_saved_local;        // camera pose at entry, planet-local
  inf::sim::Pose map_target_sys;         // stationary map pose, system frame
  inf::sim::Pose map_exit_start_sys;     // pose when the exit was triggered
  inf::sim::Pose map_current_sys;        // this frame's map camera, system frame
  int hovered_slot = -1;

  const SVec3 plane_normal{0.0, 0.0, 1.0};  // system invariant plane
  double outer_orbit_m = 0.0;
  for (int slot = 0; slot < inf::gen::kMaxPlanetSlots; ++slot) {
    const auto& entry = system.planets[static_cast<std::size_t>(slot)];
    if (entry.occupied) {
      outer_orbit_m = std::max(outer_orbit_m, entry.orbit.a_m.to_double() *
                                                  (1.0 + entry.orbit.e.to_double()));
    }
  }

  // Display names + orbit ribbon meshes per occupied slot (full ellipse,
  // dim; a brighter leading arc is rebuilt on a timer while the map is
  // up). Ribbon width ~1.6 px at framing distance.
  const double map_distance =
      outer_orbit_m * map_params.frame_margin / std::tan(kFovY * 0.5);
  const double map_px_m = 2.0 * map_distance * std::tan(kFovY * 0.5) / 720.0;
  std::array<std::string, inf::gen::kMaxPlanetSlots> slot_names;
  std::array<std::uint32_t, inf::gen::kMaxPlanetSlots> orbit_meshes{};
  std::array<std::uint32_t, inf::gen::kMaxPlanetSlots> arc_meshes{};
  for (int slot = 0; slot < inf::gen::kMaxPlanetSlots; ++slot) {
    const auto& entry = system.planets[static_cast<std::size_t>(slot)];
    if (!entry.occupied) {
      continue;
    }
    slot_names[static_cast<std::size_t>(slot)] =
        inf::gen::body_display_name(inf::gen::body_for_slot(*seed, slot).entity);
    const auto ribbon =
        orbit_ribbon_vertices(entry.orbit, 0.0, 2.0 * 3.14159265358979323846, 256,
                              map_px_m * 1.6);
    orbit_meshes[static_cast<std::size_t>(slot)] =
        rhi->create_mesh(ribbon.data(), ribbon.size());
  }
  double arc_rebuild_timer = 0.0;

  const auto slot_color = [&](int slot, float out[3]) {
    const auto& entry = system.planets[static_cast<std::size_t>(slot)];
    float c[3] = {0.55f, 0.55f, 0.58f};  // rocky default
    if (entry.landable) {
      switch (entry.surface_type) {
        case inf::gen::PlanetType::EarthLike: c[0] = 0.30f; c[1] = 0.62f; c[2] = 0.90f; break;
        case inf::gen::PlanetType::Desert:    c[0] = 0.82f; c[1] = 0.62f; c[2] = 0.38f; break;
        case inf::gen::PlanetType::Ice:       c[0] = 0.78f; c[1] = 0.88f; c[2] = 0.98f; break;
        case inf::gen::PlanetType::Barren:    c[0] = 0.55f; c[1] = 0.53f; c[2] = 0.50f; break;
      }
    } else {
      switch (entry.phys.cls) {
        case inf::core::PlanetClass::SubNeptune: c[0] = 0.45f; c[1] = 0.75f; c[2] = 0.72f; break;
        case inf::core::PlanetClass::IceGiant:   c[0] = 0.42f; c[1] = 0.58f; c[2] = 0.92f; break;
        case inf::core::PlanetClass::GasGiant:   c[0] = 0.85f; c[1] = 0.68f; c[2] = 0.45f; break;
        default: break;
      }
    }
    out[0] = c[0];
    out[1] = c[1];
    out[2] = c[2];
  };

  // Digging (M7): raycast the crosshair against the EFFECTIVE surface and
  // stamp a CSG sphere; the touched chunks re-mesh through the manager.
  const auto apply_edit = [&](bool subtract) {
    const SVec3 origin = player.position();
    const SVec3 dir = inf::sim::normalize(player.forward());
    constexpr double kStep = 0.5;
    constexpr double kMaxReach = 120.0;
    const auto density_at = [&](const SVec3& p) {
      return anchor->effective
          ->density(inf::gen::Dir3{inf::det::Real(p.x), inf::det::Real(p.y),
                                   inf::det::Real(p.z)})
          .to_double();
    };
    double t_air = 0.0;
    double t_hit = -1.0;
    for (double t = kStep; t <= kMaxReach; t += kStep) {
      if (density_at(origin + dir * t) > 0.0) {
        t_hit = t;
        break;
      }
      t_air = t;
    }
    if (t_hit < 0.0) {
      return;  // nothing but air in reach
    }
    for (int i = 0; i < 16; ++i) {
      const double mid = 0.5 * (t_air + t_hit);
      if (density_at(origin + dir * mid) > 0.0) {
        t_hit = mid;
      } else {
        t_air = mid;
      }
    }
    const double kRadius = subtract ? 2.5 : 2.2;
    // Add material just shy of the surface so it bulges toward the player.
    const SVec3 center = origin + dir * (subtract ? t_hit : t_hit - 1.0);
    // Core rejection (spec section 9): the planet core is not editable.
    if (inf::sim::length(center) - kRadius <= anchor->planet.core_radius_m.to_double()) {
      return;
    }
    inf::world::SphereEdit edit;
    edit.center_raw[0] = inf::det::Fixed64::from_double(center.x).raw();
    edit.center_raw[1] = inf::det::Fixed64::from_double(center.y).raw();
    edit.center_raw[2] = inf::det::Fixed64::from_double(center.z).raw();
    edit.radius_raw = inf::det::Fixed64::from_double(kRadius).raw();
    edit.subtract = subtract;
    anchor->edits->append(edit);
    anchor->manager->invalidate_sphere(center.x, center.y, center.z, kRadius + 6.0);
  };

  std::unordered_map<inf::core::ChunkAddr, LoadedChunk, AddrHash> loaded;
  std::vector<inf::render::Rhi::DrawItem> items;

  long frame = 0;
  while (glfwWindowShouldClose(window) == GLFW_FALSE) {
    glfwPollEvents();
    // Quit (design/map-mode.md section 5): Cmd+Q/Cmd+W on macOS, Ctrl+Q
    // elsewhere — never bare W. The diff overlay flushes on the normal
    // shutdown path below.
#ifdef __APPLE__
    const bool quit_mod = glfwGetKey(window, GLFW_KEY_LEFT_SUPER) == GLFW_PRESS ||
                          glfwGetKey(window, GLFW_KEY_RIGHT_SUPER) == GLFW_PRESS;
    const bool quit_key = glfwGetKey(window, GLFW_KEY_Q) == GLFW_PRESS ||
                          glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS;
#else
    const bool quit_mod = glfwGetKey(window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS ||
                          glfwGetKey(window, GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS;
    const bool quit_key = glfwGetKey(window, GLFW_KEY_Q) == GLFW_PRESS;
#endif
    if (quit_mod && quit_key) {
      glfwSetWindowShouldClose(window, GLFW_TRUE);
    }
    const bool esc_down = glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS;
    const bool esc_pressed = esc_down && !esc_was_down;
    esc_was_down = esc_down;
    if (esc_pressed && map_phase == MapPhase::Off) {
      glfwSetWindowShouldClose(window, GLFW_TRUE);  // prototype convenience
    }
    const inf::core::WorldTime now = world_clock.now();
    const double raw_dt = static_cast<double>(now - last_time) * 1e-9;
    const double dt = raw_dt > 0.0 ? std::min(raw_dt, 0.1) : 0.0;
    last_time = now;

    // --- input ----------------------------------------------------------
    double mx = 0.0;
    double my = 0.0;
    glfwGetCursorPos(window, &mx, &my);
    const bool e_down = glfwGetKey(window, GLFW_KEY_E) == GLFW_PRESS;

    inf::sim::InputFrame input;
    input.dt = dt;
    input.mouse_dx = mx - last_mx;
    input.mouse_dy = my - last_my;
    input.forward = glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS;
    input.back = glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS;
    input.left = glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS;
    input.right = glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS;
    input.run = glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS;
    input.fire = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
    input.interact_pressed = e_down && !e_was_down;
    input.aspect = static_cast<double>(state.width) / state.height;
    input.fov_y = kFovY;
    last_mx = mx;
    last_my = my;
    e_was_down = e_down;

    player.update(input);

    // --- live system state (ephemerides; the universe never pauses) -----
    const auto eval_pos = [&](const inf::core::OrbitalElements& orbit) {
      const auto pv = inf::core::Ephemeris::evaluate(orbit, now);
      return SVec3{pv.x.to_double(), pv.y.to_double(), pv.z.to_double()};
    };
    SVec3 planet_sys;                                       // anchor body, system frame
    std::array<SVec3, inf::gen::kMaxPlanetSlots> planet_local{};  // anchor-local centers
    const auto recompute_bodies = [&] {
      planet_sys =
          eval_pos(system.planets[static_cast<std::size_t>(anchor->slot)].orbit);
      for (int slot = 0; slot < inf::gen::kMaxPlanetSlots; ++slot) {
        const auto& entry = system.planets[static_cast<std::size_t>(slot)];
        if (entry.occupied) {
          planet_local[static_cast<std::size_t>(slot)] = eval_pos(entry.orbit) - planet_sys;
        }
      }
    };
    recompute_bodies();

    // --- anchor switching (T0014): re-anchor to the nearest landable ----
    // body once it is decisively closer than the current one. The altitude
    // governor then handles approach braking on its own.
    if (map_phase == MapPhase::Off && player.mode() == inf::sim::PlayerMode::Flight) {
      const SVec3 at = player.position();
      const double anchor_gap = inf::sim::length(at) - anchor->radius;
      int candidate = -1;
      double candidate_gap = 1e300;
      for (int slot = 0; slot < inf::gen::kMaxPlanetSlots; ++slot) {
        const auto& entry = system.planets[static_cast<std::size_t>(slot)];
        if (!entry.occupied || !entry.landable || slot == anchor->slot) {
          continue;
        }
        const double gap =
            inf::sim::length(planet_local[static_cast<std::size_t>(slot)] - at) -
            entry.phys.radius_m.to_double();
        if (gap < candidate_gap) {
          candidate = slot;
          candidate_gap = gap;
        }
      }
      if (candidate >= 0 && candidate_gap < anchor_gap * 0.5) {
        save_anchor_edits(*anchor);
        for (auto& [addr, chunk] : loaded) {
          rhi->destroy_mesh(chunk.mesh_id);
        }
        loaded.clear();
        const SVec3 new_pos = at - planet_local[static_cast<std::size_t>(candidate)];
        anchor = make_anchor(*seed, seed_text, system, candidate, std::nullopt, nullptr);
        player.rebase(*anchor->effective, new_pos);
        rebuild_sea();
        hud = std::make_unique<inf::app::Hud>(rhi.get(), anchor->field.get(), anchor->planet);
        recompute_bodies();
        std::printf("anchor: %s (slot %d, %s planet, radius %.0f km)\n",
                    slot_names[static_cast<std::size_t>(candidate)].c_str(), candidate,
                    inf::gen::to_string(anchor->planet.type), anchor->radius / 1000.0);
      }
    }

    // Star + moons in the anchor frame (for rendering, radar, keep-out).
    const SVec3 star_local = SVec3{0.0, 0.0, 0.0} - planet_sys;
    const double star_radius = system.star.radius_solar.to_double() * 6.957e7;
    struct MoonInstance {
      SVec3 pos;
      double radius;
    };
    std::vector<MoonInstance> moons_local;
    for (int slot = 0; slot < inf::gen::kMaxPlanetSlots; ++slot) {
      const auto& entry = system.planets[static_cast<std::size_t>(slot)];
      if (!entry.occupied) {
        continue;
      }
      for (const auto& moon : entry.moons) {
        moons_local.push_back(
            MoonInstance{planet_local[static_cast<std::size_t>(slot)] + eval_pos(moon.orbit),
                         moon.phys.radius_m.to_double()});
      }
    }

    // Keep-out spheres for everything that has no terrain field: the
    // star, non-anchor planets, and all moons (fly-through is not a
    // thing; the anchor's real ground is handled by the flight clamp).
    if (map_phase == MapPhase::Off) {
      player.push_out(star_local, star_radius * 1.6);
      for (int slot = 0; slot < inf::gen::kMaxPlanetSlots; ++slot) {
        const auto& entry = system.planets[static_cast<std::size_t>(slot)];
        if (entry.occupied && slot != anchor->slot) {
          player.push_out(planet_local[static_cast<std::size_t>(slot)],
                          entry.phys.radius_m.to_double() * 1.02 + 5.0);
        }
      }
      for (const MoonInstance& moon : moons_local) {
        player.push_out(moon.pos, moon.radius * 1.02 + 5.0);
      }
    }

    // --- map mode: enter / exit triggers ---------------------------------
    const bool m_down = glfwGetKey(window, GLFW_KEY_M) == GLFW_PRESS;
    const bool m_pressed = (m_down && !m_was_down) || (map_demo && frame == 100);
    const bool map_exit_scripted = map_demo && frame == 550;
    m_was_down = m_down;
    if (m_pressed && map_phase == MapPhase::Off) {
      player.enter_map();
      map_saved_local =
          inf::sim::Pose{player.position(), player.forward(), player.up()};
      map_target_sys = inf::sim::map_pose(
          plane_normal, planet_sys + map_saved_local.position, outer_orbit_m, kFovY,
          map_params);
      map_phase = MapPhase::Entering;
      map_timer = 0.0;
      hovered_slot = -1;
      glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
    } else if ((m_pressed || esc_pressed || map_exit_scripted) &&
               (map_phase == MapPhase::Entering || map_phase == MapPhase::On)) {
      map_exit_start_sys = map_phase == MapPhase::On ? map_target_sys : map_current_sys;
      map_phase = MapPhase::Exiting;
      map_timer = 0.0;
      hovered_slot = -1;
    }

    // Terrain editing: right button digs, middle button adds material
    // (suspended in map mode).
    const bool dig_down = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;
    const bool add_down = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_MIDDLE) == GLFW_PRESS;
    edit_cooldown -= dt;
    if (map_phase == MapPhase::Off && (dig_down || add_down) && edit_cooldown <= 0.0) {
      apply_edit(dig_down);
      edit_cooldown = 0.18;  // hold to keep carving
    } else if (!dig_down && !add_down && edit_cooldown < 0.0) {
      edit_cooldown = 0.0;
    }

    // Measured velocity (covers ship, walking, and later the rocket
    // backpack alike), lightly smoothed.
    if (dt > 0.0) {
      const double instantaneous =
          inf::sim::length(player.position() - last_player_pos) / dt;
      measured_speed += (instantaneous - measured_speed) * std::min(1.0, dt * 8.0);
    }
    last_player_pos = player.position();

    // --- streaming ------------------------------------------------------
    const SVec3 player_pos = player.position();
    const auto events =
        anchor->manager->update(player_pos.x, player_pos.y, player_pos.z);
    for (const auto& event : events) {
      if (event.kind == inf::world::ChunkEvent::Kind::Ready) {
        // Replace any previous mesh for this address (re-mesh on neighbor
        // lod change delivers updated geometry under the same address).
        const auto old = loaded.find(event.addr);
        if (old != loaded.end()) {
          rhi->destroy_mesh(old->second.mesh_id);
          loaded.erase(old);
        }
        if (event.data->mesh.vertices.empty()) {
          continue;
        }
        LoadedChunk chunk;
        chunk.mesh_id = rhi->create_mesh(event.data->mesh.vertices.data(),
                                         event.data->mesh.vertices.size());
        chunk.origin = RVec3{event.data->mesh.origin[0], event.data->mesh.origin[1],
                             event.data->mesh.origin[2]};
        loaded[event.addr] = chunk;
      } else {
        const auto it = loaded.find(event.addr);
        if (it != loaded.end()) {
          rhi->destroy_mesh(it->second.mesh_id);
          loaded.erase(it);
        }
      }
    }

    // --- camera (map-aware) ----------------------------------------------
    SVec3 cam_pos_local = player_pos;
    SVec3 cam_fwd_v = player.forward();
    SVec3 cam_up_sv = player.up();
    if (map_phase != MapPhase::Off) {
      map_timer += dt;
      const inf::sim::Pose from_sys{planet_sys + map_saved_local.position,
                                    map_saved_local.forward, map_saved_local.up};
      const SVec3 local_up = inf::sim::normalize(map_saved_local.position);
      if (map_phase == MapPhase::Entering) {
        const double u = map_timer / map_params.enter_duration_s;
        map_current_sys =
            inf::sim::transition_pose(from_sys, map_target_sys, local_up, u, map_params);
        if (u >= 1.0) {
          map_phase = MapPhase::On;
        }
      } else if (map_phase == MapPhase::On) {
        map_current_sys = map_target_sys;
      } else {
        const double u = map_timer / map_params.exit_duration_s;
        map_current_sys = inf::sim::transition_pose(map_exit_start_sys, from_sys, local_up,
                                                    u, map_params);
        if (u >= 1.0) {
          player.exit_map();
          map_phase = MapPhase::Off;
          glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
          glfwGetCursorPos(window, &last_mx, &last_my);
          map_current_sys = from_sys;
        }
      }
      if (map_phase != MapPhase::Off) {
        cam_pos_local = map_current_sys.position - planet_sys;
        cam_fwd_v = map_current_sys.forward;
        cam_up_sv = map_current_sys.up;
      }
    }

    // --- draw -----------------------------------------------------------
    // Simple sky (M5): blend the type's sky palette toward space black by
    // CAMERA altitude within the atmosphere band (fades out on the map
    // pull-up).
    float sky[3] = {0.05f, 0.06f, 0.12f};
    {
      double atmosphere = anchor->planet.atmosphere_height_m.to_double();
      float palette[3] = {0.05f, 0.06f, 0.12f};
      switch (anchor->planet.type) {
        case inf::gen::PlanetType::EarthLike: palette[0] = 0.45f; palette[1] = 0.65f; palette[2] = 0.95f; break;
        case inf::gen::PlanetType::Desert: palette[0] = 0.78f; palette[1] = 0.58f; palette[2] = 0.42f; break;
        case inf::gen::PlanetType::Ice: palette[0] = 0.62f; palette[1] = 0.74f; palette[2] = 0.92f; break;
        case inf::gen::PlanetType::Barren: atmosphere = 0.0; break;
      }
      if (atmosphere > 0.0) {
        const double raw_alt = inf::sim::length(cam_pos_local) - anchor->radius;
        double t = 1.0 - raw_alt / atmosphere;
        t = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
        t = std::pow(t, 0.7);
        for (int c = 0; c < 3; ++c) {
          sky[c] = sky[c] + static_cast<float>(t) * (palette[c] - sky[c]);
        }
      }
    }
    const RVec3 camera_pos = to_render(cam_pos_local);
    const RVec3 cam_forward = to_render(cam_fwd_v);
    const RVec3 cam_up = to_render(cam_up_sv);
    const double altitude = inf::render::length(camera_pos) - anchor->radius;
    const double far_z =
        std::max({10'000.0, std::abs(altitude) * 4.0 + 2.5 * anchor->radius,
                  inf::sim::length(star_local - cam_pos_local) * 2.5});
    // At map framing distance the near plane scales up with altitude so
    // the sparse far-field scene keeps usable depth precision.
    const double near_z = std::clamp(std::abs(altitude) * 1e-4, 0.3, 1e8);
    const Mat4 projection = inf::render::perspective(kFovY, input.aspect, near_z, far_z);
    const Mat4 view = inf::render::look_dir(cam_forward, cam_up);
    const Mat4 view_projection = inf::render::mul(projection, view);

    items.clear();
    items.reserve(loaded.size() + player.beams().size() + 8);
    for (const auto& [addr, chunk] : loaded) {
      inf::render::Rhi::DrawItem item;
      item.mesh = chunk.mesh_id;
      const Mat4 model = inf::render::translate(chunk.origin - camera_pos);
      const Mat4 mvp = inf::render::mul(view_projection, model);
      std::memcpy(item.mvp, mvp.m, sizeof(mvp.m));
      items.push_back(item);
    }

    // --- system bodies in normal flight (T0014) --------------------------
    // The sun, sibling planets and moons are always in the sky — real
    // scale, with a small minimum apparent size so distant planets stay
    // visible as specks. Map mode draws its own (larger) versions.
    if (map_phase == MapPhase::Off) {
      const double px_world = 2.0 * std::tan(kFovY * 0.5) / state.height;
      const auto draw_ball = [&](const SVec3& pos, double true_radius, double min_px,
                                 float r, float g, float b) {
        const double dist = inf::sim::length(pos - cam_pos_local);
        if (dist < true_radius * 1.05) {
          return;  // camera inside/at the body (the anchor renders as terrain)
        }
        const double size = std::max(true_radius, dist * px_world * min_px * 0.5);
        const Mat4 model = inf::render::from_basis(
            RVec3{size, 0.0, 0.0}, RVec3{0.0, size, 0.0}, RVec3{0.0, 0.0, size},
            to_render(pos) - camera_pos);
        const Mat4 mvp = inf::render::mul(view_projection, model);
        inf::render::Rhi::DrawItem item;
        item.mesh = body_mesh;
        std::memcpy(item.mvp, mvp.m, sizeof(mvp.m));
        item.color[0] = r;
        item.color[1] = g;
        item.color[2] = b;
        item.color[3] = 1.0f;
        items.push_back(item);
      };
      draw_ball(star_local, star_radius, 5.0, 1.0f, 0.92f, 0.72f);
      for (int slot = 0; slot < inf::gen::kMaxPlanetSlots; ++slot) {
        const auto& entry = system.planets[static_cast<std::size_t>(slot)];
        if (!entry.occupied || slot == anchor->slot) {
          continue;
        }
        float color[3];
        slot_color(slot, color);
        draw_ball(planet_local[static_cast<std::size_t>(slot)],
                  entry.phys.radius_m.to_double(), 3.0, color[0], color[1], color[2]);
      }
      // The anchor itself gets an under-the-terrain impostor while the
      // camera is far out: freshly-anchored planets are visible before
      // their coarse chunks finish streaming.
      if (inf::sim::length(player_pos) > 5.0 * anchor->radius) {
        float color[3];
        slot_color(anchor->slot, color);
        draw_ball(SVec3{0.0, 0.0, 0.0}, anchor->radius * 0.995, 3.0, color[0], color[1],
                  color[2]);
      }
      for (const MoonInstance& moon : moons_local) {
        draw_ball(moon.pos, moon.radius, 2.0, 0.62f, 0.62f, 0.66f);
      }
    }

    // Radar feed: every body in the system, relative to the player
    // (constant-size icons + elevation bars; drawn by the HUD when the
    // space radar is showing).
    std::vector<inf::app::RadarBody> radar_bodies;
    radar_bodies.reserve(2 + moons_local.size() + inf::gen::kMaxPlanetSlots);
    {
      inf::app::RadarBody star_icon;
      star_icon.rel = star_local - player_pos;
      star_icon.color[0] = 1.0f;
      star_icon.color[1] = 0.88f;
      star_icon.color[2] = 0.55f;
      star_icon.scale = 1.6f;
      radar_bodies.push_back(star_icon);
      for (int slot = 0; slot < inf::gen::kMaxPlanetSlots; ++slot) {
        const auto& entry = system.planets[static_cast<std::size_t>(slot)];
        if (!entry.occupied) {
          continue;
        }
        inf::app::RadarBody icon;
        icon.rel = planet_local[static_cast<std::size_t>(slot)] - player_pos;
        slot_color(slot, icon.color);
        icon.scale = 1.0f;
        icon.anchor = slot == anchor->slot;
        radar_bodies.push_back(icon);
      }
      for (const MoonInstance& moon : moons_local) {
        inf::app::RadarBody icon;
        icon.rel = moon.pos - player_pos;
        icon.color[0] = 0.62f;
        icon.color[1] = 0.62f;
        icon.color[2] = 0.66f;
        icon.scale = 0.55f;
        radar_bodies.push_back(icon);
      }
    }

    // Beams: thin elongated boxes along their velocity, unlit.
    for (const auto& beam : player.beams()) {
      const RVec3 dir = to_render(inf::sim::normalize(beam.velocity));
      RVec3 side = inf::render::cross(dir, cam_up);
      if (inf::render::length(side) < 1e-6) {
        side = inf::render::cross(dir, RVec3{0.0, 0.0, 1.0});
      }
      side = inf::render::normalize(side);
      const RVec3 lift = inf::render::cross(side, dir);
      const Mat4 model = inf::render::from_basis(side * 0.08, lift * 0.08, dir * 6.0,
                                                 to_render(beam.position) - camera_pos);
      inf::render::Rhi::DrawItem item;
      item.mesh = cube_mesh;
      const Mat4 mvp = inf::render::mul(view_projection, model);
      std::memcpy(item.mvp, mvp.m, sizeof(mvp.m));
      item.color[0] = 1.0f;
      item.color[1] = 0.35f;
      item.color[2] = 0.15f;
      item.color[3] = 1.0f;
      items.push_back(item);
    }

    // --- map scene: system bodies + orbit lines (T0013) ------------------
    double pointer_ndc_x = 0.0;
    double pointer_ndc_y = 0.0;
    if (map_phase != MapPhase::Off) {
      const RVec3 sys_origin_rel = to_render(SVec3{0.0, 0.0, 0.0} - planet_sys) - camera_pos;

      // Body positions + view-clamped draw radii first (picking needs
      // them before the draw items go out).
      std::array<SVec3, inf::gen::kMaxPlanetSlots> body_local{};
      std::array<double, inf::gen::kMaxPlanetSlots> body_draw_r{};
      const double min_px = 6.0;
      const double px_world = 2.0 * std::tan(kFovY * 0.5) / state.height;
      for (int slot = 0; slot < inf::gen::kMaxPlanetSlots; ++slot) {
        const auto& entry = system.planets[static_cast<std::size_t>(slot)];
        if (!entry.occupied) {
          continue;
        }
        const auto pv = inf::core::Ephemeris::evaluate(entry.orbit, now);
        body_local[static_cast<std::size_t>(slot)] =
            SVec3{pv.x.to_double(), pv.y.to_double(), pv.z.to_double()} - planet_sys;
        const double dist =
            inf::sim::length(body_local[static_cast<std::size_t>(slot)] - cam_pos_local);
        body_draw_r[static_cast<std::size_t>(slot)] =
            std::max(entry.phys.radius_m.to_double(), dist * px_world * min_px * 0.5);
      }

      // Hover picking against the enlarged screen radii (+ grace).
      hovered_slot = -1;
      if (map_phase == MapPhase::On) {
        double mx_px = 0.0;
        double my_px = 0.0;
        glfwGetCursorPos(window, &mx_px, &my_px);
        pointer_ndc_x = 2.0 * mx_px / state.width - 1.0;
        pointer_ndc_y = 1.0 - 2.0 * my_px / state.height;
        double best = 1e30;
        for (int slot = 0; slot < inf::gen::kMaxPlanetSlots; ++slot) {
          const auto& entry = system.planets[static_cast<std::size_t>(slot)];
          if (!entry.occupied) {
            continue;
          }
          const RVec3 rel = to_render(body_local[static_cast<std::size_t>(slot)]) - camera_pos;
          const auto clip = project_point(view_projection, rel);
          if (clip[3] <= 0.0) {
            continue;
          }
          const double sx = (clip[0] / clip[3] + 1.0) * 0.5 * state.width;
          const double sy = (1.0 - clip[1] / clip[3]) * 0.5 * state.height;
          const double dist = inf::sim::length(
              body_local[static_cast<std::size_t>(slot)] - cam_pos_local);
          const double r_px = body_draw_r[static_cast<std::size_t>(slot)] /
                              (dist * px_world);
          const double d_px = std::hypot(sx - mx_px, sy - my_px);
          if (d_px < r_px + 5.0 && d_px < best) {
            best = d_px;
            hovered_slot = slot;
          }
        }
      }

      // Leading arcs (brighter quarter ahead of each body), rebuilt on a
      // slow timer — bodies crawl, the arc only needs to keep up loosely.
      arc_rebuild_timer -= dt;
      if (arc_rebuild_timer <= 0.0) {
        arc_rebuild_timer = 0.5;
        for (int slot = 0; slot < inf::gen::kMaxPlanetSlots; ++slot) {
          const auto& entry = system.planets[static_cast<std::size_t>(slot)];
          if (!entry.occupied) {
            continue;
          }
          const double a = entry.orbit.a_m.to_double();
          const double period =
              2.0 * 3.14159265358979323846 *
              std::sqrt(a * a * a / entry.orbit.mu_parent.to_double());
          const double mean = entry.orbit.mean_anom_0_rad.to_double() +
                              2.0 * 3.14159265358979323846 *
                                  (static_cast<double>(now.ns_since_epoch) * 1e-9 / period);
          const double E = inf::core::Ephemeris::solve_kepler(inf::det::Real(mean),
                                                              entry.orbit.e)
                               .to_double();
          if (arc_meshes[static_cast<std::size_t>(slot)] != 0) {
            rhi->destroy_mesh(arc_meshes[static_cast<std::size_t>(slot)]);
          }
          const auto arc = orbit_ribbon_vertices(entry.orbit, E, 1.6, 64, map_px_m * 2.2);
          arc_meshes[static_cast<std::size_t>(slot)] =
              rhi->create_mesh(arc.data(), arc.size());
        }
      }

      // Orbit lines + arcs.
      for (int slot = 0; slot < inf::gen::kMaxPlanetSlots; ++slot) {
        const auto& entry = system.planets[static_cast<std::size_t>(slot)];
        if (!entry.occupied) {
          continue;
        }
        const bool hovered = slot == hovered_slot;
        const Mat4 model = inf::render::translate(sys_origin_rel);
        const Mat4 mvp = inf::render::mul(view_projection, model);
        inf::render::Rhi::DrawItem line;
        line.mesh = orbit_meshes[static_cast<std::size_t>(slot)];
        std::memcpy(line.mvp, mvp.m, sizeof(mvp.m));
        line.color[0] = hovered ? 0.75f : 0.30f;
        line.color[1] = hovered ? 0.85f : 0.38f;
        line.color[2] = hovered ? 1.00f : 0.48f;
        line.color[3] = 1.0f;
        items.push_back(line);
        if (arc_meshes[static_cast<std::size_t>(slot)] != 0) {
          inf::render::Rhi::DrawItem arc = line;
          arc.mesh = arc_meshes[static_cast<std::size_t>(slot)];
          arc.color[0] = hovered ? 0.9f : 0.55f;
          arc.color[1] = hovered ? 0.95f : 0.65f;
          arc.color[2] = 1.0f;
          items.push_back(arc);
        }
      }

      // The star (clamped like the planets, warm tint).
      {
        const double star_r_true = system.star.radius_solar.to_double() * 6.957e7;
        const double dist = inf::sim::length(SVec3{0.0, 0.0, 0.0} - planet_sys - cam_pos_local);
        const double r = std::max(star_r_true, dist * px_world * min_px * 0.75);
        const Mat4 model = inf::render::from_basis(
            RVec3{r, 0.0, 0.0}, RVec3{0.0, r, 0.0}, RVec3{0.0, 0.0, r}, sys_origin_rel);
        const Mat4 mvp = inf::render::mul(view_projection, model);
        inf::render::Rhi::DrawItem item;
        item.mesh = body_mesh;
        std::memcpy(item.mvp, mvp.m, sizeof(mvp.m));
        item.color[0] = 1.0f;
        item.color[1] = 0.92f;
        item.color[2] = 0.72f;
        item.color[3] = 1.0f;
        items.push_back(item);
      }

      // Planets + moon specks.
      for (int slot = 0; slot < inf::gen::kMaxPlanetSlots; ++slot) {
        const auto& entry = system.planets[static_cast<std::size_t>(slot)];
        if (!entry.occupied) {
          continue;
        }
        const SVec3 pos = body_local[static_cast<std::size_t>(slot)];
        double r = body_draw_r[static_cast<std::size_t>(slot)];
        if (slot == hovered_slot) {
          r *= 1.25;
        }
        const Mat4 model = inf::render::from_basis(RVec3{r, 0.0, 0.0}, RVec3{0.0, r, 0.0},
                                                   RVec3{0.0, 0.0, r},
                                                   to_render(pos) - camera_pos);
        const Mat4 mvp = inf::render::mul(view_projection, model);
        inf::render::Rhi::DrawItem item;
        item.mesh = body_mesh;
        std::memcpy(item.mvp, mvp.m, sizeof(mvp.m));
        float color[3];
        slot_color(slot, color);
        item.color[0] = color[0];
        item.color[1] = color[1];
        item.color[2] = color[2];
        item.color[3] = 1.0f;
        items.push_back(item);
        for (const auto& moon : entry.moons) {
          const auto mv = inf::core::Ephemeris::evaluate(moon.orbit, now);
          const SVec3 mpos = pos + SVec3{mv.x.to_double(), mv.y.to_double(),
                                         mv.z.to_double()};
          const double mdist = inf::sim::length(mpos - cam_pos_local);
          const double mr =
              std::max(moon.phys.radius_m.to_double(), mdist * px_world * 1.5);
          const Mat4 mmodel = inf::render::from_basis(
              RVec3{mr, 0.0, 0.0}, RVec3{0.0, mr, 0.0}, RVec3{0.0, 0.0, mr},
              to_render(mpos) - camera_pos);
          const Mat4 mmvp = inf::render::mul(view_projection, mmodel);
          inf::render::Rhi::DrawItem mitem;
          mitem.mesh = body_mesh;
          std::memcpy(mitem.mvp, mmvp.m, sizeof(mmvp.m));
          mitem.color[0] = 0.62f;
          mitem.color[1] = 0.62f;
          mitem.color[2] = 0.66f;
          mitem.color[3] = 1.0f;
          items.push_back(mitem);
        }
      }
    }

    // HUD: fixed center crosshair; in flight additionally the steering
    // reticle at its deflection (both hidden in map mode).
    const double px = 2.0 / state.height;  // one pixel in NDC-y units
    const double cross_len = 14.0 * px;
    const double cross_thick = 2.5 * px;
    const double ar = input.aspect;
    if (map_phase == MapPhase::Off) {
    items.push_back(hud_quad(cube_mesh, 0.0, 0.0, cross_len / ar, cross_thick, 0.9f, 0.95f, 1.0f));
    items.push_back(hud_quad(cube_mesh, 0.0, 0.0, cross_thick / ar, cross_len, 0.9f, 0.95f, 1.0f));
    }
    if (map_phase == MapPhase::Off &&
        (player.mode() == inf::sim::PlayerMode::Flight ||
         player.mode() == inf::sim::PlayerMode::Takeoff)) {
      const double rx = player.reticle_x();
      const double ry = player.reticle_y();
      const double box = 10.0 * px;
      const double thick = 2.5 * px;
      // Small hollow square: four bars.
      items.push_back(hud_quad(cube_mesh, rx, ry + box, box * 2.2 / ar, thick, 1.0f, 0.75f, 0.2f));
      items.push_back(hud_quad(cube_mesh, rx, ry - box, box * 2.2 / ar, thick, 1.0f, 0.75f, 0.2f));
      items.push_back(hud_quad(cube_mesh, rx + box / ar, ry, thick / ar, box * 2.2, 1.0f, 0.75f, 0.2f));
      items.push_back(hud_quad(cube_mesh, rx - box / ar, ry, thick / ar, box * 2.2, 1.0f, 0.75f, 0.2f));
    }

    if (sea_mesh != 0) {
      inf::render::Rhi::DrawItem item;
      item.mesh = sea_mesh;
      const Mat4 model = inf::render::from_basis(
          RVec3{sea_radius, 0.0, 0.0}, RVec3{0.0, sea_radius, 0.0}, RVec3{0.0, 0.0, sea_radius},
          RVec3{0.0, 0.0, 0.0} - camera_pos);
      const Mat4 mvp = inf::render::mul(view_projection, model);
      std::memcpy(item.mvp, mvp.m, sizeof(mvp.m));
      item.color[0] = 0.16f;
      item.color[1] = 0.36f;
      item.color[2] = 0.62f;
      item.color[3] = 0.42f;
      item.translucent = true;
      items.push_back(item);
    }

    if (map_phase == MapPhase::Off) {
      hud->build(&items, player, radar_bodies, measured_speed, input.aspect, state.height,
                 dt);
    } else if (map_phase == MapPhase::On && hovered_slot >= 0) {
      // Info card from the forever-state payloads (map-mode spec §3).
      const auto& entry = system.planets[static_cast<std::size_t>(hovered_slot)];
      const double a = entry.orbit.a_m.to_double();
      const double year_d =
          2.0 * 3.14159265358979323846 *
          std::sqrt(a * a * a / entry.orbit.mu_parent.to_double()) / 86400.0;
      std::vector<std::string> lines;
      char buf[96];
      lines.push_back(slot_names[static_cast<std::size_t>(hovered_slot)]);
      const char* cls = "Rocky";
      switch (entry.phys.cls) {
        case inf::core::PlanetClass::SuperEarth: cls = "Super-Earth"; break;
        case inf::core::PlanetClass::SubNeptune: cls = "Sub-Neptune"; break;
        case inf::core::PlanetClass::IceGiant: cls = "Ice giant"; break;
        case inf::core::PlanetClass::GasGiant: cls = "Gas giant"; break;
        default: break;
      }
      if (entry.landable) {
        std::snprintf(buf, sizeof(buf), "%s - %s", cls,
                      inf::gen::to_string(entry.surface_type));
      } else {
        std::snprintf(buf, sizeof(buf), "%s", cls);
      }
      lines.emplace_back(buf);
      std::snprintf(buf, sizeof(buf), "Diameter  %.0f km",
                    2.0 * entry.phys.radius_m.to_double() / 1000.0);
      lines.emplace_back(buf);
      if (entry.spin.tidally_locked) {
        lines.emplace_back("Day       tidally locked");
      } else {
        std::snprintf(buf, sizeof(buf), "Day       %.1f h",
                      2.0 * 3.14159265358979323846 /
                          std::abs(entry.spin.spin_rate_rad_s.to_double()) / 3600.0);
        lines.emplace_back(buf);
      }
      std::snprintf(buf, sizeof(buf), "Year      %.1f d", year_d);
      lines.emplace_back(buf);
      std::snprintf(buf, sizeof(buf), "Tilt      %.1f deg",
                    entry.spin.obliquity_rad.to_double() * 180.0 / 3.14159265358979323846);
      lines.emplace_back(buf);
      std::snprintf(buf, sizeof(buf), "Gravity   %.1f m/s2", entry.phys.g_surface.to_double());
      lines.emplace_back(buf);
      std::snprintf(buf, sizeof(buf), "Orbit     %.2f AU  e %.3f", a / 1.495978707e10,
                    entry.orbit.e.to_double());
      lines.emplace_back(buf);
      std::snprintf(buf, sizeof(buf), "Moons %zu   Atmosphere %s", entry.moons.size(),
                    entry.phys.atmosphere.height_m.to_double() > 0.0 ? "yes" : "no");
      lines.emplace_back(buf);
      hud->build_map_card(&items, lines, pointer_ndc_x, pointer_ndc_y, input.aspect,
                          state.height);
    }

    rhi->render_frame(sky[0], sky[1], sky[2], items.data(), items.size());

    fps_accum += dt;
    ++fps_frames;
    if (fps_accum >= 1.0) {
      const char* mode_name = player.zone() == inf::sim::FlightZone::Atmosphere
                                  ? "flight (atmo)"
                                  : "flight (space)";
      switch (player.mode()) {
        case inf::sim::PlayerMode::Landing: mode_name = "landing"; break;
        case inf::sim::PlayerMode::OnFoot: mode_name = "on foot"; break;
        case inf::sim::PlayerMode::Takeoff: mode_name = "takeoff"; break;
        case inf::sim::PlayerMode::Map: mode_name = "system map"; break;
        default: break;
      }
      char title[192];
      std::snprintf(title, sizeof(title),
                    "infinity — %s | %.0f fps | %zu chunks | alt %.0f m | %.0f m/s",
                    mode_name, fps_frames / fps_accum, loaded.size(), player.altitude(),
                    player.speed());
      glfwSetWindowTitle(window, title);
      fps_accum = 0.0;
      fps_frames = 0;
    }

    ++frame;
    if (max_frames > 0 && frame >= max_frames) {
      glfwSetWindowShouldClose(window, GLFW_TRUE);
    }
  }
  for (int slot = 0; slot < inf::gen::kMaxPlanetSlots; ++slot) {
    if (orbit_meshes[static_cast<std::size_t>(slot)] != 0) {
      rhi->destroy_mesh(orbit_meshes[static_cast<std::size_t>(slot)]);
    }
    if (arc_meshes[static_cast<std::size_t>(slot)] != 0) {
      rhi->destroy_mesh(arc_meshes[static_cast<std::size_t>(slot)]);
    }
  }
  }  // end HUD scope

  save_anchor_edits(*anchor);

  rhi.reset();
  glfwDestroyWindow(window);
  glfwTerminate();
  return EXIT_SUCCESS;
}
