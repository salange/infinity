#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <GLFW/glfw3.h>

#include "core/key.hpp"
#include "core/version.hpp"
#include "gen/mesher.hpp"
#include "gen/planet.hpp"
#include "gen/terrain.hpp"
#include "render/math.hpp"
#include "render/rhi.hpp"

namespace {

using inf::render::Mat4;
using inf::render::Vec3;

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

struct DemoChunk {
  std::uint32_t mesh_id = 0;
  Vec3 origin;
};

}  // namespace

int main(int argc, char** argv) {
  long max_frames = 0;
  const char* seed_text = "7";
  const char* type_text = "EarthLike";
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
      max_frames = std::strtol(argv[++i], nullptr, 10);
    } else if (std::strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
      seed_text = argv[++i];
    } else if (std::strcmp(argv[i], "--type") == 0 && i + 1 < argc) {
      type_text = argv[++i];
    }
  }

  const auto seed = inf::core::parse_seed(seed_text);
  if (!seed.has_value()) {
    std::fprintf(stderr, "invalid seed: %s\n", seed_text);
    return EXIT_FAILURE;
  }

  // --- generate the demo chunks (M3: one lod, 3x3 columns around a spot) ---
  const inf::core::Key universe = inf::core::universe_key(*seed);
  const inf::core::Key galaxy = inf::core::derive_child(universe, inf::core::Kind::Galaxy, 0, 0, 0);
  const inf::core::Key system = inf::core::derive_child(galaxy, inf::core::Kind::System, 0);
  const inf::core::Key body = inf::core::derive_child(system, inf::core::Kind::Body, 0);

  std::optional<inf::gen::PlanetType> forced;
  for (std::uint32_t t = 0; t < 4; ++t) {
    if (std::strcmp(type_text, inf::gen::to_string(static_cast<inf::gen::PlanetType>(t))) == 0) {
      forced = static_cast<inf::gen::PlanetType>(t);
    }
  }
  const inf::gen::PlanetParams planet = inf::gen::derive_planet_params(body, forced);
  const inf::gen::TerrainField field(body, planet);

  // Chunk lod such that a chunk spans ~64 m laterally.
  const double radius = planet.radius_m.to_double();
  int lod = 0;
  while ((2.0 * radius) / static_cast<double>(std::uint64_t{1} << lod) > 64.0 && lod < 20) {
    ++lod;
  }
  const auto cells = static_cast<std::uint32_t>(std::uint64_t{1} << lod);
  const std::uint32_t ci = cells / 2;
  const std::uint32_t cj = cells / 2;

  std::printf("infinity %s (%s) — %s planet, radius %.0f m, chunk lod %d\n",
              inf::core::kVersion, inf::core::kGitHash, inf::gen::to_string(planet.type), radius,
              lod);

  // --- window & RHI --------------------------------------------------------
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

  // Mesh a 3x3 block of chunk columns; per column, the shells containing
  // the local surface (same lod everywhere: seams are exact by shared
  // corner samples).
  std::vector<DemoChunk> chunks;
  double spawn_r = 0.0;
  for (int di = -2; di <= 2; ++di) {
    for (int dj = -2; dj <= 2; ++dj) {
      inf::core::ChunkAddr addr{};
      addr.face = 0;
      addr.lod = static_cast<std::uint8_t>(lod);
      addr.i = ci + static_cast<std::uint32_t>(di);
      addr.j = cj + static_cast<std::uint32_t>(dj);
      // Find the shell containing the surface at the column center.
      inf::gen::ChunkGrid probe = inf::gen::ChunkGrid::from_addr(addr, planet);
      const inf::gen::Dir3 center_dir = inf::gen::face_uv_to_dir(inf::gen::FaceUV{
          addr.face, inf::det::lerp(probe.u0, probe.u1, inf::det::Real(0.5)),
          inf::det::lerp(probe.v0, probe.v1, inf::det::Real(0.5))});
      const double elevation = field.elevation_m(center_dir).to_double();
      const double thickness = probe.r1.to_double() - probe.r0.to_double();
      const int shell_mid = static_cast<int>(std::floor(elevation / thickness + 0.5));
      if (di == 0 && dj == 0) {
        spawn_r = radius + elevation;
      }
      for (int shell = shell_mid - 1; shell <= shell_mid + 1; ++shell) {
        addr.shell = static_cast<std::int16_t>(shell);
        const inf::gen::ChunkGrid grid = inf::gen::ChunkGrid::from_addr(addr, planet);
        const auto densities = inf::gen::sample_chunk_density(field, grid);
        const inf::gen::ChunkMesh mesh = inf::gen::mesh_chunk(grid, densities);
        if (mesh.vertices.empty()) {
          continue;
        }
        DemoChunk chunk;
        chunk.mesh_id = rhi->create_mesh(mesh.vertices.data(), mesh.vertices.size());
        chunk.origin = Vec3{mesh.origin[0], mesh.origin[1], mesh.origin[2]};
        chunks.push_back(chunk);
      }
    }
  }
  std::printf("meshed %zu chunks\n", chunks.size());

  // --- camera --------------------------------------------------------------
  // Spawn above the center column's surface, radial up.
  inf::gen::ChunkGrid center_grid = inf::gen::ChunkGrid::from_addr(
      inf::core::ChunkAddr{0, static_cast<std::uint8_t>(lod), ci, cj, 0}, planet);
  const inf::gen::Dir3 up_dir = inf::gen::face_uv_to_dir(inf::gen::FaceUV{
      0, inf::det::lerp(center_grid.u0, center_grid.u1, inf::det::Real(0.5)),
      inf::det::lerp(center_grid.v0, center_grid.v1, inf::det::Real(0.5))});
  const Vec3 up{up_dir.x.to_double(), up_dir.y.to_double(), up_dir.z.to_double()};
  Vec3 camera_pos = up * (spawn_r + 90.0);
  double yaw = 0.0;
  double pitch = -0.55;

  AppState state{rhi.get(), 1280, 720};
  glfwSetWindowUserPointer(window, &state);
  glfwSetFramebufferSizeCallback(window, framebuffer_size_callback);
  glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
  double last_mx = 0.0;
  double last_my = 0.0;
  glfwGetCursorPos(window, &last_mx, &last_my);
  double last_time = glfwGetTime();

  long frame = 0;
  std::vector<inf::render::Rhi::DrawItem> items(chunks.size());
  while (glfwWindowShouldClose(window) == GLFW_FALSE) {
    glfwPollEvents();
    if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
      glfwSetWindowShouldClose(window, GLFW_TRUE);
    }

    const double now = glfwGetTime();
    const double dt = now - last_time;
    last_time = now;

    // Mouse look (tangent-frame free camera; proper radial-up controller
    // lands in M6).
    double mx = 0.0;
    double my = 0.0;
    glfwGetCursorPos(window, &mx, &my);
    yaw += (mx - last_mx) * 0.002;
    pitch -= (my - last_my) * 0.002;
    pitch = pitch > 1.5 ? 1.5 : (pitch < -1.5 ? -1.5 : pitch);
    last_mx = mx;
    last_my = my;

    // Camera basis in the tangent frame at the spawn point.
    const Vec3 east = inf::render::normalize(inf::render::cross(Vec3{0, 0, 1}, up));
    const Vec3 north = inf::render::cross(up, east);
    const Vec3 forward = inf::render::normalize(
        east * (std::cos(pitch) * std::sin(yaw)) + north * (std::cos(pitch) * std::cos(yaw)) +
        up * std::sin(pitch));
    const Vec3 right = inf::render::normalize(inf::render::cross(forward, up));

    double speed = 20.0;
    if (glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS) {
      speed = 120.0;
    }
    if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS) camera_pos = camera_pos + forward * (speed * dt);
    if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS) camera_pos = camera_pos - forward * (speed * dt);
    if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS) camera_pos = camera_pos + right * (speed * dt);
    if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS) camera_pos = camera_pos - right * (speed * dt);
    if (glfwGetKey(window, GLFW_KEY_Q) == GLFW_PRESS) camera_pos = camera_pos + up * (speed * dt);
    if (glfwGetKey(window, GLFW_KEY_Z) == GLFW_PRESS) camera_pos = camera_pos - up * (speed * dt);

    const double aspect = static_cast<double>(state.width) / state.height;
    const Mat4 projection = inf::render::perspective(1.1, aspect, 0.1, 50'000.0);
    const Mat4 view = inf::render::look_dir(forward, up);
    for (std::size_t i = 0; i < chunks.size(); ++i) {
      // Camera-relative: origin-to-camera offset computed in doubles,
      // collapsed before any f32 sees planet magnitudes (spec section 7).
      const Vec3 rel = chunks[i].origin - camera_pos;
      const Mat4 model = inf::render::translate(rel);
      items[i].mesh = chunks[i].mesh_id;
      const Mat4 mvp = inf::render::mul(projection, inf::render::mul(view, model));
      std::memcpy(items[i].mvp, mvp.m, sizeof(mvp.m));
    }
    rhi->render_frame(0.55f, 0.75f, 0.95f, items.data(), items.size());

    ++frame;
    if (max_frames > 0 && frame >= max_frames) {
      glfwSetWindowShouldClose(window, GLFW_TRUE);
    }
  }

  rhi.reset();
  glfwDestroyWindow(window);
  glfwTerminate();
  return EXIT_SUCCESS;
}
