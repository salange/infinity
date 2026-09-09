#include <GLFW/glfw3.h>
#include <stb_image_write.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "city/materials.hpp"
#include "city/showcase.hpp"
#include "city/towers.hpp"
#include "city_render.hpp"
#include "civ_view.hpp"
#include "core/ephem/ephemeris.hpp"
#include "core/key.hpp"
#include "core/time/world_clock.hpp"
#include "deep_sky_render.hpp"
#include "distant_galaxies.hpp"
#include "galaxy_flythrough.hpp"
#include "gen/civ_time.hpp"
#include "gen/civilization.hpp"
#include "gen/colony.hpp"
#include "gen/deep_sky.hpp"
#include "gen/effective_field.hpp"
#include "gen/galaxy.hpp"
#include "gen/galaxy_octree.hpp"
#include "gen/human.hpp"
#include "gen/planet.hpp"
#include "gen/planet_texture.hpp"
#include "gen/system.hpp"
#include "gen/terrain.hpp"
#include "gen/terrain_sampler.hpp"
#include "gen/universe.hpp"
#include "gen/version.hpp"
#include "hud.hpp"
#include "material_library.hpp"
#include "render/math.hpp"
#include "render/rhi.hpp"
#include "sim/map_camera.hpp"
#include "sim/player.hpp"
#include "stellar_stream.hpp"
#include "world/chunk_manager.hpp"
#include "world/edit_store.hpp"

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
  std::uint8_t palette[4]{0, 0, 0, 0};  // material ids for the vertex weights
  float centre[3]{0.0f, 0.0f, 0.0f};    // bounding sphere, chunk-local
  float radius{0.0f};
};

RVec3 to_render(const SVec3& v) { return RVec3{v.x, v.y, v.z}; }

// Debug script (--script <file>): one command per line, '#' comments.
//   pos <x> <y> <z>            place the player (planet-local meters)
//   aim sun | planet <slot> | dir <fx> <fy> <fz>
//   speed <m/s>                set current speed
//   thrust <0|1>               hold/release forward thrust
//   wait <seconds>             let the sim run
//   capture <path.ppm>         single-frame offscreen capture
//   record <dir> <seconds>     dump ring + record a sequence
//   quit
struct ScriptCmd {
  std::string op;
  std::vector<std::string> args;
};

std::vector<ScriptCmd> load_script(const char* path) {
  std::vector<ScriptCmd> commands;
  std::ifstream file(path);
  std::string line;
  while (std::getline(file, line)) {
    const auto hash = line.find('#');
    if (hash != std::string::npos) {
      line.resize(hash);
    }
    std::istringstream stream(line);
    ScriptCmd cmd;
    if (!(stream >> cmd.op)) {
      continue;
    }
    std::string arg;
    while (stream >> arg) {
      cmd.args.push_back(arg);
    }
    commands.push_back(std::move(cmd));
  }
  return commands;
}

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
  m.m[14] = 0.9999f;  // reversed-Z near depth: wins the Greater test
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
  int moon = -1;  // >= 0: anchored to that moon of `slot` (T0016)
  inf::gen::BodyHandle keys;
  inf::gen::PlanetParams planet;
  double radius = 0.0;
  std::string diff_path;
  std::unique_ptr<inf::world::CsgEditStore> edits;
  std::unique_ptr<inf::gen::TerrainField> field;
  std::unique_ptr<inf::gen::TerrainSampler> sampler;
  std::unique_ptr<inf::world::ChunkManager> manager;
  std::unique_ptr<inf::gen::EffectiveField> effective;
  // T0020: the civilization view of this body (nullptr = uninhabited).
  std::unique_ptr<inf::app::CivAnchor> civ;
  // Workers read the civ height modifier through the field: stop them
  // before the modifier goes away (members would otherwise be destroyed
  // in reverse order, civ first).
  ~Anchor() {
    manager.reset();
    sampler.reset();
    effective.reset();
  }
};

// What make_anchor needs to resolve the body's civilization state.
struct CivSetup {
  const inf::gen::RaceRegistry* registry{nullptr};
  const inf::gen::ColonyResolver* resolver{nullptr};
  inf::core::WorldTime now;
  inf::gen::BuildingMethod method{inf::gen::BuildingMethod::GrammarParts};
};

std::unique_ptr<Anchor> make_anchor(const inf::core::Seed128& seed, const char* seed_text,
                                    const inf::gen::StarSystemParams& system,
                                    const inf::gen::SystemCell& cell, int slot, int moon,
                                    std::optional<inf::gen::PlanetType> forced,
                                    const char* diff_override, const CivSetup* civ = nullptr) {
  auto anchor = std::make_unique<Anchor>();
  anchor->slot = slot;
  anchor->moon = moon;
  if (moon >= 0) {
    anchor->keys = inf::gen::body_for_system_moon(seed, cell, slot, moon);
    anchor->planet = inf::gen::planet_params_for_moon(system, slot, moon, anchor->keys);
  } else {
    anchor->keys = inf::gen::body_for_system_slot(seed, cell, slot);
    anchor->planet =
        forced.has_value()
            ? inf::gen::derive_planet_params(anchor->keys, forced)
            : inf::gen::planet_params_for_slot(system, slot, anchor->keys);
  }
  anchor->radius = anchor->planet.radius_m.to_double();

  // Diffs are per-BODY: foreign systems carry their octree cell in the
  // file name so no two systems ever share a diff.
  const std::string cell_tag =
      cell.is_home() ? std::string()
                     : "-g" + std::to_string(cell.level) + "_" + std::to_string(cell.x) +
                           "_" + std::to_string(cell.y) + "_" + std::to_string(cell.z);
  // Default edits location: a per-user data dir, not the CWD — an app
  // bundle launched from the Finder runs with cwd "/" where writes fail
  // silently. --diff still overrides with an explicit path.
  std::string diff_dir;
#if defined(__APPLE__)
  if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
    diff_dir = std::string(home) + "/Library/Application Support/Infinity/";
    std::error_code ec;
    std::filesystem::create_directories(diff_dir, ec);
    if (ec) {
      diff_dir.clear();  // fall back to the CWD (dev shells)
    }
  }
