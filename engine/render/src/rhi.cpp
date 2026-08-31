#include "render/rhi.hpp"

#include <webgpu/webgpu.h>
#include <webgpu/wgpu.h>  // wgpuDevicePoll (wgpu-native extension)

#include <cstdio>
#include <cstring>
#include <deque>
#include <unordered_map>
#include <utility>
#include <vector>

#include <GLFW/glfw3.h>

#if defined(__linux__)
#define GLFW_EXPOSE_NATIVE_WAYLAND
#define GLFW_EXPOSE_NATIVE_X11
#elif defined(__APPLE__)
#define GLFW_EXPOSE_NATIVE_COCOA
#elif defined(_WIN32)
#define GLFW_EXPOSE_NATIVE_WIN32
#endif
#include <GLFW/glfw3native.h>

#if defined(__APPLE__)
extern "C" void* infinityMetalLayerForCocoaWindow(void* nsWindow);
#endif

namespace inf::render {

namespace {

constexpr std::uint64_t kUniformStride = 256;  // minUniformBufferOffsetAlignment
constexpr std::uint32_t kMaxDrawItems = 4096;
constexpr std::uint64_t kItemUniformSize = 112;  // mvp + color + aux + extra
constexpr std::uint64_t kFrameUniformSize = 128;  // 8 vec4s (see Frame in WGSL)

constexpr const char* kMeshShader = R"(
// Per-item block. aux/extra are mode-specific:
//   mode 0 (extra.w): legacy — color.a == 0 lit terrain, > 0 unlit color.
//   mode 1: star photosphere — aux.xyz = camera->star-center unit dir,
//           aux.w = per-star phase seed, extra.x = spot amount.
//   mode 2: corona/glow billboard (additive pass) — aux.w = phase seed,
//           extra.x = intensity, extra.y = photosphere radius in
//           billboard units, extra.z = diffraction-spike strength [0,1].
//   mode 3: glow sprite (additive pass) — soft radial glow, or a rim halo
//           when extra.z > 0. extra.x = intensity, extra.y = falloff
//           exponent (disc) / sharpness (rim), extra.z = rim radius in
//           quad units (0 = disc). Used for lens flares, the sun veil,
//           and planet limb glow.
//   mode 4: analytic sky dome (opaque, fullscreen quad at far depth):
//           per-pixel view-ray gradient sky from the frame uniforms.
struct Uniforms {
  mvp: mat4x4<f32>,
  color: vec4<f32>,
  aux: vec4<f32>,
  extra: vec4<f32>,
};
// Per-frame globals (frame of the meshes = anchor-planet-local):
//   sun_dir.xyz light direction; sun_color.rgb light tint, .a time (s);
//   cam_right/up/fwd.xyz camera basis, right.w/up.w = tan(fov/2)*aspect
//   and tan(fov/2), fwd.w = camera altitude / atmosphere height;
//   planet_up.xyz local up at the camera; atmo.rgb sky palette.
// planet_center.xyz = anchor planet center relative to the camera,
// planet_center.w = normal blend: how far lit-terrain shading normals
// are pulled toward the analytic sphere radial (0 on the surface, 1
// from orbit — hides per-chunk normal seams at distance).
struct Frame {
  sun_dir: vec4<f32>,
  sun_color: vec4<f32>,
  cam_right: vec4<f32>,
  cam_up: vec4<f32>,
  cam_fwd: vec4<f32>,
  planet_up: vec4<f32>,
  atmo: vec4<f32>,
  planet_center: vec4<f32>,
};
@group(0) @binding(0) var<uniform> u: Uniforms;
@group(0) @binding(1) var<uniform> frame: Frame;

struct VSOut {
  @builtin(position) pos: vec4<f32>,
  @location(0) normal: vec3<f32>,
  @location(1) opos: vec3<f32>,
};

@vertex
fn vs_main(@location(0) position: vec3<f32>, @location(1) normal: vec3<f32>) -> VSOut {
  var out: VSOut;
  out.pos = u.mvp * vec4<f32>(position, 1.0);
  out.normal = normal;
  out.opos = position;
  return out;
}

// --- cheap deterministic 3D value noise + fbm (visual only) --------------
fn hash3(p: vec3<f32>) -> f32 {
  var q = fract(p * vec3<f32>(0.1031, 0.1030, 0.0973));
  q += dot(q, q.yxz + 33.33);
  return fract((q.x + q.y) * q.z);
}

fn vnoise(p: vec3<f32>) -> f32 {
  let i = floor(p);
  let fr = fract(p);
  let w = fr * fr * (3.0 - 2.0 * fr);
  let n000 = hash3(i + vec3<f32>(0.0, 0.0, 0.0));
  let n100 = hash3(i + vec3<f32>(1.0, 0.0, 0.0));
  let n010 = hash3(i + vec3<f32>(0.0, 1.0, 0.0));
  let n110 = hash3(i + vec3<f32>(1.0, 1.0, 0.0));
  let n001 = hash3(i + vec3<f32>(0.0, 0.0, 1.0));
  let n101 = hash3(i + vec3<f32>(1.0, 0.0, 1.0));
  let n011 = hash3(i + vec3<f32>(0.0, 1.0, 1.0));
  let n111 = hash3(i + vec3<f32>(1.0, 1.0, 1.0));
  let x00 = mix(n000, n100, w.x);
  let x10 = mix(n010, n110, w.x);
  let x01 = mix(n001, n101, w.x);
  let x11 = mix(n011, n111, w.x);
  return mix(mix(x00, x10, w.y), mix(x01, x11, w.y), w.z);
}

fn fbm(p: vec3<f32>) -> f32 {
  var value = 0.0;
  var amplitude = 0.5;
  var q = p;
  for (var i = 0; i < 5; i++) {
    value += amplitude * vnoise(q);
    q = q * 2.02 + vec3<f32>(17.3, 9.1, 4.7);
    amplitude *= 0.5;
  }
  return value;
}

// Narkowicz ACES filmic approximation: HDR-style highlight rolloff to
// white without a post-process chain — the "blinding but soft" look.
fn aces(x: vec3<f32>) -> vec3<f32> {
  let mapped = (x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14);
  return clamp(mapped, vec3<f32>(0.0), vec3<f32>(1.0));
}

// Star photosphere: a per-pixel TEMPERATURE field rendered through a
// blackbody-ish ramp (deep saturated intergranular lanes -> body tint ->
// white-hot granule cores), double domain warp for plasma churn,
// empirical limb darkening, faculae near the limb, sunspots with
// penumbra, chromosphere flash — then ACES-tonemapped from HDR values.
fn star_surface(n_in: vec3<f32>, tint: vec3<f32>, view_dir: vec3<f32>, phase: f32,
                spot_amount: f32, time: f32) -> vec3<f32> {
  let n = normalize(n_in);
  let drift = vec3<f32>(time * 0.006, time * 0.004, time * 0.009);
  let p = n * 15.0 + vec3<f32>(phase * 37.0) + drift;
  // Double domain warp: convection churn instead of static noise.
  let w1 = vec3<f32>(fbm(p * 0.55), fbm(p * 0.55 + 11.7), fbm(p * 0.55 + 71.3));
  let q = p * 1.1 + w1 * 2.0;
  let w2 = vec3<f32>(fbm(q + vec3<f32>(31.4)), fbm(q + vec3<f32>(53.1)),
                     fbm(q + vec3<f32>(97.7)));
  let cells = fbm(p + w2 * 2.6);                     // large convection cells
  let fine = fbm(p * 3.3 + w1 * 1.8 + drift * 2.0);  // fine granulation
  var temp_f = clamp(cells * 0.85 + fine * 0.55, 0.0, 1.3);
  // Sunspots: cool patches, soft penumbra.
  let s = fbm(n * 3.1 + vec3<f32>(phase * 53.0) + drift * 0.3);
  temp_f *= 1.0 - 0.85 * smoothstep(0.64, 0.80, s) * spot_amount;
  // Blackbody-ish ramp derived from the star's tint.
  let lane = tint * tint * 0.5;  // cooler: darker AND more saturated
  var c = mix(lane, tint * 1.2, smoothstep(0.12, 0.74, temp_f));
  c = mix(c, vec3<f32>(1.45), smoothstep(0.74, 1.12, temp_f));
  // Limb darkening (power-law fit) + faculae brightening near the limb.
  let mu = clamp(dot(n, -view_dir), 0.0, 1.0);
  let limb = 0.22 + 0.78 * pow(mu, 0.6);
  let faculae = smoothstep(0.45, 0.10, mu) * smoothstep(0.5, 0.9, fine);
  c = c * limb + tint * faculae * 0.55;
  // Chromosphere flash at the very limb.
  let rim = pow(1.0 - mu, 5.5);
  c += mix(tint, vec3<f32>(1.0, 0.42, 0.22), 0.6) * rim * 1.5;
  return aces(c * 1.7);
}

// Corona billboard (additive, opos.xy in [-1,1]): blinding rim just
// outside the photosphere silhouette, wide chromatic halo (white near
// the disc, saturated tint far out), flowing radial streamers,
// prominence arcs hugging the limb, and distance-adaptive diffraction
// spikes so far stars sparkle. disc_r = photosphere radius in billboard
// units; spike in [0,1] fades the cross out on close approach.
fn corona(opos: vec2<f32>, tint: vec3<f32>, phase: f32, intensity: f32,
          disc_r: f32, spike: f32, time: f32) -> vec3<f32> {
  let r = length(opos);
  let window = smoothstep(1.0, 0.60, r);
  let theta = atan2(opos.y, opos.x);
  let edge = max(r - disc_r, 0.0);
  var c = vec3<f32>(0.0);
  // Blinding inner rim.
  c += mix(vec3<f32>(1.35), tint, 0.3) * exp(-edge * 24.0) * 2.8;
  // Chromatic halo: hue drifts from white-hot to the star tint outward.
  let halo_tint = mix(vec3<f32>(1.0), tint, clamp(edge * 3.2, 0.0, 1.0));
  c += halo_tint * exp(-edge * 5.0) * 0.9;
  c += tint * exp(-edge * 1.8) * 0.22;  // faint far reach
  // Flowing radial streamers (polar FBM, drifting outward over time).
  let ray_p = vec3<f32>(cos(theta), sin(theta), 0.0) * 3.0 +
              vec3<f32>(phase * 19.0) + vec3<f32>(0.0, 0.0, r * 2.2 - time * 0.05);
  let streamer = pow(max(fbm(ray_p) * 1.55 - 0.35, 0.0), 2.0);
  c += tint * streamer * exp(-edge * 3.4) * 1.2;
  // Prominences: red-orange loop arcs right at the limb.
  let band = exp(-abs(r - disc_r * 1.05) * 30.0);
  let arc_p = vec3<f32>(cos(theta), sin(theta), 0.6) * 5.0 +
              vec3<f32>(phase * 71.0, 0.0, time * 0.03);
  let arcs = pow(max(fbm(arc_p) * 1.7 - 0.78, 0.0), 1.4);
  c += vec3<f32>(1.0, 0.30, 0.12) * band * arcs * 2.4;
  // Diffraction spikes: a 4-point cross, only when the star is small on
  // screen — far suns read as bright stars, near suns as raging discs.
  let cross = pow(abs(cos(theta)), 40.0) + pow(abs(sin(theta)), 40.0);
  c += mix(tint, vec3<f32>(1.0), 0.55) * cross * exp(-r * 2.6) * spike * 1.3;
  // Slow, subtle flicker (kept small — visible pulsing reads as a bug).
  let flicker = 0.97 + 0.03 * vnoise(vec3<f32>(time * 0.25, phase * 91.0, 0.0));
  return aces(c * flicker * intensity) * window;
}

// Analytic sky dome (tier-1 gradient atmosphere, sources note
// atmosphere-rendering.md): Rayleigh-ish zenith/horizon ramp, Mie
// forward lobe around the sun, sunset band at low sun elevation,
// day/night from the sun-up dot, altitude fade to space.
fn sky_dome(ndc: vec2<f32>) -> vec3<f32> {
  let view = normalize(frame.cam_right.xyz * (ndc.x * frame.cam_right.w) +
                       frame.cam_up.xyz * (ndc.y * frame.cam_up.w) +
                       frame.cam_fwd.xyz);
  let sun = normalize(frame.sun_dir.xyz);
  let up = normalize(frame.planet_up.xyz);
  // Sub-linear falloff: the sky keeps most of its color well up into the
  // band and only thins near the top (a linear ramp read as space from
  // half the atmosphere up, making entry/exit look like a hard curtain).
  let density = pow(clamp(1.0 - frame.cam_fwd.w, 0.0, 1.0), 0.45);
  let sun_h = dot(sun, up);
  let view_h = dot(view, up);
  let cos_vs = dot(view, sun);
  let day = smoothstep(-0.10, 0.30, sun_h);
  let tint = frame.atmo.rgb;
  // Zenith deepens and cools; the horizon brightens and warms.
  let zenith = tint * vec3<f32>(0.40, 0.52, 0.75);
  let horizon = mix(tint, vec3<f32>(1.0, 0.88, 0.72), 0.45) * 1.06;
  var sky = mix(horizon, zenith, pow(clamp(view_h, 0.0, 1.0), 0.55));
  // Sunset band: a low sun reddens the sky toward its azimuth.
  let low_sun = pow(clamp(1.0 - abs(sun_h) * 2.6, 0.0, 1.0), 1.4);
  let toward = pow(clamp(cos_vs, 0.0, 1.0), 2.6);
  sky = mix(sky, vec3<f32>(1.0, 0.42, 0.18), low_sun * toward * 0.75);
  // Mie forward lobe + tight glare around the sun disc.
  let mie = pow(clamp(cos_vs, 0.0, 1.0), 24.0) * 0.55 +
            pow(clamp(cos_vs, 0.0, 1.0), 220.0) * 1.6;
  var c = sky * day + frame.sun_color.rgb * mie * (0.25 + 0.75 * day);
  // Night floor: faint cold airglow instead of dead black.
  c += tint * 0.02 * (1.0 - day);
  let space = vec3<f32>(0.013, 0.015, 0.028);
  return mix(space, aces(c), density);
}

@fragment
fn fs_main(in: VSOut) -> @location(0) vec4<f32> {
  let mode = u32(u.extra.w + 0.5);
  let time = frame.sun_color.a;
  if (mode == 3u) {
    let r = length(in.opos.xy);
    var base = 0.0;
    if (u.extra.z > 0.001) {
      base = exp(-abs(r - u.extra.z) * u.extra.y);
    } else {
      base = pow(clamp(1.0 - r, 0.0, 1.0), u.extra.y);
    }
    let window = smoothstep(1.0, 0.90, r);
    return vec4<f32>(u.color.rgb * (u.extra.x * base * window), 1.0);
  }
  if (mode == 4u) {
    return vec4<f32>(sky_dome(in.opos.xy), 1.0);
  }
  if (mode == 1u) {
    let c = star_surface(in.opos, u.color.rgb, normalize(u.aux.xyz), u.aux.w,
                         u.extra.x, time);
    return vec4<f32>(min(c, vec3<f32>(1.0)), 1.0);
  }
  if (mode == 2u) {
    let c = corona(in.opos.xy, u.color.rgb, u.aux.w, u.extra.x, u.extra.y, u.extra.z,
                   time);
    return vec4<f32>(c, 1.0);
  }
  if (mode == 0u && u.color.a > 0.001) {
    // Unlit solid color; alpha passes through (blended pipeline only).
    return vec4<f32>(u.color.rgb, u.color.a);
  }
  // mode 5 falls through to the lit path below with color.a as alpha
  // (lit translucent surfaces — the sea shell).
  // Lit terrain: directional sun + a cool sky/bounce fill from the
  // opposite hemisphere so the night side stays readable and the
  // terminator picks up a blue-hour cast.
  let light = normalize(frame.sun_dir.xyz);
  var n = normalize(in.normal);
  // From orbit, pull the shading normal toward the analytic sphere
  // radial: per-chunk gradient normals disagree slightly across chunk
  // borders, which reads as a quad grid at distance. aux.xyz carries the
  // mesh's translation (camera-relative), so opos + aux is the fragment
  // in camera-relative world space.
  if (frame.planet_center.w > 0.001) {
    let radial = normalize(in.opos + u.aux.xyz - frame.planet_center.xyz);
    n = normalize(mix(n, radial, frame.planet_center.w));
  }
  let ndl = max(dot(n, light), 0.0);
  // Soft terminator wrap so the day/night line does not alias harshly.
  let wrap = max((dot(n, light) + 0.08) / 1.08, 0.0);
  // Albedo: the default terrain material, or the item's rgb when set
  // (lit colored surfaces, e.g. the ocean-blue sea-level impostor).
  var base = vec3<f32>(0.55, 0.52, 0.45);
  if (u.color.r + u.color.g + u.color.b > 0.001) {
    base = u.color.rgb;
  }
  var color = base * (0.05 + 1.05 * mix(ndl, wrap, 0.35)) * frame.sun_color.rgb;
  let fill = max(dot(n, -light), 0.0);
  color += base * fill * vec3<f32>(0.05, 0.07, 0.12);
  if (mode == 5u) {
    // Water: Fresnel-driven opacity (grazing water is opaque and dark
    // blue — a constant alpha made shallow seas read as bare ground) and
    // a sun glint. aux carries the camera-relative translation, so
    // opos + aux is the fragment's camera-relative position.
    let view = normalize(in.opos + u.aux.xyz);
    let fresnel = pow(1.0 - abs(dot(n, view)), 3.0);
    color = mix(color, vec3<f32>(0.05, 0.16, 0.30) * frame.sun_color.rgb, fresnel * 0.7);
    let refl = reflect(light * -1.0, n);
    let spec = pow(max(dot(refl, view * -1.0), 0.0), 90.0);
    color += frame.sun_color.rgb * spec * (0.9 * ndl + 0.05);
    let alpha_w = mix(u.color.a, 0.96, fresnel);
    return vec4<f32>(aces(color), alpha_w);
  }
  return vec4<f32>(aces(color), 1.0);
}
)";

WGPUStringView sv(const char* text) { return WGPUStringView{text, WGPU_STRLEN}; }

std::string to_string(WGPUStringView view) {
  if (view.data == nullptr) {
    return {};
  }
  if (view.length == WGPU_STRLEN) {
    return std::string(view.data);
  }
  return std::string(view.data, view.length);
}

struct AdapterRequest {
  WGPUAdapter adapter = nullptr;
  bool done = false;
  std::string message;
};

struct DeviceRequest {
  WGPUDevice device = nullptr;
  bool done = false;
  std::string message;
};

WGPUSurface create_surface(WGPUInstance instance, GLFWwindow* window,
                           [[maybe_unused]] std::string* error) {
  WGPUSurfaceDescriptor desc{};
#if defined(__linux__)
  const int platform = glfwGetPlatform();
  if (platform == GLFW_PLATFORM_WAYLAND) {
    WGPUSurfaceSourceWaylandSurface source{};
    source.chain.sType = WGPUSType_SurfaceSourceWaylandSurface;
    source.display = glfwGetWaylandDisplay();
    source.surface = glfwGetWaylandWindow(window);
    desc.nextInChain = &source.chain;
    return wgpuInstanceCreateSurface(instance, &desc);
  }
  if (platform == GLFW_PLATFORM_X11) {
    WGPUSurfaceSourceXlibWindow source{};
    source.chain.sType = WGPUSType_SurfaceSourceXlibWindow;
    source.display = glfwGetX11Display();
    source.window = static_cast<std::uint64_t>(glfwGetX11Window(window));
    desc.nextInChain = &source.chain;
    return wgpuInstanceCreateSurface(instance, &desc);
  }
  *error = "unsupported GLFW platform on Linux (need Wayland or X11)";
  return nullptr;
#elif defined(__APPLE__)
  WGPUSurfaceSourceMetalLayer source{};
  source.chain.sType = WGPUSType_SurfaceSourceMetalLayer;
  source.layer = infinityMetalLayerForCocoaWindow(glfwGetCocoaWindow(window));
  desc.nextInChain = &source.chain;
  return wgpuInstanceCreateSurface(instance, &desc);
#elif defined(_WIN32)
  WGPUSurfaceSourceWindowsHWND source{};
  source.chain.sType = WGPUSType_SurfaceSourceWindowsHWND;
  source.hinstance = GetModuleHandle(nullptr);
  source.hwnd = glfwGetWin32Window(window);
  desc.nextInChain = &source.chain;
  return wgpuInstanceCreateSurface(instance, &desc);
#else
  *error = "unsupported platform for surface creation";
  return nullptr;
#endif
}

struct MeshEntry {
  WGPUBuffer buffer = nullptr;
  std::uint32_t vertex_count = 0;
};

}  // namespace

