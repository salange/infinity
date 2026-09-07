#include "galaxy_flythrough.hpp"
#include "deep_sky_render.hpp"
#include "gen/universe.hpp"
#include "hud.hpp"
#include "render/math.hpp"
#include "render/rhi.hpp"
#include <GLFW/glfw3.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

namespace inf::app {
namespace {
using Clock = std::chrono::steady_clock;
double seconds(Clock::time_point a, Clock::time_point b) {
  return std::chrono::duration<double>(b - a).count();
}
struct Sample {
  double time{}, bake_ms{};
  SkyBakeResult sky;
  std::vector<float> stars;
  std::uint32_t mesh{0};
  int slot{0};
};
struct Frame {
  double time{}, ms{}, upload_ms{}, sky_age{}, bake_ms{};
  bool presented{};
  int width{}, height{};
};
} // namespace

void run_galaxy_flythrough(GLFWwindow *window, render::Rhi &rhi, Hud &hud,
                           std::uint32_t quad, const core::Seed128 &seed,
                           const gen::GalaxyParams &galaxy, int &width,
                           int &height, const char *profile_path,
                           const char *capture_prefix) {
  const GalaxyRoute route(galaxy);
  // Fixed route sampling, at most 128 queued samples + the displayed pair.
  // This is disposable render work, reconstructed from the current seed each
  // run.
  constexpr double interval = 0.5;
  constexpr std::uint32_t face_size = 512;
  std::mutex mutex;
  std::condition_variable ready;
  std::deque<Sample> queue;
  std::atomic<bool> cancel{false};
  std::string worker_error;
  std::thread worker([&] {
    try {
      const gen::GalaxyDensity density(galaxy);
      const gen::GalaxyOctree octree(gen::home_galaxy_key(seed), galaxy);
      const gen::NebulaField nebula(gen::home_galaxy_key(seed), galaxy);
      const gen::StarClusterField clusters(gen::home_galaxy_key(seed), galaxy);
      for (int index = 0; index <= 240 && !cancel.load(); ++index) {
        {
          std::unique_lock lock(mutex);
          ready.wait(lock, [&] { return cancel.load() || queue.size() < 128; });
        }
        if (cancel.load())
          break;
        const auto start = Clock::now();
        Sample sample;
        sample.time = index * interval;
        const auto pos = route.position(sample.time);
        SkyView view{};
        view.interplanetary_dust = false;
        view.eye_m = {det::Real(pos.x), det::Real(pos.y), det::Real(pos.z)};
        // No local system: omit its zodiacal wedge.
        sample.sky = bake_deep_sky(
            density, nebula, clusters, seed, view, face_size,
            static_cast<int>(std::max(4U, std::thread::hardware_concurrency()) -
                             2),
            &cancel, false);
        if (!cancel.load())
          sample.stars = build_star_field_mesh(octree, view.eye_m, 8.3, 60000,
                                               nullptr, true);
        sample.bake_ms = seconds(start, Clock::now()) * 1000.0;
        if (cancel.load())
          break;
        std::lock_guard lock(mutex);
        queue.push_back(std::move(sample));
      }
    } catch (const std::exception &e) {
      std::lock_guard lock(mutex);
      worker_error = e.what();
    }
  });
  const auto texture = rhi.create_planet_texture(face_size, 2);
  Sample left, right;
  const auto upload = [&](Sample &sample, int slot) {
    sample.slot = slot;
    for (std::uint32_t face = 0; face < 6; ++face)
      rhi.update_planet_face(texture,
                             static_cast<std::uint32_t>(slot * 6) + face,
                             sample.sky.luminance_half[face].data(),
                             sample.sky.chroma_rgba[face].data());
    if (!sample.stars.empty())
      sample.mesh =
          rhi.create_mesh_mat(sample.stars.data(), sample.stars.size());
    sample.sky = {};
    sample.stars = {};
  };
  bool running = false, complete = false, preview_ready = false;
  std::size_t buffered = 0;
  bool f6_prev = glfwGetKey(window, GLFW_KEY_F6) == GLFW_PRESS;
  bool esc_prev = glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS;
  const auto preparation = Clock::now();
  auto epoch = preparation;
  auto previous = preparation;
  std::vector<Frame> frames;
  frames.reserve(30000);
  std::vector<render::Rhi::DrawItem> items;
  std::printf("galaxy demo: %s; %dx%d; diameter %.3f ly; route %.3f ly; speed "
              "%.6g m/s; budget 16.7 ms\n",
              rhi.adapter_info().c_str(), width, height,
              galaxy.diameter_ly.to_double(),
              route.extent_m * 2.0 / gen::kLightYearM, route.speed_mps());
  std::fflush(stdout);
  double preparation_s = 0.0;
  int next_capture = 0;
  while (!glfwWindowShouldClose(window)) {
    glfwPollEvents();
    const bool f6 = glfwGetKey(window, GLFW_KEY_F6) == GLFW_PRESS;
    const bool esc = glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS;
    if ((f6 && !f6_prev) || (esc && !esc_prev))
      break;
    f6_prev = f6;
    esc_prev = esc;
#ifdef __APPLE__
    const bool mod = glfwGetKey(window, GLFW_KEY_LEFT_SUPER) == GLFW_PRESS ||
                     glfwGetKey(window, GLFW_KEY_RIGHT_SUPER) == GLFW_PRESS;
#else
    const bool mod = glfwGetKey(window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS ||
                     glfwGetKey(window, GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS;
#endif
    if (mod && glfwGetKey(window, GLFW_KEY_Q) == GLFW_PRESS) {
      glfwSetWindowShouldClose(window, GLFW_TRUE);
      break;
    }
    auto now = Clock::now();
    double t = running ? std::min(120.0, seconds(epoch, now)) : 0.0;
    const auto upload_start = Clock::now();
    {
      std::lock_guard lock(mutex);
      if (!worker_error.empty()) {
        std::fprintf(stderr, "galaxy demo: %s\n", worker_error.c_str());
        break;
      }
      buffered = queue.size();
      if (!running && !preview_ready && !queue.empty()) {
        Sample preview = queue.front();
        upload(preview, 0);
        left = std::move(preview);
        preview_ready = true;
      }
      if (!running && queue.size() >= 72) {
        if (left.mesh)
          rhi.destroy_mesh(left.mesh);
        left = std::move(queue.front());
        queue.pop_front();
        right = std::move(queue.front());
        queue.pop_front();
        upload(left, 0);
        upload(right, 1);
        epoch = Clock::now();
        preparation_s = seconds(preparation, epoch);
        previous = epoch;
        running = true;
      }
      // After an OS suspension, discard obsolete CPU views before uploading.
      // At most two samples reach the GPU in one display frame.
      while (running && queue.size() > 2 && queue[1].time <= t)
        queue.pop_front();
      while (running && t > right.time && !queue.empty()) {
        const int slot = left.slot;
        if (left.mesh)
          rhi.destroy_mesh(left.mesh);
        left = std::move(right);
        right = std::move(queue.front());
        queue.pop_front();
        upload(right, slot);
      }
    }
    ready.notify_one();
    const double upload_ms = seconds(upload_start, Clock::now()) * 1000.0;
    render::Rhi::FrameParams params;
    params.time_s = static_cast<float>(t);
    params.tan_half_y = static_cast<float>(std::tan(1.1 * 0.5));
    params.tan_half_x =
        params.tan_half_y * static_cast<float>(width) / std::max(1, height);
    // Smoothly turn to look back during the second half, keeping the core in
    // view on exit. The route itself remains one straight, constant-speed
    // diameter.
    const double u = std::clamp((t - 60.0) / 60.0, 0.0, 1.0);
    const double angle = 3.141592653589793 * u * u * (3.0 - 2.0 * u);
    const auto side =
        sim::normalize(sim::cross(route.axis, sim::Vec3{0, 0, 1}));
    const auto fwd = route.axis * std::cos(angle) + side * std::sin(angle);
    const auto right_dir = sim::normalize(sim::cross(fwd, sim::Vec3{0, 0, 1}));
    const auto up = sim::cross(right_dir, fwd);
    const auto set = [](float *out, sim::Vec3 v) {
      out[0] = static_cast<float>(v.x);
      out[1] = static_cast<float>(v.y);
      out[2] = static_cast<float>(v.z);
    };
    set(params.cam_fwd, fwd);
    set(params.cam_right, right_dir);
    set(params.cam_up, up);
    items.clear();
    const double blend =
        running ? std::clamp((t - left.time) / interval, 0.0, 1.0) : 0.0;
    if (running || preview_ready) {
      render::Rhi::DrawItem dome;
      dome.mesh = quad;
      auto m = render::Mat4::identity();
      m.m[10] = 0.00001f;
      m.m[14] = 1e-22f;
      std::memcpy(dome.mvp, m.m, sizeof(m.m));
      dome.mode = 4;
      dome.planet_texture = texture;
      dome.extra[0] = 1.0f;
      dome.aux[0] = static_cast<float>(left.slot * 6);
      dome.aux[1] = static_cast<float>(right.slot * 6);
      dome.extra[1] = static_cast<float>(blend);
      items.push_back(dome);
      const auto projection = render::perspective(
          1.1, static_cast<double>(width) / std::max(1, height), 100000.0,
          0.001);
      const auto view =
          render::look_dir({fwd.x, fwd.y, fwd.z}, {up.x, up.y, up.z});
      const auto vp = render::mul(projection, view);
      const auto eye = route.position(t);
      const auto stars = [&](const Sample &sample, double weight) {
        if (!sample.mesh || weight <= 0.0)
          return;
        render::Rhi::DrawItem item;
        item.mesh = sample.mesh;
        item.mode = 7;
        std::memcpy(item.mvp, vp.m, sizeof(vp.m));
        item.extra[0] = 10.0f / std::max(1, width);
        item.extra[1] = 10.0f / std::max(1, height);
        item.extra[2] = 1.0f;
        item.color[3] = static_cast<float>(weight);
        const auto offset =
            (route.position(sample.time) - eye) * (1.0 / gen::kLightYearM);
        set(item.aux, offset);
        items.push_back(item);
      };
      stars(left, 1.0 - blend);
      stars(right, blend);
    }
    char status[160];
    char preparation_status[96];
    std::snprintf(preparation_status, sizeof(preparation_status),
                  "Buffering live views: %zu / 72", buffered);
    std::snprintf(status, sizeof(status), "GALAXY TRAVERSAL  %.1f / 120 s   %s",
                  t,
                  !running ? preparation_status
                           : (t < 55 ? "Entry" : (t < 65 ? "Center" : "Exit")));
    const auto hud_first = items.size();
    hud.build_map_card(&items, {status, "F6 / ESC: return to player"}, -0.95,
                       0.94, static_cast<double>(width) / std::max(1, height),
                       height);
    for (std::size_t i = hud_first; i < items.size(); ++i)
      items[i].overlay = true;
    if (running && capture_prefix != nullptr && next_capture <= 2 &&
        t >= next_capture * 60.0) {
      rhi.request_capture(std::string(capture_prefix) + "-" +
                          std::to_string(next_capture * 60) + ".ppm");
      ++next_capture;
    }
    const bool displayed = rhi.render_frame(params, items.data(), items.size());
    const auto presented = Clock::now();
    if (running) {
      frames.push_back({t, seconds(previous, presented) * 1000.0, upload_ms,
                        std::max(0.0, t - right.time), right.bake_ms, displayed,
                        width, height});
      previous = presented;
    }
    if (t >= 120.0 && displayed) {
      complete = true;
      break;
    }
  }
  const auto stop_start = Clock::now();
  if (!running)
    preparation_s = seconds(preparation, stop_start);
  cancel.store(true);
  ready.notify_one();
  worker.join();
  if (left.mesh)
    rhi.destroy_mesh(left.mesh);
  if (right.mesh)
    rhi.destroy_mesh(right.mesh);
  rhi.destroy_planet_texture(texture);
  queue.clear();
  const double stop_ms = seconds(stop_start, Clock::now()) * 1000.0;
  if (profile_path != nullptr) {
    std::ofstream out(profile_path);
    out << "# adapter=" << rhi.adapter_info() << ";resolution=" << width << 'x'
        << height << ";face_size=" << face_size
        << ";sample_interval_s=" << interval
        << ";buffer_capacity=128;prebuffer=72;complete=" << complete
        << ";preparation_s=" << preparation_s << ";stop_ms=" << stop_ms
        << ";seed=" << core::to_hex(seed)
        << ";diameter_ly=" << galaxy.diameter_ly.to_double()
        << ";speed_mps=" << route.speed_mps() << '\n';
    out << "elapsed_s,frame_ms,upload_ms,sky_lag_s,bake_ms,presented,width,"
           "height\n";
    for (const auto &f : frames)
      out << f.time << ',' << f.ms << ',' << f.upload_ms << ',' << f.sky_age
          << ',' << f.bake_ms << ',' << f.presented << ',' << f.width << ','
          << f.height << '\n';
    if (!out)
      std::fprintf(stderr, "Could not write galaxy profile: %s\n",
                   profile_path);
  }
  double worst = 0;
  std::size_t missed = 0, late = 0;
  for (const auto &f : frames) {
    worst = std::max(worst, f.ms);
    missed += f.ms > 16.7;
    late += f.sky_age > 0;
  }
  std::printf("galaxy demo: %s; %zu frames; max %.3f ms; >16.7 ms %zu; "
              "stale-sky frames %zu; preparation %.3f s; stop %.3f ms\n",
              complete ? "complete" : "cancelled", frames.size(), worst, missed,
              late, preparation_s, stop_ms);
  std::fflush(stdout);
}
} // namespace inf::app
