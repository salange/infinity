// human_tech_demo — standalone city-block generator and renderer (tech
// faction). Controls: mouse look (click to capture, Esc releases), WASD move,
// Q/E down/up, Shift fast, Ctrl slow, scroll = speed, N day/night, F1 cycle
// debug views, F2 toggle SSAO, F3 shadows, F4 bloom, F5 FXAA, R new seed,
// F12 screenshot, +/- exposure.
#include <GLFW/glfw3.h>

#include <stb_image_write.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <sstream>
#include <string>
#if defined(__linux__)
#include <sys/resource.h>
#endif

#include "camera.hpp"
#include "catalog.hpp"
#include "environment_proof.hpp"
#include "gpu.hpp"
#include "ibl.hpp"
#include "lookdev.hpp"
#include "market_blade_finish_proof.hpp"
#include "market_stone_finish_proof.hpp"
#include "renderer.hpp"
#include "renderer_proof.hpp"
#include "scene.hpp"
#include "sculpted_diagrid.hpp"
#include "textures.hpp"

namespace {

struct Args {
  std::string seed{"83"};
  int width{1280}, height{720};
  bool hidden{false};
  bool fullscreen{false};
  std::string capture;
  int frames{16};
  std::string shot{"aerial"};
  std::string sky{"authored"};
  bool analytic_sky_exterior{false};
  float sky_yaw_deg{150.0f};
  float environment_light_multiplier{1.f}; // explicit global relative-radiance control
  bool night{false};
  std::string assets;
  std::string shader_dir;
  bool have_cam{false};
  bool have_target{false};
  cb::Vec3 cam_pos, cam_target;
  std::uint32_t msaa{4};
  int debug{0};
  float ev{0.0f};
  std::uint32_t tex_size{1024};
  std::uint32_t mesh_page_mib{0};
  bool vsync{true};
  int bench{0};
  int stress{0};
  bool no_ssao{false};
  bool no_shadows{false};
  bool no_point_shadows{false};
  bool no_bloom{false};
  bool no_taa{false};
  bool no_indirect{false};
  bool no_reflections{false};
  bool ssao_half{false};
  bool shadow_half_rate{false};
  bool no_occlusion{false};
  bool shadow_far_lod{false};
  bool no_pattern{false};
  int sweep_blur{0};
  int sweep{0};
  float sweep_step{0.03f};
  std::string sweep_out{"sweep"};
  int sweep_dir{0};  // 0 right, 1 forward, 2 down
  std::string asset; // --asset KIND:NAME[;key=value] (catalog mode); "list"
                     // prints the catalog
  int detail{2}; // --detail L: the level the asset (or showcase) is built at
  std::string view{"final"};
  std::string kit;
  bool procedural_only{false};
  float fov{0};
  std::string timings;
  std::string capture_initial;
  std::string scene_manifest;
  std::string gallery;
  bool gallery_analysis{false};
  bool gallery_components{false};
  bool gallery_single_shot{false};
  std::string route;
  std::string route_out;
};

bool parse_vec3(const char *s, cb::Vec3 *v) {
  char trailing;
  return std::sscanf(s, "%f,%f,%f%c", &v->x, &v->y, &v->z, &trailing) == 3 &&
         std::isfinite(v->x) && std::isfinite(v->y) && std::isfinite(v->z);
}

Args parse(int argc, char **argv) {
  Args a;
  for (int i = 1; i < argc; ++i) {
    auto next = [&](const char *name) -> const char * {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "%s needs a value\n", name);
        std::exit(2);
      }
      return argv[++i];
    };
    if (!std::strcmp(argv[i], "--seed"))
      a.seed = next("--seed");
    else if (!std::strcmp(argv[i], "--width"))
      a.width = std::atoi(next("--width"));
    else if (!std::strcmp(argv[i], "--height"))
      a.height = std::atoi(next("--height"));
    else if (!std::strcmp(argv[i], "--hidden"))
      a.hidden = true;
    else if (!std::strcmp(argv[i], "--fullscreen"))
      a.fullscreen = true;
    else if (!std::strcmp(argv[i], "--capture"))
      a.capture = next("--capture");
    else if (!std::strcmp(argv[i], "--frames"))
      a.frames = std::atoi(next("--frames"));
    else if (!std::strcmp(argv[i], "--shot"))
      a.shot = next("--shot");
    else if (!std::strcmp(argv[i], "--sky"))
      a.sky = next("--sky");
    else if (!std::strcmp(argv[i], "--sky-yaw"))
      a.sky_yaw_deg = static_cast<float>(std::atof(next("--sky-yaw")));
    else if (!std::strcmp(argv[i], "--environment-light-multiplier")) {
      const char *value=next("--environment-light-multiplier");
      char *end=nullptr;
      a.environment_light_multiplier=std::strtof(value,&end);
      if(end==value || *end!='\0' || !std::isfinite(a.environment_light_multiplier) ||
         a.environment_light_multiplier<0.f) {
        std::fprintf(stderr,"--environment-light-multiplier requires a finite nonnegative number\n");
        std::exit(2);
      }
    }
    else if (!std::strcmp(argv[i], "--analytic-sky-exterior"))
      a.analytic_sky_exterior = true;
    else if (!std::strcmp(argv[i], "--night"))
      a.night = true;
    else if (!std::strcmp(argv[i], "--assets"))
      a.assets = next("--assets");
    else if (!std::strcmp(argv[i], "--shader-dir"))
      a.shader_dir = next("--shader-dir");
    else if (!std::strcmp(argv[i], "--cam")) {
      a.have_cam = parse_vec3(next("--cam"), &a.cam_pos);
      if (!a.have_cam) {
        std::fprintf(stderr, "--cam requires three finite coordinates\n");
        std::exit(2);
      }
    } else if (!std::strcmp(argv[i], "--target")) {
      a.have_target = parse_vec3(next("--target"), &a.cam_target);
      if (!a.have_target) {
        std::fprintf(stderr, "--target requires three finite coordinates\n");
        std::exit(2);
      }
    } else if (!std::strcmp(argv[i], "--msaa"))
      a.msaa = static_cast<std::uint32_t>(std::atoi(next("--msaa")));
    else if (!std::strcmp(argv[i], "--debug"))
      a.debug = std::atoi(next("--debug"));
    else if (!std::strcmp(argv[i], "--ev"))
      a.ev = static_cast<float>(std::atof(next("--ev")));
    else if (!std::strcmp(argv[i], "--tex-size"))
      a.tex_size = static_cast<std::uint32_t>(std::atoi(next("--tex-size")));
    else if (!std::strcmp(argv[i], "--mesh-page-mib")) {
      const char *value = next("--mesh-page-mib");
      char *end = nullptr;
      const unsigned long parsed = std::strtoul(value, &end, 10);
      if (*value < '0' || *value > '9' || *end != '\0' || parsed < 1 || parsed > 4096) {
        std::fprintf(stderr, "--mesh-page-mib requires an integer from 1 to 4096\n");
        std::exit(2);
      }
      a.mesh_page_mib = static_cast<std::uint32_t>(parsed);
    }
    else if (!std::strcmp(argv[i], "--no-vsync"))
      a.vsync = false;
    else if (!std::strcmp(argv[i], "--no-ssao"))
      a.no_ssao = true;
    else if (!std::strcmp(argv[i], "--no-shadows"))
      a.no_shadows = true;
    else if (!std::strcmp(argv[i], "--no-point-shadows"))
      a.no_point_shadows = true;
    else if (!std::strcmp(argv[i], "--no-bloom"))
      a.no_bloom = true;
    else if (!std::strcmp(argv[i], "--no-taa"))
      a.no_taa = true;
    else if (!std::strcmp(argv[i], "--no-indirect"))
      a.no_indirect = true;
    else if (!std::strcmp(argv[i], "--no-reflections"))
      a.no_reflections = true;
    else if (!std::strcmp(argv[i], "--ssao-half"))
      a.ssao_half = true;
    else if (!std::strcmp(argv[i], "--shadow-half-rate"))
      a.shadow_half_rate = true;
    else if (!std::strcmp(argv[i], "--no-occlusion"))
      a.no_occlusion = true;
    else if (!std::strcmp(argv[i], "--shadow-far-lod"))
      a.shadow_far_lod = true;
    else if (!std::strcmp(argv[i], "--bench"))
      a.bench = std::atoi(next("--bench"));
    else if (!std::strcmp(argv[i], "--stress"))
      a.stress = std::atoi(next("--stress"));
    else if (!std::strcmp(argv[i], "--asset"))
      a.asset = next("--asset");
    else if (!std::strcmp(argv[i], "--detail"))
      a.detail = std::atoi(next("--detail"));
    else if (!std::strcmp(argv[i], "--view"))
      a.view = next("--view");
    else if (!std::strcmp(argv[i], "--kit"))
      a.kit = next("--kit");
    else if (!std::strcmp(argv[i], "--procedural-only"))
      a.procedural_only = true;
    else if (!std::strcmp(argv[i], "--fov"))
      a.fov = std::atof(next("--fov"));
    else if (!std::strcmp(argv[i], "--timings"))
      a.timings = next("--timings");
    else if (!std::strcmp(argv[i], "--capture-initial"))
      a.capture_initial = next("--capture-initial");
    else if (!std::strcmp(argv[i], "--scene-manifest"))
      a.scene_manifest = next("--scene-manifest");
    else if (!std::strcmp(argv[i], "--gallery"))
      a.gallery = next("--gallery");
    else if (!std::strcmp(argv[i], "--gallery-analysis"))
      a.gallery_analysis = true;
    else if (!std::strcmp(argv[i], "--gallery-components"))
      a.gallery_components = true;
    else if (!std::strcmp(argv[i], "--gallery-single-shot"))
      a.gallery_single_shot = true;
    else if (!std::strcmp(argv[i], "--route"))
      a.route = next("--route");
    else if (!std::strcmp(argv[i], "--route-out"))
      a.route_out = next("--route-out");
    else if (!std::strcmp(argv[i], "--no-pattern"))
      a.no_pattern = true;
    else if (!std::strcmp(argv[i], "--sweep-blur"))
      a.sweep_blur = std::atoi(next("--sweep-blur"));
    else if (!std::strcmp(argv[i], "--sweep"))
      a.sweep = std::atoi(next("--sweep"));
    else if (!std::strcmp(argv[i], "--sweep-step"))
      a.sweep_step = static_cast<float>(std::atof(next("--sweep-step")));
    else if (!std::strcmp(argv[i], "--sweep-out"))
      a.sweep_out = next("--sweep-out");
    else if (!std::strcmp(argv[i], "--sweep-dir")) {
      const char *d = next("--sweep-dir");
      a.sweep_dir =
          !std::strcmp(d, "forward") ? 1 : (!std::strcmp(d, "down") ? 2 : 0);
    } else if (!std::strcmp(argv[i], "--help") || !std::strcmp(argv[i], "-h")) {
      std::printf(
          "human_tech_demo [--shot aerial|galaxy|civic|street|garden|landing] "
          "[--seed S] [--width W --height H] [--hidden] [--fullscreen] [--capture out.png "
          "--frames N]\n"
          "          [--sky authored|studio|day|night|sunset|file.hdr] "
          "[--sky-yaw deg] [--night] [--assets dir] [--shader-dir dir]\n"
          "          [--environment-light-multiplier M] (sky irradiance/reflections only; default1)\n"
          "          [--analytic-sky-exterior] (environment comparison)\n"
          "          [--no-shadows] (solar) [--no-point-shadows] (local lights)\n"
          "          [--no-bloom] (disable highlight scattering for transport controls)\n"
          "          [--cam x,y,z --target x,y,z --fov degrees] [--msaa 1|4] "
          "[--debug 0-20] [--ev bias]\n"
          "          [--mesh-page-mib N] (bounded upload diagnostic; default adapter limit)\n"
          "          [--view final|clay|silhouette|neutral] [--kit "
          "file|--procedural-only]\n"
          "          [--capture-initial first.png] [--timings output.json] "
          "[--scene-manifest output.json]\n"
          "          [--gallery directory --gallery-analysis] (all six cameras "
          "in one run)\n"
          "          [--gallery-components] [--gallery-single-shot] (use --shot)\n"
          "          [--route camera-path.txt --route-out directory] "
          "(world-space camera route)\n"
          "          Keys 1-6: arrival, galaxy, civic, street, garden, "
          "landing; N: lighting; WASD/QE: move.\n");
      std::exit(0);
    } else {
      std::fprintf(stderr, "unknown argument %s\n", argv[i]);
      std::exit(2);
    }
  }
  if (a.width < 64 || a.height < 64 || a.width > 8192 || a.height > 8192 ||
      a.frames < 1 || (a.msaa != 1 && a.msaa != 4)) {
    std::fprintf(stderr, "invalid dimensions, frame count or MSAA\n");
    std::exit(2);
  }
  if (a.shot != "aerial" && a.shot != "galaxy" && a.shot != "civic" &&
      a.shot != "street" && a.shot != "terrace" && a.shot != "garden" &&
      a.shot != "landing") {
    std::fprintf(stderr, "unknown --shot; use --help for the camera list\n");
    std::exit(2);
  }
  if(a.shot=="terrace")a.shot="landing";
  if((a.gallery_components||a.gallery_single_shot)&&a.gallery.empty()) {
    std::fprintf(stderr,"gallery options require --gallery directory\n");
    std::exit(2);
  }
  if (a.view != "final" && a.view != "clay" && a.view != "silhouette" &&
      a.view != "neutral") {
    std::fprintf(stderr, "unknown --view\n");
    std::exit(2);
  }
  if (!std::isfinite(a.fov) || (a.fov != 0 && (a.fov < 15 || a.fov > 100))) {
    std::fprintf(stderr, "--fov must be 15 to 100 degrees\n");
    std::exit(2);
  }
  if (a.have_cam != a.have_target) {
    std::fprintf(stderr, "--cam and --target must be provided together\n");
    std::exit(2);
  }
  if (a.have_cam && cb::length(a.cam_pos - a.cam_target) < .001f) {
    std::fprintf(stderr, "camera and target must differ\n");
    std::exit(2);
  }
  if (a.route.empty() != a.route_out.empty()) {
    std::fprintf(stderr, "--route and --route-out must be supplied together\n");
    std::exit(2);
  }
  if (a.view == "clay")
    a.debug = 13;
  if (a.view == "silhouette")
    a.debug = 14;
  if (a.view == "neutral")
    a.debug = 15;
  if (a.shot == "galaxy" && a.view == "final")
    a.night = true;
  if (a.view != "final")
    a.night = false;
  return a;
}