struct Rhi::Impl {
  WGPUInstance instance = nullptr;
  WGPUSurface surface = nullptr;
  WGPUAdapter adapter = nullptr;
  WGPUDevice device = nullptr;
  WGPUQueue queue = nullptr;
  WGPUTextureFormat format = WGPUTextureFormat_Undefined;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::string adapter_info;

  // Mesh pipeline state (created on demand).
  WGPURenderPipeline mesh_pipeline = nullptr;
  WGPURenderPipeline mesh_pipeline_blend = nullptr;
  WGPURenderPipeline mesh_pipeline_add = nullptr;
  WGPUBindGroupLayout bind_layout = nullptr;
  WGPUBindGroup bind_group = nullptr;
  WGPUBuffer uniform_buffer = nullptr;
  WGPUBuffer frame_buffer = nullptr;
  WGPUTexture depth_texture = nullptr;
  WGPUTextureView depth_view = nullptr;
  std::unordered_map<std::uint32_t, MeshEntry> meshes;
  std::uint32_t next_mesh_id = 1;
  std::string capture_path;  // non-empty: capture on the next render_frame

  // --- debug frame recorder ---------------------------------------------
  // Reduced-resolution re-renders of the scene, every kRecInterval-th
  // frame, into an in-memory ring of the last kRingSeconds. A trigger
  // dumps the ring to disk and keeps writing frames until rec_until.
  static constexpr std::uint32_t kRecW = 640;
  static constexpr std::uint32_t kRecH = 360;
  static constexpr double kRingSeconds = 3.0;
  bool ring_enabled = false;
  float last_ring_time = -1.0f;
  WGPUTexture rec_color = nullptr;
  WGPUTextureView rec_color_view = nullptr;
  WGPUTexture rec_depth = nullptr;
  WGPUTextureView rec_depth_view = nullptr;
  WGPUBuffer rec_buffer = nullptr;
  std::uint32_t rec_bpr = 0;
  struct RingFrame {
    float time_s;
    std::vector<std::uint8_t> rgb;  // kRecW * kRecH * 3, tight
  };
  std::deque<RingFrame> ring;
  std::uint64_t frame_counter = 0;
  float last_frame_time = 0.0f;
  std::string rec_dir;      // active triggered-recording directory
  double rec_until = -1e30;  // future-record until this frame time
  int rec_seq_index = 0;

