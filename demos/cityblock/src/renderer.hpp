#pragma once
// The demo renderer: cascaded shadow maps, depth/normal prepass, SSAO,
// MSAA forward PBR with HDRI IBL and interior-mapped glass, bloom, ACES,
// FXAA. Fixed pipeline; a handful of runtime toggles.
#include <cstdint>
#include <string>
#include <vector>

#include "camera.hpp"
#include "gpu.hpp"
#include "ibl.hpp"
#include "math.hpp"
#include "scene.hpp"
#include "textures.hpp"

namespace cb {

struct RenderSettings {
  std::uint32_t msaa{4};
  bool ssao{true};
  bool shadows{true};
  bool bloom{true};
  bool fxaa{true};
  int debug_view{0};  // 0 final, 1 albedo, 2 normals, 3 ao, 4 shadow cascades, 5 roughness
  float exposure_bias{0.0f};  // EV
  std::uint32_t shadow_size{2048};
};

class Renderer {
 public:
  bool init(Gpu* gpu, const std::string& shader_dir, RenderSettings settings, std::string* error);
  void shutdown();

  void set_scene(const Scene& scene, const MaterialArrays& arrays);
  // Which environment the frame uses (day or night); night also enables
  // point lights and lit interiors.
  void set_environment(const Environment* env, bool night);
  void resize(std::uint32_t w, std::uint32_t h);

  // Renders into `target` (the acquired surface view or nullptr to skip the
  // final blit, e.g. for a capture-only frame).
  void render(const Camera& camera, float time_s, WGPUTextureView target);
  bool capture_png(const std::string& path);
  RenderSettings& settings() { return settings_; }
  std::uint32_t triangles() const { return triangles_; }

 private:
  struct Impl;
  Impl* impl_{nullptr};
  Gpu* gpu_{nullptr};
  RenderSettings settings_;
  std::uint32_t triangles_{0};
};

}  // namespace cb
