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
  bool taa{true};   // temporal AA (jittered projection + depth reprojection)
  int debug_view{0};  // 0 final, 1 albedo, 2 normals, 3 ao, 4 shadow cascades, 5 roughness, 6-9 lighting terms,
                      // 10 raw ssao, 11 prepass normals, 12 material id (linear, no tonemap)
  float exposure_bias{0.0f};  // EV
  std::uint32_t shadow_size{2048};
  float jitter_x{0.0f}, jitter_y{0.0f};  // projection offset in pixels (analysis / TAA)
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
  // Reads back the final LDR frame (RGBA8, sRGB-encoded) into rgba.
  bool read_frame(std::vector<std::uint8_t>* rgba, std::uint32_t* w, std::uint32_t* h);
  // Reads back the 1x depth buffer (0..1, near = 0) and the unjittered view-projection.
  bool read_depth(std::vector<float>* depth, std::uint32_t* w, std::uint32_t* h);
  const Mat4& last_view_proj() const;
  void reset_history();
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