  void ensure_recorder_targets() {
    if (rec_color != nullptr) {
      return;
    }
    WGPUTextureDescriptor color_desc{};
    color_desc.label = sv("rec-color");
    color_desc.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc;
    color_desc.dimension = WGPUTextureDimension_2D;
    color_desc.size = WGPUExtent3D{kRecW, kRecH, 1};
    color_desc.format = format;
    color_desc.mipLevelCount = 1;
    color_desc.sampleCount = 1;
    rec_color = wgpuDeviceCreateTexture(device, &color_desc);
    rec_color_view = wgpuTextureCreateView(rec_color, nullptr);
    WGPUTextureDescriptor depth_desc{};
    depth_desc.label = sv("rec-depth");
    depth_desc.usage = WGPUTextureUsage_RenderAttachment;
    depth_desc.dimension = WGPUTextureDimension_2D;
    depth_desc.size = WGPUExtent3D{kRecW, kRecH, 1};
    depth_desc.format = WGPUTextureFormat_Depth32Float;
    depth_desc.mipLevelCount = 1;
    depth_desc.sampleCount = 1;
    rec_depth = wgpuDeviceCreateTexture(device, &depth_desc);
    rec_depth_view = wgpuTextureCreateView(rec_depth, nullptr);
    rec_bpr = ((kRecW * 4 + 255) / 256) * 256;
    WGPUBufferDescriptor buffer_desc{};
    buffer_desc.label = sv("rec-readback");
    buffer_desc.usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst;
    buffer_desc.size = static_cast<std::uint64_t>(rec_bpr) * kRecH;
    rec_buffer = wgpuDeviceCreateBuffer(device, &buffer_desc);
  }

