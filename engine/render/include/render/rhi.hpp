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
  // Terrain path (T0019): interleaved [px py pz nx ny nz w0 w1 w2 w3] —
  // four weights over the draw item's material palette (DrawItem::
  // material_palette). create_mesh() uploads legacy 6-float soups and
  // expands them with zero weights (flat base albedo path). Star fields
  // reuse the layout with their own meaning for the attributes.
  std::uint32_t create_mesh_mat(const float* vertices, std::size_t float_count);
  void destroy_mesh(std::uint32_t mesh);

  // --- planet cube-map textures (T0016) --------------------------------
  // One height + material pair of 6-layer texture arrays per body, in
  // the engine cube-sphere frame (layer = face), sampled by the textured
  // planet impostor (mode 6). Height is R16Float, normalized to [-1, 1]
  // over the body's height amplitude; material is RGBA8 albedo. This is
  // deliberately NOT a general texture system — one pair per resident
  // body plus a shared sampler.
  std::uint32_t create_planet_texture(std::uint32_t face_size);
  // Full-layer upload of one cube face: height as raw IEEE half floats
  // (face_size^2), material as RGBA8 (face_size^2 * 4 bytes).
  void update_planet_face(std::uint32_t handle, std::uint32_t face,
                          const std::uint16_t* height_half, const std::uint8_t* rgba);
  void destroy_planet_texture(std::uint32_t handle);

  // --- surface material library (T0019, design/surface-texturing.md) --
  // Two RGBA8 texture arrays with full mip chains, one layer per material
  // id: albedo.rgb + height.a, and tangent normal.xy + roughness.z + ao.w
  // (emissive mask in .w for glowing materials). Lit terrain (mode 0 with
  // a material pair) samples them hex-tiled and biplanar in planet-local
  // metres; until a library is created (or a layer is uploaded) the
  // material's mean colour from the table below is used instead.
  void create_material_library(std::uint32_t size, std::uint32_t layers);
  // Full-layer upload (size^2 RGBA8 each); mips are generated here.
  void upload_material_layer(std::uint32_t layer, const std::uint8_t* albedo_rgba,
                             const std::uint8_t* normal_rgba);
  struct MaterialParams {
    float tint[3]{1.0f, 1.0f, 1.0f};   // albedo multiplier (planet palette)
    float tile_m{4.0f};                // metres per repeat, fine scale
    float roughness{0.85f};            // fallback / bias when no map
    float emissive{0.0f};              // night glow strength (HDR units)
    float normal_strength{1.0f};
    float mean[3]{0.5f, 0.5f, 0.5f};   // untinted mean albedo of the tile
  };
  void set_material_params(std::uint32_t layer, const MaterialParams& params);

  struct DrawItem {
    std::uint32_t mesh{0};
    // Column-major model-view-projection (camera-relative; f32-safe).
    float mvp[16]{};
    // mode 0: rgb + a == 0 => lit terrain material (rgb ignored); for
    //   lit terrain extra.xyz = the mesh origin modulo 256 m (planet-local,
    //   computed in double) so texture coordinates stay precise anywhere.
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
    // 4 = analytic sky dome (opaque fullscreen quad at far depth),
    // 6 = textured planet impostor: a unit sphere displaced in the
    //     vertex shader from the planet_texture height map and shaded
    //     from its material map (extra.x = height amplitude / radius,
    //     extra.y = slope scale for shading normals).
    std::uint32_t mode{0};
    // Mode 6 only: handle from create_planet_texture (0 = none).
    std::uint32_t planet_texture{0};
    // Mode 0 lit terrain: the four material ids the vertex weights refer
    // to (0 = unused; all zero = flat base albedo path).
    std::uint8_t material_palette[4]{0, 0, 0, 0};
    // Drawn in a second, alpha-blended, no-depth-write pass (mode 0 only).
    bool translucent = false;
    // T0018: overlay items (HUD, map cards, reticles) are drawn AFTER the
    // HDR post chain, straight onto the tonemapped image — UI must not
    // breathe with the eye's exposure. They still depth-test against the
    // scene.
    bool overlay = false;
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
