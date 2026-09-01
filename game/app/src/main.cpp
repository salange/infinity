#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <atomic>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <GLFW/glfw3.h>

#include "core/ephem/ephemeris.hpp"
#include "core/key.hpp"
#include "gen/version.hpp"
#include "core/time/world_clock.hpp"
#include "gen/planet.hpp"
#include "gen/system.hpp"
#include "gen/galaxy.hpp"
#include "gen/deep_sky.hpp"
#include "gen/galaxy_octree.hpp"
#include "gen/planet_texture.hpp"
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
#include "deep_sky_render.hpp"

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
};

std::unique_ptr<Anchor> make_anchor(const inf::core::Seed128& seed, const char* seed_text,
                                    const inf::gen::StarSystemParams& system,
                                    const inf::gen::SystemCell& cell, int slot, int moon,
                                    std::optional<inf::gen::PlanetType> forced,
                                    const char* diff_override) {
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
  anchor->diff_path =
      diff_override != nullptr
          ? std::string(diff_override)
          : std::string("infinity-") + seed_text + cell_tag + "-s" + std::to_string(slot) +
                (moon >= 0 ? "m" + std::to_string(moon) : std::string()) + ".edits";
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
  // (scan `infinity-cli dump-system` over seeds) and replace it here AND
  // in the contract test (game/tests/test_system.cpp).
  const char* seed_text = "83";
  const char* type_text = nullptr;
  const char* diff_text = nullptr;
  bool map_demo = false;  // scripted M/Esc for headless smoke + captures
  bool windowed = false;  // default is fullscreen on the primary monitor
  const char* capture_text = nullptr;  // --capture <path.ppm>: PPM of the last frame
  double pitch_deg = 0.0;  // --pitch <deg>: initial pitch-down (capture aid)
  bool release_mode = false;  // --release: debug frame ring OFF
  bool hidden = false;        // --hidden: invisible window (scripted captures)
  const char* script_text = nullptr;  // --script <file>: debug command script
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
  // T0017: `system` is mutable state — the J-jump regenerates it for the
  // octree cell it arrives in.
  inf::gen::SystemCell current_cell{};  // {0,0,0,0} = the home system
  inf::gen::StarSystemParams system =
      inf::gen::generate_system(inf::gen::default_system_key(*seed));
  const int home_slot = inf::gen::default_landable_slot(system);

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
  std::unique_ptr<Anchor> anchor =
      make_anchor(*seed, seed_text, system, current_cell, home_slot, -1, forced, diff_text);
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

  std::printf("infinity %s (%s) — %s planet (slot %d), radius %.0f m\n",
              inf::gen::kVersion, inf::gen::kGitHash,
              inf::gen::to_string(anchor->planet.type), home_slot, anchor->radius);

  if (glfwInit() != GLFW_TRUE) {
    std::fprintf(stderr, "glfwInit failed\n");
    return EXIT_FAILURE;
  }
  glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
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
  GLFWwindow* window = glfwCreateWindow(win_w, win_h, "infinity", monitor, nullptr);
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

  // --- deep sky (T0018 WP2/WP3) ---------------------------------------
  // Static per system: the resolved-star field (one mesh of billboards
  // from the octree, magnitude-limited) and the diffuse band cube map
  // (line integrals of the shared galaxy density plus nebula/cluster
  // splats). Rebaked on every jump; parallax within a system is
  // sub-pixel, so nothing moves between jumps.
  const inf::gen::NebulaField nebula_field(inf::gen::home_galaxy_key(*seed),
                                           galaxy_params);
  const inf::gen::StarClusterField cluster_field(inf::gen::home_galaxy_key(*seed),
                                                 galaxy_params);
  std::uint32_t star_field_mesh = 0;
  std::uint32_t sky_texture = 0;
  constexpr std::uint32_t kSkyFaceSize = 512;
  const auto rebuild_deep_sky = [&](const SVec3& gal_pos) {
    inf::app::SkyView view;
    view.eye_m = inf::gen::Dir3{inf::det::Real(gal_pos.x), inf::det::Real(gal_pos.y),
                                inf::det::Real(gal_pos.z)};
    // Sun direction and ecliptic (WP5) from the anchor's orbit at bake
    // time: the zodiacal wedge drifts with the planet's year, but at the
    // compressed periods that is ~1 deg/hour — a per-arrival bake holds.
    {
      const auto& orbit = system.planets[static_cast<std::size_t>(anchor->slot)].orbit;
      const inf::core::LocalClock clock;
      const auto pv = inf::core::Ephemeris::evaluate(orbit, clock.now());
      const SVec3 planet_sys{pv.x.to_double(), pv.y.to_double(), pv.z.to_double()};
      const SVec3 to_sun = inf::sim::normalize(planet_sys * -1.0);
      view.sun_dir = inf::gen::Dir3{inf::det::Real(to_sun.x), inf::det::Real(to_sun.y),
                                    inf::det::Real(to_sun.z)};
      const double i = orbit.i_rad.to_double();
      const double raan = orbit.raan_rad.to_double();
      view.ecliptic_normal =
          inf::gen::Dir3{inf::det::Real(std::sin(raan) * std::sin(i)),
                         inf::det::Real(-std::cos(raan) * std::sin(i)),
                         inf::det::Real(std::cos(i))};
    }
    const inf::gen::Dir3& eye = view.eye_m;
    if (star_field_mesh != 0) {
      rhi->destroy_mesh(star_field_mesh);
      star_field_mesh = 0;
    }
    const inf::core::LocalClock sky_clock;
    const inf::core::WorldTime sky_t0 = sky_clock.now();
    inf::app::StarCatalogStats stats;
    // m 8.3 is the display visibility floor at the night exposure
    // ceiling — fainter stars would cost bake time without ever showing.
    const std::vector<float> field =
        inf::app::build_star_field_mesh(galaxy_octree, eye, 8.3, 90000, &stats);
    // Bring-up isolation switches: INF_NOSTARS / INF_NOSKY.
    if (!field.empty() && std::getenv("INF_NOSTARS") == nullptr) {
      star_field_mesh = rhi->create_mesh_mat(field.data(), field.size());
    }
    const inf::core::WorldTime sky_t1 = sky_clock.now();
    const int threads =
        std::max(2U, std::thread::hardware_concurrency()) - 1;
    const inf::app::SkyBakeResult bake = inf::app::bake_deep_sky(
        galaxy_octree.density(), nebula_field, cluster_field, *seed, view,
        kSkyFaceSize, threads);
    if (sky_texture == 0) {
      sky_texture = rhi->create_planet_texture(kSkyFaceSize);
    }
    for (std::uint32_t face = 0; face < 6; ++face) {
      rhi->update_planet_face(sky_texture, face, bake.luminance_half[face].data(),
                              bake.chroma_rgba[face].data());
    }
    const inf::core::WorldTime sky_t2 = sky_clock.now();
    std::printf(
        "deep sky: %zu stars (brightest m=%.1f, %zu cells, %.0f ms), band %ux%u "
        "(%.0f ms)\n",
        stats.star_count, stats.brightest_apparent_mag, stats.cells_visited,
        static_cast<double>(sky_t1 - sky_t0) * 1e-6, kSkyFaceSize, kSkyFaceSize,
        static_cast<double>(sky_t2 - sky_t1) * 1e-6);
  };
  rebuild_deep_sky(galactic_pos);
  // Sea shell (spec section 5): one translucent sphere at sea level,
  // EarthLike only. Zero shading effort by design. Rebuilt per anchor.
  std::uint32_t sea_mesh = 0;
  double sea_radius = 0.0;
  // Land impostor (T0015 follow-up): a coarse elevation-displaced sphere
  // sampled ONCE per anchor. From orbit the chunk terrain is hidden (its
  // coarse LOD aliases into flickering land/water patches); this static
  // mesh carries the continents instead — same land, no churn.
  std::uint32_t land_mesh = 0;
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
      vertices.reserve(static_cast<std::size_t>(kSlices) * kStacks * 48);
      const auto point = [&](int slice, int stack, float out[8]) {
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
        // Classify at the TRUE surface radius (the mesh is sunk 300 m).
        const double true_r = r + 300.0;
        const auto vm = anchor->field->material().classify(
            nx * true_r, ny * true_r, nz * true_r, normal.x, normal.y, normal.z);
        out[6] = static_cast<float>(static_cast<int>(vm.mat0) * 256 +
                                    static_cast<int>(vm.mat1));
        out[7] = vm.blend;
      };
      for (int stack = 0; stack < kStacks; ++stack) {
        for (int slice = 0; slice < kSlices; ++slice) {
          float p00[8], p10[8], p01[8], p11[8];
          point(slice, stack, p00);
          point(slice + 1, stack, p10);
          point(slice, stack + 1, p01);
          point(slice + 1, stack + 1, p11);
          const float* quad[6] = {p00, p10, p11, p00, p11, p01};
          for (const float* v : quad) {
            vertices.insert(vertices.end(), v, v + 8);
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
  const inf::core::LocalClock world_clock;
  inf::core::WorldTime last_time = world_clock.now();
  double fps_accum = 0.0;
  int fps_frames = 0;
  bool e_was_down = false;
  bool m_was_down = false;
  bool esc_was_down = false;
  bool f9_was_down = false;
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
  std::vector<float> mat_scratch;
  std::vector<inf::render::Rhi::DrawItem> items;

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
  struct BakeResult {
    std::uint32_t key{0};
    double radius_m{0.0};
    inf::gen::PlanetTexture texture;
  };
  std::mutex bake_mutex;
  std::vector<BakeResult> bake_done;
  std::atomic<bool> bake_quit{false};
  std::thread bake_thread;
  // Restartable (T0017): a jump regenerates the system, so the worker is
  // stopped, textures dropped, and a new worker started for the arrival
  // system.
  const auto start_bake_worker = [&](const inf::gen::StarSystemParams system_copy,
                                     const inf::gen::SystemCell cell_copy) {
    bake_quit.store(false);
    bake_thread = std::thread([&bake_mutex, &bake_done, &bake_quit, &body_tex_key,
                               seed_copy = *seed, system_copy, cell_copy]() {
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
    }
    for (int slot = 0; slot < inf::gen::kMaxPlanetSlots; ++slot) {
      const auto& entry = system_copy.planets[static_cast<std::size_t>(slot)];
      for (std::size_t mi = 0; mi < entry.moons.size(); ++mi) {
        jobs.push_back({slot, static_cast<int>(mi), 192U});
      }
    }
    for (const Job& job : jobs) {
      if (bake_quit.load()) {
        return;
      }
      BakeResult result;
      result.key = body_tex_key(job.slot, job.moon);
      if (job.moon < 0) {
        const inf::gen::BodyHandle body =
            inf::gen::body_for_system_slot(seed_copy, cell_copy, job.slot);
        const inf::gen::PlanetParams planet =
            inf::gen::planet_params_for_slot(system_copy, job.slot, body);
        const inf::gen::TerrainField field(body.entity, planet);
        result.radius_m = planet.radius_m.to_double();
        result.texture = inf::gen::bake_planet_texture(field, job.size);
      } else {
        const inf::gen::BodyHandle body =
            inf::gen::body_for_system_moon(seed_copy, cell_copy, job.slot, job.moon);
        const inf::gen::PlanetParams planet =
            inf::gen::planet_params_for_moon(system_copy, job.slot, job.moon, body);
        const inf::gen::TerrainField field(body.entity, planet);
        result.radius_m = planet.radius_m.to_double();
        result.texture = inf::gen::bake_planet_texture(field, job.size);
      }
      const std::lock_guard<std::mutex> lock(bake_mutex);
      bake_done.push_back(std::move(result));
    }
    });
  };
  const auto stop_bake_worker = [&] {
    bake_quit.store(true);
    if (bake_thread.joinable()) {
      bake_thread.join();
    }
    const std::lock_guard<std::mutex> lock(bake_mutex);
    bake_done.clear();
  };
  start_bake_worker(system, current_cell);
  const auto upload_finished_bakes = [&] {
    std::vector<BakeResult> ready;
    {
      const std::lock_guard<std::mutex> lock(bake_mutex);
      ready.swap(bake_done);
    }
    for (BakeResult& result : ready) {
      const std::uint32_t handle =
          rhi->create_planet_texture(result.texture.face_size);
      for (std::uint32_t face = 0; face < 6; ++face) {
        rhi->update_planet_face(handle, face,
                                result.texture.faces[face].height_half.data(),
                                result.texture.faces[face].rgba.data());
      }
      BodyTexture entry;
      entry.handle = handle;
      entry.amp_over_radius = static_cast<float>(
          static_cast<double>(result.texture.height_amp_m) / result.radius_m);
      entry.slope_scale = static_cast<float>(
          static_cast<double>(result.texture.height_amp_m) *
          static_cast<double>(result.texture.face_size) / (3.1415926 * result.radius_m));
      body_textures[result.key] = entry;
    }
  };

  long frame = 0;
  while (glfwWindowShouldClose(window) == GLFW_FALSE) {
    glfwPollEvents();
    upload_finished_bakes();
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

    player.update(input);

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
    if (map_phase == MapPhase::Off && player.mode() == inf::sim::PlayerMode::Flight) {
      const SVec3 at = player.position();
      const double anchor_gap = inf::sim::length(at) - anchor->radius;
      const ClosestBody candidate = closest_body();
      const bool is_current =
          candidate.slot == anchor->slot && candidate.moon == anchor->moon;
      if (!is_current && candidate.gap < anchor_gap * 0.5) {
        save_anchor_edits(*anchor);
        for (auto& [addr, chunk] : loaded) {
          rhi->destroy_mesh(chunk.mesh_id);
        }
        loaded.clear();
        pending_ready.clear();  // old anchor's frames are meaningless now
        const SVec3 new_pos = at - candidate.center;
        anchor = make_anchor(*seed, seed_text, system, current_cell, candidate.slot,
                             candidate.moon, std::nullopt, nullptr);
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
                    inf::gen::to_string(anchor->planet.type),
                    candidate.moon >= 0 ? "moon" : "planet", anchor->radius / 1000.0);
      }
    }

    // --- J: interstellar jump (T0017 WP6) -------------------------------
    // Tap J: cone-search along the nose (20 ly, ~15 deg) and arm the
    // nearest system; tap again to cycle. Turning the ship clears the
    // selection. HOLD J (0.75 s) to jump — a stray tap must never fling
    // anyone 20 light-years.
    bool j_down = glfwGetKey(window, GLFW_KEY_J) == GLFW_PRESS;
    bool jump_confirm_scripted = false;
    if (script_jump) {
      script_jump = false;
      j_down = true;               // acts as a fresh J press...
      j_was_down = false;
      jump_confirm_scripted = true;  // ...that also confirms instantly
    }
    if (map_phase == MapPhase::Off && player.mode() == inf::sim::PlayerMode::Flight &&
        jump_timer <= 0.0) {
      const SVec3 fwd = inf::sim::normalize(player.forward());
      if (jump_index >= 0 && inf::sim::dot(fwd, jump_sel_forward) < 0.9848) {
        jump_index = -1;  // turned away (> ~10 deg): selection cleared
      }
      if (j_down && !j_was_down) {
        if (jump_index < 0) {
          jump_candidates.clear();
          std::vector<inf::gen::GalaxyOctree::CellId> cells;
          galaxy_octree.systems_in_ball(
              inf::gen::Dir3{inf::det::Real(galactic_pos.x), inf::det::Real(galactic_pos.y),
                             inf::det::Real(galactic_pos.z)},
              inf::det::Real(20.0 * inf::gen::kLightYearM), 512, &cells);
          for (const auto& cell : cells) {
            const inf::gen::SystemCell sys_cell{cell.x, cell.y, cell.z, cell.level};
            if (sys_cell == current_cell) {
              continue;
            }
            const inf::gen::Dir3 p = galaxy_octree.system_position_m(cell);
            const SVec3 pos{p.x.to_double(), p.y.to_double(), p.z.to_double()};
            const SVec3 rel = pos - galactic_pos;
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
        system = inf::gen::generate_system(inf::gen::system_key_for(*seed, current_cell));
        const int arrival_slot = inf::gen::default_landable_slot(system);
        anchor = make_anchor(*seed, seed_text, system, current_cell, arrival_slot, -1,
                             std::nullopt, nullptr);
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
        // New vantage, new sky: rebake the band + star field while the
        // transition still covers the screen (~0.5 s, deliberate).
        rebuild_deep_sky(galactic_pos);
        std::printf("jump: arrived at %s — %s (slot %d, %s, radius %.0f km)\n",
                    jump_sel_name.c_str(),
                    slot_names[static_cast<std::size_t>(arrival_slot)].c_str(),
                    arrival_slot, inf::gen::to_string(anchor->planet.type),
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
          player.push_out(planet_local[static_cast<std::size_t>(slot)],
                          entry.phys.radius_m.to_double() * 1.02 + 5.0);
        }
      }
      for (const MoonInstance& moon : moons_local) {
        if (!moon.is_anchor) {
          player.push_out(moon.pos, moon.radius * 1.02 + 5.0);
        }
      }
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
    const bool m_down = glfwGetKey(window, GLFW_KEY_M) == GLFW_PRESS;
    const bool m_pressed =
        (m_down && !m_was_down) || (map_demo && frame == 100) || script_map;
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
        // material/v1 (T0015 WP3): classify each vertex (planet-local
        // position + normal) and upload the 8-float terrain layout.
        const auto& mesh_vertices = data->mesh.vertices;
        const std::size_t vertex_count = mesh_vertices.size() / 6;
        mat_scratch.resize(vertex_count * 8);
        const auto& material = anchor->field->material();
        for (std::size_t v = 0; v < vertex_count; ++v) {
          const float* in_v = mesh_vertices.data() + v * 6;
          float* out_v = mat_scratch.data() + v * 8;
          std::memcpy(out_v, in_v, 6 * sizeof(float));
          const auto vm = material.classify(
              data->mesh.origin[0] + in_v[0], data->mesh.origin[1] + in_v[1],
              data->mesh.origin[2] + in_v[2], in_v[3], in_v[4], in_v[5]);
          out_v[6] = static_cast<float>(static_cast<int>(vm.mat0) * 256 +
                                        static_cast<int>(vm.mat1));
          out_v[7] = vm.blend;
        }
        LoadedChunk chunk;
        chunk.mesh_id = rhi->create_mesh_mat(mat_scratch.data(), mat_scratch.size());
        chunk.origin =
            RVec3{data->mesh.origin[0], data->mesh.origin[1], data->mesh.origin[2]};
        loaded[addr] = chunk;
      }
      it = pending_ready.erase(it);
      ++uploads;
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
        item.color[0] = star.tint[0];
        item.color[1] = star.tint[1];
        item.color[2] = star.tint[2];
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
        item.color[0] = star.tint[0];
        item.color[1] = star.tint[1];
        item.color[2] = star.tint[2];
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
    // the baked deep-sky cube map alone (density -> 0 in the shader), in
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
      dome.extra[0] = 1.0f;  // band gain
      items.push_back(dome);
      // WP2: the resolved-star field, one static mesh of billboards at
      // infinity (rotation-only transform; the shader pins depth just
      // above the dome so real geometry occludes stars).
      if (star_field_mesh != 0) {
        inf::render::Rhi::DrawItem stars_item;
        stars_item.mesh = star_field_mesh;
        std::memcpy(stars_item.mvp, view_projection.m, sizeof(view_projection.m));
        stars_item.mode = 7;
        const double star_half_px = 5.0;
        stars_item.extra[0] = static_cast<float>(2.0 * star_half_px / state.width);
        stars_item.extra[1] = static_cast<float>(2.0 * star_half_px / state.height);
        items.push_back(stars_item);
      }
    }
    for (const auto& [addr, chunk] : loaded) {
      if (!show_surface) {
        break;  // impostor-only from high orbit
      }
      inf::render::Rhi::DrawItem item;
      item.mesh = chunk.mesh_id;
      const RVec3 translation = chunk.origin - camera_pos;
      const Mat4 model = inf::render::translate(translation);
      const Mat4 mvp = inf::render::mul(view_projection, model);
      std::memcpy(item.mvp, mvp.m, sizeof(mvp.m));
      // Lit items carry their translation for the orbit normal blend.
      item.aux[0] = static_cast<float>(translation.x);
      item.aux[1] = static_cast<float>(translation.y);
      item.aux[2] = static_cast<float>(translation.z);
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
      // The static land impostor is ALWAYS drawn (map off): from orbit it
      // IS the planet (chunks hidden); nearer in it sits 300 m under the
      // real terrain as the streaming backstop, so chunks arriving late
      // refine the picture instead of flipping ocean into land.
      if (land_mesh != 0) {
        const RVec3 rel = to_render(SVec3{0.0, 0.0, 0.0}) - camera_pos;
        const Mat4 model = inf::render::translate(rel);
        const Mat4 mvp = inf::render::mul(view_projection, model);
        inf::render::Rhi::DrawItem item;
        item.mesh = land_mesh;
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
    if (map_phase == MapPhase::Off && player.mode() == inf::sim::PlayerMode::Flight) {
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
      frame_params.altitude_frac = static_cast<float>(std::clamp(dome_alt_frac, 0.0, 9.0));
      set3(frame_params.planet_center, RVec3{0.0, 0.0, 0.0} - camera_pos);
      frame_params.sea_radius_m = static_cast<float>(sea_radius);
      frame_params.palette_shift = static_cast<float>(
          static_cast<double>(anchor->planet.palette_id % 201U) / 100.0 - 1.0);
      // Blend terrain normals toward the sphere radial from ~0.25 radii
      // of altitude up (full sphere shading from one radius out).
      frame_params.normal_blend = static_cast<float>(
          std::clamp((altitude / anchor->radius - 0.25) / 0.75, 0.0, 1.0));
    }
    // Verification captures (--capture): grab the final rendered frame,
    // when the scene has had time to stream in.
    if (capture_text != nullptr && max_frames > 0 && frame == max_frames - 1) {
      rhi->request_capture(capture_text);
    }
    // Player-state sidecar for active recordings (frame-by-frame
    // correlation when analyzing a dumped sequence).
    if (rhi->recording_active() && !rec_dir_current.empty()) {
      std::FILE* meta = std::fopen((rec_dir_current + "/meta.csv").c_str(), "a");
      if (meta != nullptr) {
        const SVec3 meta_pos = player.position();
        std::fprintf(meta, "%.4f,%.1f,%.1f,%.1f,%.2f,%.1f,%d\n", frame_params.time_s,
                     meta_pos.x, meta_pos.y, meta_pos.z, player.speed(), player.altitude(),
                     static_cast<int>(player.mode()));
        std::fclose(meta);
      }
    }
    rhi->render_frame(frame_params, items.data(), items.size());

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
  stop_bake_worker();
  }  // end HUD scope

  save_anchor_edits(*anchor);

  rhi.reset();
  glfwDestroyWindow(window);
  glfwTerminate();
  return EXIT_SUCCESS;
}