  void write_ppm(const std::string& path, std::uint32_t w, std::uint32_t h,
                 const std::uint8_t* rgb) {
    std::FILE* file = std::fopen(path.c_str(), "wb");
    if (file == nullptr) {
      std::fprintf(stderr, "recorder: FAILED to open %s\n", path.c_str());
      return;
    }
    std::fprintf(file, "P6\n%u %u\n255\n", w, h);
    std::fwrite(rgb, 1, static_cast<std::size_t>(w) * h * 3, file);
    std::fclose(file);
  }

  void append_time_index(float time_s) {
    std::FILE* file = std::fopen((rec_dir + "/times.txt").c_str(), "a");
    if (file != nullptr) {
      std::fprintf(file, "seq-%04d %.4f\n", rec_seq_index, time_s);
      std::fclose(file);
    }
  }

  void emit_sequence_frame(float time_s, const std::uint8_t* rgb) {
    char name[32];
    std::snprintf(name, sizeof(name), "/seq-%04d.ppm", rec_seq_index);
    write_ppm(rec_dir + name, kRecW, kRecH, rgb);
    append_time_index(time_s);
    ++rec_seq_index;
  }

  void configure_surface() {
    WGPUSurfaceConfiguration config{};
    config.device = device;
    config.format = format;
    config.usage = WGPUTextureUsage_RenderAttachment;
    config.width = width;
    config.height = height;
    config.alphaMode = WGPUCompositeAlphaMode_Auto;
    config.presentMode = WGPUPresentMode_Fifo;
    wgpuSurfaceConfigure(surface, &config);
    recreate_depth();
  }

  void recreate_depth() {
    if (depth_view != nullptr) {
      wgpuTextureViewRelease(depth_view);
      depth_view = nullptr;
    }
    if (depth_texture != nullptr) {
      wgpuTextureRelease(depth_texture);
      depth_texture = nullptr;
    }
    WGPUTextureDescriptor desc{};
    desc.label = sv("depth");
    desc.usage = WGPUTextureUsage_RenderAttachment;
    desc.dimension = WGPUTextureDimension_2D;
    desc.size = WGPUExtent3D{width, height, 1};
    desc.format = WGPUTextureFormat_Depth32Float;
    desc.mipLevelCount = 1;
    desc.sampleCount = 1;
    depth_texture = wgpuDeviceCreateTexture(device, &desc);
    depth_view = wgpuTextureCreateView(depth_texture, nullptr);
  }

