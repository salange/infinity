// cityblock — standalone city-block generator and renderer (tech faction).
// Controls: mouse look (click to capture, Esc releases), WASD move, Q/E
// down/up, Shift fast, Ctrl slow, scroll = speed, N day/night, F1 cycle
// debug views, F2 toggle SSAO, F3 shadows, F4 bloom, F5 FXAA, R new seed,
// F12 screenshot, +/- exposure.
#include <GLFW/glfw3.h>

#include <stb_image_write.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>

#include "camera.hpp"
#include "gpu.hpp"
#include "ibl.hpp"
#include "renderer.hpp"
#include "scene.hpp"
#include "textures.hpp"

namespace {

struct Args {
  std::string seed{"83"};
  int width{1600}, height{900};
  bool hidden{false};
  std::string capture;
  int frames{3};
  std::string sky{"day"};
  float sky_yaw_deg{150.0f};
  bool night{false};
  std::string assets;
  bool have_cam{false};
  cb::Vec3 cam_pos, cam_target;
  std::uint32_t msaa{4};
  bool no_context{false};
  int debug{0};
  float ev{0.0f};
  std::uint32_t tex_size{1024};
  bool vsync{true};
  int rings{2};
  int bench{0};
  int context_detail{-1};
  bool no_ssao{false};
  bool no_shadows{false};
  bool no_taa{false};
  int size{-1};
  int sweep{0};
  float sweep_step{0.03f};
  std::string sweep_out{"sweep"};
};

bool parse_vec3(const char* s, cb::Vec3* v) {
  return std::sscanf(s, "%f,%f,%f", &v->x, &v->y, &v->z) == 3;
}

Args parse(int argc, char** argv) {
  Args a;
  for (int i = 1; i < argc; ++i) {
    auto next = [&](const char* name) -> const char* {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "%s needs a value\n", name);
        std::exit(2);
      }
      return argv[++i];
    };
    if (!std::strcmp(argv[i], "--seed")) a.seed = next("--seed");
    else if (!std::strcmp(argv[i], "--width")) a.width = std::atoi(next("--width"));
    else if (!std::strcmp(argv[i], "--height")) a.height = std::atoi(next("--height"));
    else if (!std::strcmp(argv[i], "--hidden")) a.hidden = true;
    else if (!std::strcmp(argv[i], "--capture")) a.capture = next("--capture");
    else if (!std::strcmp(argv[i], "--frames")) a.frames = std::atoi(next("--frames"));
    else if (!std::strcmp(argv[i], "--sky")) a.sky = next("--sky");
    else if (!std::strcmp(argv[i], "--sky-yaw")) a.sky_yaw_deg = static_cast<float>(std::atof(next("--sky-yaw")));
    else if (!std::strcmp(argv[i], "--night")) a.night = true;
    else if (!std::strcmp(argv[i], "--assets")) a.assets = next("--assets");
    else if (!std::strcmp(argv[i], "--cam")) a.have_cam = parse_vec3(next("--cam"), &a.cam_pos) || a.have_cam;
    else if (!std::strcmp(argv[i], "--target")) { a.have_cam = parse_vec3(next("--target"), &a.cam_target); }
    else if (!std::strcmp(argv[i], "--msaa")) a.msaa = static_cast<std::uint32_t>(std::atoi(next("--msaa")));
    else if (!std::strcmp(argv[i], "--no-context")) a.no_context = true;
    else if (!std::strcmp(argv[i], "--debug")) a.debug = std::atoi(next("--debug"));
    else if (!std::strcmp(argv[i], "--ev")) a.ev = static_cast<float>(std::atof(next("--ev")));
    else if (!std::strcmp(argv[i], "--tex-size")) a.tex_size = static_cast<std::uint32_t>(std::atoi(next("--tex-size")));
    else if (!std::strcmp(argv[i], "--no-vsync")) a.vsync = false;
    else if (!std::strcmp(argv[i], "--no-ssao")) a.no_ssao = true;
    else if (!std::strcmp(argv[i], "--no-shadows")) a.no_shadows = true;
    else if (!std::strcmp(argv[i], "--no-taa")) a.no_taa = true;
    else if (!std::strcmp(argv[i], "--rings")) a.rings = std::atoi(next("--rings"));
    else if (!std::strcmp(argv[i], "--bench")) a.bench = std::atoi(next("--bench"));
    else if (!std::strcmp(argv[i], "--sweep")) a.sweep = std::atoi(next("--sweep"));
    else if (!std::strcmp(argv[i], "--sweep-step")) a.sweep_step = static_cast<float>(std::atof(next("--sweep-step")));
    else if (!std::strcmp(argv[i], "--sweep-out")) a.sweep_out = next("--sweep-out");
    else if (!std::strcmp(argv[i], "--size")) {
      const std::string t = next("--size");
      a.size = t == "small" ? 0 : (t == "medium" ? 1 : (t == "large" ? 2 : (t == "metropolis" ? 3 : -1)));
    }
    else if (!std::strcmp(argv[i], "--context-detail")) a.context_detail = std::atoi(next("--context-detail"));
    else if (!std::strcmp(argv[i], "--help") || !std::strcmp(argv[i], "-h")) {
      std::printf("cityblock [--seed S] [--width W --height H] [--hidden] [--capture out.png --frames N]\n"
                  "          [--sky day|night|sunset|file.hdr] [--sky-yaw deg] [--night] [--assets dir]\n"
                  "          [--cam x,y,z --target x,y,z] [--msaa 1|4] [--no-context] [--debug 0-5] [--ev bias]\n");
      std::exit(0);
    } else {
      std::fprintf(stderr, "unknown argument %s\n", argv[i]);
      std::exit(2);
    }
  }
  return a;
}

