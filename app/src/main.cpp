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
#include "render/math.hpp"
#include "render/rhi.hpp"
#include "world/chunk_manager.hpp"

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
  Vec3 origin;
};

}  // namespace

int main(int argc, char** argv) {
  long max_frames = 0;
  bool autofly = false;
  const char* seed_text = "7";
  const char* type_text = nullptr;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
      max_frames = std::strtol(argv[++i], nullptr, 10);
    } else if (std::strcmp(argv[i], "--autofly") == 0) {
      autofly = true;  // scripted orbit->surface descent (smoke/capture)
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
  // Finest lod: ~32 m chunks => ~1 m voxels (spec section 6).
  std::uint8_t max_lod = 8;
  while ((2.0 * radius) / static_cast<double>(std::uint64_t{1} << max_lod) > 32.0 &&
         max_lod < 16) {
    ++max_lod;
  }
  config.max_lod = max_lod;
  inf::world::ChunkManager manager(body, planet, config);

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

  // Spawn in low orbit above the +X face center; free camera flies down.
  Vec3 camera_pos{radius * 2.2, 0.0, 0.0};
  double yaw = 0.0;
  double pitch = 0.0;

  AppState state{rhi.get(), 1280, 720};
  glfwSetWindowUserPointer(window, &state);
  glfwSetFramebufferSizeCallback(window, framebuffer_size_callback);
  glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
  double last_mx = 0.0;
  double last_my = 0.0;
  glfwGetCursorPos(window, &last_mx, &last_my);
  double last_time = glfwGetTime();
  double fps_accum = 0.0;
  int fps_frames = 0;

  std::unordered_map<inf::core::ChunkAddr, LoadedChunk, AddrHash> loaded;
  std::vector<inf::render::Rhi::DrawItem> items;

  long frame = 0;
  while (glfwWindowShouldClose(window) == GLFW_FALSE) {
    glfwPollEvents();
    if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
      glfwSetWindowShouldClose(window, GLFW_TRUE);
    }
    const double now = glfwGetTime();
    const double dt = now > last_time ? now - last_time : 0.0;
    last_time = now;

    // --- camera --------------------------------------------------------
    double mx = 0.0;
    double my = 0.0;
    glfwGetCursorPos(window, &mx, &my);
    yaw += (mx - last_mx) * 0.002;
    pitch -= (my - last_my) * 0.002;
    pitch = pitch > 1.55 ? 1.55 : (pitch < -1.55 ? -1.55 : pitch);
    last_mx = mx;
    last_my = my;

    if (autofly && max_frames > 0) {
      // Scripted descent above the +X face: cubic ease from 1.2R altitude
      // down to ~120 m, with a slow lateral drift; camera pitches from
      // level to slightly downward.
      const double t = static_cast<double>(frame) / static_cast<double>(max_frames);
      const double ease = (1.0 - t) * (1.0 - t) * (1.0 - t);
      const double drift = 0.08 * t;
      const Vec3 dir = inf::render::normalize(Vec3{1.0, drift, 0.35 * drift});
      camera_pos = dir * (radius + 120.0 + 1.2 * radius * ease);
      yaw = 0.6;
      pitch = -0.15 - 0.35 * t;
    }

    // Radial-up tangent frame at the camera (free flight, M5 refines).
    const Vec3 up = inf::render::normalize(camera_pos);
    Vec3 reference{0.0, 0.0, 1.0};
    if (std::abs(inf::render::dot(reference, up)) > 0.98) {
      reference = Vec3{0.0, 1.0, 0.0};
    }
    const Vec3 east = inf::render::normalize(inf::render::cross(reference, up));
    const Vec3 north = inf::render::cross(up, east);
    const Vec3 forward = inf::render::normalize(
        east * (std::cos(pitch) * std::sin(yaw)) + north * (std::cos(pitch) * std::cos(yaw)) +
        up * std::sin(pitch));
    const Vec3 right = inf::render::normalize(inf::render::cross(forward, up));

    // Altitude-scaled speed (M5's governor in miniature).
    const double altitude = inf::render::length(camera_pos) - radius;
    double speed = std::max(15.0, std::abs(altitude) * 0.6);
    if (glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS) {
      speed *= 4.0;
    }
    if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS) camera_pos = camera_pos + forward * (speed * dt);
    if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS) camera_pos = camera_pos - forward * (speed * dt);
    if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS) camera_pos = camera_pos + right * (speed * dt);
    if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS) camera_pos = camera_pos - right * (speed * dt);
    if (glfwGetKey(window, GLFW_KEY_Q) == GLFW_PRESS) camera_pos = camera_pos + up * (speed * dt);
    if (glfwGetKey(window, GLFW_KEY_Z) == GLFW_PRESS) camera_pos = camera_pos - up * (speed * dt);

    // --- streaming -----------------------------------------------------
    const auto events = manager.update(camera_pos.x, camera_pos.y, camera_pos.z);
    for (const auto& event : events) {
      if (event.kind == inf::world::ChunkEvent::Kind::Ready) {
        if (event.data->mesh.vertices.empty()) {
          continue;
        }
        LoadedChunk chunk;
        chunk.mesh_id = rhi->create_mesh(event.data->mesh.vertices.data(),
                                         event.data->mesh.vertices.size());
        chunk.origin = Vec3{event.data->mesh.origin[0], event.data->mesh.origin[1],
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

    // --- draw ----------------------------------------------------------
    const double aspect = static_cast<double>(state.width) / state.height;
    const double far_z = std::max(10'000.0, std::abs(altitude) * 4.0 + 2.5 * radius);
    const Mat4 projection = inf::render::perspective(1.1, aspect, 0.5, far_z);
    const Mat4 view = inf::render::look_dir(forward, up);
    items.clear();
    items.reserve(loaded.size());
    for (const auto& [addr, chunk] : loaded) {
      inf::render::Rhi::DrawItem item;
      item.mesh = chunk.mesh_id;
      const Mat4 model = inf::render::translate(chunk.origin - camera_pos);
      const Mat4 mvp = inf::render::mul(projection, inf::render::mul(view, model));
      std::memcpy(item.mvp, mvp.m, sizeof(mvp.m));
      items.push_back(item);
    }
    rhi->render_frame(0.05f, 0.06f, 0.12f, items.data(), items.size());

    fps_accum += dt;
    ++fps_frames;
    if (fps_accum >= 1.0) {
      char title[160];
      std::snprintf(title, sizeof(title),
                    "infinity — %.0f fps | %zu chunks | alt %.0f m | speed %.0f m/s",
                    fps_frames / fps_accum, loaded.size(), altitude, speed);
      glfwSetWindowTitle(window, title);
      fps_accum = 0.0;
      fps_frames = 0;
    }

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