struct Input {
  double last_mx{0}, last_my{0};
  bool captured{false};
  bool first{true};
  double scroll{0};
};
Input g_input;

void scroll_cb(GLFWwindow *, double, double y) { g_input.scroll += y; }
void mouse_cb(GLFWwindow *w, int button, int action, int) {
  if (button == GLFW_MOUSE_BUTTON_LEFT && action == GLFW_PRESS &&
      !g_input.captured) {
    glfwSetInputMode(w, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
    g_input.captured = true;
    g_input.first = true;
  }
}

} // namespace

int main(int argc, char **argv) {
  std::setvbuf(stdout, nullptr, _IOLBF, 0);
  const auto t0 = std::chrono::steady_clock::now();
  auto elapsed = [&] {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
        .count();
  };
  Args args = parse(argc, argv);
  const std::string source_dir = HUMAN_TECH_SOURCE_DIR;
  const std::string assets =
      args.assets.empty() ? source_dir + "/assets" : args.assets;
  const std::string shaders =
      args.shader_dir.empty() ? source_dir + "/shaders" : args.shader_dir;
  // An explicit shader snapshot permits reproducible renderer comparisons.
  // Reject incomplete snapshots before allocating a window or graphics device.
  for (const char *name : {"common.wgsl", "fullscreen.wgsl", "cull.wgsl", "hiz.wgsl",
                           "point_visibility.wgsl", "main.wgsl", "post.wgsl",
                           "prepass.wgsl", "shadow.wgsl", "sky.wgsl",
                           "ssao.wgsl", "taa.wgsl"}) {
    std::error_code error;
    const auto path = std::filesystem::path(shaders) / name;
    if (!std::filesystem::is_regular_file(path, error)) {
      std::fprintf(stderr, "Shader snapshot is incomplete: %s\n",
                   path.string().c_str());
      return 2;
    }
  }

  for (const auto &output : {args.scene_manifest, args.capture, args.capture_initial, args.timings}) {
    if (output.empty())
      continue;
    const auto parent = std::filesystem::path(output).parent_path();
    if (parent.empty())
      continue;
    std::error_code error;
    std::filesystem::create_directories(parent, error);
    if (error) {
      std::fprintf(stderr, "Could not create output directory %s: %s\n",
                   parent.string().c_str(), error.message().c_str());
      return 2;
    }
  }

  if (glfwInit() != GLFW_TRUE) {
    std::fprintf(stderr, "glfwInit failed\n");
    return 1;
  }
  glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
  if (args.hidden || args.fullscreen)
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
  GLFWmonitor *monitor = args.fullscreen ? glfwGetPrimaryMonitor() : nullptr;
  if (args.fullscreen && !monitor) {
    std::fprintf(stderr, "No monitor is available for full-screen presentation\n");
    glfwTerminate();
    return 1;
  }
  const GLFWvidmode *mode = monitor ? glfwGetVideoMode(monitor) : nullptr;
  GLFWwindow *window = glfwCreateWindow(
      mode ? mode->width : args.width, mode ? mode->height : args.height,
      "unendlich — Human-tech metropolis", monitor, nullptr);
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
  std::printf("human_tech_demo: %s, %ux%u\n", gpu.adapter_name.c_str(),
              gpu.width, gpu.height);

  // ---- textures
  // --------------------------------------------------------------------
  cb::MaterialArrays arrays = cb::load_material_arrays(
      gpu, assets, cb::texture_sets(), args.tex_size, true);
  if (!args.procedural_only && args.asset.empty()) {
    for (const char *name : {"marble", "paving_slabs", "terrazzo"}) {
      if (!arrays.has_file_set(name)) {
        std::fprintf(
            stderr,
            "Required reviewed material maps missing or invalid: %s. "
            "Run tools/import-surface-assets.py with the immutable "
            "source directory, or use --procedural-only for a preview.\n",
            name);
        gpu.destroy();
        glfwDestroyWindow(window);
        glfwTerminate();
        return 2;
      }
    }
  }
  const bool reviewed_material_maps = arrays.has_file_set("marble") &&
                                      arrays.has_file_set("paving_slabs") &&
                                      arrays.has_file_set("terrazzo");

  // ---- scene
  // ---------------------------------------------------------------------
  cb::SceneParams sp;
  sp.reviewed_material_maps = reviewed_material_maps;
  sp.seed = args.seed;
  sp.shot = args.shot;
  sp.far_patterns = !args.no_pattern;
  sp.asset = args.asset;
  sp.detail = args.detail;
  sp.asset_kit =
      args.procedural_only
          ? ""
          : (args.kit.empty()
                 ? source_dir + "/assets/generated/human_tech_kit.htkit"
                 : args.kit);
  if (args.asset == "list") {
    std::printf("%s", cb::asset_catalog_text().c_str());
    return 0;
  }
  cb::Scene scene;
  const bool renderer_proof = args.asset.starts_with("indirect-proof-") ||
                              args.asset.starts_with("reflection-proof-");
  const bool environment_proof =
      args.asset.starts_with("reflection-proof-environment-");
  const bool market_stone_proof =
      args.asset.starts_with("reflection-proof-market-stone-");
  const bool market_blade_proof =
      args.asset.starts_with("reflection-proof-market-blade-");
  const bool diagrid_sample = args.asset.starts_with("diagrid-sample-");
  if (environment_proof) {
    args.shot = cb::kEnvironmentProofShot;
    args.night = false;
  }
  if (market_stone_proof || market_blade_proof) {
    args.shot = cb::kMarketStoneProofShot;
    args.night = false;
  }
  try {
    if (diagrid_sample) {
      std::string view = args.asset.substr(std::string("diagrid-sample-").size());
      const bool shallow = view.ends_with("-shallow");
      if (shallow)
        view.resize(view.size() - std::string("-shallow").size());
      scene = cb::make_sculpted_diagrid_sample(view, shallow);
    } else if (market_blade_proof)
      scene = cb::generate_market_blade_finish_proof(args.asset);
    else if (market_stone_proof)
      scene = cb::generate_market_stone_finish_proof(args.asset, reviewed_material_maps);
    else if (environment_proof)
      scene = cb::generate_environment_proof();
    else if (renderer_proof)
      scene = cb::generate_renderer_proof(args.asset, sp.asset_kit);
    else if (args.asset == "lookdev")
      scene = cb::generate_lookdev(sp.asset_kit,
                                   source_dir + "/assets/lookdev.scene");
    else
      scene = cb::generate_scene(sp);
  } catch (const std::exception &e) {
    std::fprintf(stderr,
                 "Scene assets unavailable or invalid: %s\nBuild the authored "
                 "kit with the demo's asset tool, or use --procedural-only for "
                 "a reduced-detail preview.\n",
                 e.what());
    gpu.destroy();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 2;
  }
  std::printf("  content tier: %s; %zu reusable resources, %zu instances\n",
              diagrid_sample         ? "isolated production facade sample"
              : renderer_proof       ? "isolated renderer proof"
              : sp.asset_kit.empty() ? "procedural preview"
                                     : "authored production kit",
              scene.asset_library.resources.size(),
              scene.asset_instances.size());
  std::printf("  city: %s, radius %.0f m, %d blocks, %d towers, %d standard "
              "buildings, %d plazas\n",
              scene.city_size.c_str(), scene.city_radius, scene.stats_blocks,
              scene.stats_towers, scene.stats_standards, scene.stats_plazas);
  std::printf(
      "  scene: %zu opaque + %zu foliage triangles, %zu lights (%.2f s)\n",
      scene.opaque.triangle_count(), scene.foliage.triangle_count(),
      scene.lights.size(), elapsed());

  for (const auto &material : scene.materials)
    if (!material.albedo_set.empty() &&
        std::find(arrays.names.begin(), arrays.names.end(),
                  material.albedo_set) == arrays.names.end()) {
      std::fprintf(stderr,
                   "unsupported material texture set: %s (material %s)\n",
                   material.albedo_set.c_str(), material.name.c_str());
      return 2;
    }
  std::printf("  textures loaded (%.2f s)\n", elapsed());

  // ---- environments
  // ------------------------------------------------------------------
  auto sky_path = [&](const std::string &name) {
    if (name.size() > 4 && name.substr(name.size() - 4) == ".hdr")
      return name;
    return assets + "/sky/" + name + ".hdr";
  };
  const float yaw = cb::radians(args.sky_yaw_deg);
  cb::Environment env_day;
  const bool authored_sky = args.sky == "authored" && !args.procedural_only;
  if (authored_sky) {
    if (scene.has_lighting_override) {
      env_day = cb::load_environment(
          gpu, source_dir + "/assets/generated/sky-sunset-v1.png",
          cb::radians(45.f), 1024, 128, true);
    } else {
      cb::Vec3 arrival_position, arrival_target;
      cb::shot_camera("aerial", arrival_position, arrival_target);
      env_day = cb::load_perspective_environment(
          gpu, source_dir + "/assets/generated/sky-arrival-perspective-v2.png",
          cb::normalize(arrival_target - arrival_position),
          cb::radians(cb::shot_fov_degrees("aerial")), 1280.f / 720.f, 1536,
          128, false);
    }
    if (!env_day.ok) {
      std::fprintf(stderr, "required authored sunset sky missing; import the "
                           "source environment assets or use --sky studio\n");
      return 2;
    }
    env_day.sun_dir = cb::normalize(cb::Vec3{.89f, .047f, -.45f});
    env_day.sun_color = {4.8f, 3.45f, 2.18f};
    env_day.has_sun = true;
    env_day.exposure = .86f;
    env_day.lighting_scale = scene.has_lighting_override ? 1.f : 2.4f;
    std::printf("  sunset: authored relative-linear sky, explicit finite sun "
                "irradiance; cinematic exposure\n");
  } else if (args.sky != "studio" && args.sky != "authored")
    env_day =
        cb::load_environment(gpu, sky_path(args.sky), yaw, 512, 128, true);
  if (!env_day.ok) {
    std::printf("  using the built-in late-afternoon sky\n");
    env_day = cb::make_analytic_environment(gpu, cb::Vec3{0.82f, 0.18f, -0.42f},
                                            512, 128);
  }
  cb::Environment env_night;
  if (authored_sky) {
    cb::Vec3 galaxy_position, galaxy_target;
    cb::shot_camera("galaxy", galaxy_position, galaxy_target);
    const cb::Vec3 galaxy_forward =
        cb::normalize(galaxy_target - galaxy_position);
    env_night = cb::load_perspective_environment(
        gpu, source_dir + "/assets/generated/sky-galaxy-perspective-v3.png",
        galaxy_forward, cb::radians(cb::shot_fov_degrees("galaxy")),
        1280.f / 720.f, 1536, 128);
    if (!env_night.ok) {
      std::fprintf(stderr, "required authored galaxy sky missing; import the "
                           "source environment assets or use --sky studio\n");
      return 2;
    }
    const cb::Vec3 moon_right = cb::normalize(
                       cb::cross(galaxy_forward, cb::Vec3{0, 1, 0})),
                   moon_up = cb::cross(moon_right, galaxy_forward);
    const float moon_projection =
        std::tan(cb::radians(cb::shot_fov_degrees("galaxy")) * .5f);
    env_night.moon_dir =
        cb::normalize(galaxy_forward +
                      moon_right * (.836f * moon_projection * 1280.f / 720.f) +
                      moon_up * (.724f * moon_projection));
    // Finite crescent irradiance is consistent with its small solid angle;
    // diffuse visibility is supplied independently by the surrounding sky.
    env_night.moon_color = {.00095f, .000875f, .000725f};
    env_night.moon_visible = true;
    // Reference06 solar centre (.016,.455), fitted once in this world preset.
    cb::Vec3 twilight_direction =
        cb::normalize(galaxy_forward +
                      moon_right * (-.968f * moon_projection * 1280.f / 720.f) +
                      moon_up * (.09f * moon_projection));
    const float illustrated_sun_elevation = std::asin(twilight_direction.y);
    const float minimum_sun_elevation = cb::radians(.18f);
    if (illustrated_sun_elevation < minimum_sun_elevation) {
      cb::Vec3 horizontal = cb::normalize(
          cb::Vec3{twilight_direction.x, 0, twilight_direction.z});
      twilight_direction = horizontal * std::cos(minimum_sun_elevation) +
                           cb::Vec3{0, std::sin(minimum_sun_elevation), 0};
    }
    env_night.sun_dir = twilight_direction;
    env_night.sun_color = {.75f, .34f, .13f};
    env_night.sun_visible = true;
    std::printf("  twilight sun: reference elevation %.3f deg, world elevation "
                "%.3f deg; separate moon\n",
                illustrated_sun_elevation * 180.f / cb::kPi,
                std::asin(twilight_direction.y) * 180.f / cb::kPi);
    env_night.lighting_scale = 4.4f;
    env_night.has_sun = true;
    env_night.exposure = 2.4f;
    std::printf("  galaxy: authored relative-linear stellar sky, explicit moon "
                "irradiance; cinematic exposure\n");
  } else
    env_night =
        cb::load_environment(gpu, sky_path("night"), yaw, 512, 128, true);
  if (!env_night.ok)
    env_night =
        cb::make_analytic_environment(gpu, {-0.7f, 0.10f, 0.5f}, 512, 128);
  if (scene.has_lighting_override) {
    env_day.sun_dir = cb::normalize(scene.sun_direction);
    env_day.sun_color = scene.sun_irradiance;
    env_day.has_sun = true;
    env_day.exposure = scene.lighting_exposure;
  }
  struct CloseLighting {
    const char *shot;
    cb::Vec2 solar_image;
    cb::Vec3 irradiance;
    float exposure, sky_scale, evening;
  };
  // Each weather preset is fixed in world directions. Subsequent flight does
  // not refit its sky or lights to the moving camera.
  const std::array<CloseLighting, 4> close_lighting{{
      {"civic", {.80f, .42f}, {.40f, .25f, .15f}, 1.25f, 1.45f, .65f},
      {"street", {.66f, .43f}, {3.8f, 3.1f, 2.35f}, 1.0f, 1.65f, .08f},
      {"garden", {.12f, .13f}, {3.0f, 2.7f, 2.15f}, 1.0f, 1.55f, 0.f},
      {"landing", {-.12f, .43f}, {4.6f, 3.25f, 1.9f}, .95f, 1.75f, .20f},
  }};
  auto image_direction = [&](const char *shot, cb::Vec2 xy) {
    cb::Vec3 position, target;
    cb::shot_camera(shot, position, target);
    const cb::Vec3 forward = cb::normalize(target - position);
    const cb::Vec3 right = cb::normalize(cb::cross(forward, {0, 1, 0}));
    const cb::Vec3 up = cb::cross(right, forward);
    const float scale = std::tan(cb::radians(cb::shot_fov_degrees(shot)) * .5f);
    return cb::normalize(forward +
                         right * ((xy.x * 2 - 1) * scale * 1280.f / 720.f) +
                         up * ((1 - xy.y * 2) * scale));
  };
  std::array<cb::Environment, 4> env_close;
  if (authored_sky && (args.asset.empty() ||
                       args.asset.starts_with("indirect-proof-foliage-") ||
                       args.asset.starts_with("reflection-proof-environment-") ||
                       market_stone_proof || market_blade_proof ||
                       args.asset.starts_with("reflection-proof-moon-"))) {
    for (std::size_t i = 0; i < close_lighting.size(); ++i) {
      const auto &look = close_lighting[i];
      cb::Vec3 position, target;
      cb::shot_camera(look.shot, position, target);
      auto &env = env_close[i];
      env = cb::load_perspective_environment(
          gpu,
          source_dir + "/assets/generated/sky-" + look.shot +
              "-perspective-v1.png",
          cb::normalize(target - position),
          cb::radians(cb::shot_fov_degrees(look.shot)), 1280.f / 720.f, 1536,
          128, false,
          i == 0 && !args.analytic_sky_exterior
              ? source_dir + "/assets/generated/sky-civic-full-v1.png"
              : std::string{});
      if (!env.ok) {
        std::fprintf(stderr,
                     "required %s weather source missing; import the source "
                     "environments\n",
                     look.shot);
        return 2;
      }
      const bool garden_weather = std::strcmp(look.shot, "garden") == 0;
      // The garden reference contains no visible solar disk. Its dappled
      // floor and west-facing ceramic require an elevated key from the west,
      // independently of a bright patch within the authored sky image.
      env.sun_dir = garden_weather
                        ? cb::normalize(cb::Vec3{-.75f, .62f, -.24f})
                        : image_direction(look.shot, look.solar_image);
      env.sun_color = look.irradiance;
      env.exposure = look.exposure;
      env.lighting_scale = look.sky_scale;
      env.night_factor = look.evening;
      env.has_sun = true;
      env.sun_visible = i != 0 && !garden_weather;
      if (i == 3) {
        env.moon_dir = image_direction("landing", {.337f, .153f});
        env.moon_color = {.0015f, .0014f, .0012f};
        env.moon_visible = true;
        env.moon_phase = {1.f, 0.f};
      }
      std::printf("  %s: fixed weather preset, solar elevation %.2f deg, "
                  "exposure %.2f, sky lighting %.2f\n",
                  look.shot, std::asin(env.sun_dir.y) * 180.f / cb::kPi,
                  env.exposure, env.lighting_scale);
    }
  }
  // Apply once before any renderer/voxel consumer sees the environment. This
  // changes its relative sky radiance only: no exposure, source schedule,
  // room output, direct sun/moon, material or display-background changes.
  auto scale_environment_light = [&](cb::Environment &env) {
    env.lighting_scale *= args.environment_light_multiplier;
  };
  scale_environment_light(env_day);
  scale_environment_light(env_night);
  for(auto &env:env_close)scale_environment_light(env);
  std::printf("  environment light multiplier %.6g; resolved galaxy sky scale %.6g\n",
              args.environment_light_multiplier,env_night.lighting_scale);
  auto environment_for = [&](const std::string &shot, bool is_night) {
    if (is_night)
      return &env_night;
    for (std::size_t i = 0; i < close_lighting.size(); ++i)
      if (env_close[i].ok && shot == close_lighting[i].shot)
        return &env_close[i];
    return &env_day;
  };
  auto release_environments = [&] {
    for (auto &env : env_close) {
      env.background.release();
      env.specular.release();
    }
    if (env_night.background.texture != env_day.background.texture) {
      env_night.background.release();
      env_night.specular.release();
    }
    env_day.background.release();
    env_day.specular.release();
  };
  if (!args.scene_manifest.empty()) {
    const auto geometry = cb::scene_layout_manifest(sp.seed);
    std::ofstream manifest(args.scene_manifest);
    manifest << geometry.substr(0, geometry.rfind('}'))
             << ",\"lighting_presets\":[";
    bool first = true;
    auto vector = [&](cb::Vec3 value) {
      manifest << '[' << value.x << ',' << value.y << ',' << value.z << ']';
    };
    for (const char *name :
         {"aerial", "galaxy", "civic", "street", "garden", "landing"}) {
      const bool is_night = std::string(name) == "galaxy";
      const auto &env = *environment_for(name, is_night);
      manifest << (first ? "" : ",") << "{\"id\":\"" << name
               << "\",\"sun_direction\":";
      vector(env.sun_dir);
      manifest << ",\"sun_irradiance\":";
      vector(env.sun_color);
      manifest << ",\"moon_direction\":";
      vector(env.moon_dir);
      manifest << ",\"moon_irradiance\":";
      vector(env.moon_color);
      manifest << ",\"moon_phase\":[" << env.moon_phase.x << ','
               << env.moon_phase.y << ']'
               << ",\"moon_angular_radius\":" << env.moon_angular_radius;
      manifest << ",\"moon_visible\":" << (env.moon_visible ? "true" : "false")
               << ",\"sun_visible\":" << (env.sun_visible ? "true" : "false")
               << ",\"exposure\":" << env.exposure
               << ",\"sky_lighting_scale\":" << env.lighting_scale
               << ",\"night_factor\":"
               << (env.night_factor >= 0 ? env.night_factor : float(is_night))
               << ",\"display_referred_background\":"
               << (env.display_referred_background ? "true" : "false") << '}';
      first = false;
    }
    manifest << "],\"reviewed_material_maps\":"
             << (reviewed_material_maps ? "true" : "false")
             << ",\"material_layers\":[";
    for (std::size_t i = 0; i < arrays.names.size(); ++i) {
      manifest << (i ? "," : "") << "{\"name\":\"" << arrays.names[i]
               << "\",\"source\":\""
               << (arrays.file_sets[i] ? "file_maps" : "generated") << "\"}";
    }
    manifest << "]}\n";
    if (!manifest) {
      std::fprintf(stderr, "could not write scene manifest\n");
      return 2;
    }
  }
  std::printf("  environments ready (%.2f s)\n", elapsed());

  // ---- renderer
  // ---------------------------------------------------------------------
  cb::RenderSettings settings;
  settings.msaa = args.msaa;
  settings.mesh_page_bytes = std::uint64_t(args.mesh_page_mib) * 1024 * 1024;
  settings.debug_view = args.debug;
  settings.exposure_bias = args.ev;
  settings.ssao = !args.no_ssao;
  settings.shadows = !args.no_shadows;
  settings.point_shadows = !args.no_point_shadows;
  settings.bloom = !args.no_bloom;
  settings.taa = !args.no_taa;
  settings.indirect = !args.no_indirect;
  settings.reflections = !args.no_reflections;
  settings.ssao_half = args.ssao_half;
  settings.shadow_half_rate = args.shadow_half_rate;
  settings.occlusion = !args.no_occlusion;
  settings.shadow_far_lod = args.shadow_far_lod;
  cb::Renderer renderer;
  auto stop_renderer = [&](const std::string &message) {
    std::fprintf(stderr, "Rendering stopped: %s\n", message.c_str());
    renderer.shutdown();
    arrays.albedo.release();
    arrays.normal.release();
    arrays.arm.release();
    release_environments();
    gpu.destroy();
    glfwDestroyWindow(window);
    glfwTerminate();
    std::exit(2);
  };
  try {
    if (!renderer.init(&gpu, shaders, settings, &error,
                       args.fullscreen ? args.width : 0,
                       args.fullscreen ? args.height : 0))
      stop_renderer("renderer init failed: " + error);
    renderer.set_scene(scene, arrays);
  } catch (const std::exception &e) {
    stop_renderer(e.what());
  }
  bool night = args.night;
  renderer.set_environment(environment_for(args.shot, night), night);
  std::printf("  renderer ready, %u unique mesh triangles (%.2f s)\n",
              renderer.triangles(), elapsed());
  if (args.fullscreen && !args.hidden)
    glfwShowWindow(window);

  cb::Camera cam;
  cam.speed =
      args.asset.empty() && (args.shot == "aerial" || args.shot == "galaxy")
          ? 25.f
          : 2.f;
  cam.position = args.have_cam ? args.cam_pos : scene.camera_position;
  cam.look_at_point(args.have_cam ? args.cam_target : scene.camera_target);
  cam.fov_y = cb::radians(args.fov > 0 ? args.fov
                          : scene.camera_fov_degrees > 0
                              ? scene.camera_fov_degrees
                              : cb::shot_fov_degrees(args.shot));
  std::printf("  camera %s: %.3f,%.3f,%.3f, vertical FOV %.2f degrees\n",
              args.shot.c_str(), cam.position.x, cam.position.y, cam.position.z,
              cam.fov_y * 180.0f / cb::kPi);
  const double startup_seconds = elapsed();
  auto stop_for_gpu_failure = [&] {
    if (gpu.healthy())
      return;
    stop_renderer(gpu.failure_message());
  };
  stop_for_gpu_failure();
  std::vector<double> frame_ms;
  auto timed_frame = [&](float time_s, WGPUTextureView target) {
    const double begin = elapsed();
    renderer.render(cam, time_s, target);
    // Bound work in flight and measure a useful completed frame, including GPU
    // wait.
    gpu.poll(true);
    stop_for_gpu_failure();
    frame_ms.push_back((elapsed() - begin) * 1000.0);
  };
  auto report_timings = [&] {
    if (frame_ms.empty())
      return;
    auto sorted = frame_ms;
    std::sort(sorted.begin(), sorted.end());
    auto percentile = [&](double q) {
      return sorted[static_cast<std::size_t>(std::ceil(q * sorted.size())) - 1];
    };
    const double mean =
        std::accumulate(sorted.begin(), sorted.end(), 0.0) / sorted.size();
    long peak_kib = 0;
#if defined(__linux__)
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) == 0)
      peak_kib = usage.ru_maxrss;