  void ensure_mesh_pipeline() {
    if (mesh_pipeline != nullptr) {
      return;
    }
    WGPUShaderSourceWGSL wgsl{};
    wgsl.chain.sType = WGPUSType_ShaderSourceWGSL;
    wgsl.code = sv(kMeshShader);
    WGPUShaderModuleDescriptor module_desc{};
    module_desc.nextInChain = &wgsl.chain;
    WGPUShaderModule module = wgpuDeviceCreateShaderModule(device, &module_desc);

    WGPUBindGroupLayoutEntry layout_entries[2] = {};
    layout_entries[0].binding = 0;
    layout_entries[0].visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
    layout_entries[0].buffer.type = WGPUBufferBindingType_Uniform;
    layout_entries[0].buffer.hasDynamicOffset = 1U;
    layout_entries[0].buffer.minBindingSize = kItemUniformSize;
    layout_entries[1].binding = 1;
    layout_entries[1].visibility = WGPUShaderStage_Fragment;
    layout_entries[1].buffer.type = WGPUBufferBindingType_Uniform;
    layout_entries[1].buffer.hasDynamicOffset = 0U;
    layout_entries[1].buffer.minBindingSize = kFrameUniformSize;
    WGPUBindGroupLayoutDescriptor layout_desc{};
    layout_desc.entryCount = 2;
    layout_desc.entries = layout_entries;
    bind_layout = wgpuDeviceCreateBindGroupLayout(device, &layout_desc);

    WGPUPipelineLayoutDescriptor pipeline_layout_desc{};
    pipeline_layout_desc.bindGroupLayoutCount = 1;
    pipeline_layout_desc.bindGroupLayouts = &bind_layout;
    WGPUPipelineLayout pipeline_layout =
        wgpuDeviceCreatePipelineLayout(device, &pipeline_layout_desc);

    WGPUVertexAttribute attributes[2] = {};
    attributes[0].format = WGPUVertexFormat_Float32x3;
    attributes[0].offset = 0;
    attributes[0].shaderLocation = 0;
    attributes[1].format = WGPUVertexFormat_Float32x3;
    attributes[1].offset = 12;
    attributes[1].shaderLocation = 1;
    WGPUVertexBufferLayout vertex_layout{};
    vertex_layout.arrayStride = 24;
    vertex_layout.stepMode = WGPUVertexStepMode_Vertex;
    vertex_layout.attributeCount = 2;
    vertex_layout.attributes = attributes;

    WGPUDepthStencilState depth_state{};
    depth_state.format = WGPUTextureFormat_Depth32Float;
    depth_state.depthWriteEnabled = WGPUOptionalBool_True;
    // Reversed-Z: the app builds its projection with near/far swapped, so
    // closer = LARGER depth; cleared to 0, tested with Greater. This is
    // what keeps solar-system-scale distances stable in an f32 depth
    // buffer (classic-Z quantized them onto the far plane).
    depth_state.depthCompare = WGPUCompareFunction_Greater;
    depth_state.stencilFront.compare = WGPUCompareFunction_Always;
    depth_state.stencilFront.failOp = WGPUStencilOperation_Keep;
    depth_state.stencilFront.depthFailOp = WGPUStencilOperation_Keep;
    depth_state.stencilFront.passOp = WGPUStencilOperation_Keep;
    depth_state.stencilBack = depth_state.stencilFront;
    depth_state.stencilReadMask = 0xFFFFFFFF;
    depth_state.stencilWriteMask = 0xFFFFFFFF;

    WGPUColorTargetState color_target{};
    color_target.format = format;
    color_target.writeMask = WGPUColorWriteMask_All;
    WGPUFragmentState fragment{};
    fragment.module = module;
    fragment.entryPoint = sv("fs_main");
    fragment.targetCount = 1;
    fragment.targets = &color_target;

    WGPURenderPipelineDescriptor pipeline_desc{};
    pipeline_desc.label = sv("mesh");
    pipeline_desc.layout = pipeline_layout;
    pipeline_desc.vertex.module = module;
    pipeline_desc.vertex.entryPoint = sv("vs_main");
    pipeline_desc.vertex.bufferCount = 1;
    pipeline_desc.vertex.buffers = &vertex_layout;
    pipeline_desc.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    pipeline_desc.primitive.frontFace = WGPUFrontFace_CCW;
    pipeline_desc.primitive.cullMode = WGPUCullMode_None;
    pipeline_desc.depthStencil = &depth_state;
    pipeline_desc.multisample.count = 1;
    pipeline_desc.multisample.mask = 0xFFFFFFFF;
    pipeline_desc.fragment = &fragment;
    mesh_pipeline = wgpuDeviceCreateRenderPipeline(device, &pipeline_desc);

    // Translucent variant: alpha blending, no depth writes.
    WGPUBlendState blend{};
    blend.color.operation = WGPUBlendOperation_Add;
    blend.color.srcFactor = WGPUBlendFactor_SrcAlpha;
    blend.color.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
    blend.alpha.operation = WGPUBlendOperation_Add;
    blend.alpha.srcFactor = WGPUBlendFactor_One;
    blend.alpha.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
    color_target.blend = &blend;
    depth_state.depthWriteEnabled = WGPUOptionalBool_False;
    pipeline_desc.label = sv("mesh-blend");
    mesh_pipeline_blend = wgpuDeviceCreateRenderPipeline(device, &pipeline_desc);

    // Additive variant (corona/glow): src One + dst One, depth-tested so
    // planets occlude the glow, but no depth writes.
    WGPUBlendState additive{};
    additive.color.operation = WGPUBlendOperation_Add;
    additive.color.srcFactor = WGPUBlendFactor_One;
    additive.color.dstFactor = WGPUBlendFactor_One;
    additive.alpha.operation = WGPUBlendOperation_Add;
    additive.alpha.srcFactor = WGPUBlendFactor_One;
    additive.alpha.dstFactor = WGPUBlendFactor_One;
    color_target.blend = &additive;
    pipeline_desc.label = sv("mesh-add");
    mesh_pipeline_add = wgpuDeviceCreateRenderPipeline(device, &pipeline_desc);

    wgpuPipelineLayoutRelease(pipeline_layout);
    wgpuShaderModuleRelease(module);

    WGPUBufferDescriptor uniform_desc{};
    uniform_desc.label = sv("uniforms");
    uniform_desc.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
    uniform_desc.size = kUniformStride * kMaxDrawItems;
    uniform_buffer = wgpuDeviceCreateBuffer(device, &uniform_desc);

    WGPUBufferDescriptor frame_desc{};
    frame_desc.label = sv("frame-uniforms");
    frame_desc.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
    frame_desc.size = kFrameUniformSize;
    frame_buffer = wgpuDeviceCreateBuffer(device, &frame_desc);

    WGPUBindGroupEntry bind_entries[2] = {};
    bind_entries[0].binding = 0;
    bind_entries[0].buffer = uniform_buffer;
    bind_entries[0].offset = 0;
    bind_entries[0].size = kItemUniformSize;
    bind_entries[1].binding = 1;
    bind_entries[1].buffer = frame_buffer;
    bind_entries[1].offset = 0;
    bind_entries[1].size = kFrameUniformSize;
    WGPUBindGroupDescriptor bind_desc{};
    bind_desc.layout = bind_layout;
    bind_desc.entryCount = 2;
    bind_desc.entries = bind_entries;
    bind_group = wgpuDeviceCreateBindGroup(device, &bind_desc);
  }

  ~Impl() {
    for (auto& [id, mesh] : meshes) {
      wgpuBufferRelease(mesh.buffer);
    }
    if (rec_buffer != nullptr) wgpuBufferRelease(rec_buffer);
    if (rec_color_view != nullptr) wgpuTextureViewRelease(rec_color_view);
    if (rec_color != nullptr) wgpuTextureRelease(rec_color);
    if (rec_depth_view != nullptr) wgpuTextureViewRelease(rec_depth_view);
    if (rec_depth != nullptr) wgpuTextureRelease(rec_depth);
    if (bind_group != nullptr) wgpuBindGroupRelease(bind_group);
    if (mesh_pipeline_blend != nullptr) wgpuRenderPipelineRelease(mesh_pipeline_blend);
    if (mesh_pipeline_add != nullptr) wgpuRenderPipelineRelease(mesh_pipeline_add);
    if (uniform_buffer != nullptr) wgpuBufferRelease(uniform_buffer);
    if (frame_buffer != nullptr) wgpuBufferRelease(frame_buffer);
    if (bind_layout != nullptr) wgpuBindGroupLayoutRelease(bind_layout);
    if (mesh_pipeline != nullptr) wgpuRenderPipelineRelease(mesh_pipeline);
    if (depth_view != nullptr) wgpuTextureViewRelease(depth_view);
    if (depth_texture != nullptr) wgpuTextureRelease(depth_texture);
    if (surface != nullptr) wgpuSurfaceUnconfigure(surface);
    if (queue != nullptr) wgpuQueueRelease(queue);
    if (device != nullptr) wgpuDeviceRelease(device);
    if (adapter != nullptr) wgpuAdapterRelease(adapter);
    if (surface != nullptr) wgpuSurfaceRelease(surface);
    if (instance != nullptr) wgpuInstanceRelease(instance);
  }
};

