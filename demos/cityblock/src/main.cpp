// cityblock — standalone city-block generator and renderer (tech faction).
// Controls: mouse look (click to capture, Esc releases), WASD move, Q/E
// down/up, Shift fast, Ctrl slow, scroll = speed, N day/night, F1 cycle
// debug views, F2 toggle SSAO, F3 shadows, F4 bloom, F5 FXAA, R new seed,
// F12 screenshot, +/- exposure.
#include <GLFW/glfw3.h>

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
  cb::Scene scene = cb::generate_scene(sp);
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