#endif
    std::printf("  completed frames: mean %.2f ms, median %.2f ms, p95 %.2f "
                "ms, max %.2f ms; startup %.2f s; peak RSS %.1f MiB\n",
                mean, percentile(.5), percentile(.95), sorted.back(),
                startup_seconds, peak_kib / 1024.0);
    if (args.timings.empty())
      return;
    std::ofstream out(args.timings);
    out << "{\n  \"schema_version\": 1,\n  \"resolution\": [" << gpu.width
        << ',' << gpu.height << "],\n  \"shot\": \"" << args.shot
        << "\",\n  \"view\": \"" << args.view
        << "\",\n  \"debug_view\": " << renderer.settings().debug_view
        << ",\n  \"taa\": " << (renderer.settings().taa ? "true" : "false")
        << ",\n  \"bloom\": " << (renderer.settings().bloom ? "true" : "false")
        << ",\n  \"mesh_page_bytes_requested\": " << renderer.settings().mesh_page_bytes
        << ",\n  \"startup_seconds\": " << startup_seconds
        << ",\n  \"peak_rss_mib\": " << peak_kib / 1024.0
        << ",\n  \"frame_count\": " << sorted.size()
        << ",\n  \"first_updated_frame_ms\": " << frame_ms.front()
        << ",\n  \"mean_ms\": " << mean
        << ",\n  \"median_ms\": " << percentile(.5)
        << ",\n  \"p95_ms\": " << percentile(.95)
        << ",\n  \"max_ms\": " << sorted.back()
        << ",\n  \"completed_frame_ms\": [";
    for (std::size_t i = 0; i < frame_ms.size(); ++i)
      out << (i ? "," : "") << frame_ms[i];
    out << "]\n}\n";
    if (!out) {
      std::fprintf(stderr, "could not write timing report\n");
      std::exit(2);
    }
  };

  if (!args.gallery.empty()) {
    const std::filesystem::path dir(args.gallery);
    std::filesystem::create_directories(dir);
    bool ok = true;
    const int saved_debug = renderer.settings().debug_view;
    auto capture_view = [&](const std::string &name, int debug, int frame_count = 0) {
      renderer.settings().debug_view = debug;
      renderer.reset_history();
      frame_ms.clear();
      args.timings = (dir / (name + "-timings.json")).string();
      for (int i = 0; i < (frame_count > 0 ? frame_count : args.frames); ++i) {
        glfwPollEvents();
        timed_frame(0.0f, nullptr);
        if (i == 0)
          ok = renderer.capture_png((dir / (name + "-initial.png")).string()) &&
               ok;
      }
      ok = renderer.capture_png((dir / (name + ".png")).string()) && ok;
      std::printf("  gallery %s: %s\n", name.c_str(), ok ? "ok" : "FAILED");
      report_timings();
    };
    const std::string selected_shot=args.shot;
    for (const char *shot_name :
         {"aerial", "galaxy", "civic", "street", "garden", "landing"}) {
      if(args.gallery_single_shot && selected_shot != shot_name)continue;
      args.shot = shot_name;
      cb::Vec3 target;
      cb::shot_camera(args.shot, cam.position, target);
      cam.look_at_point(target);
      cam.velocity = {};
      cam.fov_y = cb::radians(cb::shot_fov_degrees(args.shot));
      night = args.shot == "galaxy";
      renderer.set_environment(environment_for(args.shot, night), night);
      args.view = "final";
      capture_view(args.shot, saved_debug);
      if(args.gallery_components) {
        // Components are diagnostic single-frame outputs, never settled beauty
        // images. Keep them in this process with the exact same scene and light.
        const bool saved_bloom=renderer.settings().bloom;
        const bool saved_taa=renderer.settings().taa;
        renderer.settings().bloom=false;
        renderer.settings().taa=false;
        for(const auto& item:{std::pair{"solar-diffuse",6},
                              std::pair{"diffuse-and-rooms",7},
                              std::pair{"reflections",8},
                              std::pair{"solar-specular",9},
                              std::pair{"window-fields",16}}) {
          args.view=item.first;
          capture_view(args.shot+"-"+item.first,item.second,1);
        }
        args.view="final";
        renderer.settings().bloom=saved_bloom;
        renderer.settings().taa=saved_taa;
        renderer.settings().debug_view=saved_debug;
        renderer.reset_history();
      }
      if (args.gallery_analysis) {
        if (args.shot == "aerial" || args.shot == "galaxy") {
          for (const auto &item :
               {std::pair{"clay", 13}, std::pair{"silhouette", 14},
                std::pair{"neutral", 15}}) {
            args.view = item.first;
            capture_view(args.shot + "-" + item.first, item.second);
          }
        }
        renderer.settings().debug_view = saved_debug;
        renderer.reset_history();
        frame_ms.clear();
        args.view = "motion";
        for (int i = 0; i < 12; ++i) {
          cam.position +=
              cam.right() *
              ((args.shot == "aerial" || args.shot == "galaxy") ? .35f : .08f);
          timed_frame(0.0f, nullptr);
          if (i == 0 || i == 5 || i == 11)
            ok = renderer.capture_png((dir / (args.shot + "-motion-" +
                                              std::to_string(i) + ".png"))
                                          .string()) &&
                 ok;
        }
        args.timings = (dir / (args.shot + "-motion-timings.json")).string();
        report_timings();
      }
    }
    renderer.shutdown();
    arrays.albedo.release();
    arrays.normal.release();
    arrays.arm.release();
    release_environments();
    gpu.destroy();
    glfwDestroyWindow(window);
    glfwTerminate();
    return ok ? 0 : 1;
  }

  if (args.sweep > 0) {
    // Temporal-artifact analysis under motion. The camera moves by
    // `sweep_step` per frame (sideways, forward or down). Each pixel of the
    // current frame is reprojected into the previous frame through the
    // depth buffer and the two view-projections (exact for a static world),
    // the previous frame is sampled there bilinearly, and the difference is
    // taken. A band-limited image would give zero up to resampling blur, so
    // a tolerance of half a pixel of local gradient is subtracted. What is
    // left is temporal aliasing: shimmer, moire, crawling edges, popping.
    // Luminance and chroma are kept apart so colour-only flicker shows up.
    std::vector<std::uint8_t> prev, cur, ids;
    std::vector<float> depth;
    std::uint32_t w = 0, h = 0;
    const int saved_debug = renderer.settings().debug_view;
    renderer.settings().debug_view = 12;
    renderer.render(cam, 0.0f, nullptr);
    renderer.read_frame(&ids, &w, &h);
    stop_for_gpu_failure();
    renderer.settings().debug_view = saved_debug;
    renderer.reset_history();
    std::vector<float> resid(static_cast<std::size_t>(w) * h, 0.0f),
        raw(resid.size(), 0.0f), chroma(resid.size(), 0.0f);
    cb::Camera c2 = cam;
    // warm-up so TAA history converges before measuring
    for (int i = 0; i < 12; ++i)
      renderer.render(c2, 0.0f, nullptr);
    renderer.render(c2, 0.0f, nullptr);
    renderer.read_frame(&prev, &w, &h);
    stop_for_gpu_failure();
    cb::Mat4 prev_vp = renderer.last_view_proj();
    // optional box blur (radius sweep_blur) of both frames: a well-filtered
    // 1 px line still trips the differencing when it moves; the blur leaves
    // the low-frequency beating (moire, shimmer) that the eye actually sees
    auto blur = [&](std::vector<std::uint8_t> &img) {
      const int R = args.sweep_blur;
      if (R <= 0)
        return;
      std::vector<std::uint8_t> tmp(img.size());
      for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x)
          for (int c = 0; c < 3; ++c) {
            int sum = 0, n = 0;
            for (int k = -R; k <= R; ++k) {
              const int xx = static_cast<int>(x) + k;
              if (xx < 0 || xx >= static_cast<int>(w))
                continue;
              sum += img[(static_cast<std::size_t>(y) * w + xx) * 4 + c];
              ++n;
            }
            tmp[(static_cast<std::size_t>(y) * w + x) * 4 + c] =
                static_cast<std::uint8_t>(sum / std::max(n, 1));
          }
      for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x)
          for (int c = 0; c < 3; ++c) {
            int sum = 0, n = 0;
            for (int k = -R; k <= R; ++k) {
              const int yy = static_cast<int>(y) + k;
              if (yy < 0 || yy >= static_cast<int>(h))
                continue;
              sum += tmp[(static_cast<std::size_t>(yy) * w + x) * 4 + c];
              ++n;
            }
            img[(static_cast<std::size_t>(y) * w + x) * 4 + c] =
                static_cast<std::uint8_t>(sum / std::max(n, 1));
          }
    };
    auto lum = [](const std::vector<std::uint8_t> &img, std::size_t px) {
      return (0.299f * img[px * 4] + 0.587f * img[px * 4 + 1] +
              0.114f * img[px * 4 + 2]) /
             255.0f;
    };
    // bilinear sample of one channel (0..2 = rgb, 3 = luminance) at a
    // fractional position
    auto sample = [&](const std::vector<std::uint8_t> &img, float fx, float fy,
                      int ch) {
      fx = std::min(std::max(fx, 0.0f), static_cast<float>(w) - 1.001f);
      fy = std::min(std::max(fy, 0.0f), static_cast<float>(h) - 1.001f);
      const std::uint32_t x0 = static_cast<std::uint32_t>(fx),
                          y0 = static_cast<std::uint32_t>(fy);
      const float tx = fx - static_cast<float>(x0),
                  ty = fy - static_cast<float>(y0);
      auto at = [&](std::uint32_t x, std::uint32_t y) {
        const std::size_t px = static_cast<std::size_t>(y) * w + x;
        return ch == 3 ? lum(img, px)
                       : img[px * 4 + static_cast<std::size_t>(ch)] / 255.0f;
      };
      return (at(x0, y0) * (1.0f - tx) + at(x0 + 1, y0) * tx) * (1.0f - ty) +
             (at(x0, y0 + 1) * (1.0f - tx) + at(x0 + 1, y0 + 1) * tx) * ty;
    };
    blur(prev);
    int measured = 0;
    for (int i = 0; i < args.sweep; ++i) {
      if (args.sweep_dir == 1)
        c2.position += c2.forward() * args.sweep_step;
      else if (args.sweep_dir == 2)
        c2.position.y -= args.sweep_step;
      else
        c2.position += c2.right() * args.sweep_step;
      renderer.render(c2, static_cast<float>(i + 1) * 0.016f, nullptr);
      renderer.read_frame(&cur, &w, &h);
      stop_for_gpu_failure();
      blur(cur);
      renderer.read_depth(&depth, &w, &h);
      stop_for_gpu_failure();
      const cb::Mat4 cur_vp = renderer.last_view_proj();
      const cb::Mat4 inv_cur = cb::inverse(cur_vp);
      for (std::uint32_t y = 1; y + 1 < h; ++y) {
        for (std::uint32_t x = 1; x + 1 < w; ++x) {
          const std::size_t px = static_cast<std::size_t>(y) * w + x;
          const float lc = lum(cur, px);
          raw[px] += std::fabs(lc - lum(prev, px));
          // where was this pixel's surface in the previous frame?
          const float z = depth[px];
          float fx = static_cast<float>(x) + 0.5f,
                fy = static_cast<float>(y) + 0.5f;
          if (z > 1e-7f) { // not sky (reversed Z)
            const cb::Vec4 ndc{fx / w * 2.0f - 1.0f, 1.0f - fy / h * 2.0f, z,
                               1.0f};
            cb::Vec4 wp = cb::mul(inv_cur, ndc);
            cb::Vec3 world = cb::Vec3{wp.x / wp.w, wp.y / wp.w, wp.z / wp.w};
            cb::Vec4 pc = cb::mul(prev_vp, cb::Vec4{world, 1.0f});
            if (pc.w <= 1e-4f)
              continue;
            fx = (pc.x / pc.w * 0.5f + 0.5f) * w;
            fy = (0.5f - pc.y / pc.w * 0.5f) * h;
            if (fx < 1.0f || fy < 1.0f || fx > w - 2.0f || fy > h - 2.0f)
              continue; // came from off-screen
          }
          const float lp = sample(prev, fx - 0.5f, fy - 0.5f, 3);
          const float gx =
              0.5f * std::fabs(lum(cur, px + 1) - lum(cur, px - 1));
          const float gy =
              0.5f * std::fabs(lum(cur, px + w) - lum(cur, px - w));
          const float tol = 0.5f * (gx + gy) + 0.004f;
          resid[px] += std::max(0.0f, std::fabs(lc - lp) - tol);
          // chroma: red and blue relative to green, the same tolerance rule
          const float rc = cur[px * 4] / 255.0f - cur[px * 4 + 1] / 255.0f;
          const float bc = cur[px * 4 + 2] / 255.0f - cur[px * 4 + 1] / 255.0f;
          const float rp = sample(prev, fx - 0.5f, fy - 0.5f, 0) -
                           sample(prev, fx - 0.5f, fy - 0.5f, 1);
          const float bp = sample(prev, fx - 0.5f, fy - 0.5f, 2) -
                           sample(prev, fx - 0.5f, fy - 0.5f, 1);
          chroma[px] += std::max(
              0.0f, 0.5f * (std::fabs(rc - rp) + std::fabs(bc - bp)) - tol);
        }
      }
      prev.swap(cur);
      prev_vp = cur_vp;
      ++measured;
    }
    const float norm = 1.0f / static_cast<float>(std::max(measured, 1));
    double mat_sum[256] = {}, mat_cnt[256] = {}, mat_chroma[256] = {},
           total = 0.0, total_raw = 0.0, total_chroma = 0.0;
    std::vector<std::uint8_t> heat(static_cast<std::size_t>(w) * h * 4, 255);
    for (std::size_t px = 0; px < resid.size(); ++px) {
      const float f = resid[px] * norm;
      total += f;
      total_raw += raw[px] * norm;
      total_chroma += chroma[px] * norm;
      const bool sky = ids[px * 4 + 1] > 8 || ids[px * 4 + 2] > 8;
      const int id = sky ? 255 : ids[px * 4];
      mat_sum[id] += f;
      mat_chroma[id] += chroma[px] * norm;
      mat_cnt[id] += 1.0;
      const float v = std::min(1.0f, f * 10.0f);
      heat[px * 4 + 0] =
          static_cast<std::uint8_t>(255.0f * std::min(1.0f, v * 2.0f));
      heat[px * 4 + 1] = static_cast<std::uint8_t>(
          255.0f * std::max(0.0f, std::min(1.0f, v * 2.0f - 0.6f)));
      heat[px * 4 + 2] = static_cast<std::uint8_t>(
          255.0f * std::max(0.0f, 1.0f - v * 4.0f) * 0.25f);
    }
    const char *dir_name = args.sweep_dir == 1
                               ? "forward"
                               : (args.sweep_dir == 2 ? "down" : "right");
    std::printf("  sweep: %d steps of %.3f m %s, taa %s; mean temporal "
                "residual %.5f, chroma %.5f (raw frame difference %.5f)\n",
                measured, args.sweep_step, dir_name,
                renderer.settings().taa ? "on" : "off",
                total / static_cast<double>(resid.size()),
                total_chroma / static_cast<double>(resid.size()),
                total_raw / static_cast<double>(resid.size()));
    struct Row {
      int id;
      double mean;
      double chroma;
      double share;
    };
    std::vector<Row> rows;
    for (int id = 0; id < 256; ++id) {
      if (mat_cnt[id] < 200)
        continue;
      rows.push_back(Row{id, mat_sum[id] / mat_cnt[id],
                         mat_chroma[id] / mat_cnt[id],
                         mat_sum[id] / std::max(total, 1e-9)});
    }
    std::sort(rows.begin(), rows.end(),
              [](const Row &p1, const Row &p2) { return p1.share > p2.share; });
    std::printf("  %-4s %-18s %-10s %-10s %-10s %s\n", "id", "material",
                "pixels", "residual", "chroma", "share");
    for (std::size_t i = 0; i < rows.size() && i < 12; ++i) {
      const Row &r = rows[i];
      const char *name =
          r.id == 255 ? "(sky)"
                      : (r.id < static_cast<int>(scene.materials.size())
                             ? scene.materials[static_cast<std::size_t>(r.id)]
                                   .name.c_str()
                             : "?");
      std::printf("  %-4d %-18s %-10.0f %-10.5f %-10.5f %.1f%%\n", r.id, name,
                  mat_cnt[r.id], r.mean, r.chroma, r.share * 100.0);
    }
    stbi_write_png((args.sweep_out + "-heat.png").c_str(), static_cast<int>(w),
                   static_cast<int>(h), 4, heat.data(),
                   static_cast<int>(w * 4));
    renderer.capture_png(args.sweep_out + "-frame.png");
    std::printf("  wrote %s-heat.png and %s-frame.png\n",
                args.sweep_out.c_str(), args.sweep_out.c_str());
    renderer.shutdown();
    gpu.destroy();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
  }
  if (args.stress > 0) {
    // Offscreen stress test: recreate the render targets every frame (as a
    // window resize or an option toggle would) and check the renderer survives.
    for (int i = 0; i < args.stress; ++i) {
      renderer.settings().msaa = (i & 1) ? 1 : 4;
      renderer.settings().ssao_half = (i & 2) != 0;
      renderer.apply_settings();
      renderer.render(cam, static_cast<float>(i) * 0.016f, nullptr);
    }
    gpu.poll(true);
    stop_for_gpu_failure();
    std::printf("  stress: %d target recreations survived\n", args.stress);
    renderer.shutdown();
    gpu.destroy();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
  }
  if (args.bench > 0) {
    // Offscreen benchmark: render N frames without presenting, wait for the
    // GPU, report the average. Independent of vsync and window visibility.
    for (int i = 0; i < 5; ++i)
      renderer.render(cam, 0.0f, nullptr);
    gpu.poll(true);
    stop_for_gpu_failure();
    const double b0 = elapsed();
    for (int i = 0; i < args.bench; ++i)
      timed_frame(0.0f, nullptr);
    gpu.poll(true);
    stop_for_gpu_failure();
    const double b1 = elapsed();
    const cb::Renderer::Stats &rs = renderer.stats();
    std::printf("  bench: %d frames, %.2f ms/frame, %u triangles resident, "
                "%.2f M drawn, %u/%u ranges (%u gpu-occluded), %ux%u, msaa %u, "
                "ssao %d%s, shadows %d%s%s, taa %d, occlusion %d\n",
                args.bench, (b1 - b0) * 1000.0 / args.bench,
                renderer.triangles(),
                static_cast<double>(rs.indices_drawn) / 3.0e6, rs.ranges_drawn,
                rs.ranges_total, rs.ranges_occluded, gpu.width, gpu.height,
                renderer.settings().msaa, settings.ssao ? 1 : 0,
                settings.ssao_half ? " half" : "", settings.shadows ? 1 : 0,
                settings.shadow_half_rate ? " half-rate" : "",
                settings.shadow_far_lod ? " far-lod" : "", settings.taa ? 1 : 0,
                settings.occlusion ? 1 : 0);
    report_timings();
    renderer.shutdown();
    gpu.destroy();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
  }
  if (!args.route.empty()) {
    struct RouteView {
      cb::Vec3 position, target;
    };
    std::vector<RouteView> points;
    std::ifstream input(args.route);
    std::string line;
    while (std::getline(input, line)) {
      if (line.empty() || line.front() == '#')
        continue;
      std::istringstream record(line);
      RouteView view;
      std::string extra;
      if (!(record >> view.position.x >> view.position.y >> view.position.z >>
            view.target.x >> view.target.y >> view.target.z) ||
          (record >> extra) || !std::isfinite(cb::length(view.position)) ||
          !std::isfinite(cb::length(view.target)) ||
          cb::length(view.target - view.position) < .001f) {
        std::fprintf(stderr, "invalid camera route record\n");
        return 2;
      }
      points.push_back(view);
    }
    if (points.size() < 2) {
      std::fprintf(stderr, "camera route requires at least two points\n");
      return 2;
    }
    const std::filesystem::path dir(args.route_out);
    std::filesystem::create_directories(dir);
    renderer.reset_history();
    frame_ms.clear();
    cam.velocity = {};
    cam.position = points.front().position;
    cam.look_at_point(points.front().target);
    bool ok = true;
    int index = 0;
    constexpr float max_distance_step = 1.f, max_angle_step = cb::kPi / 36.f;
    std::vector<int> segment_steps(points.size());
    std::size_t total_frames = 1;
    for (std::size_t i = 1; i < points.size(); ++i) {
      const float distance =
          cb::length(points[i].position - points[i - 1].position);
      const float angle = std::acos(cb::clampf(
          cb::dot(cb::normalize(points[i - 1].target - points[i - 1].position),
                  cb::normalize(points[i].target - points[i].position)),
          -1.f, 1.f));
      segment_steps[i] =
          std::max({4, int(std::ceil(distance / max_distance_step)),
                    int(std::ceil(angle / max_angle_step))});
      total_frames += segment_steps[i];
    }
    const std::size_t capture_stride =
        std::max<std::size_t>(1, total_frames / 64);
    auto record_frame = [&](bool endpoint) {
      glfwPollEvents();
      timed_frame(0.f, nullptr);
      if (index == 0 || endpoint || std::size_t(index) % capture_stride == 0) {
        char name[64];
        std::snprintf(name, sizeof(name), "route-%05d.png", index);
        ok = renderer.capture_png((dir / name).string()) && ok;
      }
      ++index;
    };
    record_frame(false);
    for (std::size_t i = 1; i < points.size(); ++i) {
      const cb::Vec3 from = cb::normalize(points[i - 1].target -
                                          points[i - 1].position),
                     to = cb::normalize(points[i].target - points[i].position);
      const float cosine = cb::clampf(cb::dot(from, to), -1.f, 1.f),
                  angle = std::acos(cosine);
      for (int step = 1; step <= segment_steps[i]; ++step) {
        const float t = float(step) / segment_steps[i];
        cam.position =
            points[i - 1].position * (1 - t) + points[i].position * t;
        cb::Vec3 direction;
        if (cosine > .9995f)
          direction = cb::normalize(from * (1 - t) + to * t);
        else if (cosine < -.9995f) {
          cb::Vec3 axis = cb::cross(from, cb::Vec3{0, 1, 0});
          if (cb::length(axis) < .001f)
            axis = cb::cross(from, cb::Vec3{1, 0, 0});
          axis = cb::normalize(axis);
          direction = from * std::cos(angle * t) +
                      cb::cross(axis, from) * std::sin(angle * t);
          if (step == segment_steps[i])
            direction = to;
        } else
          direction =
              (from * std::sin((1 - t) * angle) + to * std::sin(t * angle)) *
              (1.f / std::sin(angle));
        cam.look_at_point(cam.position + direction);
        record_frame(i + 1 == points.size() && step == segment_steps[i]);
      }
    }
    std::ofstream sampling(dir / "route-sampling.json");
    sampling << "{\n  \"maximum_translation_step_metres\": 1,\n  "
                "\"maximum_direction_step_degrees\": 5,\n  "
                "\"includes_initial_waypoint\": true,\n  \"frame_count\": "
             << index << ",\n  \"continuous_temporal_history\": true\n}\n";
    ok = bool(sampling) && ok;
    args.view = "route";
    args.timings = (dir / "route-timings.json").string();
    report_timings();
    std::printf("  camera route: %zu waypoints, %d useful frames, %s\n",
                points.size(), index, ok ? "ok" : "FAILED");
    renderer.shutdown();
    arrays.albedo.release();
    arrays.normal.release();
    arrays.arm.release();
    release_environments();
    gpu.destroy();
    glfwDestroyWindow(window);
    glfwTerminate();
    return ok ? 0 : 1;
  }
  if (!args.capture.empty()) {
    for (int i = 0; i < args.frames; ++i) {
      timed_frame(0.0f, nullptr);
      if (i == 0 && !args.capture_initial.empty() &&
          !renderer.capture_png(args.capture_initial)) {
        std::fprintf(stderr, "initial capture failed\n");
        return 1;
      }
    }
    gpu.poll(true);
    stop_for_gpu_failure();
    const bool ok = renderer.capture_png(args.capture);
    std::printf("  capture %s: %s (%d fixed-time frames)\n",
                args.capture.c_str(), ok ? "ok" : "FAILED", args.frames);
    report_timings();
    renderer.shutdown();
    arrays.albedo.release();
    arrays.normal.release();
    arrays.arm.release();
    release_environments();
    gpu.destroy();
    glfwDestroyWindow(window);
    glfwTerminate();
    return ok ? 0 : 1;
  }
  double last = elapsed();
  const double fps_first = last;
  int frame = 0;
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
    const float dt = static_cast<float>(std::min(now - last, 1.0));
    last = now;
    // resize
    int fw = 0, fh = 0;
    glfwGetFramebufferSize(window, &fw, &fh);
    if (fw > 0 && fh > 0 &&
        (static_cast<std::uint32_t>(fw) != gpu.width ||
         static_cast<std::uint32_t>(fh) != gpu.height)) {
      gpu.resize(static_cast<std::uint32_t>(fw),
                 static_cast<std::uint32_t>(fh));
      if (!args.fullscreen)
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
      if (!g_input.first)
        cam.look(static_cast<float>(mx - g_input.last_mx),
                 static_cast<float>(my - g_input.last_my));
      g_input.first = false;
      g_input.last_mx = mx;
      g_input.last_my = my;
    }
    if (g_input.scroll != 0.0) {
      cam.speed = cb::clampf(
          cam.speed * std::pow(1.25f, static_cast<float>(g_input.scroll)), 0.5f,
          400.0f);
      g_input.scroll = 0.0;
    }
    float fwd = 0, strafe = 0, up = 0;
    if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS)
      fwd += 1;
    if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS)
      fwd -= 1;
    if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS)
      strafe += 1;
    if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS)
      strafe -= 1;
    if (glfwGetKey(window, GLFW_KEY_E) == GLFW_PRESS ||
        glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS)
      up += 1;
    if (glfwGetKey(window, GLFW_KEY_Q) == GLFW_PRESS)
      up -= 1;
    float mult = 1.0f;
    if (glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS)
      mult = 5.0f;
    if (glfwGetKey(window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS)
      mult = 0.2f;
    cam.update(fwd, strafe, up, dt, mult);
    if (pressed(GLFW_KEY_N)) {
      night = !night;
      renderer.set_environment(environment_for(args.shot, night), night);
    }
    if (pressed(GLFW_KEY_F1))
      renderer.settings().debug_view =
          (renderer.settings().debug_view + 1) % 10;
    if (pressed(GLFW_KEY_F2))
      renderer.settings().ssao = !renderer.settings().ssao;
    if (pressed(GLFW_KEY_F3))
      renderer.settings().shadows = !renderer.settings().shadows;
    if (pressed(GLFW_KEY_F4))
      renderer.settings().bloom = !renderer.settings().bloom;
    if (pressed(GLFW_KEY_F5))
      renderer.settings().fxaa = !renderer.settings().fxaa;
    if (pressed(GLFW_KEY_F6))
      renderer.settings().taa = !renderer.settings().taa;
    auto print_settings = [&] {
      const cb::RenderSettings &st = renderer.settings();
      std::printf("  settings: msaa %u, ssao %s%s, shadows %s%s%s, taa %s, "
                  "occlusion %s, bloom %s, fxaa %s\n",
                  st.msaa, st.ssao ? "on" : "off",
                  st.ssao_half ? " (half res)" : "", st.shadows ? "on" : "off",
                  st.shadow_half_rate ? " (far cascades half rate)" : "",
                  st.shadow_far_lod ? " (far lod)" : "", st.taa ? "on" : "off",
                  st.occlusion ? "on" : "off", st.bloom ? "on" : "off",
                  st.fxaa ? "on" : "off");
    };
    if (pressed(GLFW_KEY_F7)) {
      renderer.settings().occlusion = !renderer.settings().occlusion;
      print_settings();
    }
    if (pressed(GLFW_KEY_F8)) {
      renderer.settings().ssao_half = !renderer.settings().ssao_half;
      renderer.apply_settings();
      print_settings();
    }
    if (pressed(GLFW_KEY_F9)) {
      renderer.settings().msaa = renderer.settings().msaa > 1 ? 1 : 4;
      renderer.apply_settings();
      print_settings();
    }
    if (pressed(GLFW_KEY_F10)) {
      renderer.settings().shadow_half_rate =
          !renderer.settings().shadow_half_rate;
      print_settings();
    }
    if (pressed(GLFW_KEY_F11)) {
      renderer.settings().shadow_far_lod = !renderer.settings().shadow_far_lod;
      print_settings();
    }
    if (pressed(GLFW_KEY_EQUAL) || pressed(GLFW_KEY_KP_ADD))
      renderer.settings().exposure_bias += 0.25f;
    if (pressed(GLFW_KEY_MINUS) || pressed(GLFW_KEY_KP_SUBTRACT))
      renderer.settings().exposure_bias -= 0.25f;
    auto regenerate = [&] {
      try {
        scene = cb::generate_scene(sp);
        renderer.set_scene(scene, arrays);
      } catch (const std::exception &e) {
        stop_renderer(e.what());
      }
      renderer.reset_history();
      std::printf("  seed %s, size %s: %u triangles, %d towers, %d standard "
                  "buildings\n",
                  sp.seed.c_str(), scene.city_size.c_str(),
                  renderer.triangles(), scene.stats_towers,
                  scene.stats_standards);
    };
    if (pressed(GLFW_KEY_R)) {
      sp.seed = std::to_string(std::strtoull(sp.seed.c_str(), nullptr, 16) + 1);
      regenerate();
    }
    for (int i = 0; i < 6; ++i)
      if (pressed(GLFW_KEY_1 + i)) {
        static const char *shots[] = {"aerial", "galaxy", "civic",
                                      "street", "garden", "landing"};
        cb::Vec3 target;
        cb::shot_camera(shots[i], cam.position, target);
        cam.fov_y = cb::radians(cb::shot_fov_degrees(shots[i]));
        cam.look_at_point(target);
        cam.velocity = {};
        renderer.reset_history();
        args.shot = shots[i];
        cam.speed = i < 2 ? 25.f : 2.f;
        night = i == 1;
        renderer.set_environment(environment_for(args.shot, night), night);
        std::printf("  view: %s\n", shots[i]);
      }
    if (pressed(GLFW_KEY_P)) {
      std::printf("  camera --cam %.1f,%.1f,%.1f --target %.1f,%.1f,%.1f\n",
                  cam.position.x, cam.position.y, cam.position.z,
                  (cam.position + cam.forward() * 100.0f).x,
                  (cam.position + cam.forward() * 100.0f).y,
                  (cam.position + cam.forward() * 100.0f).z);
    }
    // render
    WGPUTexture surface_tex = nullptr;
    WGPUTextureView view = gpu.acquire_frame(&surface_tex);
    if (view == nullptr)
      continue;
    renderer.render(cam, static_cast<float>(now), view);
    gpu.poll(true);
    stop_for_gpu_failure();
    if (pressed(GLFW_KEY_F12)) {
      char name[64];
      std::snprintf(name, sizeof(name), "human_tech_demo-%02d.png", shot++);
      if (renderer.capture_png(name))
        std::printf("  wrote %s\n", name);
    }
    if (!args.capture.empty() && frame == args.frames - 1) {
      if (renderer.capture_png(args.capture))
        std::printf("  wrote %s\n", args.capture.c_str());
      else
        std::fprintf(stderr, "capture failed\n");
    }
    gpu.present();
    if (frame == 0) {
      if (!args.capture_initial.empty() && !renderer.capture_png(args.capture_initial))
        std::fprintf(stderr, "Initial live capture failed: %s\n", args.capture_initial.c_str());
      std::printf("  first live frame presented (%s, render %ux%u, display %ux%u)\n",
                   args.fullscreen ? "full screen" : "window",
                   args.fullscreen ? static_cast<unsigned>(args.width) : gpu.width,
                   args.fullscreen ? static_cast<unsigned>(args.height) : gpu.height,
                   gpu.width, gpu.height);
    }
    wgpuTextureViewRelease(view);
    wgpuTextureRelease(surface_tex);
    gpu.poll(false);
    stop_for_gpu_failure();
    ++frame;
    if (!args.capture.empty() && frame >= args.frames)
      break;
  }
  gpu.poll(true);
  stop_for_gpu_failure();
  if (frame > 0) {
    std::printf("  %d frames, %.1f ms/frame average (includes setup-free "
                "steady state only if run long)\n",
                frame, (elapsed() - fps_first) * 1000.0 / frame);
  }
  renderer.shutdown();
  arrays.albedo.release();
  arrays.normal.release();
  arrays.arm.release();
  release_environments();
  gpu.destroy();
  glfwDestroyWindow(window);
  glfwTerminate();
  return 0;
}