Rhi::Rhi(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Rhi::~Rhi() = default;

std::unique_ptr<Rhi> Rhi::create(GLFWwindow* window, std::string* error) {
  auto impl = std::make_unique<Impl>();

  WGPUInstanceDescriptor instance_desc{};
  impl->instance = wgpuCreateInstance(&instance_desc);
  if (impl->instance == nullptr) {
    *error = "wgpuCreateInstance failed";
    return nullptr;
  }

  impl->surface = create_surface(impl->instance, window, error);
  if (impl->surface == nullptr) {
    if (error->empty()) {
      *error = "surface creation failed";
    }
    return nullptr;
  }

  AdapterRequest adapter_request;
  {
    WGPURequestAdapterOptions options{};
    options.compatibleSurface = impl->surface;
    options.powerPreference = WGPUPowerPreference_HighPerformance;
    WGPURequestAdapterCallbackInfo callback_info{};
    callback_info.mode = WGPUCallbackMode_AllowProcessEvents;
    callback_info.callback = [](WGPURequestAdapterStatus status, WGPUAdapter adapter,
                                WGPUStringView message, void* userdata1, void*) {
      auto* request = static_cast<AdapterRequest*>(userdata1);
      if (status == WGPURequestAdapterStatus_Success) {
        request->adapter = adapter;
      } else {
        request->message = to_string(message);
      }
      request->done = true;
    };
    callback_info.userdata1 = &adapter_request;
    wgpuInstanceRequestAdapter(impl->instance, &options, callback_info);
    while (!adapter_request.done) {
      wgpuInstanceProcessEvents(impl->instance);
    }
  }
  if (adapter_request.adapter == nullptr) {
    *error = "no suitable GPU adapter: " + adapter_request.message;
    return nullptr;
  }
  impl->adapter = adapter_request.adapter;

  {
    WGPUAdapterInfo info{};
    if (wgpuAdapterGetInfo(impl->adapter, &info) == WGPUStatus_Success) {
      impl->adapter_info = to_string(info.device) + " (" + to_string(info.description) + ")";
      wgpuAdapterInfoFreeMembers(info);
    }
  }

  DeviceRequest device_request;
  {
    WGPUDeviceDescriptor device_desc{};
    device_desc.uncapturedErrorCallbackInfo.callback =
        [](WGPUDevice const*, WGPUErrorType type, WGPUStringView message, void*, void*) {
          std::fprintf(stderr, "[wgpu] uncaptured error (%d): %.*s\n", static_cast<int>(type),
                       static_cast<int>(message.length), message.data);
        };
    WGPURequestDeviceCallbackInfo callback_info{};
    callback_info.mode = WGPUCallbackMode_AllowProcessEvents;
    callback_info.callback = [](WGPURequestDeviceStatus status, WGPUDevice device,
                                WGPUStringView message, void* userdata1, void*) {
      auto* request = static_cast<DeviceRequest*>(userdata1);
      if (status == WGPURequestDeviceStatus_Success) {
        request->device = device;
      } else {
        request->message = to_string(message);
      }
      request->done = true;
    };
    callback_info.userdata1 = &device_request;
    wgpuAdapterRequestDevice(impl->adapter, &device_desc, callback_info);
    while (!device_request.done) {
      wgpuInstanceProcessEvents(impl->instance);
    }
  }
  if (device_request.device == nullptr) {
    *error = "device request failed: " + device_request.message;
    return nullptr;
  }
  impl->device = device_request.device;
  impl->queue = wgpuDeviceGetQueue(impl->device);

  {
    WGPUSurfaceCapabilities capabilities{};
    if (wgpuSurfaceGetCapabilities(impl->surface, impl->adapter, &capabilities) !=
            WGPUStatus_Success ||
        capabilities.formatCount == 0) {
      *error = "surface reports no supported formats";
      return nullptr;
    }
    impl->format = capabilities.formats[0];
    wgpuSurfaceCapabilitiesFreeMembers(capabilities);
  }

  int fb_width = 0;
  int fb_height = 0;
  glfwGetFramebufferSize(window, &fb_width, &fb_height);
  impl->width = static_cast<std::uint32_t>(fb_width > 0 ? fb_width : 1);
  impl->height = static_cast<std::uint32_t>(fb_height > 0 ? fb_height : 1);
  impl->configure_surface();

  return std::unique_ptr<Rhi>(new Rhi(std::move(impl)));
}

void Rhi::resize(std::uint32_t width, std::uint32_t height) {
  if (width == 0 || height == 0) {
    return;
  }
  impl_->width = width;
  impl_->height = height;
  impl_->configure_surface();
}

std::uint32_t Rhi::create_mesh(const float* vertices, std::size_t float_count) {
  WGPUBufferDescriptor desc{};
  desc.label = sv("chunk-mesh");
  desc.usage = WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst;
  desc.size = float_count * sizeof(float);
  WGPUBuffer buffer = wgpuDeviceCreateBuffer(impl_->device, &desc);
  wgpuQueueWriteBuffer(impl_->queue, buffer, 0, vertices, desc.size);
  const std::uint32_t id = impl_->next_mesh_id++;
  impl_->meshes.emplace(id, MeshEntry{buffer, static_cast<std::uint32_t>(float_count / 6)});
  return id;
}

void Rhi::destroy_mesh(std::uint32_t mesh) {
  auto it = impl_->meshes.find(mesh);
  if (it != impl_->meshes.end()) {
    wgpuBufferRelease(it->second.buffer);
    impl_->meshes.erase(it);
  }
}

bool Rhi::render_frame(const FrameParams& frame, const DrawItem* items,
                       std::size_t item_count) {
  impl_->ensure_mesh_pipeline();

  {
    float frame_block[32] = {
        frame.sun_dir[0],    frame.sun_dir[1],    frame.sun_dir[2],    0.0f,
        frame.sun_color[0],  frame.sun_color[1],  frame.sun_color[2],  frame.time_s,
        frame.cam_right[0],  frame.cam_right[1],  frame.cam_right[2],  frame.tan_half_x,
        frame.cam_up[0],     frame.cam_up[1],     frame.cam_up[2],     frame.tan_half_y,
        frame.cam_fwd[0],    frame.cam_fwd[1],    frame.cam_fwd[2],    frame.altitude_frac,
        frame.planet_up[0],  frame.planet_up[1],  frame.planet_up[2],  0.0f,
        frame.atmo_tint[0],  frame.atmo_tint[1],  frame.atmo_tint[2],  1.0f,
        frame.planet_center[0], frame.planet_center[1], frame.planet_center[2],
        frame.normal_blend,
    };
    wgpuQueueWriteBuffer(impl_->queue, impl_->frame_buffer, 0, frame_block,
                         sizeof(frame_block));
  }

  // Upload all uniforms before the command buffer executes.
  const std::size_t count = item_count > kMaxDrawItems ? kMaxDrawItems : item_count;
  for (std::size_t i = 0; i < count; ++i) {
    float block[28];
    std::memcpy(block, items[i].mvp, sizeof(items[i].mvp));
    std::memcpy(block + 16, items[i].color, sizeof(items[i].color));
    std::memcpy(block + 20, items[i].aux, sizeof(items[i].aux));
    std::memcpy(block + 24, items[i].extra, sizeof(items[i].extra));
    block[27] = static_cast<float>(items[i].mode);
    wgpuQueueWriteBuffer(impl_->queue, impl_->uniform_buffer, i * kUniformStride, block,
                         sizeof(block));
  }

  // Attachment templates: the main pass fills in the surface view; the
  // capture/recorder paths swap in their own offscreen views. Keeping
  // them independent of surface acquisition means captures and the ring
  // keep working when the window is hidden and macOS stops handing out
  // surface textures (fully headless operation).
  WGPURenderPassColorAttachment attachment{};
  attachment.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
  attachment.loadOp = WGPULoadOp_Clear;
  attachment.storeOp = WGPUStoreOp_Store;
  attachment.clearValue = WGPUColor{frame.sky[0], frame.sky[1], frame.sky[2], 1.0};
  WGPURenderPassDepthStencilAttachment depth_attachment{};
  depth_attachment.view = impl_->depth_view;
  depth_attachment.depthLoadOp = WGPULoadOp_Clear;
  depth_attachment.depthStoreOp = WGPUStoreOp_Store;
  depth_attachment.depthClearValue = 0.0f;  // reversed-Z: far plane

  WGPUSurfaceTexture surface_texture{};
  wgpuSurfaceGetCurrentTexture(impl_->surface, &surface_texture);
  const bool have_surface =
      surface_texture.status == WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal ||
      surface_texture.status == WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal;
  if (!have_surface) {
    if (surface_texture.texture != nullptr) {
      wgpuTextureRelease(surface_texture.texture);
    }
    if (surface_texture.status == WGPUSurfaceGetCurrentTextureStatus_Outdated ||
        surface_texture.status == WGPUSurfaceGetCurrentTextureStatus_Lost) {
      impl_->configure_surface();
    }
  }

  enum class Pass { Opaque, Blend, Additive };
  const auto record_scene = [&](WGPURenderPassEncoder pass) {
    const auto draw_items = [&](Pass which) {
      for (std::size_t i = 0; i < count; ++i) {
        const Pass item_pass = items[i].mode == 2 || items[i].mode == 3
                                   ? Pass::Additive
                               : items[i].translucent ? Pass::Blend
                                                      : Pass::Opaque;
        if (item_pass != which) {
          continue;
        }
        const auto it = impl_->meshes.find(items[i].mesh);
        if (it == impl_->meshes.end() || it->second.vertex_count == 0) {
          continue;
        }
        const std::uint32_t offset = static_cast<std::uint32_t>(i * kUniformStride);
        wgpuRenderPassEncoderSetBindGroup(pass, 0, impl_->bind_group, 1, &offset);
        wgpuRenderPassEncoderSetVertexBuffer(pass, 0, it->second.buffer, 0, WGPU_WHOLE_SIZE);
        wgpuRenderPassEncoderDraw(pass, it->second.vertex_count, 1, 0, 0);
      }
    };
    wgpuRenderPassEncoderSetPipeline(pass, impl_->mesh_pipeline);
    draw_items(Pass::Opaque);
    wgpuRenderPassEncoderSetPipeline(pass, impl_->mesh_pipeline_blend);
    draw_items(Pass::Blend);
    wgpuRenderPassEncoderSetPipeline(pass, impl_->mesh_pipeline_add);
    draw_items(Pass::Additive);
  };

  if (have_surface) {
    WGPUTextureView view = wgpuTextureCreateView(surface_texture.texture, nullptr);
    WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(impl_->device, nullptr);
    attachment.view = view;
    WGPURenderPassDescriptor pass_desc{};
    pass_desc.colorAttachmentCount = 1;
    pass_desc.colorAttachments = &attachment;
    pass_desc.depthStencilAttachment = &depth_attachment;
    WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(encoder, &pass_desc);
    record_scene(pass);
    wgpuRenderPassEncoderEnd(pass);
    wgpuRenderPassEncoderRelease(pass);

    WGPUCommandBuffer commands = wgpuCommandEncoderFinish(encoder, nullptr);
    wgpuCommandEncoderRelease(encoder);
    wgpuQueueSubmit(impl_->queue, 1, &commands);
    wgpuCommandBufferRelease(commands);
    wgpuTextureViewRelease(view);

    wgpuSurfacePresent(impl_->surface);
    wgpuTextureRelease(surface_texture.texture);
  }

  // --- one-shot capture: identical scene into an offscreen target -------
  if (!impl_->capture_path.empty()) {
    const std::string path = impl_->capture_path;
    impl_->capture_path.clear();
    const std::uint32_t width = impl_->width;
    const std::uint32_t height = impl_->height;
    const std::uint32_t bytes_per_row = ((width * 4 + 255) / 256) * 256;

    WGPUTextureDescriptor color_desc{};
    color_desc.label = sv("capture-color");
    color_desc.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc;
    color_desc.dimension = WGPUTextureDimension_2D;
    color_desc.size = WGPUExtent3D{width, height, 1};
    color_desc.format = impl_->format;
    color_desc.mipLevelCount = 1;
    color_desc.sampleCount = 1;
    WGPUTexture color_tex = wgpuDeviceCreateTexture(impl_->device, &color_desc);
    WGPUTextureView color_view = wgpuTextureCreateView(color_tex, nullptr);

    WGPUBufferDescriptor read_desc{};
    read_desc.label = sv("capture-readback");
    read_desc.usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst;
    read_desc.size = static_cast<std::uint64_t>(bytes_per_row) * height;
    WGPUBuffer read_buffer = wgpuDeviceCreateBuffer(impl_->device, &read_desc);

    WGPUCommandEncoder cap_encoder = wgpuDeviceCreateCommandEncoder(impl_->device, nullptr);
    WGPURenderPassColorAttachment cap_attachment = attachment;
    cap_attachment.view = color_view;
    WGPURenderPassDepthStencilAttachment cap_depth = depth_attachment;  // reuse, re-cleared
    WGPURenderPassDescriptor cap_pass_desc{};
    cap_pass_desc.colorAttachmentCount = 1;
    cap_pass_desc.colorAttachments = &cap_attachment;
    cap_pass_desc.depthStencilAttachment = &cap_depth;
    WGPURenderPassEncoder cap_pass =
        wgpuCommandEncoderBeginRenderPass(cap_encoder, &cap_pass_desc);
    record_scene(cap_pass);
    wgpuRenderPassEncoderEnd(cap_pass);
    wgpuRenderPassEncoderRelease(cap_pass);

    WGPUTexelCopyTextureInfo src{};
    src.texture = color_tex;
    WGPUTexelCopyBufferInfo dst{};
    dst.buffer = read_buffer;
    dst.layout.bytesPerRow = bytes_per_row;
    dst.layout.rowsPerImage = height;
    const WGPUExtent3D extent{width, height, 1};
    wgpuCommandEncoderCopyTextureToBuffer(cap_encoder, &src, &dst, &extent);
    WGPUCommandBuffer cap_commands = wgpuCommandEncoderFinish(cap_encoder, nullptr);
    wgpuCommandEncoderRelease(cap_encoder);
    wgpuQueueSubmit(impl_->queue, 1, &cap_commands);
    wgpuCommandBufferRelease(cap_commands);

    bool mapped_done = false;
    bool mapped_ok = false;
    WGPUBufferMapCallbackInfo map_info{};
    map_info.mode = WGPUCallbackMode_AllowProcessEvents;
    map_info.callback = [](WGPUMapAsyncStatus status, WGPUStringView, void* u1, void* u2) {
      *static_cast<bool*>(u1) = true;
      *static_cast<bool*>(u2) = status == WGPUMapAsyncStatus_Success;
    };
    map_info.userdata1 = &mapped_done;
    map_info.userdata2 = &mapped_ok;
    wgpuBufferMapAsync(read_buffer, WGPUMapMode_Read, 0, read_desc.size, map_info);
    while (!mapped_done) {
      wgpuDevicePoll(impl_->device, 1U, nullptr);
    }
    if (mapped_ok) {
      const auto* data = static_cast<const std::uint8_t*>(
          wgpuBufferGetConstMappedRange(read_buffer, 0, read_desc.size));
      const bool bgra = impl_->format == WGPUTextureFormat_BGRA8Unorm ||
                        impl_->format == WGPUTextureFormat_BGRA8UnormSrgb;
      std::FILE* file = std::fopen(path.c_str(), "wb");
      if (file != nullptr && data != nullptr) {
        std::fprintf(file, "P6\n%u %u\n255\n", width, height);
        std::vector<std::uint8_t> row(static_cast<std::size_t>(width) * 3);
        for (std::uint32_t y = 0; y < height; ++y) {
          const std::uint8_t* src_row = data + static_cast<std::size_t>(y) * bytes_per_row;
          for (std::uint32_t x = 0; x < width; ++x) {
            row[x * 3 + 0] = src_row[x * 4 + (bgra ? 2 : 0)];
            row[x * 3 + 1] = src_row[x * 4 + 1];
            row[x * 3 + 2] = src_row[x * 4 + (bgra ? 0 : 2)];
          }
          std::fwrite(row.data(), 1, row.size(), file);
        }
        std::fclose(file);
        std::printf("capture: wrote %ux%u to %s\n", width, height, path.c_str());
      } else {
        std::fprintf(stderr, "capture: FAILED to open %s\n", path.c_str());
        if (file != nullptr) {
          std::fclose(file);
        }
      }
      wgpuBufferUnmap(read_buffer);
    } else {
      std::fprintf(stderr, "capture: readback map failed\n");
    }
    wgpuBufferRelease(read_buffer);
    wgpuTextureViewRelease(color_view);
    wgpuTextureRelease(color_tex);
  }

  // --- debug recorder: ring buffer + triggered sequences ----------------
  impl_->last_frame_time = frame.time_s;
  ++impl_->frame_counter;
  const bool future_active = !impl_->rec_dir.empty() &&
                             static_cast<double>(frame.time_s) < impl_->rec_until;
  // Time-based cadence (~30 fps) so uncapped headless runs don't hammer
  // the readback path; the < comparison handles the 4096 s time wrap.
  const bool ring_due = frame.time_s - impl_->last_ring_time >= 0.0333f ||
                        frame.time_s < impl_->last_ring_time;
  if ((impl_->ring_enabled || future_active) && ring_due) {
    impl_->last_ring_time = frame.time_s;
    impl_->ensure_recorder_targets();
    WGPUCommandEncoder rec_encoder = wgpuDeviceCreateCommandEncoder(impl_->device, nullptr);
    WGPURenderPassColorAttachment rec_attachment = attachment;
    rec_attachment.view = impl_->rec_color_view;
    WGPURenderPassDepthStencilAttachment rec_depth_att = depth_attachment;
    rec_depth_att.view = impl_->rec_depth_view;
    WGPURenderPassDescriptor rec_pass_desc{};
    rec_pass_desc.colorAttachmentCount = 1;
    rec_pass_desc.colorAttachments = &rec_attachment;
    rec_pass_desc.depthStencilAttachment = &rec_depth_att;
    WGPURenderPassEncoder rec_pass =
        wgpuCommandEncoderBeginRenderPass(rec_encoder, &rec_pass_desc);
    record_scene(rec_pass);
    wgpuRenderPassEncoderEnd(rec_pass);
    wgpuRenderPassEncoderRelease(rec_pass);
    WGPUTexelCopyTextureInfo rec_src{};
    rec_src.texture = impl_->rec_color;
    WGPUTexelCopyBufferInfo rec_dst{};
    rec_dst.buffer = impl_->rec_buffer;
    rec_dst.layout.bytesPerRow = impl_->rec_bpr;
    rec_dst.layout.rowsPerImage = Impl::kRecH;
    const WGPUExtent3D rec_extent{Impl::kRecW, Impl::kRecH, 1};
    wgpuCommandEncoderCopyTextureToBuffer(rec_encoder, &rec_src, &rec_dst, &rec_extent);
    WGPUCommandBuffer rec_commands = wgpuCommandEncoderFinish(rec_encoder, nullptr);
    wgpuCommandEncoderRelease(rec_encoder);
    wgpuQueueSubmit(impl_->queue, 1, &rec_commands);
    wgpuCommandBufferRelease(rec_commands);

    bool map_done = false;
    bool map_ok = false;
    WGPUBufferMapCallbackInfo map_info{};
    map_info.mode = WGPUCallbackMode_AllowProcessEvents;
    map_info.callback = [](WGPUMapAsyncStatus status, WGPUStringView, void* u1, void* u2) {
      *static_cast<bool*>(u1) = true;
      *static_cast<bool*>(u2) = status == WGPUMapAsyncStatus_Success;
    };
    map_info.userdata1 = &map_done;
    map_info.userdata2 = &map_ok;
    wgpuBufferMapAsync(impl_->rec_buffer, WGPUMapMode_Read, 0,
                       static_cast<std::uint64_t>(impl_->rec_bpr) * Impl::kRecH, map_info);
    while (!map_done) {
      wgpuDevicePoll(impl_->device, 1U, nullptr);
    }
    if (map_ok) {
      const auto* data = static_cast<const std::uint8_t*>(wgpuBufferGetConstMappedRange(
          impl_->rec_buffer, 0, static_cast<std::uint64_t>(impl_->rec_bpr) * Impl::kRecH));
      if (data != nullptr) {
        const bool bgra = impl_->format == WGPUTextureFormat_BGRA8Unorm ||
                          impl_->format == WGPUTextureFormat_BGRA8UnormSrgb;
        std::vector<std::uint8_t> rgb(static_cast<std::size_t>(Impl::kRecW) * Impl::kRecH * 3);
        for (std::uint32_t y = 0; y < Impl::kRecH; ++y) {
          const std::uint8_t* src_row = data + static_cast<std::size_t>(y) * impl_->rec_bpr;
          std::uint8_t* dst_row = rgb.data() + static_cast<std::size_t>(y) * Impl::kRecW * 3;
          for (std::uint32_t x = 0; x < Impl::kRecW; ++x) {
            dst_row[x * 3 + 0] = src_row[x * 4 + (bgra ? 2 : 0)];
            dst_row[x * 3 + 1] = src_row[x * 4 + 1];
            dst_row[x * 3 + 2] = src_row[x * 4 + (bgra ? 0 : 2)];
          }
        }
        if (future_active) {
          impl_->emit_sequence_frame(frame.time_s, rgb.data());
        } else {
          impl_->ring.push_back(Impl::RingFrame{frame.time_s, std::move(rgb)});
          while (!impl_->ring.empty() &&
                 (frame.time_s - impl_->ring.front().time_s > Impl::kRingSeconds ||
                  impl_->ring.size() > 120)) {
            impl_->ring.pop_front();
          }
        }
      }
      wgpuBufferUnmap(impl_->rec_buffer);
    }
  }
  return have_surface;
}

void Rhi::request_capture(const std::string& path) { impl_->capture_path = path; }

void Rhi::set_ring_enabled(bool enabled) { impl_->ring_enabled = enabled; }

void Rhi::trigger_recording(const std::string& dir, double future_seconds) {
  impl_->rec_dir = dir;
  impl_->rec_seq_index = 0;
  impl_->rec_until = static_cast<double>(impl_->last_frame_time) + future_seconds;
  // Dump the ring (the immediate past) as the sequence prefix.
  for (const Impl::RingFrame& ring_frame : impl_->ring) {
    impl_->emit_sequence_frame(ring_frame.time_s, ring_frame.rgb.data());
  }
  impl_->ring.clear();
  std::printf("recorder: %d ring frames dumped to %s, recording %.1fs more\n",
              impl_->rec_seq_index, dir.c_str(), future_seconds);
}

bool Rhi::recording_active() const {
  return !impl_->rec_dir.empty() &&
         static_cast<double>(impl_->last_frame_time) < impl_->rec_until;
}

bool Rhi::render_clear(float r, float g, float b) {
  FrameParams frame;
  frame.sky[0] = r;
  frame.sky[1] = g;
  frame.sky[2] = b;
  return render_frame(frame, nullptr, 0);
}

const std::string& Rhi::adapter_info() const { return impl_->adapter_info; }

}  // namespace inf::render