#endif
  anchor->diff_path =
      diff_override != nullptr
          ? std::string(diff_override)
          : diff_dir + "infinity-" + seed_text + cell_tag + "-s" + std::to_string(slot) +
                (moon >= 0 ? "m" + std::to_string(moon) : std::string()) + ".edits";
  anchor->edits = std::make_unique<inf::world::CsgEditStore>();
  if (anchor->edits->load(anchor->diff_path)) {
    std::printf("diff: loaded %zu edits from %s\n", anchor->edits->size(),
                anchor->diff_path.c_str());
  }

  anchor->field = std::make_unique<inf::gen::TerrainField>(anchor->keys.entity, anchor->planet);
  // T0020: the civilization layers of this body, and the civil/v1 height
  // modifier set on the field BEFORE any worker samples it.
  if (civ != nullptr && civ->registry != nullptr && !forced.has_value()) {
    anchor->civ = inf::app::build_civ_anchor(seed, *civ->registry, *civ->resolver, cell, slot, moon,
                                             anchor->keys.entity, anchor->field.get(), civ->now);
    if (anchor->civ) anchor->civ->method = civ->method;
  }
  anchor->sampler =
      std::make_unique<inf::gen::TerrainSampler>(*anchor->field, anchor->edits.get());

  inf::world::ChunkManagerConfig config;
  const unsigned hardware = std::thread::hardware_concurrency();
  config.worker_count = hardware > 4 ? (hardware - 2 > 10 ? 10 : hardware - 2) : 2;
  // Split aggressiveness + residency raised (2026-08-31): chunks refine
  // much earlier, which pushes the coarse-LOD aliasing band (false
  // land/water patches over the ocean) far out and brings walking-scale
  // detail in sooner.
  config.split_factor = 2.6;
  config.resident_budget = 4096;
  // Deepen the quadtree until the finest chunk is ~32 m across (~1 m
  // voxels). The cap has to clear the largest bodies: a 1:10 gas giant is
  // ~7000 km, so 16 levels would leave 200 m chunks and unusably blocky
  // digging. 20 levels covers the whole class range; pack_column gives
  // i/j 26 bits each, so there is plenty of address headroom.
  std::uint8_t max_lod = 8;
  while ((2.0 * anchor->radius) / static_cast<double>(std::uint64_t{1} << max_lod) > 32.0 &&
         max_lod < 20) {
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

// Unit quad in the xy plane ([-1,1]^2, z = 0), used camera-oriented as
// the corona/glow billboard (the shader shapes it radially).
std::vector<float> unit_quad_vertices() {
  static constexpr float kCorners[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
  static constexpr int kTri[6] = {0, 1, 2, 0, 2, 3};
  std::vector<float> vertices;
  vertices.reserve(36);
  for (const int index : kTri) {
    vertices.insert(vertices.end(), {kCorners[index][0], kCorners[index][1], 0.0f, 0.0f,
                                     0.0f, 1.0f});
  }
  return vertices;
}

// Display label for a system body: giants show their CLASS (they have
// no meaningful surface type), rocky worlds show the surface type.
const char* body_type_label(const inf::gen::SystemPlanet& entry) {
  switch (entry.phys.cls) {
    case inf::core::PlanetClass::GasGiant: return "Gas Giant";
    case inf::core::PlanetClass::IceGiant: return "Ice Giant";
    case inf::core::PlanetClass::SubNeptune: return "Sub-Neptune";
    default: return inf::gen::to_string(entry.surface_type);
  }
}

// Blackbody-ish tint for a star's effective temperature: M dwarfs deep
// orange through G yellow-white up to B blue (piecewise linear).
void star_tint(double temp_k, float out[3]) {
  struct Stop {
    double temp;
    float r, g, b;
  };
  static constexpr Stop kStops[] = {
      {2500.0, 1.00f, 0.42f, 0.22f}, {3500.0, 1.00f, 0.60f, 0.40f},
      {4500.0, 1.00f, 0.77f, 0.56f}, {5800.0, 1.00f, 0.93f, 0.82f},
      {7000.0, 1.00f, 0.98f, 0.97f}, {8500.0, 0.83f, 0.90f, 1.00f},
      {12000.0, 0.72f, 0.82f, 1.00f}, {30000.0, 0.60f, 0.74f, 1.00f},
  };
  constexpr int kCount = static_cast<int>(sizeof(kStops) / sizeof(kStops[0]));
  if (temp_k <= kStops[0].temp) {
    out[0] = kStops[0].r; out[1] = kStops[0].g; out[2] = kStops[0].b;
    return;
  }
  for (int i = 1; i < kCount; ++i) {
    if (temp_k <= kStops[i].temp) {
      const float t = static_cast<float>((temp_k - kStops[i - 1].temp) /
                                         (kStops[i].temp - kStops[i - 1].temp));
      out[0] = kStops[i - 1].r + t * (kStops[i].r - kStops[i - 1].r);
      out[1] = kStops[i - 1].g + t * (kStops[i].g - kStops[i - 1].g);
      out[2] = kStops[i - 1].b + t * (kStops[i].b - kStops[i - 1].b);
      return;
    }
  }
  out[0] = kStops[kCount - 1].r;
  out[1] = kStops[kCount - 1].g;
  out[2] = kStops[kCount - 1].b;
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
  // DEFAULT-SEED CONTRACT (2026-08-31): the default seed must produce a
  // system with >= 5 planets, at least one of them EarthLike with >= 1
  // moon. Seed "83" (hex, = 0x83): G star, 9 planets, EarthLike
  // super-earth with 2 moons at slot 1. If a generation change breaks
  // these properties for this seed, search for a new qualifying seed
  // (scan `unendlich-cli dump-system` over seeds) and replace it here AND
  // in the contract test (game/tests/test_system.cpp).
  const char* seed_text = "83";
  const char* type_text = nullptr;
  const char* diff_text = nullptr;
  bool pixel_window = false;
  bool galaxy_demo = false;
  const char* galaxy_profile = nullptr;
  bool map_demo = false;  // scripted M/Esc for headless smoke + captures
  bool windowed = false;  // default is fullscreen on the primary monitor
  const char* capture_text = nullptr;  // --capture <path.ppm>: PPM of the last frame
  double pitch_deg = 0.0;  // --pitch <deg>: initial pitch-down (capture aid)
  bool release_mode = false;  // --release: debug frame ring OFF
  bool hidden = false;        // --hidden: invisible window (scripted captures)
  const char* script_text = nullptr;  // --script <file>: debug command script
  inf::gen::SystemCell start_cell{};  // --system: the starting octree cell (T0020)
  double civ_time_offset_years = 0.0; // --civ-time: civilization clock offset (T0020)
  long long clock_offset_s = 0;       // --clock-offset-s: world clock offset (captures)
  inf::gen::BuildingMethod building_method = inf::gen::BuildingMethod::GrammarParts;  // --buildings
  bool city_showcase = false;  // --city-showcase: T0021 pipeline check (catalog scene at the first town)
  int city_debug = 0;          // --city-debug N: city pipeline debug view
  int bench_frames = 0;        // --bench N: after the script/warm-up, mean frame time over N frames, then exit
  int window_w = 0;            // --window WxH: windowed at this size (measurements at a fixed resolution)
  int window_h = 0;
  bool no_city = false;        // --no-city: sites through the mass path only (baseline measurements)
  bool beacons = true;         // --no-beacons: the light beams over every settlement (B toggles)
  bool no_ssao = false;        // --no-ssao / --no-shadows / --no-taa: renderer feature toggles
  bool no_shadows = false;
  bool no_taa = false;
  // T0022 B.2: the performance options (each a runtime toggle, see the
  // F-keys below): GPU occlusion culling, AO at full instead of half
  // resolution, the far cascades at half rate, the far-cascade LOD.
  bool no_occlusion = false;
  bool ssao_full = false;
  bool shadow_half_rate = false;
  bool shadow_far_lod = false;
  int stress_frames = 0;       // --stress N: recreate the render targets every frame N times, then exit
  bool no_far_patterns = false;  // --no-far-patterns: sub-pixel member geometry at the far levels (A/B)
  int sweep_frames = 0;        // --sweep N: temporal-artifact analysis (the demo's tool)
  double sweep_step = 0.03;    // --sweep-step m
  std::string sweep_out = "sweep";  // --sweep-out name
  int sweep_dir = 0;           // --sweep-dir right|forward|down (0/1/2): the flight direction
  int sweep_blur = 0;          // --sweep-blur R: box-blur both frames so only low-frequency shimmer counts
  const char* assets_text = nullptr;  // --assets <dir>: tile library root
  std::uint32_t tex_size = 1024;      // --tex-size N: material tile resolution
  int spawn_slot = -1;                // --slot N: spawn on this system slot
  int spawn_moon = -1;                // --moon M: spawn on that moon of the slot
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
    } else if (std::strcmp(argv[i], "--render-width") == 0 && i + 1 < argc) {
      char* end = nullptr;
      const long pixels = std::strtol(argv[++i], &end, 10);
      if (*end != '\0' || pixels < 320 || pixels > 16384) {
        std::fprintf(stderr, "--render-width must be 320..16384 pixels\n");
        return EXIT_FAILURE;
      }
      window_w = static_cast<int>(pixels);
      window_h = window_w * 9 / 16;
      pixel_window = true;
      windowed = true;
    } else if (std::strcmp(argv[i], "--galaxy-demo") == 0) {
      galaxy_demo = true;
    } else if (std::strcmp(argv[i], "--galaxy-profile") == 0 && i + 1 < argc) {
      galaxy_profile = argv[++i];
    } else if (std::strcmp(argv[i], "--map-demo") == 0) {
      map_demo = true;
    } else if (std::strcmp(argv[i], "--windowed") == 0) {
      windowed = true;
    } else if (std::strcmp(argv[i], "--capture") == 0 && i + 1 < argc) {
      capture_text = argv[++i];
    } else if (std::strcmp(argv[i], "--pitch") == 0 && i + 1 < argc) {
      pitch_deg = std::strtod(argv[++i], nullptr);
    } else if (std::strcmp(argv[i], "--release") == 0) {
      release_mode = true;
    } else if (std::strcmp(argv[i], "--hidden") == 0) {
      hidden = true;
    } else if (std::strcmp(argv[i], "--script") == 0 && i + 1 < argc) {
      script_text = argv[++i];
    } else if (std::strcmp(argv[i], "--assets") == 0 && i + 1 < argc) {
      assets_text = argv[++i];
    } else if (std::strcmp(argv[i], "--system") == 0 && i + 4 < argc) {
      // T0020: start anchored in another octree system (x y z level).
      start_cell = inf::gen::SystemCell{std::atoll(argv[i + 1]), std::atoll(argv[i + 2]),
                                        std::atoll(argv[i + 3]), std::atoi(argv[i + 4])};
      i += 4;
    } else if (std::strcmp(argv[i], "--buildings") == 0 && i + 1 < argc) {
      // T0020 WP6 comparison: mass | grammar | parts (the decision).
      const char* m = argv[++i];
      building_method = std::strcmp(m, "mass") == 0      ? inf::gen::BuildingMethod::Mass
                        : std::strcmp(m, "grammar") == 0 ? inf::gen::BuildingMethod::Grammar
                                                         : inf::gen::BuildingMethod::GrammarParts;
    } else if (std::strcmp(argv[i], "--city-showcase") == 0) {
      city_showcase = true;
    } else if (std::strcmp(argv[i], "--city-debug") == 0 && i + 1 < argc) {
      city_debug = std::atoi(argv[++i]);
    } else if (std::strcmp(argv[i], "--sweep") == 0 && i + 1 < argc) {
      sweep_frames = std::atoi(argv[++i]);
    } else if (std::strcmp(argv[i], "--sweep-step") == 0 && i + 1 < argc) {
      sweep_step = std::atof(argv[++i]);
    } else if (std::strcmp(argv[i], "--sweep-out") == 0 && i + 1 < argc) {
      sweep_out = argv[++i];
    } else if (std::strcmp(argv[i], "--sweep-dir") == 0 && i + 1 < argc) {
      const char* d = argv[++i];
      sweep_dir = std::strcmp(d, "forward") == 0 ? 1 : (std::strcmp(d, "down") == 0 ? 2 : 0);
    } else if (std::strcmp(argv[i], "--sweep-blur") == 0 && i + 1 < argc) {
      sweep_blur = std::atoi(argv[++i]);
    } else if (std::strcmp(argv[i], "--bench") == 0 && i + 1 < argc) {
      bench_frames = std::atoi(argv[++i]);
    } else if (std::strcmp(argv[i], "--window") == 0 && i + 1 < argc) {
      if (std::sscanf(argv[++i], "%dx%d", &window_w, &window_h) != 2) window_w = window_h = 0;
    } else if (std::strcmp(argv[i], "--no-city") == 0) {
      no_city = true;
    } else if (std::strcmp(argv[i], "--no-beacons") == 0) {
      beacons = false;
    } else if (std::strcmp(argv[i], "--no-ssao") == 0) {
      no_ssao = true;
    } else if (std::strcmp(argv[i], "--no-shadows") == 0) {
      no_shadows = true;
    } else if (std::strcmp(argv[i], "--no-taa") == 0) {
      no_taa = true;
    } else if (std::strcmp(argv[i], "--no-occlusion") == 0) {
      no_occlusion = true;
    } else if (std::strcmp(argv[i], "--ssao-full") == 0) {
      ssao_full = true;
    } else if (std::strcmp(argv[i], "--shadow-half-rate") == 0) {
      shadow_half_rate = true;
    } else if (std::strcmp(argv[i], "--shadow-far-lod") == 0) {
      shadow_far_lod = true;
    } else if (std::strcmp(argv[i], "--stress") == 0 && i + 1 < argc) {
      stress_frames = std::atoi(argv[++i]);
    } else if (std::strcmp(argv[i], "--no-far-patterns") == 0) {
      no_far_patterns = true;
    } else if (std::strcmp(argv[i], "--clock-offset-s") == 0 && i + 1 < argc) {
      // Shift the world clock (planet rotation, orbits): capture aid to
      // put a site into daylight. A per-save constant offset is exactly
      // the escape hatch the systems spec reserved (section 5).
      clock_offset_s = std::atoll(argv[++i]);
    } else if (std::strcmp(argv[i], "--civ-time") == 0 && i + 1 < argc) {
      // T0020: offset the civilization clock by real years (captures of
      // "one week later" / "one year later").
      civ_time_offset_years = std::atof(argv[++i]);
    } else if (std::strcmp(argv[i], "--slot") == 0 && i + 1 < argc) {
      spawn_slot = static_cast<int>(std::strtol(argv[++i], nullptr, 10));
    } else if (std::strcmp(argv[i], "--moon") == 0 && i + 1 < argc) {
      spawn_moon = static_cast<int>(std::strtol(argv[++i], nullptr, 10));
    } else if (std::strcmp(argv[i], "--tex-size") == 0 && i + 1 < argc) {
      tex_size = static_cast<std::uint32_t>(std::strtol(argv[++i], nullptr, 10));
    }
  }

  inf::city::set_far_patterns(!no_far_patterns);
  const auto seed = inf::core::parse_seed(seed_text);
  if (!seed.has_value()) {
    std::fprintf(stderr, "invalid seed: %s\n", seed_text);
    return EXIT_FAILURE;
  }

  // The home world comes from the generated system (T0012/T0013): the
  // first landable slot. --type still forces a standalone planet draw for
  // debugging; the system layout stays authoritative for the map.
  // T0017: `system` is mutable state — the J-jump regenerates it for the
  // octree cell it arrives in.
  inf::gen::SystemCell current_cell = start_cell;  // {0,0,0,0} = the home system
  // T0020: the civilization registry for the home galaxy (aliens in the
  // 125-cell block around the current system + humans), the owner
  // resolver, and the race-home override planets/v1 consults.
  const inf::gen::GalaxyParams civ_galaxy_params = inf::gen::home_galaxy_params(*seed);
  const inf::core::Key civ_galaxy_key = inf::gen::home_galaxy_key(*seed);
  inf::gen::RaceRegistry civ_registry(
      civ_galaxy_key, civ_galaxy_params,
      inf::gen::derive_civilization(civ_galaxy_key, civ_galaxy_params, true));
  civ_registry.set_human(inf::gen::human_race(civ_galaxy_key, civ_galaxy_params));
  const inf::gen::ColonyResolver civ_resolver(civ_registry);
  const auto generate_system_at = [&](const inf::gen::SystemCell& cell) {
    const auto over = civ_registry.home_override(cell);
    inf::gen::HomeSlotOverride slot_override;
    if (over.has_value()) {
      slot_override.habitat = over->habitat;
      slot_override.preferred_flux = over->preferred_flux;
      slot_override.force_biosphere = over->force_biosphere;
    }
    return inf::gen::generate_system(inf::gen::system_key_for(*seed, cell),
                                     over.has_value() ? &slot_override : nullptr);
  };
  inf::gen::StarSystemParams system = generate_system_at(current_cell);
  // Per-slot civilization readout lines for the current system (owner
  // race, faction, level), refreshed on arrival and every few minutes —
  // the state is a closed-form function of the clock, so a refresh is a
  // recomputation, never an accumulation.
  std::string civ_slot_line[inf::gen::kMaxPlanetSlots];
  std::string civ_system_line;
  const auto refresh_civ = [&](const inf::gen::SystemCell& cell, inf::core::WorldTime now) {
    for (auto& line : civ_slot_line) {
      line.clear();
    }
    civ_system_line.clear();
    const inf::gen::SystemCivContext context =
        inf::gen::gather_system_context(*seed, civ_registry, cell, false);
    const inf::gen::Owner owner = civ_resolver.owner(context, now);
    if (!owner.owned) {
      civ_system_line = "Uninhabited";
      return;
    }
    const auto& races = civ_resolver.candidates(context.position_m);
    const inf::gen::Race& race = races[owner.candidate];
    civ_system_line = race.params.name + " space";
    const auto states = civ_resolver.system_states(context, owner, now);
    for (std::size_t i = 0; i < states.size(); ++i) {
      const inf::gen::CivState& st = states[i];
      const int slot = context.bodies[i].slot;
      if (!st.settled) {
        continue;
      }
      const inf::gen::FactionParams* f =
          st.faction_index >= 0 && st.faction_index < static_cast<int>(race.factions.size())
              ? &race.factions[static_cast<std::size_t>(st.faction_index)]
              : nullptr;
      char line[160];
      std::snprintf(line, sizeof(line), "%s%s - %s%s%s - L%d %s", race.params.name.c_str(),
                    st.is_home ? " (home)" : "", f != nullptr ? f->name.c_str() : "",
                    f != nullptr ? " / " : "",
                    f != nullptr ? inf::gen::faction_type_label(f->type, race.params.type,
                                                                 race.params.is_human)
                                 : "",
                    st.level, st.ruined ? "ruins" : (st.domed ? "domed" : inf::gen::to_string(static_cast<inf::gen::DevLevel>(st.level))));
      civ_slot_line[static_cast<std::size_t>(slot)] = line;
    }
  };
  const int home_slot =
      spawn_slot >= 0 && spawn_slot < inf::gen::kMaxPlanetSlots &&
              system.planets[static_cast<std::size_t>(spawn_slot)].occupied
          ? spawn_slot
          : inf::gen::default_landable_slot(system);

  // Galaxy frame (T0017): the octree that owns every star system, and the
  // current system's galactocentric position. System/planet axes are all
  // galaxy-aligned, so the ship's forward vector IS a galactic direction.
  const inf::gen::GalaxyParams galaxy_params = inf::gen::home_galaxy_params(*seed);
  const inf::gen::GalaxyOctree galaxy_octree(inf::gen::home_galaxy_key(*seed),
                                             galaxy_params);
  const auto system_galactic_pos = [&](const inf::gen::SystemCell& cell) {
    if (cell.is_home()) {
      const inf::gen::Dir3 home = inf::gen::home_system_position_m(galaxy_params);
      return SVec3{home.x.to_double(), home.y.to_double(), home.z.to_double()};
    }
    const inf::gen::Dir3 p = galaxy_octree.system_position_m(
        {cell.x, cell.y, cell.z, cell.level});
    return SVec3{p.x.to_double(), p.y.to_double(), p.z.to_double()};
  };
  SVec3 galactic_pos = system_galactic_pos(current_cell);

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
  const inf::core::LocalClock civ_clock;
  const auto civ_now = [&](inf::core::WorldTime t) {
    return inf::core::WorldTime::from_ns(t.ns_since_epoch + inf::gen::real_years_to_ns(civ_time_offset_years));
  };
  const CivSetup civ_setup{&civ_registry, &civ_resolver, civ_now(civ_clock.now()), building_method};
  std::unique_ptr<Anchor> anchor =
      make_anchor(*seed, seed_text, system, current_cell, home_slot,
                  spawn_moon >= 0 &&
                          spawn_moon < static_cast<int>(system.planets[static_cast<std::size_t>(home_slot)]
                                                            .moons.size())
                      ? spawn_moon
                      : -1,
                  forced, diff_text, &civ_setup);
  const double spawn_r =
      spawn_altitude >= 0.0 ? anchor->radius + spawn_altitude : anchor->radius * 2.2;
  inf::sim::Player player(*anchor->effective,
                          inf::sim::normalize(SVec3{1.0, 0.15, 0.3}) * spawn_r);
  if (pitch_deg != 0.0) {
    // Capture aid: pitch the spawn attitude down toward the planet.
    const SVec3 fwd = player.forward();
    const SVec3 up = player.up();
    const SVec3 right = inf::sim::normalize(inf::sim::cross(fwd, up));
    const double rad = -pitch_deg * 3.14159265358979323846 / 180.0;
    player.set_attitude(inf::sim::rotate(fwd, right, rad), inf::sim::rotate(up, right, rad));
  }

  std::printf("unendlich %s (%s) — %s planet (slot %d), radius %.0f m\n",
              inf::gen::kVersion, inf::gen::kGitHash,
              inf::gen::to_string(anchor->planet.type), home_slot, anchor->radius);

  if (glfwInit() != GLFW_TRUE) {
    std::fprintf(stderr, "glfwInit failed\n");
    return EXIT_FAILURE;
  }
  glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
  if (pixel_window) {
    glfwWindowHint(GLFW_SCALE_FRAMEBUFFER, GLFW_FALSE);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
  }
  if (hidden) {
    // Scripted/headless captures: render into an invisible window — no
    // window appears, nothing steals focus.
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    windowed = true;
  }
  // Fullscreen on the primary monitor by default (borderless at the
  // desktop video mode); --windowed keeps the old 1280x720 window.
  GLFWmonitor* monitor = nullptr;
  int win_w = 1280;
  int win_h = 720;
  if (window_w > 0 && window_h > 0) {
    windowed = true;
    win_w = window_w;
    win_h = window_h;
  }
  if (!windowed) {
    monitor = glfwGetPrimaryMonitor();
    const GLFWvidmode* mode = monitor != nullptr ? glfwGetVideoMode(monitor) : nullptr;
    if (mode != nullptr) {
      win_w = mode->width;
      win_h = mode->height;
      glfwWindowHint(GLFW_RED_BITS, mode->redBits);
      glfwWindowHint(GLFW_GREEN_BITS, mode->greenBits);
      glfwWindowHint(GLFW_BLUE_BITS, mode->blueBits);
      glfwWindowHint(GLFW_REFRESH_RATE, mode->refreshRate);
    } else {
      monitor = nullptr;  // no usable video mode: fall back to windowed
    }
  }
  GLFWwindow* window = glfwCreateWindow(win_w, win_h, "unendlich", monitor, nullptr);
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

  // Debug mode is the default: the frame ring keeps the last ~3 s of
  // reduced-res frames in memory for F9/scripted dumps. --release turns
  // the ring off (F9 still records the 3 s of future frames).
  rhi->set_ring_enabled(!release_mode);

  const std::vector<float> cube = unit_cube_vertices();
  const std::uint32_t cube_mesh = rhi->create_mesh(cube.data(), cube.size());
  const std::vector<float> ball = unit_sphere_vertices(32, 16);
  const std::uint32_t body_mesh = rhi->create_mesh(ball.data(), ball.size());
  const std::vector<float> star_ball = unit_sphere_vertices(48, 24);
  const std::uint32_t star_mesh = rhi->create_mesh(star_ball.data(), star_ball.size());
  // High-tessellation sphere for the planet impostor: at 32x16 the
  // silhouette reads visibly polygonal when the planet fills the view.
  const std::vector<float> fine_ball = unit_sphere_vertices(96, 48);
  const std::uint32_t impostor_mesh = rhi->create_mesh(fine_ball.data(), fine_ball.size());
  const std::vector<float> quad = unit_quad_vertices();
  const std::uint32_t glow_mesh = rhi->create_mesh(quad.data(), quad.size());
  // Site beacon (mode 9): two crossed vertical quads, x/z in [-0.5, 0.5],
  // y in [0, 1]; weights.x = 1 at the base .. 0 at the top, weights.y =
  // 0 at the centre line .. 1 at the edge. Scaled per site per frame.
  const std::uint32_t beacon_mesh = [&]() {
    std::vector<float> v;
    const auto vert = [&](float x, float y, float z, float edge) {
      const float row[10] = {x, y, z, 0.0f, 1.0f, 0.0f, 1.0f - y, edge, 0.0f, 0.0f};
      v.insert(v.end(), row, row + 10);
    };
    for (int q = 0; q < 2; ++q) {
      const auto at = [&](float u, float y, float edge) {
        if (q == 0) vert(u, y, 0.0f, edge);
        else vert(0.0f, y, u, edge);
      };
      // Three strips across (edge 1, 0, 1) so the softness is per-vertex.
      const float us[3] = {-0.5f, 0.0f, 0.5f};
      const float edges[3] = {1.0f, 0.0f, 1.0f};
      for (int k = 0; k < 2; ++k) {
        at(us[k], 0.0f, edges[k]); at(us[k + 1], 0.0f, edges[k + 1]); at(us[k + 1], 1.0f, edges[k + 1]);
        at(us[k], 0.0f, edges[k]); at(us[k + 1], 1.0f, edges[k + 1]); at(us[k], 1.0f, edges[k]);
      }
    }
    return rhi->create_mesh_mat(v.data(), v.size());
  }();

  // Both free flight and J arrivals use the same spatial sky and star stream.
  auto sky_volume = inf::app::build_galaxy_volume(*seed, galaxy_params);
  const auto sky_texture = rhi->create_sky_volume(sky_volume.size);
  for (std::uint32_t z = 0; z < sky_volume.size; ++z)
    rhi->update_sky_slice(sky_texture, z,
                          sky_volume.rgba_half.data() +
                              static_cast<std::size_t>(z) * sky_volume.size *
                                  sky_volume.size * 4);
  sky_volume.rgba_half.clear();
  sky_volume.rgba_half.shrink_to_fit();
  const auto neighbour_galaxies = inf::app::distant_galaxies(*seed);
  inf::app::StellarStream stellar_stream(*seed, galaxy_params);
  auto star_catalog = inf::app::build_stellar_catalog(
      galaxy_octree, galactic_pos, {}, nullptr, 5.5);
  std::uint32_t star_field_mesh =
      star_catalog.vertices.empty()
          ? 0
          : rhi->create_mesh_mat(star_catalog.vertices.data(),
                                 star_catalog.vertices.size());
  star_catalog.vertices.clear();
  using FlightClock = inf::core::MonotonicClock;
  auto star_requested = FlightClock::now();

  // Sea shell (spec section 5): one translucent sphere at sea level,
  // EarthLike only. Zero shading effort by design. Rebuilt per anchor.
  std::uint32_t sea_mesh = 0;
  double sea_radius = 0.0;
  // Land impostor (T0015 follow-up): a coarse elevation-displaced sphere
  // sampled ONCE per anchor. From orbit the chunk terrain is hidden (its
  // coarse LOD aliases into flickering land/water patches); this static
  // mesh carries the continents instead — same land, no churn.
  std::uint32_t land_mesh = 0;
  std::uint8_t land_palette[4] = {0, 0, 0, 0};
  const auto rebuild_sea = [&] {
    if (sea_mesh != 0) {
      rhi->destroy_mesh(sea_mesh);
      sea_mesh = 0;
    }
    if (land_mesh != 0) {
      rhi->destroy_mesh(land_mesh);
      land_mesh = 0;
    }
    sea_radius = 0.0;
    if (anchor->planet.type == inf::gen::PlanetType::EarthLike) {
      // Dense enough that the water silhouette stays round at low flight.
      const std::vector<float> sphere = unit_sphere_vertices(192, 96);
      sea_mesh = rhi->create_mesh(sphere.data(), sphere.size());
      sea_radius = anchor->radius + anchor->planet.sea_level_m.to_double();
    }
    {
      constexpr int kSlices = 160;
      constexpr int kStacks = 80;
      const double pi = 3.14159265358979323846;
      inf::gen::TerrainField::ParamCache cache;
      const auto vertex_radius = [&](int slice, int stack) {
        const double phi = pi * stack / kStacks - pi * 0.5;
        const double theta = 2.0 * pi * slice / kSlices;
        const inf::gen::Dir3 dir{inf::det::Real(std::cos(phi) * std::cos(theta)),
                                 inf::det::Real(std::cos(phi) * std::sin(theta)),
                                 inf::det::Real(std::sin(phi))};
        const auto canonical =
            anchor->field->canonical_params(inf::gen::dir_to_face_uv(dir), &cache);
        inf::gen::BlendedParams params = inf::gen::TerrainField::to_blended(canonical);
        // Sunk 300 m below the true surface: streamed chunks always win
        // depth (no z-fighting), while unstreamed regions show the right
        // continents instead of bare ocean. Invisible from orbit
        // (0.03% of R) — it just stops the ocean->land streaming pop.
        return anchor->radius - 300.0 +
               anchor->field->elevation_from_params(dir, params, canonical.macro_rel)
                   .to_double();
      };
      // Radius table first: every grid point sampled exactly once.
      std::vector<double> radii(static_cast<std::size_t>(kSlices + 1) * (kStacks + 1));
      for (int stack = 0; stack <= kStacks; ++stack) {
        for (int slice = 0; slice <= kSlices; ++slice) {
          radii[static_cast<std::size_t>(stack) * (kSlices + 1) + slice] =
              vertex_radius(slice % kSlices, stack);
        }
      }
      const auto grid_pos = [&](int slice, int stack) {
        slice = ((slice % kSlices) + kSlices) % kSlices;
        stack = std::clamp(stack, 0, kStacks);
        const double phi = pi * stack / kStacks - pi * 0.5;
        const double theta = 2.0 * pi * slice / kSlices;
        const double r = radii[static_cast<std::size_t>(stack) * (kSlices + 1) + slice];
        return RVec3{std::cos(phi) * std::cos(theta) * r,
                     std::cos(phi) * std::sin(theta) * r, std::sin(phi) * r};
      };
      std::vector<float> vertices;
      vertices.reserve(static_cast<std::size_t>(kSlices) * kStacks * 60);
      // The land mesh carries one four-material palette for the whole
      // planet: the materials with the most presence over the grid.
      std::vector<double> grid_weights(static_cast<std::size_t>(kSlices + 1) * (kStacks + 1) *
                                       inf::gen::kMaterialCount);
      double palette_total[inf::gen::kMaterialCount] = {};
      const auto point = [&](int slice, int stack, float out[10]) {
        const double phi = pi * stack / kStacks - pi * 0.5;
        const double theta = 2.0 * pi * slice / kSlices;
        const double nx = std::cos(phi) * std::cos(theta);
        const double ny = std::cos(phi) * std::sin(theta);
        const double nz = std::sin(phi);
        const double r = radii[static_cast<std::size_t>(stack) * (kSlices + 1) + slice];
        out[0] = static_cast<float>(nx * r);
        out[1] = static_cast<float>(ny * r);
        out[2] = static_cast<float>(nz * r);
        // REAL terrain normals from the height grid, not the sphere
        // radial: with radial normals the impostor lit up at grazing sun
        // angles where the (correctly shaded) streamed terrain stayed
        // dark — a bright band at the horizon at night.
        const RVec3 du = grid_pos(slice + 1, stack) - grid_pos(slice - 1, stack);
        const RVec3 dv = grid_pos(slice, stack + 1) - grid_pos(slice, stack - 1);
        RVec3 normal = inf::render::cross(du, dv);
        const double len = inf::render::length(normal);
        if (len < 1e-9) {
          normal = RVec3{nx, ny, nz};
        } else {
          normal = normal * (1.0 / len);
          if (normal.x * nx + normal.y * ny + normal.z * nz < 0.0) {
            normal = normal * -1.0;  // outward
          }
        }
        out[3] = static_cast<float>(normal.x);
        out[4] = static_cast<float>(normal.y);
        out[5] = static_cast<float>(normal.z);
        // Weights at the TRUE surface radius (the mesh is sunk 300 m),
        // over the planet palette chosen below.
        const std::size_t gi =
            static_cast<std::size_t>(std::clamp(stack, 0, kStacks)) * (kSlices + 1) +
            static_cast<std::size_t>(((slice % kSlices) + kSlices) % kSlices);
        const double* w = grid_weights.data() + gi * inf::gen::kMaterialCount;
        double sum = 0.0;
        double ws[4] = {0.0, 0.0, 0.0, 0.0};
        for (int k = 0; k < 4; ++k) {
          ws[k] = land_palette[k] != 0 ? w[land_palette[k]] : 0.0;
          sum += ws[k];
        }
        for (int k = 0; k < 4; ++k) {
          out[6 + k] = sum > 0.0 ? static_cast<float>(ws[k] / sum) : (k == 0 ? 1.0f : 0.0f);
        }
      };
      // Pass 1: weights at every grid point + palette pick.
      for (int stack = 0; stack <= kStacks; ++stack) {
        for (int slice = 0; slice <= kSlices; ++slice) {
          const double phi = pi * stack / kStacks - pi * 0.5;
          const double theta = 2.0 * pi * (slice % kSlices) / kSlices;
          const double nx = std::cos(phi) * std::cos(theta);
          const double ny = std::cos(phi) * std::sin(theta);
          const double nz = std::sin(phi);
          const double r =
              radii[static_cast<std::size_t>(stack) * (kSlices + 1) + slice] + 300.0;
          double* w = grid_weights.data() +
                      (static_cast<std::size_t>(stack) * (kSlices + 1) + slice) *
                          inf::gen::kMaterialCount;
          anchor->field->material_weights(nx * r, ny * r, nz * r, nx, ny, nz, &cache, w);
          double vmax = 0.0;
          for (std::uint32_t m = 1; m < inf::gen::kMaterialCount; ++m) {
            vmax = std::max(vmax, w[m]);
          }
          if (vmax > 0.0) {
            for (std::uint32_t m = 1; m < inf::gen::kMaterialCount; ++m) {
              palette_total[m] += w[m] / vmax;
            }
          }
        }
      }
      for (int k = 0; k < 4; ++k) {
        double best = 0.0;
        std::uint32_t pick = 0;
        for (std::uint32_t m = 1; m < inf::gen::kMaterialCount; ++m) {
          if (palette_total[m] > best) {
            best = palette_total[m];
            pick = m;
          }
        }
        land_palette[k] = static_cast<std::uint8_t>(pick);
        if (pick != 0) {
          palette_total[pick] = -1.0;
        }
      }
      for (int stack = 0; stack < kStacks; ++stack) {
        for (int slice = 0; slice < kSlices; ++slice) {
          float p00[10], p10[10], p01[10], p11[10];
          point(slice, stack, p00);
          point(slice + 1, stack, p10);
          point(slice, stack + 1, p01);
          point(slice + 1, stack + 1, p11);
          const float* quad[6] = {p00, p10, p11, p00, p11, p01};
          for (const float* v : quad) {
            vertices.insert(vertices.end(), v, v + 10);
          }
        }
      }
      land_mesh = rhi->create_mesh_mat(vertices.data(), vertices.size());
    }
  };
  rebuild_sea();
  {  // HUD scope: must destruct before the RHI is torn down.
  auto hud = std::make_unique<inf::app::Hud>(rhi.get(), anchor->field.get(), anchor->planet);
  SVec3 last_player_pos = player.position();
  double measured_speed = 0.0;

  AppState state{rhi.get(), 1280, 720};
  glfwGetFramebufferSize(window, &state.width, &state.height);
  glfwSetWindowUserPointer(window, &state);
  glfwSetFramebufferSizeCallback(window, framebuffer_size_callback);
  glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
  if (glfwRawMouseMotionSupported() == GLFW_TRUE) {
    glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
  }
  double last_mx = 0.0;
  double last_my = 0.0;
  glfwGetCursorPos(window, &last_mx, &last_my);
  const inf::core::SyncedClock world_clock(std::make_shared<inf::core::LocalClock>(),
                                           clock_offset_s * 1'000'000'000LL);
  refresh_civ(current_cell, civ_now(world_clock.now()));
  CivSetup civ_setup_now{&civ_registry, &civ_resolver, civ_now(world_clock.now()), building_method};
  inf::core::WorldTime last_time = world_clock.now();
  double fps_accum = 0.0;
  int fps_frames = 0;
  bool e_was_down = false;
  bool m_was_down = false;
  bool esc_was_down = false;
  bool f9_was_down = false;
  bool b_was_down = false;
  double beacon_night = 0.0;  // the app's night factor, for the beacons' intensity
  double edit_cooldown = 0.0;
  double rec_flash = 0.0;          // REC icon flash after the F9 press
  std::string rec_dir_current;     // active recording dir (meta.csv sink)
  std::vector<ScriptCmd> script;
  if (script_text != nullptr) {
    script = load_script(script_text);
    std::printf("script: %zu commands from %s\n", script.size(), script_text);
  }
  std::size_t script_pc = 0;
  double script_wait = 0.0;
  bool script_thrust = false;
  bool script_land = false;
  bool script_map = false;   // scripted M press (map captures)
  bool script_jump = false;  // scripted J select + instant confirm
  bool script_exposure_locked = false;
  bool script_hud = true;    // scripted HUD visibility (clean captures)

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

  // --- interstellar jump state (T0017 WP6) ----------------------------
  struct JumpCandidate {
    inf::gen::SystemCell cell;
    SVec3 pos_gal;
    double dist_ly{0.0};
  };
  std::vector<JumpCandidate> jump_candidates;
  int jump_index = -1;           // armed selection into jump_candidates
  SVec3 jump_sel_forward{0.0, 0.0, 0.0};
  std::string jump_sel_name;
  bool j_was_down = false;
  double j_hold = 0.0;
  double jump_timer = 0.0;       // > 0: transition running
  bool jump_swapped = false;
  JumpCandidate jump_target{};

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
  std::array<std::vector<std::string>, inf::gen::kMaxPlanetSlots> moon_names;
  std::array<std::uint32_t, inf::gen::kMaxPlanetSlots> orbit_meshes{};
  std::array<std::uint32_t, inf::gen::kMaxPlanetSlots> arc_meshes{};
  // Per-system UI state; re-run after a jump regenerates `system`.
  const auto rebuild_system_ui = [&] {
    for (int slot = 0; slot < inf::gen::kMaxPlanetSlots; ++slot) {
      if (orbit_meshes[static_cast<std::size_t>(slot)] != 0) {
        rhi->destroy_mesh(orbit_meshes[static_cast<std::size_t>(slot)]);
        orbit_meshes[static_cast<std::size_t>(slot)] = 0;
      }
      if (arc_meshes[static_cast<std::size_t>(slot)] != 0) {
        rhi->destroy_mesh(arc_meshes[static_cast<std::size_t>(slot)]);
        arc_meshes[static_cast<std::size_t>(slot)] = 0;
      }
      slot_names[static_cast<std::size_t>(slot)].clear();
      moon_names[static_cast<std::size_t>(slot)].clear();
      const auto& entry = system.planets[static_cast<std::size_t>(slot)];
      if (!entry.occupied) {
        continue;
      }
      slot_names[static_cast<std::size_t>(slot)] = inf::gen::body_display_name(
          inf::gen::body_for_system_slot(*seed, current_cell, slot).entity);
      for (std::size_t mi = 0; mi < entry.moons.size(); ++mi) {
        moon_names[static_cast<std::size_t>(slot)].push_back(
            inf::gen::body_display_name(inf::gen::body_for_system_moon(
                                            *seed, current_cell, slot,
                                            static_cast<int>(mi))
                                            .entity));
      }
      const auto ribbon =
          orbit_ribbon_vertices(entry.orbit, 0.0, 2.0 * 3.14159265358979323846, 256,
                                map_px_m * 1.6);
      orbit_meshes[static_cast<std::size_t>(slot)] =
          rhi->create_mesh(ribbon.data(), ribbon.size());
    }
  };
  rebuild_system_ui();
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
  std::unordered_map<inf::core::ChunkAddr, std::shared_ptr<const inf::world::ChunkData>,
                     AddrHash>
      pending_ready;
  std::vector<inf::render::Rhi::DrawItem> items;
  std::vector<inf::render::Rhi::CityRange> city_ranges;  // T0022: the ranges the mode-8 items refer to

  // --- far-view planet textures (T0016) --------------------------------
  // A background worker bakes one (height, albedo) cube-map pair per
  // system body from the live generators; the main loop uploads finished
  // bakes and swaps the flat draw_ball spheres for displaced, textured
  // impostors. Pure cosmetic cache of a pure function: nothing here may
  // ever feed collision or gameplay, and nothing is persisted.
  struct BodyTexture {
    std::uint32_t handle{0};
    float amp_over_radius{0.0f};
    float slope_scale{0.0f};
  };
  const auto body_tex_key = [](int slot, int moon) {
    return static_cast<std::uint32_t>(moon < 0 ? slot : 0x1000 + slot * 32 + moon);
  };
  std::unordered_map<std::uint32_t, BodyTexture> body_textures;
  // Surface tile library (T0019): loads in the background; the far-view
  // baker takes its measured tile means so orbit and ground agree.
  inf::app::MaterialLibrary materials;
  {
    const std::string assets_dir = inf::app::find_assets_dir(assets_text, argv[0]);
    std::printf("materials: %s, %ux%u tiles\n",
                assets_dir.empty() ? "no asset directory (procedural tiles)" : assets_dir.c_str(),
                tex_size, tex_size);
    materials.start(assets_dir, tex_size);
  }
  // T0021 --city-showcase: the catalog scene on the first town's plateau
  // through the city pipeline (a renderer check, not gameplay).
  inf::app::CityUpload city_upload;
  Mat4 city_prev_view_proj = Mat4::identity();
  RVec3 city_prev_camera{0.0, 0.0, 0.0};
  // --sweep: temporal-artifact analysis (the demo's tool): after the
  // script/warm-up the camera slides sideways per frame; each final frame
  // and depth buffer are read back, every pixel is reprojected into the
  // previous frame and the band-limited change (gradient x motion) is
  // subtracted. What remains is temporal aliasing.
  // T0022 C.4: every pixel of the current frame is reprojected into the
  // previous frame through the depth buffer and the two view-projections
  // (exact for a static world), the previous frame is sampled there and
  // differenced; half a pixel of local gradient is tolerated. Luminance
  // and chroma are kept apart (chroma is what exposed the z-fighting),
  // the flight direction is a choice, and an optional blur of both
  // frames leaves only the low-frequency shimmer the eye sees. A first
  // frame in the material-id debug view attributes the residual per
  // material.
  struct Sweep {
    int phase{0};   // 0 ask for the id frame, 1 take it, 2 settle, 3 measuring
    int settle{0};
    int step{0};
    int saved_debug{0};
    std::vector<std::uint8_t> ids, prev;
    std::vector<float> resid, raw, chroma;
    Mat4 prev_vp = Mat4::identity();
    RVec3 prev_cam{0.0, 0.0, 0.0};
    std::uint32_t w{0}, h{0};
    int measured{0};
  } sweep;
  const long sweep_warmup = 40;
  bool city_active = false;
  long chunk_uploads = 0;  // chunk meshes uploaded (bench diagnostics)
  const auto build_city_showcase = [&]() {
    city_active = false;
    if (!city_showcase || !anchor || !anchor->civ || !anchor->civ->sites) {
      if (city_showcase) std::printf("city-showcase: no settled site on the anchor body\n");
      return;
    }
    const inf::gen::Site* pick = nullptr;
    for (const inf::gen::Site& site : anchor->civ->sites->sites()) {
      if (site.tier >= static_cast<int>(inf::gen::SettlementTier::Town) &&
          (pick == nullptr || site.tier < pick->tier)) {
        pick = &site;
      }
    }
    if (pick == nullptr && !anchor->civ->sites->sites().empty()) pick = &anchor->civ->sites->sites().front();
    if (pick == nullptr) return;
    inf::city::Scene scene;
    scene.materials = inf::city::make_materials();
    inf::city::generate_showcase_small(scene, inf::city::Rng(anchor->keys.entity).child(0x51));
    inf::app::upload_city_materials(*rhi, scene.materials);
    city_upload = inf::app::upload_city_scene(*rhi, scene, pick->frame, pick->datum_m);
    city_active = city_upload.drawable();
    const inf::gen::Dir3& up = pick->frame.up;
    const double r = anchor->radius + pick->datum_m;
    std::printf("city-showcase: %u triangles on site %u (%s) at planet-local (%.1f, %.1f, %.1f)\n",
                city_upload.triangles, pick->province,
                inf::gen::to_string(static_cast<inf::gen::SettlementTier>(pick->tier)),
                up.x.to_double() * r, up.y.to_double() * r, up.z.to_double() * r);
    // Capture lines: 220 m south-west of the centre, 90 m up, looking at it.
    {
      const inf::gen::Dir3& north = pick->frame.north;
      const inf::gen::Dir3& east = pick->frame.east;
      const double back = 200.0;
      const double side = 90.0;
      const double height = 80.0;
      const double px = up.x.to_double() * (r + height) - north.x.to_double() * back - east.x.to_double() * side;
      const double py = up.y.to_double() * (r + height) - north.y.to_double() * back - east.y.to_double() * side;
      const double pz = up.z.to_double() * (r + height) - north.z.to_double() * back - east.z.to_double() * side;
      const double tx = up.x.to_double() * (r + 30.0) - px;
      const double ty = up.y.to_double() * (r + 30.0) - py;
      const double tz = up.z.to_double() * (r + 30.0) - pz;
      const double len = std::sqrt(tx * tx + ty * ty + tz * tz);
      std::printf("city-showcase: pos %.1f %.1f %.1f\ncity-showcase: aim dir %.5f %.5f %.5f\n", px, py, pz, tx / len, ty / len, tz / len);
      std::fflush(stdout);
    }
  };
  build_city_showcase();
  const inf::gen::TerrainField* materials_anchor = nullptr;

  struct BakeResult {
    std::uint32_t key{0};
    double radius_m{0.0};
    inf::gen::PlanetTexture texture;
  };
  std::mutex bake_mutex;
  std::vector<BakeResult> bake_done;
  std::atomic<bool> bake_quit{false};
  std::atomic<bool> bake_running{false};
  bool bake_restart_pending = false;
  std::thread bake_thread;
  std::vector<inf::app::CivBodyInputs> bake_civ_inputs;
  std::unique_ptr<BakeResult> uploading_body;
  std::uint32_t uploading_body_handle = 0, uploading_body_face = 0;
  // Restartable (T0017): a jump regenerates the system, so the worker is
  // stopped, textures dropped, and a new worker started for the arrival
  // system.
  const auto start_bake_worker = [&](const inf::gen::StarSystemParams
                                         system_copy,
                                     const inf::gen::SystemCell cell_copy,
                                     bool refresh_inputs = true) {
    bake_quit.store(false);
    bake_restart_pending = false;
    std::vector<float> means_copy(materials.mean_albedo_table(),
                                  materials.mean_albedo_table() + inf::gen::kMaterialCount * 3);
    // T0020 WP7: the settled bodies' civ inputs, gathered on this thread
    // (the registry is not thread-safe); the worker rebuilds each body's
    // modifier on its own field so the bake shows plates, urban albedo
    // and night lights from orbit.
    if (refresh_inputs) {
      bake_civ_inputs =
          inf::app::gather_civ_bodies(*seed, civ_registry, civ_resolver,
                                      cell_copy, civ_now(world_clock.now()));
    }
    const auto civ_bodies = bake_civ_inputs;
    bake_running.store(true);
    bake_thread = std::thread([&bake_mutex, &bake_done, &bake_quit,
                               &bake_running, &body_tex_key, seed_copy = *seed,
                               system_copy, cell_copy, means_copy,
                               civ_bodies]() {
      struct Job {
        int slot;
        int moon;  // -1 = the planet itself
        std::uint32_t size;
      };
      std::vector<Job> jobs;
      for (int slot = 0; slot < inf::gen::kMaxPlanetSlots; ++slot) {
        const auto& entry = system_copy.planets[static_cast<std::size_t>(slot)];
        if (!entry.occupied) {
          continue;
        }
        // Giants get a coarse map only: their parameter lattice is ~200 km
        // per cell, so extra texels buy nothing yet (T0015 section 13).
        const bool giant = entry.phys.radius_m.to_double() > 2.0e6;
        jobs.push_back({slot, -1, giant ? 128U : 256U});
        // Moons ride right behind their planet (was: all moons last) — an
        // early moon visit found a flat untextured ball otherwise.
        for (std::size_t mi = 0; mi < entry.moons.size(); ++mi) {
          jobs.push_back({slot, static_cast<int>(mi), 192U});
        }
      }
      for (const Job& job : jobs) {
        if (bake_quit.load()) {
          break;
        }
        BakeResult result;
        result.key = body_tex_key(job.slot, job.moon);
        const inf::app::CivBodyInputs* civ_inputs = nullptr;
        for (const auto& b : civ_bodies) {
          if (b.slot == job.slot && b.moon == job.moon) civ_inputs = &b;
        }
        if (job.moon < 0) {
          const inf::gen::BodyHandle body =
              inf::gen::body_for_system_slot(seed_copy, cell_copy, job.slot);
          const auto& entry =
              system_copy.planets[static_cast<std::size_t>(job.slot)];
          if (!entry.landable) {
            // Giants: a banded gas ball, not rocky terrain (2026-09-01).
            result.radius_m = entry.phys.radius_m.to_double();
            result.texture = inf::gen::bake_gas_texture(
                body.entity, entry.phys.cls, job.size);
          } else {
            const inf::gen::PlanetParams planet =
                inf::gen::planet_params_for_slot(system_copy, job.slot, body);
            inf::gen::TerrainField field(body.entity, planet);
            std::unique_ptr<inf::app::CivModifier> modifier;
            if (civ_inputs != nullptr)
              modifier = inf::app::build_civ_modifier(body.entity, &field,
                                                      *civ_inputs);
            result.radius_m = planet.radius_m.to_double();
            result.texture = inf::gen::bake_planet_texture(field, job.size,
                                                           means_copy.data());
          }
        } else {
          const inf::gen::BodyHandle body = inf::gen::body_for_system_moon(
              seed_copy, cell_copy, job.slot, job.moon);
          const inf::gen::PlanetParams planet =
              inf::gen::planet_params_for_moon(system_copy, job.slot, job.moon,
                                               body);
          inf::gen::TerrainField field(body.entity, planet);
          std::unique_ptr<inf::app::CivModifier> modifier;
          if (civ_inputs != nullptr)
            modifier =
                inf::app::build_civ_modifier(body.entity, &field, *civ_inputs);
          result.radius_m = planet.radius_m.to_double();
          result.texture =
              inf::gen::bake_planet_texture(field, job.size, means_copy.data());
        }
        if (bake_quit.load()) break;
        std::printf(
            "bake: slot %d moon %d done%s\n", job.slot, job.moon,
            civ_inputs != nullptr ? " (with civilization surface)" : "");
        std::fflush(stdout);
        const std::lock_guard<std::mutex> lock(bake_mutex);
        bake_done.push_back(std::move(result));
      }
      bake_running.store(false);
    });
  };
  const auto stop_bake_worker = [&] {
    bake_restart_pending = false;
    bake_quit.store(true);
    if (bake_thread.joinable()) {
      bake_thread.join();
    }
    const std::lock_guard<std::mutex> lock(bake_mutex);
    bake_done.clear();
    if (uploading_body_handle)
      rhi->destroy_planet_texture(uploading_body_handle);
    uploading_body_handle = uploading_body_face = 0;
    uploading_body.reset();
  };
  start_bake_worker(system, current_cell);
  const auto upload_finished_bakes = [&] {
    if (!uploading_body) {
      const std::lock_guard<std::mutex> lock(bake_mutex);
      if (bake_done.empty()) return;
      uploading_body =
          std::make_unique<BakeResult>(std::move(bake_done.front()));
      bake_done.erase(bake_done.begin());
      uploading_body_handle =
          rhi->create_planet_texture(uploading_body->texture.face_size);
      uploading_body_face = 0;
    }
    // Publish only a complete texture, but spread its six uploads over frames.
    auto& result = *uploading_body;
    const auto face = uploading_body_face++;
    rhi->update_planet_face(uploading_body_handle, face,
                            result.texture.faces[face].height_half.data(),
                            result.texture.faces[face].rgba.data());
    if (uploading_body_face == 6) {
      BodyTexture entry;
      entry.handle = uploading_body_handle;
      entry.amp_over_radius = static_cast<float>(
          static_cast<double>(result.texture.height_amp_m) / result.radius_m);
      entry.slope_scale = static_cast<float>(
          static_cast<double>(result.texture.height_amp_m) *
          static_cast<double>(result.texture.face_size) / (3.1415926 * result.radius_m));
      if (const auto old = body_textures.find(result.key); old != body_textures.end()) {
        rhi->destroy_planet_texture(old->second.handle);
      }
      body_textures[result.key] = entry;
      uploading_body.reset();
      uploading_body_handle = uploading_body_face = 0;
    }
  };

  bool f6_was_down = false;
  inf::app::GalaxyFlight galaxy_flight;
  bool flight_active = false, flight_profile_exit = false,
       flight_final_frame = false;
  bool scene_presented = false;
  FlightClock::time_point flight_epoch;
  double flight_elapsed = 0;
  SVec3 flight_start_local{}, flight_start_planet{};
  std::ofstream flight_csv;
  std::printf("F6: continuous galaxy flight; F6/Esc: brake and resume here\n");
  long frame = 0;
  while (glfwWindowShouldClose(window) == GLFW_FALSE) {
    const auto frame_started = FlightClock::now();
    glfwPollEvents();
    flight_final_frame = false;
    const bool f6_down = glfwGetKey(window, GLFW_KEY_F6) == GLFW_PRESS;
    const bool flight_toggle = f6_down && !f6_was_down;
    // Automatic activation follows the first displayed scene, just as an F6
    // press does. Initial scene uploads do not consume the departure ramp.
    bool flight_requested =
        (galaxy_demo && scene_presented) || (flight_toggle && !flight_active);
    flight_profile_exit =
        flight_profile_exit || (galaxy_demo && galaxy_profile != nullptr);
    if (flight_requested) galaxy_demo = false;
    f6_was_down = f6_down;
    upload_finished_bakes();
    if (materials.poll(*rhi)) {
      // Every tile is resident: re-bake the far views with the measured
      // tile means so the planet from orbit matches the ground.
      materials_anchor = nullptr;
      bake_quit.store(true);
      bake_restart_pending = true;
    }
    // A material refresh must not join a body bake that is still computing.
    // Keep displaying resident textures while cancellation completes
    // off-thread.
    if (bake_restart_pending && !bake_running.load()) {
      stop_bake_worker();
      start_bake_worker(system, current_cell, false);
    }
    if (materials_anchor != anchor->field.get()) {
      materials.apply_planet(*rhi, anchor->field->material());
      materials_anchor = anchor->field.get();
    }
    const auto resources_finished = FlightClock::now();
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
    if (esc_pressed && map_phase == MapPhase::Off && !flight_active) {
      glfwSetWindowShouldClose(window, GLFW_TRUE);  // prototype convenience
    }
    const inf::core::WorldTime now = world_clock.now();
    civ_setup_now.now = civ_now(now);
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
    input.forward = glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS || script_thrust;
    input.back = glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS;
    input.left = glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS;
    input.right = glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS;
    input.run = glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS;
    input.fire = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
    input.interact_pressed = (e_down && !e_was_down) || script_land;
    script_land = false;
    input.aspect = static_cast<double>(state.width) / state.height;
    input.fov_y = kFovY;
    last_mx = mx;
    last_my = my;
    e_was_down = e_down;
    if (jump_timer > 0.0) {
      // The jump transition suspends the pilot: one atomic frame swap in
      // the middle, no control input on either side of it.
      input.forward = false;
      input.back = false;
      input.left = false;
      input.right = false;
      input.fire = false;
      input.interact_pressed = false;
      input.mouse_dx = 0.0;
      input.mouse_dy = 0.0;
    }

    // F9: debug recording — dump the last ~3 s ring and keep recording
    // 3 s of future frames (REC icon top right while active).
    const bool f9_down = glfwGetKey(window, GLFW_KEY_F9) == GLFW_PRESS;
    if (f9_down && !f9_was_down) {
      char dir_name[64];
      std::snprintf(dir_name, sizeof(dir_name), "debug-rec-%lld",
                    static_cast<long long>(now.ns_since_epoch / 1'000'000LL));
      std::error_code ec;
      std::filesystem::create_directories(dir_name, ec);
      rec_dir_current = dir_name;
      rhi->trigger_recording(rec_dir_current, 3.0);
      rec_flash = 0.7;
    }
    f9_was_down = f9_down;
    const bool b_down = glfwGetKey(window, GLFW_KEY_B) == GLFW_PRESS;
    if (b_down && !b_was_down) beacons = !beacons;
    b_was_down = b_down;
    // T0022 B.2: the city performance options (F7 occlusion culling, F8
    // AO resolution, F10 far cascades at half rate, F11 far-cascade LOD).
    {
      static bool opt_was_down[4] = {false, false, false, false};
      const int keys[4] = {GLFW_KEY_F7, GLFW_KEY_F8, GLFW_KEY_F10, GLFW_KEY_F11};
      bool* flags[4] = {&no_occlusion, &ssao_full, &shadow_half_rate, &shadow_far_lod};
      bool changed = false;
      for (int k = 0; k < 4; ++k) {
        const bool down = glfwGetKey(window, keys[k]) == GLFW_PRESS;
        if (down && !opt_was_down[k]) {
          *flags[k] = !*flags[k];
          changed = true;
        }
        opt_was_down[k] = down;
      }
      if (changed) {
        std::printf("city options: occlusion %s, ssao %s, far cascades %s, far-cascade lod %s\n", no_occlusion ? "off" : "on",
                    ssao_full ? "full" : "half", shadow_half_rate ? "half rate" : "every frame", shadow_far_lod ? "on" : "off");
      }
    }

    if (!flight_active) player.update(input);

    // --- live system state (ephemerides; the universe never pauses) -----
    const auto eval_pos = [&](const inf::core::OrbitalElements& orbit) {
      const auto pv = inf::core::Ephemeris::evaluate(orbit, now);
      return SVec3{pv.x.to_double(), pv.y.to_double(), pv.z.to_double()};
    };
    // planet_sys = the ANCHOR BODY's system-frame position (planet, or
    // planet + moon offset when anchored to a moon — T0016).
    SVec3 planet_sys;
    std::array<SVec3, inf::gen::kMaxPlanetSlots> planet_local{};  // anchor-local centers
    struct MoonInstance {
      SVec3 pos;  // anchor-local
      double radius;
      int slot;
      int index;
      bool is_anchor;
    };
    std::vector<MoonInstance> moons_local;
    const auto recompute_bodies = [&] {
      planet_sys =
          eval_pos(system.planets[static_cast<std::size_t>(anchor->slot)].orbit);
      if (anchor->moon >= 0) {
        planet_sys = planet_sys +
                     eval_pos(system.planets[static_cast<std::size_t>(anchor->slot)]
                                  .moons[static_cast<std::size_t>(anchor->moon)]
                                  .orbit);
      }
      for (int slot = 0; slot < inf::gen::kMaxPlanetSlots; ++slot) {
        const auto& entry = system.planets[static_cast<std::size_t>(slot)];
        if (entry.occupied) {
          planet_local[static_cast<std::size_t>(slot)] = eval_pos(entry.orbit) - planet_sys;
        }
      }
      moons_local.clear();
      for (int slot = 0; slot < inf::gen::kMaxPlanetSlots; ++slot) {
        const auto& entry = system.planets[static_cast<std::size_t>(slot)];
        if (!entry.occupied) {
          continue;
        }
        for (std::size_t mi = 0; mi < entry.moons.size(); ++mi) {
          MoonInstance moon;
          moon.pos = planet_local[static_cast<std::size_t>(slot)] +
                     eval_pos(entry.moons[mi].orbit);
          moon.radius = entry.moons[mi].phys.radius_m.to_double();
          moon.slot = slot;
          moon.index = static_cast<int>(mi);
          moon.is_anchor = slot == anchor->slot && static_cast<int>(mi) == anchor->moon;
          moons_local.push_back(moon);
        }
      }
    };
    recompute_bodies();

    // --- closest body (uniform-planet rule; moons count too, T0016) -----
    // Whichever body is closest by surface gap governs the speed limit,
    // the flight zone, and landing.
    struct ClosestBody {
      int slot;
      int moon;  // -1 = the planet itself
      double gap;
      SVec3 center;
      double radius;
      double atmosphere;
    };
    const auto closest_body = [&]() {
      const SVec3 at = player.position();
      ClosestBody best;
      best.slot = anchor->slot;
      best.moon = anchor->moon;
      best.gap = inf::sim::length(at) - anchor->radius;
      best.center = SVec3{0.0, 0.0, 0.0};
      best.radius = anchor->radius;
      best.atmosphere = anchor->planet.atmosphere_height_m.to_double();
      for (int slot = 0; slot < inf::gen::kMaxPlanetSlots; ++slot) {
        const auto& entry = system.planets[static_cast<std::size_t>(slot)];
        if (!entry.occupied || (slot == anchor->slot && anchor->moon < 0)) {
          continue;
        }
        const double radius = entry.phys.radius_m.to_double();
        const double gap =
            inf::sim::length(planet_local[static_cast<std::size_t>(slot)] - at) - radius;
        if (gap < best.gap) {
          best = ClosestBody{slot, -1, gap, planet_local[static_cast<std::size_t>(slot)],
                             radius, entry.phys.atmosphere.height_m.to_double()};
        }
      }
      for (const MoonInstance& moon : moons_local) {
        if (moon.is_anchor) {
          continue;
        }
        const double gap = inf::sim::length(moon.pos - at) - moon.radius;
        if (gap < best.gap) {
          best = ClosestBody{moon.slot, moon.index, gap, moon.pos, moon.radius, 0.0};
        }
      }
      return best;
    };

    // --- anchor switching (T0014): re-anchor to the closest planet ------
    // once it is decisively closer than the current one. The altitude
    // governor then handles approach braking on its own.
    if (!flight_active && map_phase == MapPhase::Off &&
        player.mode() == inf::sim::PlayerMode::Flight) {
      const SVec3 at = player.position();
      const double anchor_gap = inf::sim::length(at) - anchor->radius;
      const ClosestBody candidate = closest_body();
      const bool is_current =
          candidate.slot == anchor->slot && candidate.moon == anchor->moon;
      // Giants stay in closest_body (the governor must brake for them)
      // but never become the anchor: there is no surface to anchor to.
      const bool candidate_landable =
          candidate.moon >= 0 ||
          system.planets[static_cast<std::size_t>(candidate.slot)].landable;
      if (!is_current && candidate_landable && candidate.gap < anchor_gap * 0.5) {
        save_anchor_edits(*anchor);
        for (auto& [addr, chunk] : loaded) {
          rhi->destroy_mesh(chunk.mesh_id);
        }
        loaded.clear();
        pending_ready.clear();  // old anchor's frames are meaningless now
        const SVec3 new_pos = at - candidate.center;
        if (anchor && anchor->civ) { inf::app::release_civ_meshes(anchor->civ.get(), rhi.get()); }
        anchor = make_anchor(*seed, seed_text, system, current_cell, candidate.slot,
                             candidate.moon, std::nullopt, nullptr, &civ_setup_now);
        player.rebase(*anchor->effective, new_pos);
        rebuild_sea();
        hud = std::make_unique<inf::app::Hud>(rhi.get(), anchor->field.get(), anchor->planet);
        recompute_bodies();
        const std::string& name =
            candidate.moon >= 0
                ? moon_names[static_cast<std::size_t>(candidate.slot)]
                            [static_cast<std::size_t>(candidate.moon)]
                : slot_names[static_cast<std::size_t>(candidate.slot)];
        const std::string moon_suffix =
            candidate.moon >= 0 ? " moon " + std::to_string(candidate.moon) : std::string();
        std::printf("anchor: %s (slot %d%s, %s %s, radius %.0f km)\n", name.c_str(),
                    candidate.slot, moon_suffix.c_str(),
                    candidate.moon >= 0
                        ? inf::gen::to_string(anchor->planet.type)
                        : body_type_label(
                              system.planets[static_cast<std::size_t>(candidate.slot)]),
                    candidate.moon >= 0 ? "moon" : "planet", anchor->radius / 1000.0);
      }
    }

    // --- J: interstellar jump (T0017 WP6) -------------------------------
    // Tap J: cone-search along the nose (20 ly, ~15 deg) and arm the
    // nearest system; tap again to cycle. Turning the ship clears the
    // selection. HOLD J (0.75 s) to jump — a stray tap must never fling
    // anyone 20 light-years.
    const SVec3 jump_observer = galactic_pos + planet_sys + player.position();
    bool j_down = glfwGetKey(window, GLFW_KEY_J) == GLFW_PRESS;
    bool jump_confirm_scripted = false;
    if (script_jump) {
      script_jump = false;
      j_down = true;               // acts as a fresh J press...
      j_was_down = false;
      jump_confirm_scripted = true;  // ...that also confirms instantly
    }
    if (!flight_active && map_phase == MapPhase::Off &&
        player.mode() == inf::sim::PlayerMode::Flight && jump_timer <= 0.0) {
      const SVec3 fwd = inf::sim::normalize(player.forward());
      if (jump_index >= 0 && inf::sim::dot(fwd, jump_sel_forward) < 0.9848) {
        jump_index = -1;  // turned away (> ~10 deg): selection cleared
      }
      if (j_down && !j_was_down) {
        if (jump_index < 0) {
          jump_candidates.clear();
          std::vector<inf::gen::GalaxyOctree::CellId> cells;
          galaxy_octree.systems_in_ball(
              inf::gen::Dir3{inf::det::Real(jump_observer.x),
                             inf::det::Real(jump_observer.y),
                             inf::det::Real(jump_observer.z)},
              inf::det::Real(20.0 * inf::gen::kLightYearM), 512, &cells);
          for (const auto& cell : cells) {
            const inf::gen::SystemCell sys_cell{cell.x, cell.y, cell.z, cell.level};
            if (sys_cell == current_cell) {
              continue;
            }
            const inf::gen::Dir3 p = galaxy_octree.system_position_m(cell);
            const SVec3 pos{p.x.to_double(), p.y.to_double(), p.z.to_double()};
            const SVec3 rel = pos - jump_observer;
            const double dist = inf::sim::length(rel);
            if (dist < 0.01 * inf::gen::kLightYearM) {
              continue;
            }
            if (inf::sim::dot(fwd, rel * (1.0 / dist)) < 0.9659) {  // cos 15 deg
              continue;
            }
            jump_candidates.push_back({sys_cell, pos, dist / inf::gen::kLightYearM});
          }
          std::sort(jump_candidates.begin(), jump_candidates.end(),
                    [](const JumpCandidate& a, const JumpCandidate& b) {
                      return a.dist_ly < b.dist_ly;
                    });
          if (!jump_candidates.empty()) {
            jump_index = 0;
            jump_sel_forward = fwd;
          }
        } else {
          jump_index = (jump_index + 1) % static_cast<int>(jump_candidates.size());
        }
        if (jump_index >= 0) {
          jump_sel_name = inf::gen::body_display_name(inf::gen::system_key_for(
              *seed, jump_candidates[static_cast<std::size_t>(jump_index)].cell));
        }
      }
      if (j_down && jump_index >= 0) {
        j_hold += dt;
        if (jump_confirm_scripted) {
          j_hold = 1.0;
        }
        if (j_hold >= 0.75) {
          jump_target = jump_candidates[static_cast<std::size_t>(jump_index)];
          jump_timer = 2.5;
          jump_swapped = false;
          jump_index = -1;
          j_hold = 0.0;
          std::printf("jump: engaging -> %s (%.2f ly)\n", jump_sel_name.c_str(),
                      jump_target.dist_ly);
        }
      } else if (!j_down) {
        j_hold = 0.0;
      }
    }
    j_was_down = j_down;

    if (jump_timer > 0.0) {
      jump_timer -= dt;
      if (!jump_swapped && jump_timer <= 1.25) {
        // The atomic swap, mid-transition: system and body frame change
        // together while the player is suspended (the map-mode lesson —
        // never interpolate across a frame change).
        jump_swapped = true;
        // Attitude survives the jump: the ship keeps looking exactly
        // where it looked (all system frames are galaxy-aligned, so the
        // vectors carry over verbatim) — arrival feels like translation,
        // not a cut.
        const SVec3 keep_fwd = player.forward();
        const SVec3 keep_up = player.up();
        save_anchor_edits(*anchor);
        for (auto& [addr, chunk] : loaded) {
          rhi->destroy_mesh(chunk.mesh_id);
        }
        loaded.clear();
        pending_ready.clear();
        stop_bake_worker();
        for (auto& [tex_key, tex] : body_textures) {
          rhi->destroy_planet_texture(tex.handle);
        }
        body_textures.clear();
        current_cell = jump_target.cell;
        system = generate_system_at(current_cell);
        refresh_civ(current_cell, civ_now(world_clock.now()));
        const int arrival_slot = inf::gen::default_landable_slot(system);
        if (anchor && anchor->civ) { inf::app::release_civ_meshes(anchor->civ.get(), rhi.get()); }
        anchor = make_anchor(*seed, seed_text, system, current_cell, arrival_slot, -1,
                             std::nullopt, nullptr, &civ_setup_now);
        galactic_pos = jump_target.pos_gal;
        // Arrival point: on the approach side of the system (the ship
        // was flying toward this star), at a radius that puts ~2/3 of
        // the planets sunward of it and ~1/3 outside.
        std::vector<double> orbit_radii;
        for (int slot = 0; slot < inf::gen::kMaxPlanetSlots; ++slot) {
          const auto& entry = system.planets[static_cast<std::size_t>(slot)];
          if (entry.occupied) {
            orbit_radii.push_back(entry.orbit.a_m.to_double());
          }
        }
        std::sort(orbit_radii.begin(), orbit_radii.end());
        const std::size_t n_orbits = orbit_radii.size();
        const std::size_t split = (2 * n_orbits + 2) / 3;  // ceil(2n/3)
        const double arrive_r =
            split >= n_orbits
                ? orbit_radii.back() * 1.25
                : 0.5 * (orbit_radii[split - 1] + orbit_radii[split]);
        const SVec3 ship_sys = keep_fwd * (-arrive_r);  // star dead ahead
        const auto& arrival_orbit =
            system.planets[static_cast<std::size_t>(arrival_slot)].orbit;
        const auto arrival_pv = inf::core::Ephemeris::evaluate(arrival_orbit, now);
        const SVec3 arrival_planet_sys{arrival_pv.x.to_double(),
                                       arrival_pv.y.to_double(),
                                       arrival_pv.z.to_double()};
        player.rebase(*anchor->effective, ship_sys - arrival_planet_sys);
        player.set_attitude(keep_fwd, keep_up);
        rebuild_sea();
        hud = std::make_unique<inf::app::Hud>(rhi.get(), anchor->field.get(),
                                              anchor->planet);
        rebuild_system_ui();
        recompute_bodies();
        start_bake_worker(system, current_cell);
        // The spatial sky follows this position immediately; request its stars.
        stellar_stream.request(
            galactic_pos, {},
            std::clamp(
                9.05 + 2.5 * std::log10(std::max(.01f, rhi->exposure()) / 35.0),
                -4.0, 8.3));
        std::printf("jump: arrived at %s — %s (slot %d, %s, radius %.0f km)\n",
                    jump_sel_name.c_str(),
                    slot_names[static_cast<std::size_t>(arrival_slot)].c_str(),
                    arrival_slot,
                    body_type_label(
                        system.planets[static_cast<std::size_t>(arrival_slot)]),
                    anchor->radius / 1000.0);
      }
    }

    // Feed the closest body to the player (speed governor, zone, the
    // E-landing gate). Moons and planets are treated identically.
    {
      const ClosestBody closest = closest_body();
      inf::sim::NearestBody nearest;
      nearest.center = closest.center;
      nearest.radius_m = closest.radius;
      nearest.atmosphere_m = closest.atmosphere;
      nearest.is_anchor = closest.slot == anchor->slot && closest.moon == anchor->moon;
      player.set_nearest_body(nearest);
    }

    // Stars (primary + companions) + moons in the anchor frame (for
    // rendering, lighting, radar, keep-out).
    const SVec3 star_local = SVec3{0.0, 0.0, 0.0} - planet_sys;
    const double star_radius = system.star.radius_solar.to_double() * 6.957e7;
    struct StarInstance {
      SVec3 pos;
      double radius;
      double luminosity;
      float tint[3];
      float phase;
    };
    std::vector<StarInstance> stars_local;
    {
      StarInstance primary;
      primary.pos = star_local;
      primary.radius = star_radius;
      primary.luminosity = system.star.luminosity_solar.to_double();
      star_tint(system.star.temperature_k.to_double(), primary.tint);
      primary.phase = 0.618f;
      stars_local.push_back(primary);
      for (std::size_t ci = 0; ci < system.companions.size(); ++ci) {
        const auto& companion = system.companions[ci];
        StarInstance star;
        star.pos = eval_pos(companion.orbit) - planet_sys;
        star.radius = companion.phys.radius_solar.to_double() * 6.957e7;
        star.luminosity = companion.phys.luminosity_solar.to_double();
        star_tint(companion.phys.temperature_k.to_double(), star.tint);
        star.phase = 0.618f + 0.731f * static_cast<float>(ci + 1);
        stars_local.push_back(star);
      }
    }
    // Keep-out spheres for everything that has no terrain field: the
    // star, non-anchor planets, and non-anchor moons (fly-through is not
    // a thing; the anchor's real ground is handled by the flight clamp).
    if (map_phase == MapPhase::Off) {
      for (const StarInstance& star : stars_local) {
        player.push_out(star.pos, star.radius * 1.6);
      }
      for (int slot = 0; slot < inf::gen::kMaxPlanetSlots; ++slot) {
        const auto& entry = system.planets[static_cast<std::size_t>(slot)];
        if (entry.occupied && !(slot == anchor->slot && anchor->moon < 0)) {
          // Giants get a fatter keep-out: there is no surface under the
          // cloud tops, so the ship stops a little above them.
          const double margin = entry.landable ? 1.02 : 1.03;
          const double clearance = entry.landable ? 5.0 : 30.0;
          player.push_out(planet_local[static_cast<std::size_t>(slot)],
                          entry.phys.radius_m.to_double() * margin + clearance);
        }
      }
      for (const MoonInstance& moon : moons_local) {
        if (!moon.is_anchor) {
          player.push_out(moon.pos, moon.radius * 1.02 + 5.0);
        }
      }
    }

    if (flight_requested && map_phase == MapPhase::Off &&
        player.mode() == inf::sim::PlayerMode::Flight && jump_timer <= 0.0) {
      flight_start_local = player.position();
      flight_start_planet = planet_sys;
      galaxy_flight.start(
          galaxy_params,
          {galactic_pos + planet_sys + player.position(), player.forward(),
           player.up(), player.forward() * player.speed()});
      flight_epoch = FlightClock::now();
      const auto departure_body = closest_body();
      galaxy_flight.clear_departure_body(
          player.position() - departure_body.center, departure_body.radius);
      flight_elapsed = 0;
      flight_active = true;
      if (galaxy_profile) {
        flight_csv.close();
        flight_csv.open(galaxy_profile);
        flight_csv << std::setprecision(17);
        flight_csv << "# adapter=" << rhi->adapter_info()
                   << ";duration_s=120;center_s=" << galaxy_flight.center_time()
                   << ";budget_ms=41.6667;prediction_s=1;volume_size="
                   << sky_volume.size << '\n';
        flight_csv
            << "elapsed_s,frame_ms,catalog_ms,presented,width,height,x_"
               "m,y_m,z_m,resources_ms,world_ms,stars_ms,draw_ms,render_ms\n";
      }
    }
    if (flight_active) {
      flight_elapsed =
          flight_requested
              ? 0.0
              : std::chrono::duration<double>(FlightClock::now() - flight_epoch)
                    .count();
      if ((!flight_requested && flight_toggle) || esc_pressed)
        galaxy_flight.stop(flight_elapsed);
      flight_elapsed = std::min(flight_elapsed, galaxy_flight.end_time());
      const auto pose = galaxy_flight.sample(flight_elapsed);
      player.set_position(flight_start_local +
                          galaxy_flight.displacement(flight_elapsed) +
                          (flight_start_planet - planet_sys));
      player.set_attitude(pose.forward, pose.up);
      player.set_speed(inf::sim::length(pose.velocity));
      flight_final_frame = galaxy_flight.finished(flight_elapsed);
    }

    // --- debug script step (--script) ------------------------------------
    if (script_wait > 0.0) {
      script_wait -= dt;
    }
    while (script_pc < script.size() && script_wait <= 0.0) {
      const ScriptCmd& cmd = script[script_pc];
      ++script_pc;
      const auto arg_d = [&](std::size_t index) {
        return index < cmd.args.size() ? std::strtod(cmd.args[index].c_str(), nullptr) : 0.0;
      };
      const auto aim_at = [&](const SVec3& dir) {
        SVec3 up_ref = inf::sim::normalize(player.position());
        if (std::abs(inf::sim::dot(up_ref, dir)) > 0.98) {
          up_ref = SVec3{0.0, 0.0, 1.0};
        }
        player.set_attitude(dir, up_ref);
      };
      if (cmd.op == "pos" && cmd.args.size() >= 3) {
        player.set_position(SVec3{arg_d(0), arg_d(1), arg_d(2)});
      } else if (cmd.op == "possun" && !cmd.args.empty()) {
        // Place on the sun-facing side of the anchor at the given
        // altitude (day-side captures).
        player.set_position(inf::sim::normalize(star_local) *
                            (anchor->radius + arg_d(0)));
      } else if (cmd.op == "possun2" && cmd.args.size() >= 2) {
        // Like possun, but rotated <deg> around the planet Z axis — walk
        // the terminator to find day-side land or ocean.
        const SVec3 sun = inf::sim::normalize(star_local);
        const double rad = arg_d(1) * 3.14159265358979323846 / 180.0;
        const double c = std::cos(rad);
        const double s = std::sin(rad);
        const SVec3 dir = inf::sim::normalize(
            SVec3{sun.x * c - sun.y * s, sun.x * s + sun.y * c, sun.z});
        player.set_position(dir * (anchor->radius + arg_d(0)));
      } else if (cmd.op == "posmoon" && cmd.args.size() >= 3) {
        // Place near moon <slot> <index> at <alt> above its surface.
        for (const MoonInstance& moon : moons_local) {
          if (moon.slot == static_cast<int>(arg_d(0)) &&
              moon.index == static_cast<int>(arg_d(1))) {
            const SVec3 out = inf::sim::normalize(
                inf::sim::length(moon.pos) > 1.0 ? moon.pos : SVec3{1.0, 0.0, 0.0});
            player.set_position(moon.pos + out * (moon.radius + arg_d(2)));
            aim_at(inf::sim::normalize(moon.pos - player.position()));
          }
        }
      } else if (cmd.op == "posplanet" && cmd.args.size() >= 2) {
        // Place near planet <slot> at <alt> above its nominal radius.
        const int slot = static_cast<int>(arg_d(0));
        if (slot >= 0 && slot < inf::gen::kMaxPlanetSlots &&
            system.planets[static_cast<std::size_t>(slot)].occupied) {
          const SVec3 center = slot == anchor->slot && anchor->moon < 0
                                   ? SVec3{0.0, 0.0, 0.0}
                                   : planet_local[static_cast<std::size_t>(slot)];
          const double radius =
              system.planets[static_cast<std::size_t>(slot)].phys.radius_m.to_double();
          const SVec3 out = inf::sim::normalize(
              inf::sim::length(center) > 1.0 ? center : SVec3{1.0, 0.0, 0.0});
          player.set_position(center + out * (radius + arg_d(1)));
          aim_at(inf::sim::normalize(center - player.position()));
        }
      } else if (cmd.op == "posnight" && !cmd.args.empty()) {
        // True antisolar point at the given altitude (night-sky captures).
        player.set_position(inf::sim::normalize(star_local) * -1.0 *
                            (anchor->radius + arg_d(0)));
      } else if (cmd.op == "posmoonsun" && cmd.args.size() >= 3) {
        // Like posmoon, but on the moon's SUNWARD side (day-side
        // captures; moons move too fast for precomputed positions).
        for (const MoonInstance& moon : moons_local) {
          if (moon.slot == static_cast<int>(arg_d(0)) &&
              moon.index == static_cast<int>(arg_d(1))) {
            const SVec3 to_sun = inf::sim::normalize(star_local - moon.pos);
            player.set_position(moon.pos + to_sun * (moon.radius + arg_d(2)));
            aim_at(inf::sim::normalize(moon.pos - player.position()));
          }
        }
      } else if (cmd.op == "land") {
        script_land = true;
      } else if (cmd.op == "exposure-lock" && !cmd.args.empty()) {
        script_exposure_locked = arg_d(0) != 0.0;
      } else if (cmd.op == "attitude" && cmd.args.size() >= 6) {
        player.set_attitude({arg_d(0), arg_d(1), arg_d(2)},
                            {arg_d(3), arg_d(4), arg_d(5)});
      } else if (cmd.op == "pose") {
        const auto at = galactic_pos + planet_sys + player.position();
        const auto f = player.forward();
        const auto up = player.up();
        std::printf(
            "capture pose: %.17g %.17g %.17g %.17g %.17g %.17g %.17g %.17g "
            "%.17g speed=%.17g\n",
            at.x, at.y, at.z, f.x, f.y, f.z, up.x, up.y, up.z, player.speed());
      } else if (cmd.op == "galaxy") {
        galaxy_demo = true;
      } else if (cmd.op == "galaxy-stop") {
        if (flight_active) galaxy_flight.stop(flight_elapsed);
      } else if (cmd.op == "galactic" && cmd.args.size() >= 3) {
        const SVec3 target{arg_d(0), arg_d(1), arg_d(2)};
        player.set_position((target - galactic_pos) - planet_sys);
        stellar_stream.request(target, {});
      } else if (cmd.op == "map") {
        script_map = true;
      } else if (cmd.op == "jump") {
        script_jump = true;
      } else if (cmd.op == "aim" && !cmd.args.empty()) {
        if (cmd.args[0] == "sun") {
          aim_at(inf::sim::normalize(star_local - player.position()));
        } else if (cmd.args[0] == "planet" && cmd.args.size() >= 2) {
          const int slot = static_cast<int>(arg_d(1));
          if (slot >= 0 && slot < inf::gen::kMaxPlanetSlots) {
            const SVec3 center = slot == anchor->slot
                                     ? SVec3{0.0, 0.0, 0.0}
                                     : planet_local[static_cast<std::size_t>(slot)];
            aim_at(inf::sim::normalize(center - player.position()));
          }
        } else if (cmd.args[0] == "dir" && cmd.args.size() >= 4) {
          aim_at(inf::sim::normalize(SVec3{arg_d(1), arg_d(2), arg_d(3)}));
        }
      } else if (cmd.op == "aimhorizon" && !cmd.args.empty()) {
        // Frame the sky over the local horizon: look along the galactic
        // plane, dipped <deg> toward the planet — the limb sits at the
        // bottom of frame with the band above it (capture composition).
        const SVec3 up_ref = inf::sim::normalize(player.position());
        SVec3 tangent = inf::sim::cross(SVec3{0.0, 0.0, 1.0}, up_ref);
        if (inf::sim::length(tangent) < 0.05) {
          tangent = SVec3{1.0, 0.0, 0.0};
        }
        tangent = inf::sim::normalize(tangent);
        // Optional second arg: yaw around the local vertical, so the
        // composition can sweep the horizon toward the band or a moon.
        if (cmd.args.size() >= 2) {
          const double yaw = arg_d(1) * 3.14159265358979323846 / 180.0;
          const SVec3 side = inf::sim::cross(up_ref, tangent);
          tangent = inf::sim::normalize(tangent * std::cos(yaw) + side * std::sin(yaw));
        }
        const double dip = arg_d(0) * 3.14159265358979323846 / 180.0;
        const SVec3 fwd = inf::sim::normalize(tangent * std::cos(dip) -
                                              up_ref * std::sin(dip));
        player.set_attitude(fwd, up_ref);
      } else if (cmd.op == "hud" && !cmd.args.empty()) {
        script_hud = arg_d(0) != 0.0;  // clean-frame captures
      } else if (cmd.op == "speed" && !cmd.args.empty()) {
        player.set_speed(arg_d(0));
      } else if (cmd.op == "thrust" && !cmd.args.empty()) {
        script_thrust = arg_d(0) != 0.0;
      } else if (cmd.op == "wait" && !cmd.args.empty()) {
        script_wait = arg_d(0);
      } else if (cmd.op == "capture" && !cmd.args.empty()) {
        rhi->request_capture(cmd.args[0]);
      } else if (cmd.op == "record" && cmd.args.size() >= 2) {
        std::error_code ec;
        std::filesystem::create_directories(cmd.args[0], ec);
        rec_dir_current = cmd.args[0];
        rhi->trigger_recording(rec_dir_current, arg_d(1));
      } else if (cmd.op == "quit") {
        glfwSetWindowShouldClose(window, GLFW_TRUE);
      } else {
        std::fprintf(stderr, "script: unknown command '%s'\n", cmd.op.c_str());
      }
    }

    // --- map mode: enter / exit triggers ---------------------------------
    const bool m_down =
        !flight_active && glfwGetKey(window, GLFW_KEY_M) == GLFW_PRESS;
    const bool m_pressed =
        (m_down && !m_was_down) ||
        (!flight_active && ((map_demo && frame == 100) || script_map));
    script_map = false;
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
        // Staged: uploads happen a few per frame below, so a big batch
        // (fast descent, re-anchor) dribbles in over a fraction of a
        // second instead of slamming the whole horizon in one frame.
        pending_ready[event.addr] = event.data;
      } else {
        pending_ready.erase(event.addr);
        const auto it = loaded.find(event.addr);
        if (it != loaded.end()) {
          rhi->destroy_mesh(it->second.mesh_id);
          loaded.erase(it);
        }
      }
    }
    int uploads = 0;
    for (auto it = pending_ready.begin(); it != pending_ready.end() && uploads < 64;) {
      const auto& addr = it->first;
      const auto& data = it->second;
      // Replace any previous mesh for this address (re-mesh on neighbor
      // lod change delivers updated geometry under the same address).
      const auto old = loaded.find(addr);
      if (old != loaded.end()) {
        rhi->destroy_mesh(old->second.mesh_id);
        loaded.erase(old);
      }
      if (!data->mesh.vertices.empty()) {
        // material/v2 (T0019): the worker already classified every vertex
        // (8-float layout), so this is a straight upload.
        LoadedChunk chunk;
        chunk.mesh_id =
            rhi->create_mesh_mat(data->mesh.vertices.data(), data->mesh.vertices.size());
        {
          // Bounding sphere from the vertex positions (10 floats each).
          float lo[3] = {1e30f, 1e30f, 1e30f};
          float hi[3] = {-1e30f, -1e30f, -1e30f};
          const std::vector<float>& vv = data->mesh.vertices;
          for (std::size_t v = 0; v + 9 < vv.size(); v += 10) {
            for (int c = 0; c < 3; ++c) {
              lo[c] = std::min(lo[c], vv[v + c]);
              hi[c] = std::max(hi[c], vv[v + c]);
            }
          }
          float r2 = 0.0f;
          for (int c = 0; c < 3; ++c) {
            chunk.centre[c] = 0.5f * (lo[c] + hi[c]);
            r2 += 0.25f * (hi[c] - lo[c]) * (hi[c] - lo[c]);
          }
          chunk.radius = std::sqrt(r2);
        }
        ++chunk_uploads;
        std::memcpy(chunk.palette, data->mesh.palette, sizeof(chunk.palette));
        chunk.origin =
            RVec3{data->mesh.origin[0], data->mesh.origin[1], data->mesh.origin[2]};
        loaded[addr] = chunk;
      }
      it = pending_ready.erase(it);
      ++uploads;
    }

    const auto world_finished = FlightClock::now();
    const double star_visibility = std::clamp(
        8.3 + 2.5 * std::log10(std::max(.01f, rhi->exposure()) / 35.0), -4.0,
        8.3);
    const SVec3 observer_gal = galactic_pos + planet_sys + player.position();
    if (std::chrono::duration<double>(FlightClock::now() - star_requested)
            .count() >= 0.05) {
      const SVec3 velocity = flight_active
                                 ? galaxy_flight.sample(flight_elapsed).velocity
                                 : player.forward() * player.speed();
      stellar_stream.request(observer_gal, velocity,
                             std::min(8.3, star_visibility + 0.75));
      star_requested = FlightClock::now();
    }
    if (auto catalog = stellar_stream.take_ready()) {
      const auto replacement =
          catalog->vertices.empty()
              ? 0
              : rhi->create_mesh_mat(catalog->vertices.data(),
                                     catalog->vertices.size());
      if (replacement || catalog->vertices.empty()) {
        if (star_field_mesh) rhi->destroy_mesh(star_field_mesh);
        star_field_mesh = replacement;
        star_catalog = std::move(*catalog);
        star_catalog.vertices.clear();
      }
    }

    const auto stars_finished = FlightClock::now();
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
    // Clear color is CONSTANT deep space — it must match the sky dome's
    // fade-out color exactly, or leaving the atmosphere pops in
    // brightness (the dome renders the in-atmosphere sky per pixel).
    // T0018: genuinely dark — the HDR eye adapts; a bright fake floor
    // exposed to a flat wash. Matches the dome's space floor.
    const float sky[3] = {0.00004f, 0.00005f, 0.0001f};
    double atmosphere = anchor->planet.atmosphere_height_m.to_double();
    float palette[3] = {0.05f, 0.06f, 0.12f};
    switch (anchor->planet.type) {
      case inf::gen::PlanetType::EarthLike: palette[0] = 0.45f; palette[1] = 0.65f; palette[2] = 0.95f; break;
      case inf::gen::PlanetType::Desert: palette[0] = 0.78f; palette[1] = 0.58f; palette[2] = 0.42f; break;
      case inf::gen::PlanetType::Ice: palette[0] = 0.62f; palette[1] = 0.74f; palette[2] = 0.92f; break;
      case inf::gen::PlanetType::Barren: atmosphere = 0.0; break;
    }
    const RVec3 camera_pos = to_render(cam_pos_local);
    const RVec3 cam_forward = to_render(cam_fwd_v);
    const RVec3 cam_up = to_render(cam_up_sv);
    const double altitude = inf::render::length(camera_pos) - anchor->radius;
    // From orbit the chunk terrain is hidden entirely: at that range its
    // coarse LOD only produces lattice artifacts (false land/water
    // diamonds over the ocean), and the lit ocean/terrain impostor
    // carries the planet's look instead. Threshold tightened 0.7R ->
    // 0.35R (2026-08-31): with the more aggressive split factor the
    // terrain is properly refined by the time it appears.
    const bool show_surface = altitude < 0.35 * anchor->radius;
    double farthest_star = inf::sim::length(star_local - cam_pos_local);
    for (const StarInstance& star : stars_local) {
      farthest_star = std::max(farthest_star, inf::sim::length(star.pos - cam_pos_local));
    }
    const double far_z =
        std::max({10'000.0, std::abs(altitude) * 4.0 + 2.5 * anchor->radius,
                  farthest_star * 2.5});
    // At map framing distance the near plane scales up with altitude so
    // the sparse far-field scene keeps usable depth precision.
    const double near_z = std::clamp(std::abs(altitude) * 1e-4, 0.3, 1e8);
    // REVERSED-Z (2026-08-31): near/far swapped + Greater depth test in
    // the RHI. With a classic [0,1] mapping, bodies at system distances
    // (1e9+ m) quantized to depth 1.0 in f32 and randomly failed against
    // the clear value — the sun's corona billboard flickered in and out.
    // Reversed-Z puts float precision at the far range where it's needed.
    const Mat4 projection = inf::render::perspective(kFovY, input.aspect, far_z, near_z);
    const Mat4 view = inf::render::look_dir(cam_forward, cam_up);
    const Mat4 view_projection = inf::render::mul(projection, view);

    // Star renderer: animated photosphere sphere (mode 1) plus an
    // additive corona billboard (mode 2), camera-aligned; shared by the
    // flight scene and map mode. Distant stars keep a minimum apparent
    // size so they read as suns, not specks.
    const double px_world_all = 2.0 * std::tan(kFovY * 0.5) / state.height;
    const auto draw_star = [&](const StarInstance& star, double min_px) {
      const double dist = inf::sim::length(star.pos - cam_pos_local);
      if (dist < star.radius * 1.05) {
        return;
      }
      const double size = std::max(star.radius, dist * px_world_all * min_px * 0.5);
      const double apparent_px = size / (dist * px_world_all);
      // Preserve integrated light below the minimum raster footprint. Without
      // this factor the departure sun remains a glaring disc across the galaxy.
      const double coverage = (star.radius / size) * (star.radius / size);
      // Distant suns get a disproportionally larger, brighter corona so
      // they stay spectacular as they shrink toward a point.
      const double far_boost = std::clamp(60.0 / std::max(apparent_px, 1.0), 1.0, 2.6);
      const RVec3 rel = to_render(star.pos) - camera_pos;
      {
        const Mat4 model = inf::render::from_basis(
            RVec3{size, 0.0, 0.0}, RVec3{0.0, size, 0.0}, RVec3{0.0, 0.0, size}, rel);
        const Mat4 mvp = inf::render::mul(view_projection, model);
        inf::render::Rhi::DrawItem item;
        item.mesh = star_mesh;
        std::memcpy(item.mvp, mvp.m, sizeof(mvp.m));
        item.color[0] = star.tint[0] * static_cast<float>(coverage);
        item.color[1] = star.tint[1] * static_cast<float>(coverage);
        item.color[2] = star.tint[2] * static_cast<float>(coverage);
        item.color[3] = 1.0f;
        item.mode = 1;
        const RVec3 view_dir = inf::render::normalize(rel);
        item.aux[0] = static_cast<float>(view_dir.x);
        item.aux[1] = static_cast<float>(view_dir.y);
        item.aux[2] = static_cast<float>(view_dir.z);
        item.aux[3] = star.phase;
        item.extra[0] = 0.85f;  // sunspot amount
        items.push_back(item);
      }
      {
        // The billboard sits 3% closer to the camera than the star: at
        // system distances the depth buffer cannot separate the quad
        // from the photosphere sphere, and the z-fight showed as corona
        // flicker around the disc.
        const double glow = size * 6.0 * far_boost;
        const RVec3 rel_glow = rel * 0.97;
        const RVec3 bill_right =
            inf::render::normalize(inf::render::cross(cam_forward, cam_up));
        const Mat4 model = inf::render::from_basis(bill_right * glow, cam_up * glow,
                                                   cam_forward * glow, rel_glow);
        const Mat4 mvp = inf::render::mul(view_projection, model);
        inf::render::Rhi::DrawItem item;
        item.mesh = glow_mesh;
        std::memcpy(item.mvp, mvp.m, sizeof(mvp.m));
        item.color[0] = star.tint[0] * static_cast<float>(coverage);
        item.color[1] = star.tint[1] * static_cast<float>(coverage);
        item.color[2] = star.tint[2] * static_cast<float>(coverage);
        item.color[3] = 1.0f;
        item.mode = 2;
        item.aux[3] = star.phase;
        item.extra[0] = static_cast<float>(0.8 + 0.5 * far_boost);  // glow intensity
        item.extra[1] = static_cast<float>(size / glow);            // silhouette radius
        // Diffraction spikes only while the star is small on screen: a
        // far sun sparkles, a near sun is a raging disc.
        item.extra[2] =
            static_cast<float>(std::clamp(1.0 - apparent_px / 60.0, 0.0, 1.0)) * 0.9f;
        items.push_back(item);
      }
    };

    items.clear();
    city_ranges.clear();
    items.reserve(loaded.size() + player.beams().size() + 8);

    // --- sky dome (mode 4): analytic atmosphere while inside the band ---
    // Fullscreen quad at far depth; the shader builds the per-pixel view
    // ray from the frame camera basis. Fades itself out toward space via
    // altitude_frac (also passed below in the frame params).
    const double dome_alt_frac =
        atmosphere > 0.0
            ? (inf::sim::length(cam_pos_local) - anchor->radius) / atmosphere
            : 9.0;
    // T0018 WP3: the dome is ALWAYS drawn in flight — in space it carries
    // the spatial deep sky alone (density -> 0 in the shader), in
    // an atmosphere it adds the scattered daylight on top.
    if (map_phase == MapPhase::Off) {
      inf::render::Rhi::DrawItem dome;
      dome.mesh = glow_mesh;
      Mat4 m{};
      m.m[0] = 1.0f;
      m.m[5] = 1.0f;
      m.m[10] = 0.00001f;
      // Reversed-Z: barely above the clear value (0) so the dome renders,
      // but BELOW any real geometry's depth — including stars and corona
      // billboards at 1e9+ m, whose depth is ~1e-10. At the old 5e-5 the
      // dome popped on at the atmosphere boundary and erased the sun.
      m.m[14] = 1e-22f;
      m.m[15] = 1.0f;
      std::memcpy(dome.mvp, m.m, sizeof(m.m));
      dome.mode = 4;
      static const bool no_sky_tex = std::getenv("INF_NOSKY") != nullptr;
      dome.planet_texture = no_sky_tex ? 0 : sky_texture;
      dome.extra[0] = no_sky_tex ? 0.0f : 1.0f;
      const auto sky_eye = (galactic_pos + planet_sys + cam_pos_local) *
                           (1.0 / sky_volume.radius_m);
      dome.aux[0] = static_cast<float>(sky_eye.x);
      dome.aux[1] = static_cast<float>(sky_eye.y);
      dome.aux[2] = static_cast<float>(sky_eye.z);
      const auto& dust_orbit =
          system.planets[static_cast<std::size_t>(anchor->slot)].orbit;
      const double inclination = dust_orbit.i_rad.to_double();
      const double ascending = dust_orbit.raan_rad.to_double();
      dome.color[0] =
          static_cast<float>(std::sin(ascending) * std::sin(inclination));
      dome.color[1] =
          static_cast<float>(-std::cos(ascending) * std::sin(inclination));
      dome.color[2] = static_cast<float>(std::cos(inclination));
      const double dust_scale =
          dust_orbit.a_m.to_double() /
          std::max(1.0, inf::sim::length(planet_sys + cam_pos_local));
      dome.color[3] =
          static_cast<float>(std::min(1.0, dust_scale * dust_scale));
      items.push_back(dome);
      inf::app::draw_distant_galaxies(neighbour_galaxies,
                                      galactic_pos + planet_sys + cam_pos_local,
                                      view_projection, glow_mesh, items);
      // Stable spatial stars: current-eye parallax and photometry in the
      // shader, with ordinary scene geometry providing occlusion.
      static const bool no_stars = std::getenv("INF_NOSTARS") != nullptr;
      if (star_field_mesh != 0 && !no_stars) {
        inf::render::Rhi::DrawItem stars_item;
        stars_item.mesh = star_field_mesh;
        std::memcpy(stars_item.mvp, view_projection.m, sizeof(view_projection.m));
        stars_item.mode = 7;
        stars_item.color[3] = static_cast<float>(star_visibility);
        const SVec3 offset = (star_catalog.origin -
                              (galactic_pos + planet_sys + cam_pos_local)) *
                             (1.0 / inf::gen::kLightYearM);
        stars_item.aux[0] = static_cast<float>(offset.x);
        stars_item.aux[1] = static_cast<float>(offset.y);
        stars_item.aux[2] = static_cast<float>(offset.z);
        const double star_half_px = 5.0;
        stars_item.extra[0] = static_cast<float>(2.0 * star_half_px / state.width);
        stars_item.extra[1] = static_cast<float>(2.0 * star_half_px / state.height);
        items.push_back(stars_item);
      }
    }
    inf::app::CityDrawStats city_stats;
    inf::app::CityDrawOptions city_options;
    city_options.fine_ranges = !no_occlusion;
    city_options.shadow_far_lod = shadow_far_lod;
    // T0020: settlement mass models of the anchor body.
    if (show_surface && anchor->civ != nullptr && !city_active) {
      anchor->civ->city_enabled = !no_city;
      inf::app::draw_civ_sites(anchor->civ.get(), rhi.get(), *anchor->field, to_render(player.position()),
                               camera_pos, view_projection, city_options, &items, &city_ranges, &city_stats);
    }
    // T0021: city scenes through the city pipeline.
    if (show_surface && city_active) {
      inf::app::draw_city_upload(city_upload, camera_pos, view_projection, city_options, &items, &city_ranges, &city_stats);
    }
    // Site beacons: a light beam over every settlement, its height and
    // colour by tier, its width a few pixels at any distance so it reads
    // from orbit as well as from the street. Additive, so it never hides
    // anything; the planet occludes it.
    if (beacons && show_surface && anchor->civ != nullptr && anchor->civ->sites != nullptr) {
      const double px_world = 2.0 * std::tan(kFovY * 0.5) / state.height;
      const double R = anchor->radius;
      const double night = beacon_night;  // last frame's night factor
      // Outpost, hamlet, village green; town cyan; city blue; metropolis
      // orange; capital gold.
      static const float kTierColour[8][3] = {{0.35f, 0.9f, 0.45f}, {0.35f, 0.9f, 0.45f}, {0.4f, 0.95f, 0.5f},
                                              {0.3f, 0.95f, 0.75f}, {0.35f, 0.85f, 1.0f}, {0.25f, 0.55f, 1.0f},
                                              {1.0f, 0.55f, 0.2f}, {1.0f, 0.8f, 0.25f}};
      static const double kTierHeightM[8] = {600.0, 800.0, 1200.0, 1600.0, 2200.0, 3000.0, 4000.0, 5000.0};
      for (const inf::gen::Site& site : anchor->civ->sites->sites()) {
        const int tier = std::clamp(site.tier, 0, 7);
        const RVec3 up{site.frame.up.x.to_double(), site.frame.up.y.to_double(), site.frame.up.z.to_double()};
        const RVec3 east{site.frame.east.x.to_double(), site.frame.east.y.to_double(), site.frame.east.z.to_double()};
        const RVec3 north{site.frame.north.x.to_double(), site.frame.north.y.to_double(), site.frame.north.z.to_double()};
        const RVec3 base = up * (R + site.datum_m);
        const RVec3 rel = base - camera_pos;
        const double dist = inf::render::length(rel);
        if (dist > 2.5e6) continue;
        const double height = kTierHeightM[tier];
        const double width = std::max(5.0, dist * px_world * 2.5);
        const Mat4 model = inf::render::from_basis(east * width, up * height, north * width, rel);
        const Mat4 mvp = inf::render::mul(view_projection, model);
        inf::render::Rhi::DrawItem item;
        item.mesh = beacon_mesh;
        std::memcpy(item.mvp, mvp.m, sizeof(mvp.m));
        item.color[0] = kTierColour[tier][0];
        item.color[1] = kTierColour[tier][1];
        item.color[2] = kTierColour[tier][2];
        item.color[3] = 1.0f;
        item.extra[0] = static_cast<float>(0.2 * (1.0 - 0.985 * night) * (site.capital ? 1.6 : 1.0));
        item.extra[3] = 9.0f;
        items.push_back(item);
      }
    }
    for (const auto& [addr, chunk] : loaded) {
      if (!show_surface) {
        break;  // impostor-only from high orbit
      }
      inf::render::Rhi::DrawItem item;
      item.mesh = chunk.mesh_id;
      item.shadow_caster = true;  // T0021: terrain shadows the city and itself
      const RVec3 translation = chunk.origin - camera_pos;
      item.bounds[0] = static_cast<float>(translation.x) + chunk.centre[0];
      item.bounds[1] = static_cast<float>(translation.y) + chunk.centre[1];
      item.bounds[2] = static_cast<float>(translation.z) + chunk.centre[2];
      item.bounds[3] = chunk.radius;
      const Mat4 model = inf::render::translate(translation);
      const Mat4 mvp = inf::render::mul(view_projection, model);
      std::memcpy(item.mvp, mvp.m, sizeof(mvp.m));
      // Lit items carry their translation for the orbit normal blend.
      item.aux[0] = static_cast<float>(translation.x);
      item.aux[1] = static_cast<float>(translation.y);
      item.aux[2] = static_cast<float>(translation.z);
      // Texture-space origin: the chunk origin modulo the tiling period,
      // in double, so chunk-local f32 coordinates tile seamlessly at any
      // planet radius (T0019 WP5).
      constexpr double kTilePeriod = 256.0;
      item.extra[0] = static_cast<float>(std::fmod(chunk.origin.x, kTilePeriod));
      item.extra[1] = static_cast<float>(std::fmod(chunk.origin.y, kTilePeriod));
      item.extra[2] = static_cast<float>(std::fmod(chunk.origin.z, kTilePeriod));
      std::memcpy(item.material_palette, chunk.palette, sizeof(item.material_palette));
      // Far field: fade into the anchor's baked cube map when it exists.
      if (const auto tex_it = body_textures.find(body_tex_key(anchor->slot, anchor->moon));
          tex_it != body_textures.end()) {
        item.planet_texture = tex_it->second.handle;
        item.aux[3] = 1.0f;
      }
      items.push_back(item);
    }

    // --- system bodies in normal flight (T0014) --------------------------
    // The sun, sibling planets and moons are always in the sky — real
    // scale, with a small minimum apparent size so distant planets stay
    // visible as specks. Map mode draws its own (larger) versions.
    if (map_phase == MapPhase::Off) {
      const double px_world = 2.0 * std::tan(kFovY * 0.5) / state.height;
      const auto draw_ball = [&](const SVec3& pos, double true_radius, double min_px,
                                 float r, float g, float b, std::uint32_t tex_key) {
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
        std::memcpy(item.mvp, mvp.m, sizeof(mvp.m));
        // Textured, displaced impostor once this body's bake landed
        // (T0016); flat color ball until then.
        const auto tex_it = body_textures.find(tex_key);
        if (tex_it != body_textures.end()) {
          item.mesh = impostor_mesh;
          item.mode = 6;
          item.planet_texture = tex_it->second.handle;
          item.extra[0] = tex_it->second.amp_over_radius;
          item.extra[1] = tex_it->second.slope_scale;
          // The body's own sun direction (planet-local == system axes).
          const SVec3 to_sun = inf::sim::normalize(
              (stars_local.empty() ? SVec3{0.0, 0.0, 1.0e12} : stars_local[0].pos) - pos);
          item.aux[0] = static_cast<float>(to_sun.x);
          item.aux[1] = static_cast<float>(to_sun.y);
          item.aux[2] = static_cast<float>(to_sun.z);
        } else {
          item.mesh = body_mesh;
          item.color[0] = r;
          item.color[1] = g;
          item.color[2] = b;
          item.color[3] = 1.0f;
        }
        items.push_back(item);
      };
      for (const StarInstance& star : stars_local) {
        draw_star(star, 7.0);
      }
      for (int slot = 0; slot < inf::gen::kMaxPlanetSlots; ++slot) {
        const auto& entry = system.planets[static_cast<std::size_t>(slot)];
        if (!entry.occupied || (slot == anchor->slot && anchor->moon < 0)) {
          continue;
        }
        float color[3];
        slot_color(slot, color);
        draw_ball(planet_local[static_cast<std::size_t>(slot)],
                  entry.phys.radius_m.to_double(), 3.0, color[0], color[1], color[2],
                  body_tex_key(slot, -1));
      }
      // The static land impostor: from orbit it IS the planet (chunks
      // hidden above 0.35R); through the transition band it backstops
      // late-arriving chunks. BELOW 0.22R it is hidden — its ~40 km
      // lattice interpolates linearly across real km-scale relief, so
      // near the ground its sheets poked ABOVE true terrain as phantom
      // collision-less surfaces (the "fall through the top layer onto
      // stacked panes" bug, 2026-09-01) — UNLESS the chunk set is still
      // sparse: on a fresh anchor (a first moon landing) hiding it left
      // the player descending through visible-nothing onto invisible
      // ground under the dome. A briefly-poking sheet beats a void.
      if (land_mesh != 0 &&
          (altitude > 0.22 * anchor->radius || loaded.size() < 200)) {
        const RVec3 rel = to_render(SVec3{0.0, 0.0, 0.0}) - camera_pos;
        const Mat4 model = inf::render::translate(rel);
        const Mat4 mvp = inf::render::mul(view_projection, model);
        inf::render::Rhi::DrawItem item;
        item.mesh = land_mesh;
        item.prepass = true;  // T0021: occluder for the screen-space passes
        std::memcpy(item.material_palette, land_palette, sizeof(item.material_palette));
        if (const auto tex_it = body_textures.find(body_tex_key(anchor->slot, anchor->moon));
            tex_it != body_textures.end()) {
          item.planet_texture = tex_it->second.handle;
          item.aux[3] = 1.0f;
        }
        std::memcpy(item.mvp, mvp.m, sizeof(mvp.m));
        item.aux[0] = static_cast<float>(rel.x);
        item.aux[1] = static_cast<float>(rel.y);
        item.aux[2] = static_cast<float>(rel.z);
        items.push_back(item);
      }

      // The anchor always keeps an under-the-terrain impostor sphere,
      // LIT with the same terrain material: freshly-anchored planets are
      // visible before their chunks stream in, and LOD-seam pinholes at
      // chunk corners show matching-shaded ground instead of black space
      // (the "transparent quad grid" seen from orbit). Skips itself when
      // the camera is near the surface, where terrain fully covers it.
      {
        // EarthLike: the impostor is the OCEAN — opaque, lit, sea-blue,
        // at sea level. Coarse orbital LOD sags terrain below its true
        // height between lattice points, which used to let the
        // translucent sea shell show through as a blue quad grid; with
        // the ocean ball underneath, dips below sea level simply read as
        // sea, which is exactly what a planet looks like from space.
        const bool has_sea = sea_radius > 0.0;
        const double impostor_r = has_sea ? sea_radius : anchor->radius * 0.995;
        if (inf::sim::length(cam_pos_local) > impostor_r * 1.05) {
          const RVec3 rel = to_render(SVec3{0.0, 0.0, 0.0}) - camera_pos;
          const Mat4 model = inf::render::from_basis(
              RVec3{impostor_r, 0.0, 0.0}, RVec3{0.0, impostor_r, 0.0},
              RVec3{0.0, 0.0, impostor_r}, rel);
          const Mat4 mvp = inf::render::mul(view_projection, model);
          inf::render::Rhi::DrawItem item;
          item.mesh = impostor_mesh;
          std::memcpy(item.mvp, mvp.m, sizeof(mvp.m));
          // color.a == 0: the lit material path (rgb = albedo override).
          if (has_sea) {
            item.color[0] = 0.10f;
            item.color[1] = 0.28f;
            item.color[2] = 0.52f;
          }
          item.aux[0] = static_cast<float>(rel.x);
          item.aux[1] = static_cast<float>(rel.y);
          item.aux[2] = static_cast<float>(rel.z);
          items.push_back(item);
        }
      }
      for (const MoonInstance& moon : moons_local) {
        if (!moon.is_anchor) {
          draw_ball(moon.pos, moon.radius, 2.0, 0.62f, 0.62f, 0.66f,
                    body_tex_key(moon.slot, moon.index));
        }
      }

      // --- atmosphere limb glow (space view) ---------------------------
      // A soft additive rim halo around every planet with an atmosphere,
      // the thin bright shell games use for planets seen from orbit.
      const auto draw_limb_halo = [&](const SVec3& pos, double radius, const float tint[3],
                                      double intensity) {
        const double dist = inf::sim::length(pos - cam_pos_local);
        // Fade in smoothly with distance instead of popping at a gate —
        // the hard cutoff was visible when leaving the atmosphere.
        const double fade =
            std::clamp((dist / radius - 1.10) / 0.5, 0.0, 1.0);
        if (fade <= 0.0) {
          return;  // inside/very near: the sky dome takes over
        }
        const double halo = radius * 1.22;
        // Phase-modulated: the halo is sunlight forward-scattered by the
        // atmosphere, so it belongs on the lit limb — over a night-side
        // nadir it drops to a whisper. (Unmodulated, HDR night exposure
        // turned it into a neon ring swallowing the deep sky.)
        const double phase =
            inf::sim::dot(inf::sim::normalize(star_local - pos),
                          inf::sim::normalize(cam_pos_local - pos));
        const double lit = std::clamp((phase + 0.4) / 0.9, 0.0, 1.0);
        intensity *= 0.04 + 0.96 * lit;
        const RVec3 bill_right =
            inf::render::normalize(inf::render::cross(cam_forward, cam_up));
        const Mat4 model =
            inf::render::from_basis(bill_right * halo, cam_up * halo, cam_forward * halo,
                                    to_render(pos) - camera_pos);
        const Mat4 mvp = inf::render::mul(view_projection, model);
        inf::render::Rhi::DrawItem item;
        item.mesh = glow_mesh;
        std::memcpy(item.mvp, mvp.m, sizeof(mvp.m));
        item.color[0] = tint[0];
        item.color[1] = tint[1];
        item.color[2] = tint[2];
        item.color[3] = 1.0f;
        item.mode = 3;
        item.extra[0] = static_cast<float>(intensity * fade);
        item.extra[1] = 18.0f;                                  // rim sharpness
        item.extra[2] = static_cast<float>(radius / halo);      // rim radius
        items.push_back(item);
      };
      const auto palette_for = [&](inf::gen::PlanetType type, float out[3]) {
        out[0] = 0.45f; out[1] = 0.65f; out[2] = 0.95f;
        switch (type) {
          case inf::gen::PlanetType::Desert: out[0] = 0.85f; out[1] = 0.62f; out[2] = 0.40f; break;
          case inf::gen::PlanetType::Ice:    out[0] = 0.62f; out[1] = 0.76f; out[2] = 0.95f; break;
          default: break;
        }
      };
      for (int slot = 0; slot < inf::gen::kMaxPlanetSlots; ++slot) {
        const auto& entry = system.planets[static_cast<std::size_t>(slot)];
        if (!entry.occupied || entry.phys.atmosphere.height_m.to_double() <= 0.0) {
          continue;
        }
        float tint[3];
        palette_for(entry.surface_type, tint);
        const SVec3 pos = slot == anchor->slot ? SVec3{0.0, 0.0, 0.0}
                                               : planet_local[static_cast<std::size_t>(slot)];
        const double radius = slot == anchor->slot ? anchor->radius
                                                   : entry.phys.radius_m.to_double();
        draw_limb_halo(pos, radius, tint, 0.5);
      }
    }

    // --- sun veil + lens flare (screen space, additive) -----------------
    // Near a star the whole view stays awash in its light even when the
    // camera turns away (veil), and while it is on screen it throws a
    // core glare, an anamorphic streak, and a train of flare ghosts.
    // Deliberately theatrical rather than physical.
    const auto glare_sprite = [&](double x, double y, double sx, double sy,
                                  const float tint[3], double intensity, double falloff) {
      inf::render::Rhi::DrawItem item;
      item.mesh = glow_mesh;
      Mat4 m{};
      m.m[0] = static_cast<float>(sx / input.aspect);
      m.m[5] = static_cast<float>(sy);
      m.m[10] = 0.00001f;
      m.m[12] = static_cast<float>(x);
      m.m[13] = static_cast<float>(y);
      m.m[14] = 0.99994f;  // reversed-Z: just over the HUD
      m.m[15] = 1.0f;
      std::memcpy(item.mvp, m.m, sizeof(m.m));
      item.color[0] = tint[0];
      item.color[1] = tint[1];
      item.color[2] = tint[2];
      item.color[3] = 1.0f;
      item.mode = 3;
      item.extra[0] = static_cast<float>(intensity);
      item.extra[1] = static_cast<float>(falloff);
      items.push_back(item);
    };
    if (map_phase == MapPhase::Off) {
      for (const StarInstance& star : stars_local) {
        const SVec3 rel = star.pos - cam_pos_local;
        const double dist = inf::sim::length(rel);
        if (dist < 1.0) {
          continue;
        }
        const SVec3 dir = rel * (1.0 / dist);
        // Occlusion by any planet (incl. the anchor: covers night side
        // and the sun below the horizon). SOFT: fades across ~6% of the
        // occluder's radius at the limb — a hard binary test made the
        // glare snap on/off whenever the line of sight grazed a planet
        // edge (the reported sun flicker).
        double visibility = 1.0;
        const auto occlude = [&](const SVec3& body_center, double radius) {
          const SVec3 center = body_center - cam_pos_local;
          const double t = inf::sim::dot(center, dir);
          if (t > 0.0 && t < dist) {
            const double c2 = std::max(0.0, inf::sim::dot(center, center) - t * t);
            const double closest = std::sqrt(c2);
            visibility = std::min(
                visibility,
                std::clamp((closest - radius) / (radius * 0.06), 0.0, 1.0));
          }
        };
        for (int slot = 0; slot < inf::gen::kMaxPlanetSlots; ++slot) {
          const auto& entry = system.planets[static_cast<std::size_t>(slot)];
          if (!entry.occupied) {
            continue;
          }
          occlude(planet_local[static_cast<std::size_t>(slot)],
                  slot == anchor->slot && anchor->moon < 0
                      ? anchor->radius
                      : entry.phys.radius_m.to_double());
        }
        for (const MoonInstance& moon : moons_local) {
          occlude(moon.pos, moon.is_anchor ? anchor->radius : moon.radius);
        }
        // Closeness drives everything: 1 within ~8 star radii, fading
        // with distance (at a habitable-zone planet the veil is a subtle
        // few percent, not a wash).
        const double closeness = std::clamp(star.radius * 8.0 / dist, 0.0, 1.0);
        if (closeness < 0.02) {
          continue;
        }
        const double cos_ang = inf::sim::dot(inf::sim::normalize(cam_fwd_v), dir);
        // Veil: glare that HUGS the sun direction (pow-5 falloff: full
        // when staring, ~5% at 60 deg off-axis, zero behind) and zero
        // when occluded. The old near-flat 0.06+0.94*cos washed the
        // whole frame beige anywhere in an inner system — jump arrivals
        // now face the star dead-on, which made that permanent.
        const double veil = closeness * closeness *
                            std::pow(std::max(cos_ang, 0.0), 5.0) * visibility;
        if (veil > 0.004) {
          float warm[3] = {star.tint[0], star.tint[1], star.tint[2]};
          // Size 2.0 + falloff 3.0 (was 3.0 / 0.55): at 3x screen size
          // the frame corners sat at r~0.38 of the sprite where any
          // falloff barely bites, so a sun-stare painted the whole
          // frame beige. Now the glare grades to ~0.08 at the corners —
          // hard glow at center, sky survives at the rim.
          glare_sprite(0.0, 0.0, 2.0 * input.aspect, 2.0, warm, veil * 0.30, 3.0);
        }
        if (visibility <= 0.0 || cos_ang <= 0.0) {
          continue;
        }
        // Screen position for the flare train.
        const auto clip = project_point(view_projection, to_render(rel));
        if (clip[3] <= 0.0) {
          continue;
        }
        const double sx = clip[0] / clip[3];
        const double sy = clip[1] / clip[3];
        const double edge = std::max(std::abs(sx), std::abs(sy));
        const double edge_fade = std::clamp(1.0 - (edge - 1.0) / 0.35, 0.0, 1.0);
        const double flare = closeness * edge_fade * visibility;
        if (flare < 0.01) {
          continue;
        }
        float white_mix[3] = {star.tint[0] * 0.5f + 0.5f, star.tint[1] * 0.5f + 0.5f,
                              star.tint[2] * 0.5f + 0.5f};
        // Core glare + anamorphic streak (the JJ-Abrams special).
        glare_sprite(sx, sy, 0.55, 0.55, white_mix, flare * 1.1, 3.0);
        glare_sprite(sx, sy, 2.6, 0.05, white_mix, flare * 0.75, 2.0);
        // Ghost train along the axis through the screen center.
        static constexpr double kGhostPos[4] = {0.55, 0.25, -0.25, -0.55};
        static constexpr double kGhostSize[4] = {0.10, 0.06, 0.08, 0.14};
        for (int g = 0; g < 4; ++g) {
          float ghost_tint[3] = {star.tint[0] * (g % 2 == 0 ? 0.4f : 0.9f),
                                 star.tint[1] * 0.7f,
                                 star.tint[2] * (g % 2 == 0 ? 0.9f : 0.4f)};
          glare_sprite(sx * kGhostPos[g], sy * kGhostPos[g], kGhostSize[g], kGhostSize[g],
                       ghost_tint, flare * 0.5, 1.6);
        }
      }
    }

    // Radar feed: every body in the system, relative to the player
    // (constant-size icons + elevation bars; drawn by the HUD when the
    // space radar is showing).
    std::vector<inf::app::RadarBody> radar_bodies;
    radar_bodies.reserve(2 + moons_local.size() + inf::gen::kMaxPlanetSlots);
    {
      for (const StarInstance& star : stars_local) {
        inf::app::RadarBody star_icon;
        star_icon.rel = star.pos - player_pos;
        star_icon.color[0] = star.tint[0];
        star_icon.color[1] = star.tint[1];
        star_icon.color[2] = star.tint[2];
        star_icon.scale = 1.6f;
        radar_bodies.push_back(star_icon);
      }
      for (int slot = 0; slot < inf::gen::kMaxPlanetSlots; ++slot) {
        const auto& entry = system.planets[static_cast<std::size_t>(slot)];
        if (!entry.occupied) {
          continue;
        }
        inf::app::RadarBody icon;
        icon.rel = planet_local[static_cast<std::size_t>(slot)] - player_pos;
        slot_color(slot, icon.color);
        icon.scale = 1.0f;
        icon.anchor = slot == anchor->slot && anchor->moon < 0;
        radar_bodies.push_back(icon);
      }
      for (const MoonInstance& moon : moons_local) {
        inf::app::RadarBody icon;
        icon.rel = moon.pos - player_pos;
        icon.color[0] = 0.62f;
        icon.color[1] = 0.62f;
        icon.color[2] = 0.66f;
        icon.scale = 0.55f;
        icon.anchor = moon.is_anchor;
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

      // The stars (clamped like the planets, full photosphere + corona).
      for (const StarInstance& star : stars_local) {
        draw_star(star, min_px * 0.75);
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
        // Map bodies share the far-view texture cache (T0016) — the same
        // displaced impostor, just at the map's clamped size.
        if (const auto tex_it = body_textures.find(body_tex_key(slot, -1));
            tex_it != body_textures.end()) {
          item.mesh = impostor_mesh;
          item.mode = 6;
          item.planet_texture = tex_it->second.handle;
          item.color[3] = 0.0f;
          item.extra[0] = tex_it->second.amp_over_radius;
          item.extra[1] = tex_it->second.slope_scale;
          const SVec3 to_sun = inf::sim::normalize(
              (stars_local.empty() ? SVec3{0.0, 0.0, 1.0e12} : stars_local[0].pos) - pos);
          item.aux[0] = static_cast<float>(to_sun.x);
          item.aux[1] = static_cast<float>(to_sun.y);
          item.aux[2] = static_cast<float>(to_sun.z);
        }
        items.push_back(item);
        for (std::size_t mi = 0; mi < entry.moons.size(); ++mi) {
          const auto& moon = entry.moons[mi];
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
          if (const auto tex_it =
                  body_textures.find(body_tex_key(slot, static_cast<int>(mi)));
              tex_it != body_textures.end()) {
            mitem.mesh = impostor_mesh;
            mitem.mode = 6;
            mitem.planet_texture = tex_it->second.handle;
            mitem.color[3] = 0.0f;
            mitem.extra[0] = tex_it->second.amp_over_radius;
            mitem.extra[1] = tex_it->second.slope_scale;
            const SVec3 to_sun = inf::sim::normalize(
                (stars_local.empty() ? SVec3{0.0, 0.0, 1.0e12} : stars_local[0].pos) -
                mpos);
            mitem.aux[0] = static_cast<float>(to_sun.x);
            mitem.aux[1] = static_cast<float>(to_sun.y);
            mitem.aux[2] = static_cast<float>(to_sun.z);
          }
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
    if (map_phase == MapPhase::Off && script_hud) {
    items.push_back(hud_quad(cube_mesh, 0.0, 0.0, cross_len / ar, cross_thick, 0.9f, 0.95f, 1.0f));
    items.push_back(hud_quad(cube_mesh, 0.0, 0.0, cross_thick / ar, cross_len, 0.9f, 0.95f, 1.0f));
    }
    if (map_phase == MapPhase::Off && script_hud &&
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

    // REC indicator: red square top right — solid flash on the F9 press,
    // then blinking while the triggered recording is still capturing.
    rec_flash = std::max(0.0, rec_flash - dt);
    if (rec_flash > 0.0 || rhi->recording_active()) {
      const double blink = std::fmod(static_cast<double>(now.ns_since_epoch) * 1e-9, 0.8);
      if (rec_flash > 0.0 || blink < 0.55) {
        items.push_back(
            hud_quad(cube_mesh, 0.94, 0.90, 0.030 / ar, 0.05, 1.0f, 0.16f, 0.12f));
      }
    }

    if (sea_mesh != 0 && show_surface) {
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
      // Fade the shell in below ~0.3R so the show_surface crossing does
      // not step the ocean's brightness in a single frame (the depth-
      // tinted seabed carries the ocean color from orbit).
      const double shell_fade =
          std::clamp((0.30 * anchor->radius - altitude) / (0.10 * anchor->radius), 0.0, 1.0);
      item.color[3] = static_cast<float>(0.42 * shell_fade);
      item.translucent = true;
      // Lit translucent (mode 5): the sea shades with the sun like the
      // terrain — an unlit shell used to glow bright blue through
      // coarse-LOD terrain dips on the night side (the orbit quad grid).
      item.mode = 5;
      const RVec3 sea_rel = RVec3{0.0, 0.0, 0.0} - camera_pos;
      item.aux[0] = static_cast<float>(sea_rel.x);
      item.aux[1] = static_cast<float>(sea_rel.y);
      item.aux[2] = static_cast<float>(sea_rel.z);
      items.push_back(item);
    }

    // Crosshair target: the planet whose disc (plus a small grace angle)
    // the crosshair rests on — name, surface distance, and an ETA while
    // actually closing on it.
    inf::app::TargetInfo target;
    if (!flight_active && map_phase == MapPhase::Off &&
        player.mode() == inf::sim::PlayerMode::Flight) {
      const SVec3 fwd = inf::sim::normalize(player.forward());
      double best_margin = 0.03;  // radians of grace beyond the disc edge
      for (int slot = 0; slot < inf::gen::kMaxPlanetSlots; ++slot) {
        const auto& entry = system.planets[static_cast<std::size_t>(slot)];
        if (!entry.occupied) {
          continue;
        }
        const SVec3 rel = planet_local[static_cast<std::size_t>(slot)] - player_pos;
        const double dist = inf::sim::length(rel);
        const double radius = slot == anchor->slot ? anchor->radius
                                                   : entry.phys.radius_m.to_double();
        // Skip the anchor while flying close over it — the whole screen
        // is planet there, the readout would be noise.
        if (slot == anchor->slot && dist - radius < radius) {
          continue;
        }
        if (dist <= radius) {
          continue;
        }
        const double cos_ang = inf::sim::dot(fwd, rel * (1.0 / dist));
        if (cos_ang <= 0.0) {
          continue;
        }
        const double ang = std::acos(std::clamp(cos_ang, -1.0, 1.0));
        const double ang_radius = std::asin(std::clamp(radius / dist, 0.0, 1.0));
        const double margin = ang - ang_radius;
        if (margin < best_margin) {
          best_margin = margin;
          target.valid = true;
          target.name = slot_names[static_cast<std::size_t>(slot)];
          target.distance_m = dist - radius;
          const double closing = player.speed() * cos_ang;
          target.eta_s = closing > 1.0 ? target.distance_m / closing : -1.0;
          target.civ_line = civ_slot_line[static_cast<std::size_t>(slot)];
        }
      }
      // Moons under the crosshair (T0016: full bodies with names).
      for (const MoonInstance& moon : moons_local) {
        const SVec3 rel = moon.pos - player_pos;
        const double dist = inf::sim::length(rel);
        if (moon.is_anchor && dist - moon.radius < moon.radius) {
          continue;  // flying close over the anchor moon: readout is noise
        }
        if (dist <= moon.radius) {
          continue;
        }
        const double cos_ang = inf::sim::dot(fwd, rel * (1.0 / dist));
        if (cos_ang <= 0.0) {
          continue;
        }
        const double ang = std::acos(std::clamp(cos_ang, -1.0, 1.0));
        const double ang_radius = std::asin(std::clamp(moon.radius / dist, 0.0, 1.0));
        const double margin = ang - ang_radius;
        if (margin < best_margin) {
          best_margin = margin;
          target.valid = true;
          target.name = moon_names[static_cast<std::size_t>(moon.slot)]
                                  [static_cast<std::size_t>(moon.index)];
          target.distance_m = dist - moon.radius;
          const double closing = player.speed() * cos_ang;
          target.eta_s = closing > 1.0 ? target.distance_m / closing : -1.0;
        }
      }
    }

    // An armed jump target overrides the crosshair readout — the brief's
    // "extend TargetInfo, don't build a second widget".
    if (jump_timer > 0.0) {
      target.valid = true;
      target.name = "JUMPING > " + jump_sel_name;
      target.distance_m = jump_target.dist_ly * inf::gen::kLightYearM;
      target.eta_s = -1.0;
    } else if (jump_index >= 0 && map_phase == MapPhase::Off) {
      const auto& candidate = jump_candidates[static_cast<std::size_t>(jump_index)];
      char line[192];
      std::snprintf(line, sizeof(line), "JUMP %d/%zu %s  %.2f ly  [hold J]",
                    jump_index + 1, jump_candidates.size(), jump_sel_name.c_str(),
                    candidate.dist_ly);
      target.valid = true;
      target.name = line;
      target.distance_m = candidate.dist_ly * inf::gen::kLightYearM;
      target.eta_s = -1.0;
    }

    if (map_phase == MapPhase::Off && script_hud) {
      const std::string& location_name =
          anchor->moon >= 0 ? moon_names[static_cast<std::size_t>(anchor->slot)]
                                        [static_cast<std::size_t>(anchor->moon)]
                            : slot_names[static_cast<std::size_t>(anchor->slot)];
      const std::size_t hud_start = items.size();
      hud->build(&items, player, radar_bodies, measured_speed, input.aspect, state.height,
                 dt, location_name, target);
      // HUD is UI: it rides the LDR overlay pass, immune to eye
      // adaptation (T0018).
      for (std::size_t i = hud_start; i < items.size(); ++i) {
        items[i].overlay = true;
      }
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
      // T0020: who owns it (design/map-mode: extend the card, no new widget).
      lines.push_back(civ_slot_line[static_cast<std::size_t>(hovered_slot)].empty()
                          ? civ_system_line
                          : civ_slot_line[static_cast<std::size_t>(hovered_slot)]);
      const std::size_t card_start = items.size();
      hud->build_map_card(&items, lines, pointer_ndc_x, pointer_ndc_y, input.aspect,
                          state.height);
      for (std::size_t i = card_start; i < items.size(); ++i) {
        items[i].overlay = true;
      }
    }

    // Frame lighting: the star with the highest apparent flux at the
    // player is the directional sun (matters in bi/tri-star systems when
    // roaming near a companion); tint softened toward white.
    inf::render::Rhi::FrameParams frame_params;
    frame_params.sky[0] = sky[0];
    frame_params.sky[1] = sky[1];
    frame_params.sky[2] = sky[2];
    {
      const StarInstance* dominant = &stars_local.front();
      double best_flux = -1.0;
      for (const StarInstance& star : stars_local) {
        const double dist = std::max(1.0, inf::sim::length(star.pos - player_pos));
        const double flux = star.luminosity / (dist * dist);
        if (flux > best_flux) {
          best_flux = flux;
          dominant = &star;
        }
      }
      const SVec3 sun_dir = inf::sim::normalize(dominant->pos - player_pos);
      frame_params.sun_dir[0] = static_cast<float>(sun_dir.x);
      frame_params.sun_dir[1] = static_cast<float>(sun_dir.y);
      frame_params.sun_dir[2] = static_cast<float>(sun_dir.z);
      for (int c = 0; c < 3; ++c) {
        frame_params.sun_color[c] = dominant->tint[c] + 0.25f * (1.0f - dominant->tint[c]);
      }
    }
    // Wrapped so the f32 shader time keeps sub-ms precision forever.
    frame_params.time_s = static_cast<float>(
        std::fmod(static_cast<double>(now.ns_since_epoch) * 1e-9, 4096.0));
    {
      // Camera basis + atmosphere state for the mode-4 sky dome.
      const RVec3 cam_right_v =
          inf::render::normalize(inf::render::cross(cam_forward, cam_up));
      const SVec3 up_local = inf::sim::normalize(cam_pos_local);
      const double tan_half = std::tan(kFovY * 0.5);
      const auto set3 = [](float out[3], const RVec3& v) {
        out[0] = static_cast<float>(v.x);
        out[1] = static_cast<float>(v.y);
        out[2] = static_cast<float>(v.z);
      };
      set3(frame_params.cam_right, cam_right_v);
      set3(frame_params.cam_up, cam_up);
      set3(frame_params.cam_fwd, cam_forward);
      set3(frame_params.planet_up, to_render(up_local));
      for (int c = 0; c < 3; ++c) {
        frame_params.atmo_tint[c] = palette[c];
      }
      frame_params.tan_half_x = static_cast<float>(tan_half * input.aspect);
      frame_params.tan_half_y = static_cast<float>(tan_half);
      {
        // Camera basis as the view matrix uses it (T0021: the city frame,
        // cascade fitting and the screen-space passes rebuild the view
        // from these).
        const RVec3 f = inf::render::normalize(cam_forward);
        const RVec3 s = inf::render::normalize(inf::render::cross(f, cam_up));
        const RVec3 u = inf::render::cross(s, f);
        frame_params.cam_right[0] = static_cast<float>(s.x);
        frame_params.cam_right[1] = static_cast<float>(s.y);
        frame_params.cam_right[2] = static_cast<float>(s.z);
        frame_params.cam_up[0] = static_cast<float>(u.x);
        frame_params.cam_up[1] = static_cast<float>(u.y);
        frame_params.cam_up[2] = static_cast<float>(u.z);
        frame_params.cam_fwd[0] = static_cast<float>(f.x);
        frame_params.cam_fwd[1] = static_cast<float>(f.y);
        frame_params.cam_fwd[2] = static_cast<float>(f.z);
      }
      frame_params.altitude_frac = static_cast<float>(std::clamp(dome_alt_frac, 0.0, 9.0));
      set3(frame_params.planet_center, RVec3{0.0, 0.0, 0.0} - camera_pos);
      frame_params.sea_radius_m = static_cast<float>(sea_radius);
      frame_params.palette_shift = static_cast<float>(
          static_cast<double>(anchor->planet.palette_id % 201U) / 100.0 - 1.0);
      // Blend terrain normals toward the sphere radial from ~0.25 radii
      // of altitude up (full sphere shading from one radius out).
      frame_params.normal_blend = static_cast<float>(
          std::clamp((altitude / anchor->radius - 0.25) / 0.75, 0.0, 1.0));
      // T0021: the camera-relative view-projection of this and the last
      // frame for the city pipeline (shadows, AO, temporal AA).
      frame_params.have_view_proj = true;
      std::memcpy(frame_params.view_proj, view_projection.m, sizeof(view_projection.m));
      {
        const RVec3 shift = camera_pos - city_prev_camera;
        const Mat4 prev_rel = inf::render::mul(city_prev_view_proj, inf::render::translate(shift));
        std::memcpy(frame_params.prev_view_proj, prev_rel.m, sizeof(prev_rel.m));
        frame_params.camera_delta[0] = static_cast<float>(shift.x);
        frame_params.camera_delta[1] = static_cast<float>(shift.y);
        frame_params.camera_delta[2] = static_cast<float>(shift.z);
        city_prev_view_proj = view_projection;
        city_prev_camera = camera_pos;
      }
      // Night for lit rooms and lamps: the sun below the local horizon.
      inf::render::Rhi::CitySettings city_settings;
      const double sun_up = static_cast<double>(frame_params.sun_dir[0]) * frame_params.planet_up[0] +
                            static_cast<double>(frame_params.sun_dir[1]) * frame_params.planet_up[1] +
                            static_cast<double>(frame_params.sun_dir[2]) * frame_params.planet_up[2];
      city_settings.night = static_cast<float>(std::clamp((0.03 - sun_up) / 0.12, 0.0, 1.0));
      beacon_night = city_settings.night;
      {
        // Twilight: the sun's light through the long atmospheric path at
        // the horizon — dim and warm as it sets, gone a few degrees under
        // (until now a sun just below the horizon still lit tower tops and
        // far hills at full strength, blowing out under night exposure).
        // Cosmetic; the terrain, the city and the dome share the tint.
        const double t = std::clamp((sun_up + 0.03) / 0.15, 0.0, 1.0);
        const double tw = t * t * (3.0 - 2.0 * t);
        const double warm[3] = {1.0, 0.55 + 0.45 * tw, 0.35 + 0.65 * tw};
        for (int c = 0; c < 3; ++c) {
          frame_params.sun_color[c] *= static_cast<float>((0.02 + 0.98 * tw) * warm[c]);
        }
      }
      city_settings.debug_view = city_debug;
      city_settings.ssao = !no_ssao;
      city_settings.shadows = !no_shadows;
      city_settings.taa = !no_taa;
      city_settings.occlusion = !no_occlusion;
      city_settings.ssao_half = !ssao_full;
      city_settings.shadow_half_rate = shadow_half_rate;
      city_settings.shadow_far_lod = shadow_far_lod;
      if (stress_frames > 0) {
        // --stress: flip the size-dependent options every frame so the
        // targets are recreated N times (the leak regression of T0022 C.3).
        city_settings.ssao_half = (frame & 1) != 0;
        city_settings.occlusion = (frame & 2) != 0;
      }
      rhi->set_city_settings(city_settings);
      {
        std::vector<inf::render::Rhi::CityLight> lights;
        if (city_active) {
          inf::app::city_lights_for_frame(city_upload, camera_pos, city_settings.night > 0.05f, &lights);
        } else if (anchor && anchor->civ) {
          inf::app::civ_city_lights(anchor->civ.get(), camera_pos, city_settings.night > 0.05f, &lights);
        }
        rhi->set_city_lights(lights.data(), lights.size());
      }
    }
    // Verification captures (--capture): grab the final rendered frame,
    // when the scene has had time to stream in.
    if (capture_text != nullptr && max_frames > 0 && frame == max_frames - 1) {
      rhi->request_capture(capture_text);
    }
    // Player-state sidecar for active recordings (frame-by-frame
    // correlation when analyzing a dumped sequence).
    if (rhi->recording_active() && !rec_dir_current.empty()) {
      const std::unique_ptr<std::FILE, int (*)(std::FILE*)> meta(
          std::fopen((rec_dir_current + "/meta.csv").c_str(), "a"), &std::fclose);
      if (meta != nullptr) {
        const SVec3 meta_pos = player.position();
        std::fprintf(meta.get(), "%.4f,%.1f,%.1f,%.1f,%.2f,%.1f,%d\n", frame_params.time_s,
                     meta_pos.x, meta_pos.y, meta_pos.z, player.speed(), player.altitude(),
                     static_cast<int>(player.mode()));
      }
    }
    frame_params.city_ranges = city_ranges.data();
    frame_params.city_range_count = city_ranges.size();
    frame_params.lock_exposure =
        script_exposure_locked ||
        (sweep_frames > 0 && frame >= sweep_warmup - 8);
    if (flight_active && script_hud) {
      char status[120];
      std::snprintf(status, sizeof(status),
                    "GALAXY FLIGHT %.1f / %.0f s | F6 / Esc: brake",
                    flight_elapsed, galaxy_flight.end_time());
      const auto first = items.size();
      hud->build_map_card(&items, {status}, -0.95, 0.94,
                          static_cast<double>(state.width) / state.height,
                          state.height);
      for (auto i = first; i < items.size(); ++i) items[i].overlay = true;
    }
    const auto render_started = FlightClock::now();
    const bool presented =
        rhi->render_frame(frame_params, items.data(), items.size());
    scene_presented = scene_presented || presented;
    if (flight_active && flight_csv) {
      const double ms = std::chrono::duration<double, std::milli>(
                            FlightClock::now() - frame_started)
                            .count();
      const auto elapsed_ms = [](auto from, auto to) {
        return std::chrono::duration<double, std::milli>(to - from).count();
      };
      flight_csv << flight_elapsed << ',' << ms << ',' << star_catalog.build_ms
                 << ',' << presented << ',' << state.width << ','
                 << state.height << ',' << observer_gal.x << ','
                 << observer_gal.y << ',' << observer_gal.z << ','
                 << elapsed_ms(frame_started, resources_finished) << ','
                 << elapsed_ms(resources_finished, world_finished) << ','
                 << elapsed_ms(world_finished, stars_finished) << ','
                 << elapsed_ms(stars_finished, render_started) << ','
                 << elapsed_ms(render_started, FlightClock::now()) << '\n';
    }
    if (flight_final_frame && presented) {
      flight_active = false;
      player.set_speed(0);
      flight_csv << "# complete=" << (!galaxy_flight.cancelled())
                 << ";cancelled=" << galaxy_flight.cancelled() << '\n';
      flight_csv.close();
      if (flight_profile_exit) glfwSetWindowShouldClose(window, GLFW_TRUE);
    }
    if (stress_frames > 0 && frame >= static_cast<long>(stress_frames)) {
      std::printf("stress: %d target recreations survived\n", stress_frames);
      std::fflush(stdout);
      break;
    }

    if (bench_frames > 0 && frame >= sweep_warmup && script_pc >= script.size() && script_wait <= 0.0) {
      // Frame time from the wall clock around whole frames (the GPU is
      // synchronised by the swap chain).
      static double bench_start = 0.0;
      static int bench_count = 0;
      static long bench_uploads_start = 0;
      const inf::core::LocalClock bench_clock;
      const double now_s = static_cast<double>(bench_clock.now().ns_since_epoch) * 1e-9;
      if (bench_count == 0) {
        bench_start = now_s;
        bench_uploads_start = chunk_uploads;
      }
      if (++bench_count > bench_frames) {
        const double ms = (now_s - bench_start) / bench_frames * 1000.0;
        const inf::render::Rhi::CityPassStats& ps = rhi->city_pass_stats();
        std::printf("bench: %d frames, %.2f ms/frame, %dx%d, city triangles %zu resident / %zu drawn / %zu shadow in %zu ranges (%u main, %u occluded, %u shadow) over %zu meshes, %zu chunks (%ld uploaded during), %zu items, ssao %d%s shadows %d%s%s taa %d occlusion %d\n",
                    bench_frames, ms, state.width, state.height, city_stats.resident_triangles, city_stats.drawn_triangles,
                    city_stats.shadow_triangles, city_stats.ranges, ps.ranges_main, ps.ranges_occluded, ps.ranges_shadow, city_stats.items,
                    loaded.size(), chunk_uploads - bench_uploads_start, items.size(), no_ssao ? 0 : 1, ssao_full ? "" : " (half)",
                    no_shadows ? 0 : 1, shadow_half_rate ? " (half rate)" : "", shadow_far_lod ? " (far lod)" : "", no_taa ? 0 : 1,
                    no_occlusion ? 0 : 1);
        std::fflush(stdout);
        break;
      }
    }
    fps_accum += dt;
    ++fps_frames;
    if (fps_accum >= 1.0) {
      const char* mode_name = player.zone() == inf::sim::FlightZone::Atmosphere
                                  ? "flight (atmo)"
                                  : "flight (space)";
      if (player.can_land()) {
        mode_name = "flight (E to land)";
      }
      switch (player.mode()) {
        case inf::sim::PlayerMode::Landing: mode_name = "landing"; break;
        case inf::sim::PlayerMode::OnFoot: mode_name = "on foot"; break;
        case inf::sim::PlayerMode::Takeoff: mode_name = "takeoff"; break;
        case inf::sim::PlayerMode::Map: mode_name = "system map"; break;
        default: break;
      }
      char title[192];
      std::snprintf(title, sizeof(title),
                    "unendlich — %s | %.0f fps | %zu chunks | alt %.0f m | %.0f m/s",
                    mode_name, fps_frames / fps_accum, loaded.size(), player.altitude(),
                    player.speed());
      glfwSetWindowTitle(window, title);
      fps_accum = 0.0;
      fps_frames = 0;
    }

    ++frame;
    if (sweep_frames > 0 && frame >= sweep_warmup) {
      std::vector<std::uint8_t> cur;
      std::vector<float> depth;
      std::uint32_t w = 0, h = 0;
      const auto lum = [](const std::vector<std::uint8_t>& img, std::size_t px) {
        return (0.299f * img[px * 4] + 0.587f * img[px * 4 + 1] + 0.114f * img[px * 4 + 2]) / 255.0f;
      };
      // Box blur of radius sweep_blur, separable, on the three colour channels.
      const auto blur = [&](std::vector<std::uint8_t>& img) {
        const int R = sweep_blur;
        if (R <= 0) return;
        std::vector<std::uint8_t> tmp(img.size());
        for (std::uint32_t y = 0; y < h; ++y) for (std::uint32_t x = 0; x < w; ++x) for (int c = 0; c < 3; ++c) {
          int sum = 0, n = 0;
          for (int k = -R; k <= R; ++k) { const int xx = static_cast<int>(x) + k; if (xx < 0 || xx >= static_cast<int>(w)) continue; sum += img[(static_cast<std::size_t>(y) * w + xx) * 4 + c]; ++n; }
          tmp[(static_cast<std::size_t>(y) * w + x) * 4 + c] = static_cast<std::uint8_t>(sum / std::max(n, 1));
        }
        for (std::uint32_t y = 0; y < h; ++y) for (std::uint32_t x = 0; x < w; ++x) for (int c = 0; c < 3; ++c) {
          int sum = 0, n = 0;
          for (int k = -R; k <= R; ++k) { const int yy = static_cast<int>(y) + k; if (yy < 0 || yy >= static_cast<int>(h)) continue; sum += tmp[(static_cast<std::size_t>(yy) * w + x) * 4 + c]; ++n; }
          img[(static_cast<std::size_t>(y) * w + x) * 4 + c] = static_cast<std::uint8_t>(sum / std::max(n, 1));
        }
      };
      // Bilinear sample of one channel (0..2 rgb, 3 luminance) at a fractional pixel position.
      const auto sample = [&](const std::vector<std::uint8_t>& img, float fx, float fy, int ch) {
        fx = std::min(std::max(fx, 0.0f), static_cast<float>(w) - 1.001f);
        fy = std::min(std::max(fy, 0.0f), static_cast<float>(h) - 1.001f);
        const std::uint32_t x0 = static_cast<std::uint32_t>(fx), y0 = static_cast<std::uint32_t>(fy);
        const float tx = fx - static_cast<float>(x0), ty = fy - static_cast<float>(y0);
        const auto at = [&](std::uint32_t x, std::uint32_t y) {
          const std::size_t px = static_cast<std::size_t>(y) * w + x;
          return ch == 3 ? lum(img, px) : img[px * 4 + static_cast<std::size_t>(ch)] / 255.0f;
        };
        return (at(x0, y0) * (1.0f - tx) + at(x0 + 1, y0) * tx) * (1.0f - ty) + (at(x0, y0 + 1) * (1.0f - tx) + at(x0 + 1, y0 + 1) * tx) * ty;
      };
      const auto move = [&]() {
        SVec3 dir = inf::sim::normalize(inf::sim::cross(player.forward(), player.up()));
        if (sweep_dir == 1) dir = player.forward();
        else if (sweep_dir == 2) dir = player.up() * -1.0;
        player.set_position(player.position() + dir * sweep_step);
      };
      if (sweep.phase == 0) {
        // The material-id frame: the city shader writes ids, the composite
        // passes them raw.
        sweep.saved_debug = city_debug;
        city_debug = 12;
        rhi->request_readback();
        sweep.phase = 1;
      } else if (sweep.phase == 1) {
        if (rhi->take_readback(&cur, &depth, &w, &h)) {
          sweep.ids.swap(cur);
          sweep.w = w;
          sweep.h = h;
          city_debug = sweep.saved_debug;
          sweep.phase = 2;
          sweep.settle = 0;
        }
      } else if (sweep.phase == 2) {
        // Let the TAA history forget the id frame, then take the first frame.
        if (++sweep.settle == 16) rhi->request_readback();
        if (sweep.settle >= 16 && rhi->take_readback(&cur, &depth, &w, &h)) {
          if (w != sweep.w || h != sweep.h) { sweep.w = w; sweep.h = h; sweep.ids.clear(); }
          blur(cur);
          sweep.prev.swap(cur);
          sweep.prev_vp = view_projection;
          sweep.prev_cam = camera_pos;
          sweep.resid.assign(static_cast<std::size_t>(w) * h, 0.0f);
          sweep.raw.assign(sweep.resid.size(), 0.0f);
          sweep.chroma.assign(sweep.resid.size(), 0.0f);
          sweep.phase = 3;
          move();
          rhi->request_readback();
          ++sweep.step;
        }
      } else if (rhi->take_readback(&cur, &depth, &w, &h) && w == sweep.w && h == sweep.h) {
        blur(cur);
        // Column-major 4x4 inverse (cofactors), doubles.
        const auto inverse4 = [](const Mat4& a, double* out) {
          double inv[16];
          const float* m = a.m;
          inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
          inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
          inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
          inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
          inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
          inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
          inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
          inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
          inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
          inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
          inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
          inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
          inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
          inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
          inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
          inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
          const double det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
          const double id = det != 0.0 ? 1.0 / det : 0.0;
          for (int i = 0; i < 16; ++i) out[i] = inv[i] * id;
        };
        const auto xform = [](const double* m, const double* v, double* o) {
          for (int r = 0; r < 4; ++r) o[r] = m[r] * v[0] + m[4 + r] * v[1] + m[8 + r] * v[2] + m[12 + r] * v[3];
        };
        double inv_cur[16];
        inverse4(view_projection, inv_cur);
        // Previous VP in this frame's camera-relative space.
        const Mat4 prev_rel_f = inf::render::mul(sweep.prev_vp, inf::render::translate(camera_pos - sweep.prev_cam));
        double prev_rel[16];
        for (int i = 0; i < 16; ++i) prev_rel[i] = prev_rel_f.m[i];
        for (std::uint32_t y = 1; y + 1 < h; ++y) {
          for (std::uint32_t x = 1; x + 1 < w; ++x) {
            const std::size_t px = static_cast<std::size_t>(y) * w + x;
            const float lc = lum(cur, px);
            sweep.raw[px] += std::fabs(lc - lum(sweep.prev, px));
            float fx = static_cast<float>(x) + 0.5f, fy = static_cast<float>(y) + 0.5f;
            if (depth[px] > 1e-7f) {  // not sky (reversed Z)
              const double ndc[4] = {fx / w * 2.0 - 1.0, 1.0 - fy / h * 2.0, depth[px], 1.0};
              double wp[4];
              xform(inv_cur, ndc, wp);
              const double world[4] = {wp[0] / wp[3], wp[1] / wp[3], wp[2] / wp[3], 1.0};
              double pc[4];
              xform(prev_rel, world, pc);
              if (pc[3] <= 1e-4) continue;
              fx = static_cast<float>((pc[0] / pc[3] * 0.5 + 0.5) * w);
              fy = static_cast<float>((0.5 - pc[1] / pc[3] * 0.5) * h);
              if (fx < 1.0f || fy < 1.0f || fx > w - 2.0f || fy > h - 2.0f) continue;  // came from off-screen
            }
            const float lp = sample(sweep.prev, fx - 0.5f, fy - 0.5f, 3);
            const float gx = 0.5f * std::fabs(lum(cur, px + 1) - lum(cur, px - 1));
            const float gy = 0.5f * std::fabs(lum(cur, px + w) - lum(cur, px - w));
            const float tol = 0.5f * (gx + gy) + 0.004f;
            sweep.resid[px] += std::max(0.0f, std::fabs(lc - lp) - tol);
            // Chroma: red and blue relative to green, the same tolerance rule.
            const float rc = cur[px * 4] / 255.0f - cur[px * 4 + 1] / 255.0f;
            const float bc = cur[px * 4 + 2] / 255.0f - cur[px * 4 + 1] / 255.0f;
            const float rp = sample(sweep.prev, fx - 0.5f, fy - 0.5f, 0) - sample(sweep.prev, fx - 0.5f, fy - 0.5f, 1);
            const float bp = sample(sweep.prev, fx - 0.5f, fy - 0.5f, 2) - sample(sweep.prev, fx - 0.5f, fy - 0.5f, 1);
            sweep.chroma[px] += std::max(0.0f, 0.5f * (std::fabs(rc - rp) + std::fabs(bc - bp)) - tol);
          }
        }
        sweep.prev.swap(cur);
        sweep.prev_vp = view_projection;
        sweep.prev_cam = camera_pos;
        ++sweep.measured;
        if (sweep.step < sweep_frames) {
          move();
          rhi->request_readback();
          ++sweep.step;
        } else {
          const float norm = 1.0f / static_cast<float>(std::max(sweep.measured, 1));
          double mat_sum[256] = {}, mat_cnt[256] = {}, mat_chroma[256] = {};
          double total = 0.0, total_raw = 0.0, total_chroma = 0.0;
          std::vector<std::uint8_t> heat(sweep.resid.size() * 4, 255);
          const auto srgb_lin = [](std::uint8_t b) {
            const float v = b / 255.0f;
            return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
          };
          for (std::size_t px = 0; px < sweep.resid.size(); ++px) {
            const float f = sweep.resid[px] * norm;
            total += f;
            total_raw += sweep.raw[px] * norm;
            total_chroma += sweep.chroma[px] * norm;
            int id = 255;
            if (sweep.ids.size() == sweep.resid.size() * 4) {
              const float r = srgb_lin(sweep.ids[px * 4]), g = srgb_lin(sweep.ids[px * 4 + 1]), b = srgb_lin(sweep.ids[px * 4 + 2]);
              if (b < 0.2f) id = static_cast<int>(std::lround(r * 15.0f)) + 16 * static_cast<int>(std::lround(g * 15.0f));
              else if (b > 0.35f && b < 0.65f && r < 0.1f && g < 0.1f) id = 254;
            }
            mat_sum[id] += f;
            mat_chroma[id] += sweep.chroma[px] * norm;
            mat_cnt[id] += 1.0;
            const float v = std::min(1.0f, f * 10.0f);
            heat[px * 4 + 0] = static_cast<std::uint8_t>(255.0f * std::min(1.0f, v * 2.0f));
            heat[px * 4 + 1] = static_cast<std::uint8_t>(255.0f * std::max(0.0f, std::min(1.0f, v * 2.0f - 0.6f)));
            heat[px * 4 + 2] = static_cast<std::uint8_t>(255.0f * std::max(0.0f, 1.0f - v * 4.0f) * 0.25f);
          }
          const char* dir_name = sweep_dir == 1 ? "forward" : (sweep_dir == 2 ? "down" : "right");
          std::printf("sweep: %d steps of %.3f m %s%s; mean temporal residual %.5f, chroma %.5f (raw frame difference %.5f) at %ux%u\n",
                      sweep.measured, sweep_step, dir_name, sweep_blur > 0 ? " (blurred)" : "", total / static_cast<double>(sweep.resid.size()),
                      total_chroma / static_cast<double>(sweep.resid.size()), total_raw / static_cast<double>(sweep.resid.size()), sweep.w, sweep.h);
          struct Row { int id; double mean; double chroma; double share; };
          std::vector<Row> rows;
          for (int id = 0; id < 256; ++id) {
            if (mat_cnt[id] < 200) continue;
            rows.push_back(Row{id, mat_sum[id] / mat_cnt[id], mat_chroma[id] / mat_cnt[id], mat_sum[id] / std::max(total, 1e-9)});
          }
          std::sort(rows.begin(), rows.end(), [](const Row& p1, const Row& p2) { return p1.share > p2.share; });
          const std::vector<inf::city::MaterialDesc> names = inf::city::make_materials();
          std::printf("  %-4s %-18s %-10s %-10s %-10s %s\n", "id", "material", "pixels", "residual", "chroma", "share");
          for (std::size_t i = 0; i < rows.size() && i < 12; ++i) {
            const Row& r = rows[i];
            const char* name = r.id == 255 ? "(sky)" : (r.id == 254 ? "(terrain)" : (r.id < static_cast<int>(names.size()) ? names[static_cast<std::size_t>(r.id)].name.c_str() : "?"));
            std::printf("  %-4d %-18s %-10.0f %-10.5f %-10.5f %.1f%%\n", r.id, name, mat_cnt[r.id], r.mean, r.chroma, r.share * 100.0);
          }
          stbi_write_png((sweep_out + "-heat.png").c_str(), static_cast<int>(sweep.w), static_cast<int>(sweep.h), 4,
                         heat.data(), static_cast<int>(sweep.w * 4));
          stbi_write_png((sweep_out + "-frame.png").c_str(), static_cast<int>(sweep.w), static_cast<int>(sweep.h), 4,
                         sweep.prev.data(), static_cast<int>(sweep.w * 4));
          if (sweep.ids.size() == sweep.resid.size() * 4) {
            stbi_write_png((sweep_out + "-ids.png").c_str(), static_cast<int>(sweep.w), static_cast<int>(sweep.h), 4,
                           sweep.ids.data(), static_cast<int>(sweep.w * 4));
          }
          {
            // The chroma heat map (x50): colour-only flicker such as z-fighting between two textures.
            std::vector<std::uint8_t> cheat(sweep.chroma.size() * 4, 255);
            for (std::size_t px = 0; px < sweep.chroma.size(); ++px) {
              const float v = std::min(1.0f, sweep.chroma[px] * norm * 50.0f);
              cheat[px * 4 + 0] = static_cast<std::uint8_t>(255.0f * std::min(1.0f, v * 2.0f));
              cheat[px * 4 + 1] = static_cast<std::uint8_t>(255.0f * std::max(0.0f, std::min(1.0f, v * 2.0f - 0.6f)));
              cheat[px * 4 + 2] = static_cast<std::uint8_t>(255.0f * std::max(0.0f, 1.0f - v * 4.0f) * 0.25f);
            }
            stbi_write_png((sweep_out + "-chroma.png").c_str(), static_cast<int>(sweep.w), static_cast<int>(sweep.h), 4,
                           cheat.data(), static_cast<int>(sweep.w * 4));
          }
          std::printf("sweep: wrote %s-heat.png and %s-frame.png\n", sweep_out.c_str(), sweep_out.c_str());
          std::fflush(stdout);
          break;
        }
      }
    }
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
  stop_bake_worker();
  }  // end HUD scope

  save_anchor_edits(*anchor);

  rhi.reset();
  glfwDestroyWindow(window);
  glfwTerminate();
  return EXIT_SUCCESS;
}
