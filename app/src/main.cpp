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

#include "core/key.hpp"
#include "core/version.hpp"
#include "gen/planet.hpp"
#include "hud.hpp"
#include "render/math.hpp"
#include "render/rhi.hpp"
#include "sim/player.hpp"
#include "world/chunk_manager.hpp"
#include "world/effective_field.hpp"

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
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
      max_frames = std::strtol(argv[++i], nullptr, 10);
    } else if (std::strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
      seed_text = argv[++i];
    } else if (std::strcmp(argv[i], "--type") == 0 && i + 1 < argc) {
      type_text = argv[++i];
    } else if (std::strcmp(argv[i], "--spawn-alt") == 0 && i + 1 < argc) {
      spawn_altitude = std::strtod(argv[++i], nullptr);
    }
  }

  const auto seed = inf::core::parse_seed(seed_text);
  if (!seed.has_value()) {
    std::fprintf(stderr, "invalid seed: %s\n", seed_text);
    return EXIT_FAILURE;
  }

  const inf::core::Key universe = inf::core::universe_key(*seed);
  const inf::core::Key galaxy = inf::core::derive_child(universe, inf::core::Kind::Galaxy, 0, 0, 0);
  const inf::core::Key system = inf::core::derive_child(galaxy, inf::core::Kind::System, 0);
  const inf::core::Key body = inf::core::derive_child(system, inf::core::Kind::Body, 0);

  std::optional<inf::gen::PlanetType> forced;
  if (type_text != nullptr) {
    for (std::uint32_t t = 0; t < 4; ++t) {
      if (std::strcmp(type_text, inf::gen::to_string(static_cast<inf::gen::PlanetType>(t))) ==
          0) {
        forced = static_cast<inf::gen::PlanetType>(t);
      }
    }
  }
  const inf::gen::PlanetParams planet = inf::gen::derive_planet_params(body, forced);
  const double radius = planet.radius_m.to_double();

  inf::world::ChunkManagerConfig config;
  const unsigned hardware = std::thread::hardware_concurrency();
  config.worker_count = hardware > 4 ? (hardware - 2 > 8 ? 8 : hardware - 2) : 2;
  config.split_factor = 1.5;
  std::uint8_t max_lod = 8;
  while ((2.0 * radius) / static_cast<double>(std::uint64_t{1} << max_lod) > 32.0 &&
         max_lod < 16) {
    ++max_lod;
  }
  config.max_lod = max_lod;
  inf::world::ChunkManager manager(body, planet, config);
  const inf::world::EffectiveField effective(manager.field());
  const double spawn_r = spawn_altitude >= 0.0 ? radius + spawn_altitude : radius * 2.2;
  inf::sim::Player player(effective,
                          inf::sim::normalize(SVec3{1.0, 0.15, 0.3}) * spawn_r);

  std::printf("infinity %s (%s) — %s planet, radius %.0f m, %u workers\n", inf::core::kVersion,
              inf::core::kGitHash, inf::gen::to_string(planet.type), radius,
              config.worker_count);

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
  // Sea shell (spec section 5): one translucent sphere at sea level,
  // EarthLike only. Zero shading effort by design.
  std::uint32_t sea_mesh = 0;
  double sea_radius = 0.0;
  if (planet.type == inf::gen::PlanetType::EarthLike) {
    const std::vector<float> sphere = unit_sphere_vertices(64, 32);
    sea_mesh = rhi->create_mesh(sphere.data(), sphere.size());
    sea_radius = radius + planet.sea_level_m.to_double();
  }
  {  // HUD scope: must destruct before the RHI is torn down.
  inf::app::Hud hud(rhi.get(), &manager.field(), planet);
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
  double last_time = glfwGetTime();
  double fps_accum = 0.0;
  int fps_frames = 0;
  bool e_was_down = false;

  std::unordered_map<inf::core::ChunkAddr, LoadedChunk, AddrHash> loaded;
  std::vector<inf::render::Rhi::DrawItem> items;

  long frame = 0;
  while (glfwWindowShouldClose(window) == GLFW_FALSE) {
    glfwPollEvents();
    if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
      glfwSetWindowShouldClose(window, GLFW_TRUE);
    }
    const double now = glfwGetTime();
    const double dt = now > last_time ? std::min(now - last_time, 0.1) : 0.0;
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
    const auto events = manager.update(player_pos.x, player_pos.y, player_pos.z);
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

    // --- draw -----------------------------------------------------------
    // Simple sky (M5): blend the type's sky palette toward space black by
    // altitude within the atmosphere band.
    float sky[3] = {0.05f, 0.06f, 0.12f};
    {
      double atmosphere = planet.atmosphere_height_m.to_double();
      float palette[3] = {0.05f, 0.06f, 0.12f};
      switch (planet.type) {
        case inf::gen::PlanetType::EarthLike: palette[0] = 0.45f; palette[1] = 0.65f; palette[2] = 0.95f; break;
        case inf::gen::PlanetType::Desert: palette[0] = 0.78f; palette[1] = 0.58f; palette[2] = 0.42f; break;
        case inf::gen::PlanetType::Ice: palette[0] = 0.62f; palette[1] = 0.74f; palette[2] = 0.92f; break;
        case inf::gen::PlanetType::Barren: atmosphere = 0.0; break;
      }
      if (atmosphere > 0.0) {
        const double raw_alt = inf::render::length(to_render(player_pos)) - radius;
        double t = 1.0 - raw_alt / atmosphere;
        t = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
        t = std::pow(t, 0.7);
        for (int c = 0; c < 3; ++c) {
          sky[c] = sky[c] + static_cast<float>(t) * (palette[c] - sky[c]);
        }
      }
    }
    const RVec3 camera_pos = to_render(player_pos);
    const RVec3 cam_forward = to_render(player.forward());
    const RVec3 cam_up = to_render(player.up());
    const double altitude = inf::render::length(camera_pos) - radius;
    const double far_z = std::max(10'000.0, std::abs(altitude) * 4.0 + 2.5 * radius);
    const Mat4 projection = inf::render::perspective(kFovY, input.aspect, 0.3, far_z);
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

    // HUD: fixed center crosshair; in flight additionally the steering
    // reticle at its deflection.
    const double px = 2.0 / state.height;  // one pixel in NDC-y units
    const double cross_len = 14.0 * px;
    const double cross_thick = 2.5 * px;
    const double ar = input.aspect;
    items.push_back(hud_quad(cube_mesh, 0.0, 0.0, cross_len / ar, cross_thick, 0.9f, 0.95f, 1.0f));
    items.push_back(hud_quad(cube_mesh, 0.0, 0.0, cross_thick / ar, cross_len, 0.9f, 0.95f, 1.0f));
    if (player.mode() == inf::sim::PlayerMode::Flight ||
        player.mode() == inf::sim::PlayerMode::Takeoff) {
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

    hud.build(&items, player, measured_speed, input.aspect, state.height, dt);

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
  }  // end HUD scope

  rhi.reset();
  glfwDestroyWindow(window);
  glfwTerminate();
  return EXIT_SUCCESS;
}