struct Input {
  double last_mx{0}, last_my{0};
  bool captured{false};
  bool first{true};
  double scroll{0};
};
Input g_input;

void scroll_cb(GLFWwindow*, double, double y) { g_input.scroll += y; }
void mouse_cb(GLFWwindow* w, int button, int action, int) {
  if (button == GLFW_MOUSE_BUTTON_LEFT && action == GLFW_PRESS && !g_input.captured) {
    glfwSetInputMode(w, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
    g_input.captured = true;
    g_input.first = true;
  }
}

}  // namespace

int main(int argc, char** argv) {
  Args args = parse(argc, argv);
  const std::string source_dir = CITYBLOCK_SOURCE_DIR;
  const std::string assets = args.assets.empty() ? source_dir + "/assets" : args.assets;
  const std::string shaders = source_dir + "/shaders";

  if (glfwInit() != GLFW_TRUE) {
    std::fprintf(stderr, "glfwInit failed\n");
    return 1;
  }
  glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
  if (args.hidden) glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
  GLFWwindow* window = glfwCreateWindow(args.width, args.height, "cityblock", nullptr, nullptr);
  if (window == nullptr) {
    std::fprintf(stderr, "glfwCreateWindow failed\n");
    return 1;
  }
  glfwSetScrollCallback(window, scroll_cb);
  glfwSetMouseButtonCallback(window, mouse_cb);

  cb::Gpu gpu;
  gpu.vsync = args.vsync;
  std::string error;
  if (!cb::Gpu::create(window, &gpu, &error)) {
    std::fprintf(stderr, "GPU init failed: %s\n", error.c_str());
    return 1;
  }
  std::printf("cityblock: %s, %ux%u\n", gpu.adapter_name.c_str(), gpu.width, gpu.height);

  const auto t0 = std::chrono::steady_clock::now();
  auto elapsed = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); };

  // ---- scene ---------------------------------------------------------------------
  cb::SceneParams sp;
  sp.seed = args.seed;
  sp.context_buildings = !args.no_context;
  sp.context_rings = args.rings;
  sp.context_detail = args.context_detail;
  sp.size = args.size;
  cb::Scene scene = cb::generate_scene(sp);
  std::printf("  city: %s, radius %.0f m, %d blocks, %d towers, %d standard buildings, %d plazas\n", scene.city_size.c_str(),
              scene.city_radius, scene.stats_blocks, scene.stats_towers, scene.stats_standards, scene.stats_plazas);
  std::printf("  scene: %zu opaque + %zu foliage triangles, %zu lights (%.2f s)\n", scene.opaque.triangle_count(),
              scene.foliage.triangle_count(), scene.lights.size(), elapsed());

  // ---- textures --------------------------------------------------------------------
  cb::MaterialArrays arrays = cb::load_material_arrays(gpu, assets, scene.texture_sets(), args.tex_size, true);
  std::printf("  textures loaded (%.2f s)\n", elapsed());

  // ---- environments ------------------------------------------------------------------
  auto sky_path = [&](const std::string& name) {
    if (name.size() > 4 && name.substr(name.size() - 4) == ".hdr") return name;
    return assets + "/sky/" + name + ".hdr";
  };
  const float yaw = cb::radians(args.sky_yaw_deg);
  cb::Environment env_day = cb::load_environment(gpu, sky_path(args.sky), yaw, 512, 128, true);
  if (!env_day.ok) {
    std::printf("  sky %s missing — analytic sky\n", sky_path(args.sky).c_str());
    env_day = cb::make_analytic_environment(gpu, cb::Vec3{0.4f, 0.7f, 0.5f}, 512, 128);
  }
  cb::Environment env_night = cb::load_environment(gpu, sky_path("night"), yaw, 512, 128, true);
  if (!env_night.ok) env_night = env_day;
  std::printf("  environments ready (%.2f s)\n", elapsed());

  // ---- renderer ---------------------------------------------------------------------
  cb::RenderSettings settings;
  settings.msaa = args.msaa;
  settings.debug_view = args.debug;
  settings.exposure_bias = args.ev;
  settings.ssao = !args.no_ssao;
  settings.shadows = !args.no_shadows;
  settings.taa = !args.no_taa;
  cb::Renderer renderer;
  if (!renderer.init(&gpu, shaders, settings, &error)) {
    std::fprintf(stderr, "renderer init failed: %s\n", error.c_str());
    return 1;
  }
  renderer.set_scene(scene, arrays);
  bool night = args.night;
  renderer.set_environment(night ? &env_night : &env_day, night);
  std::printf("  renderer ready, %u triangles (%.2f s)\n", renderer.triangles(), elapsed());

  cb::Camera cam;
  cam.position = args.have_cam ? args.cam_pos : scene.camera_position;
  cam.look_at_point(args.have_cam ? args.cam_target : scene.camera_target);

  if (args.sweep > 0) {
    // Temporal-artifact analysis under motion. The camera slides sideways
    // by `sweep_step` per frame; consecutive final frames are differenced,
    // and the per-pixel screen motion (from the depth buffer and the two
    // view-projections, exact for a static world) times the local gradient
    // is subtracted: that is the change a band-limited image would show.
    // The residual is temporal aliasing: shimmer, moire, crawling edges.
    std::vector<std::uint8_t> prev, cur, ids;
    std::vector<float> depth;
    std::uint32_t w = 0, h = 0;
    const int saved_debug = renderer.settings().debug_view;
    renderer.settings().debug_view = 12;
    renderer.render(cam, 0.0f, nullptr);
    renderer.read_frame(&ids, &w, &h);
    renderer.settings().debug_view = saved_debug;
    renderer.reset_history();
    std::vector<float> resid(static_cast<std::size_t>(w) * h, 0.0f), raw(resid.size(), 0.0f);
    cb::Camera c2 = cam;
    // warm-up so TAA history converges before measuring
    for (int i = 0; i < 12; ++i) renderer.render(c2, 0.0f, nullptr);
    renderer.render(c2, 0.0f, nullptr);
    renderer.read_frame(&prev, &w, &h);
    cb::Mat4 prev_vp = renderer.last_view_proj();
    auto lum = [](const std::vector<std::uint8_t>& img, std::size_t px) {
      return (0.299f * img[px * 4] + 0.587f * img[px * 4 + 1] + 0.114f * img[px * 4 + 2]) / 255.0f;
    };
    int measured = 0;
    for (int i = 0; i < args.sweep; ++i) {
      c2.position += c2.right() * args.sweep_step;
      renderer.render(c2, static_cast<float>(i + 1) * 0.016f, nullptr);
      renderer.read_frame(&cur, &w, &h);
      renderer.read_depth(&depth, &w, &h);
      const cb::Mat4 cur_vp = renderer.last_view_proj();
      const cb::Mat4 inv_cur = cb::inverse(cur_vp);
      for (std::uint32_t y = 1; y + 1 < h; ++y) {
        for (std::uint32_t x = 1; x + 1 < w; ++x) {
          const std::size_t px = static_cast<std::size_t>(y) * w + x;
          const float d = std::fabs(lum(cur, px) - lum(prev, px));
          raw[px] += d;
          // where was this pixel's surface in the previous frame?
          const float z = depth[px];
          const cb::Vec4 ndc{(static_cast<float>(x) + 0.5f) / w * 2.0f - 1.0f, 1.0f - (static_cast<float>(y) + 0.5f) / h * 2.0f, z, 1.0f};
          cb::Vec4 wp = cb::mul(inv_cur, ndc);
          cb::Vec3 world = cb::Vec3{wp.x / wp.w, wp.y / wp.w, wp.z / wp.w};
          cb::Vec4 pc = cb::mul(prev_vp, cb::Vec4{world, 1.0f});
          float mx = 0.0f, my = 0.0f;
          if (pc.w > 1e-4f) {
            mx = (pc.x / pc.w * 0.5f + 0.5f) * w - (static_cast<float>(x) + 0.5f);
            my = (0.5f - pc.y / pc.w * 0.5f) * h - (static_cast<float>(y) + 0.5f);
          }
          const float gx = 0.5f * std::fabs(lum(cur, px + 1) - lum(cur, px - 1));
          const float gy = 0.5f * std::fabs(lum(cur, px + w) - lum(cur, px - w));
          const float expected = std::fabs(mx) * gx + std::fabs(my) * gy;
          resid[px] += std::max(0.0f, d - 1.5f * expected - 0.004f);
        }
      }
      prev.swap(cur);
      prev_vp = cur_vp;
      ++measured;
    }
    const float norm = 1.0f / static_cast<float>(std::max(measured, 1));
    double mat_sum[256] = {}, mat_cnt[256] = {}, total = 0.0, total_raw = 0.0;
    std::vector<std::uint8_t> heat(static_cast<std::size_t>(w) * h * 4, 255);
    for (std::size_t px = 0; px < resid.size(); ++px) {
      const float f = resid[px] * norm;
      total += f;
      total_raw += raw[px] * norm;
      const bool sky = ids[px * 4 + 1] > 8 || ids[px * 4 + 2] > 8;
      const int id = sky ? 255 : ids[px * 4];
      mat_sum[id] += f;
      mat_cnt[id] += 1.0;
      const float v = std::min(1.0f, f * 10.0f);
      heat[px * 4 + 0] = static_cast<std::uint8_t>(255.0f * std::min(1.0f, v * 2.0f));
      heat[px * 4 + 1] = static_cast<std::uint8_t>(255.0f * std::max(0.0f, std::min(1.0f, v * 2.0f - 0.6f)));
      heat[px * 4 + 2] = static_cast<std::uint8_t>(255.0f * std::max(0.0f, 1.0f - v * 4.0f) * 0.25f);
    }
    std::printf("  sweep: %d steps of %.3f m, taa %s; mean temporal residual %.5f (raw frame difference %.5f)\n", measured,
                args.sweep_step, renderer.settings().taa ? "on" : "off", total / static_cast<double>(resid.size()),
                total_raw / static_cast<double>(resid.size()));
    struct Row { int id; double mean; double share; };
    std::vector<Row> rows;
    for (int id = 0; id < 256; ++id) {
      if (mat_cnt[id] < 200) continue;
      rows.push_back(Row{id, mat_sum[id] / mat_cnt[id], mat_sum[id] / std::max(total, 1e-9)});
    }
    std::sort(rows.begin(), rows.end(), [](const Row& p1, const Row& p2) { return p1.share > p2.share; });
    std::printf("  %-4s %-18s %-10s %-10s %s\n", "id", "material", "pixels", "residual", "share");
    for (std::size_t i = 0; i < rows.size() && i < 10; ++i) {
      const Row& r = rows[i];
      const char* name = r.id == 255 ? "(sky)" : (r.id < static_cast<int>(scene.materials.size()) ? scene.materials[static_cast<std::size_t>(r.id)].name.c_str() : "?");
      std::printf("  %-4d %-18s %-10.0f %-10.5f %.1f%%\n", r.id, name, mat_cnt[r.id], r.mean, r.share * 100.0);
    }
    stbi_write_png((args.sweep_out + "-heat.png").c_str(), static_cast<int>(w), static_cast<int>(h), 4, heat.data(), static_cast<int>(w * 4));
    renderer.capture_png(args.sweep_out + "-frame.png");
    std::printf("  wrote %s-heat.png and %s-frame.png\n", args.sweep_out.c_str(), args.sweep_out.c_str());
    renderer.shutdown();
    gpu.destroy();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
  }
  if (args.bench > 0) {
    // Offscreen benchmark: render N frames without presenting, wait for the
    // GPU, report the average. Independent of vsync and window visibility.
    for (int i = 0; i < 5; ++i) renderer.render(cam, 0.0f, nullptr);
    gpu.poll(true);
    const double b0 = elapsed();
    for (int i = 0; i < args.bench; ++i) renderer.render(cam, static_cast<float>(i) * 0.016f, nullptr);
    gpu.poll(true);
    const double b1 = elapsed();
    std::printf("  bench: %d frames, %.2f ms/frame, %u triangles, %ux%u, msaa %u, ssao %d, shadows %d\n", args.bench,
                (b1 - b0) * 1000.0 / args.bench, renderer.triangles(), gpu.width, gpu.height, settings.msaa,
                settings.ssao ? 1 : 0, settings.shadows ? 1 : 0);
    renderer.shutdown();
    gpu.destroy();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
  }
  double last = elapsed();
  const double fps_first = last;
  int frame = 0;
  double fps_t = last;
  int fps_n = 0;
  int shot = 0;
  bool key_prev[512] = {};
  auto pressed = [&](int key) {
    const bool down = glfwGetKey(window, key) == GLFW_PRESS;
    const bool p = down && !key_prev[key];
    key_prev[key] = down;
    return p;
  };
  while (!glfwWindowShouldClose(window)) {
    glfwPollEvents();
    const double now = elapsed();
    const float dt = static_cast<float>(std::min(now - last, 0.1));
    last = now;
    // resize
    int fw = 0, fh = 0;
    glfwGetFramebufferSize(window, &fw, &fh);
    if (fw > 0 && fh > 0 && (static_cast<std::uint32_t>(fw) != gpu.width || static_cast<std::uint32_t>(fh) != gpu.height)) {
      gpu.resize(static_cast<std::uint32_t>(fw), static_cast<std::uint32_t>(fh));
      renderer.resize(gpu.width, gpu.height);
    }
    // input
    if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS && g_input.captured) {
      glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
      g_input.captured = false;
    }
    if (g_input.captured) {
      double mx = 0, my = 0;
      glfwGetCursorPos(window, &mx, &my);
      if (!g_input.first) cam.look(static_cast<float>(mx - g_input.last_mx), static_cast<float>(my - g_input.last_my));
      g_input.first = false;
      g_input.last_mx = mx;
      g_input.last_my = my;
    }
    if (g_input.scroll != 0.0) {
      cam.speed = cb::clampf(cam.speed * std::pow(1.25f, static_cast<float>(g_input.scroll)), 0.5f, 400.0f);
      g_input.scroll = 0.0;
    }
    float fwd = 0, strafe = 0, up = 0;
    if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS) fwd += 1;
    if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS) fwd -= 1;
    if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS) strafe += 1;
    if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS) strafe -= 1;
    if (glfwGetKey(window, GLFW_KEY_E) == GLFW_PRESS || glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS) up += 1;
    if (glfwGetKey(window, GLFW_KEY_Q) == GLFW_PRESS) up -= 1;
    float mult = 1.0f;
    if (glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS) mult = 4.0f;
    if (glfwGetKey(window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS) mult = 0.25f;
    cam.move(fwd, strafe, up, dt, mult);
    if (pressed(GLFW_KEY_N)) {
      night = !night;
      renderer.set_environment(night ? &env_night : &env_day, night);
    }
    if (pressed(GLFW_KEY_F1)) renderer.settings().debug_view = (renderer.settings().debug_view + 1) % 10;
    if (pressed(GLFW_KEY_F2)) renderer.settings().ssao = !renderer.settings().ssao;
    if (pressed(GLFW_KEY_F3)) renderer.settings().shadows = !renderer.settings().shadows;
    if (pressed(GLFW_KEY_F4)) renderer.settings().bloom = !renderer.settings().bloom;
    if (pressed(GLFW_KEY_F5)) renderer.settings().fxaa = !renderer.settings().fxaa;
    if (pressed(GLFW_KEY_F6)) renderer.settings().taa = !renderer.settings().taa;
    if (pressed(GLFW_KEY_EQUAL) || pressed(GLFW_KEY_KP_ADD)) renderer.settings().exposure_bias += 0.25f;
    if (pressed(GLFW_KEY_MINUS) || pressed(GLFW_KEY_KP_SUBTRACT)) renderer.settings().exposure_bias -= 0.25f;
    if (pressed(GLFW_KEY_R)) {
      sp.seed = std::to_string(std::strtoull(sp.seed.c_str(), nullptr, 16) + 1);
      scene = cb::generate_scene(sp);
      renderer.set_scene(scene, arrays);
      std::printf("  seed %s: %u triangles\n", sp.seed.c_str(), renderer.triangles());
    }
    if (pressed(GLFW_KEY_P)) {
      std::printf("  camera --cam %.1f,%.1f,%.1f --target %.1f,%.1f,%.1f\n", cam.position.x, cam.position.y, cam.position.z,
                  (cam.position + cam.forward() * 100.0f).x, (cam.position + cam.forward() * 100.0f).y,
                  (cam.position + cam.forward() * 100.0f).z);
    }
    // render
    WGPUTexture surface_tex = nullptr;
    WGPUTextureView view = gpu.acquire_frame(&surface_tex);
    if (view == nullptr) continue;
    renderer.render(cam, static_cast<float>(now), view);
    if (pressed(GLFW_KEY_F12)) {
      char name[64];
      std::snprintf(name, sizeof(name), "cityblock-%02d.png", shot++);
      if (renderer.capture_png(name)) std::printf("  wrote %s\n", name);
    }
    if (!args.capture.empty() && frame == args.frames - 1) {
      if (renderer.capture_png(args.capture)) std::printf("  wrote %s\n", args.capture.c_str());
      else std::fprintf(stderr, "capture failed\n");
    }
    gpu.present();
    wgpuTextureViewRelease(view);
    wgpuTextureRelease(surface_tex);
    gpu.poll(false);
    ++frame;
    ++fps_n;
    if (now - fps_t > 2.0) {
      char title[128];
      std::snprintf(title, sizeof(title), "cityblock — %.0f fps, %u tris, speed %.0f m/s%s", fps_n / (now - fps_t),
                    renderer.triangles(), cam.speed, night ? ", night" : "");
      glfwSetWindowTitle(window, title);
      fps_t = now;
      fps_n = 0;
    }
    if (!args.capture.empty() && frame >= args.frames) break;
  }
  gpu.poll(true);
  if (frame > 0) {
    std::printf("  %d frames, %.1f ms/frame average (includes setup-free steady state only if run long)\n", frame,
                (elapsed() - fps_first) * 1000.0 / frame);
  }
  renderer.shutdown();
  arrays.albedo.release();
  arrays.normal.release();
  arrays.arm.release();
  env_day.background.release();
  env_day.specular.release();
  if (env_night.background.texture != env_day.background.texture) {
    env_night.background.release();
    env_night.specular.release();
  }
  gpu.destroy();
  glfwDestroyWindow(window);
  glfwTerminate();
  return 0;
}
