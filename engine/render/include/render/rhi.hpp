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
  // Terrain path (T0015 WP3): interleaved
  // [px py pz nx ny nz mat_pack blend] — mat_pack = mat0 * 256 + mat1
  // (material ids), blend = fraction of mat1. create_mesh() uploads
  // legacy 6-float soups and expands them with mat_pack = 0 (flat base
  // albedo path).
  std::uint32_t create_mesh_mat(const float* vertices, std::size_t float_count);
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
    // 0 = legacy lit/unlit, 1 = star surface, 2 = additive corona/glow,
    // 3 = additive glow sprite (lens flare / veil / limb halo; extra.x
    // intensity, extra.y falloff, extra.z rim radius or 0 for a disc),
    // 4 = analytic sky dome (opaque fullscreen quad at far depth).
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
    // Camera basis + projection half-tangents and atmosphere state for
    // the mode-4 sky dome (all in the mesh frame). altitude_frac is the
    // camera altitude over the atmosphere height (>= 1 = space); the
    // defaults keep the dome black when an app never fills these in.
    float cam_right[3]{1.0f, 0.0f, 0.0f};
    float cam_up[3]{0.0f, 1.0f, 0.0f};
    float cam_fwd[3]{0.0f, 0.0f, -1.0f};
    float tan_half_x{1.0f};
    float tan_half_y{1.0f};
    float planet_up[3]{0.0f, 0.0f, 1.0f};
    float altitude_frac{2.0f};
    float atmo_tint[3]{0.4f, 0.6f, 0.9f};
    // Anchor planet center relative to the camera, and how strongly lit
    // terrain normals blend toward the analytic sphere radial (0 on the
    // surface, 1 from orbit; hides per-chunk normal seams at distance —
    // lit items must then carry their translation in DrawItem.aux).
    float planet_center[3]{0.0f, 0.0f, 0.0f};
    float normal_blend{0.0f};
    // Sea-surface radius from the planet center (0 = no water). Lit
    // terrain below it shades toward deep-water blue by depth, so the
    // seabed, the ocean impostor and the translucent shell agree.
    float sea_radius_m{0.0f};
    // Per-planet palette variation applied to material albedos (-1..1).
    float palette_shift{0.0f};
  };

  // Clears, draws the items (sun-lit terrain, unlit overlays, star
  // surfaces, additive glows), presents.
  bool render_frame(const FrameParams& frame, const DrawItem* items,
                    std::size_t item_count);

  // One-shot capture: the next render_frame additionally renders the
  // identical scene into an offscreen target, reads it back, and writes
  // it as a binary PPM (P6) to path. Blocks that frame on the GPU
  // readback — a debug/verification tool, not a per-frame feature.
  void request_capture(const std::string& path);

  // --- debug frame recorder (ring buffer + triggered sequences) --------
  // While the ring is enabled (debug mode), every few frames the scene
  // is re-rendered at a reduced resolution and kept in an in-memory ring
  // holding the last few seconds. trigger_recording dumps that ring to
  // dir as numbered PPMs and keeps recording for future_seconds more
  // (this part works even with the ring disabled — release mode).
  // Frames are named seq-%04d.ppm in capture order; times.txt maps each
  // to its frame time.
  void set_ring_enabled(bool enabled);
  void trigger_recording(const std::string& dir, double future_seconds);
  // True while a triggered recording is still capturing future frames
  // (drives the on-screen REC indicator).
  bool recording_active() const;

  // Human-readable adapter description ("<name> (<backend>)").
  const std::string& adapter_info() const;

 private:
  struct Impl;
  explicit Rhi(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace inf::render
