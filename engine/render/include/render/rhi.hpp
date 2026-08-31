#pragma once

#include <cstdint>
#include <memory>
#include <string>

struct GLFWwindow;

namespace inf::render {

// Thin RHI over wgpu-native. Deliberately minimal: it grows only with
// demonstrated need (T0001 risk note). The world never depends on this.
class Rhi {
 public:
  // Creates instance/surface/adapter/device and configures the swapchain
  // for the window's current framebuffer size. Returns nullptr on failure
  // with a human-readable reason in *error.
  static std::unique_ptr<Rhi> create(GLFWwindow* window, std::string* error);

  Rhi(const Rhi&) = delete;
  Rhi& operator=(const Rhi&) = delete;
  ~Rhi();

  void resize(std::uint32_t width, std::uint32_t height);

  // Acquires the next surface texture, clears it to the given color,
  // presents. Returns false if the frame had to be skipped (e.g. surface
  // outdated mid-resize); rendering can continue next frame.
  bool render_clear(float r, float g, float b);

  // --- meshes (M3+) ----------------------------------------------------
  // Uploads an interleaved [px py pz nx ny nz] f32 triangle soup; returns
  // a handle. Meshes are static in v0 (re-upload = new mesh).
  std::uint32_t create_mesh(const float* vertices, std::size_t float_count);
  void destroy_mesh(std::uint32_t mesh);

  struct DrawItem {
    std::uint32_t mesh{0};
    // Column-major model-view-projection (camera-relative; f32-safe).
    float mvp[16]{};
    // mode 0: rgb + a == 0 => lit terrain material (rgb ignored);
    // a > 0 => unlit solid color with that alpha (opaque pipeline ignores
    // alpha; the translucent pipeline blends it).
    // mode 1 (star photosphere): rgb = blackbody tint.
    // mode 2 (corona billboard, additive pass): rgb = glow tint.
    float color[4]{0.0f, 0.0f, 0.0f, 0.0f};
    // Mode-specific extras (see the shader block comment):
    // star: aux.xyz camera->star-center unit dir, aux.w phase seed;
    // extra.x spot amount / glow intensity, extra.y ray sharpness.
    // extra.w is reserved (carries the mode to the shader).
    float aux[4]{};
    float extra[4]{};
    // 0 = legacy lit/unlit, 1 = star surface, 2 = additive corona/glow.
    std::uint32_t mode{0};
    // Drawn in a second, alpha-blended, no-depth-write pass (mode 0 only).
    bool translucent = false;
  };

  // Per-frame globals: sky clear color, directional sun light (unit
  // vector, in the same frame as the mesh normals — pointing FROM the
  // surface TOWARD the sun), light tint, and a time in seconds that
  // drives the animated star shaders.
  struct FrameParams {
    float sky[3]{0.0f, 0.0f, 0.0f};
    float sun_dir[3]{0.45f, 0.75f, 0.5f};
    float sun_color[3]{1.0f, 1.0f, 1.0f};
    float time_s{0.0f};
  };

  // Clears, draws the items (sun-lit terrain, unlit overlays, star
  // surfaces, additive glows), presents.
  bool render_frame(const FrameParams& frame, const DrawItem* items,
                    std::size_t item_count);

  // Human-readable adapter description ("<name> (<backend>)").
  const std::string& adapter_info() const;

 private:
  struct Impl;
  explicit Rhi(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace inf::render
