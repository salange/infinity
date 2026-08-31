#include "render/rhi.hpp"

#include <webgpu/webgpu.h>

#include <cstdio>
#include <cstring>
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
constexpr std::uint64_t kFrameUniformSize = 32;  // sun_dir + sun_color/time

constexpr const char* kMeshShader = R"(
// Per-item block. aux/extra are mode-specific:
//   mode 0 (extra.w): legacy — color.a == 0 lit terrain, > 0 unlit color.
//   mode 1: star photosphere — aux.xyz = camera->star-center unit dir,
//           aux.w = per-star phase seed, extra.x = spot amount.
//   mode 2: corona/glow billboard (additive pass) — aux.w = phase seed,
//           extra.x = intensity, extra.y = photosphere radius in
//           billboard units, extra.z = diffraction-spike strength [0,1].
struct Uniforms {
  mvp: mat4x4<f32>,
  color: vec4<f32>,
  aux: vec4<f32>,
  extra: vec4<f32>,
};
// Per-frame globals: sun_dir.xyz = light direction (frame of the meshes),
// sun_color.rgb = light tint, sun_color.a = time in seconds.
struct Frame {
  sun_dir: vec4<f32>,
  sun_color: vec4<f32>,
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
  // Slow flicker.
  let flicker = 0.94 + 0.06 * vnoise(vec3<f32>(time * 0.5, phase * 91.0, 0.0));
  return aces(c * flicker * intensity) * window;
}

@fragment
fn fs_main(in: VSOut) -> @location(0) vec4<f32> {
  let mode = u32(u.extra.w + 0.5);
  let time = frame.sun_color.a;
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
  if (u.color.a > 0.001) {
    // Unlit solid color; alpha passes through (blended pipeline only).
    return vec4<f32>(u.color.rgb, u.color.a);
  }
  // Lit terrain: directional sun + a cool sky/bounce fill from the
  // opposite hemisphere so the night side stays readable and the
  // terminator picks up a blue-hour cast.
  let light = normalize(frame.sun_dir.xyz);
  let n = normalize(in.normal);
  let ndl = max(dot(n, light), 0.0);
  // Soft terminator wrap so the day/night line does not alias harshly.
  let wrap = max((dot(n, light) + 0.08) / 1.08, 0.0);
  let base = vec3<f32>(0.55, 0.52, 0.45);
  var color = base * (0.05 + 1.05 * mix(ndl, wrap, 0.35)) * frame.sun_color.rgb;
  let fill = max(dot(n, -light), 0.0);
  color += base * fill * vec3<f32>(0.05, 0.07, 0.12);
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
    depth_state.depthCompare = WGPUCompareFunction_Less;
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
    float frame_block[8] = {frame.sun_dir[0],   frame.sun_dir[1],   frame.sun_dir[2],
                            0.0f,               frame.sun_color[0], frame.sun_color[1],
                            frame.sun_color[2], frame.time_s};
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

  WGPUSurfaceTexture surface_texture{};
  wgpuSurfaceGetCurrentTexture(impl_->surface, &surface_texture);
  if (surface_texture.status != WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal &&
      surface_texture.status != WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal) {
    if (surface_texture.texture != nullptr) {
      wgpuTextureRelease(surface_texture.texture);
    }
    if (surface_texture.status == WGPUSurfaceGetCurrentTextureStatus_Outdated ||
        surface_texture.status == WGPUSurfaceGetCurrentTextureStatus_Lost) {
      impl_->configure_surface();
    }
    return false;
  }
  WGPUTextureView view = wgpuTextureCreateView(surface_texture.texture, nullptr);

  WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(impl_->device, nullptr);
  WGPURenderPassColorAttachment attachment{};
  attachment.view = view;
  attachment.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
  attachment.loadOp = WGPULoadOp_Clear;
  attachment.storeOp = WGPUStoreOp_Store;
  attachment.clearValue = WGPUColor{frame.sky[0], frame.sky[1], frame.sky[2], 1.0};
  WGPURenderPassDepthStencilAttachment depth_attachment{};
  depth_attachment.view = impl_->depth_view;
  depth_attachment.depthLoadOp = WGPULoadOp_Clear;
  depth_attachment.depthStoreOp = WGPUStoreOp_Store;
  depth_attachment.depthClearValue = 1.0f;
  WGPURenderPassDescriptor pass_desc{};
  pass_desc.colorAttachmentCount = 1;
  pass_desc.colorAttachments = &attachment;
  pass_desc.depthStencilAttachment = &depth_attachment;

  WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(encoder, &pass_desc);
  enum class Pass { Opaque, Blend, Additive };
  const auto draw_items = [&](Pass which) {
    for (std::size_t i = 0; i < count; ++i) {
      const Pass item_pass = items[i].mode == 2 ? Pass::Additive
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
  wgpuRenderPassEncoderEnd(pass);
  wgpuRenderPassEncoderRelease(pass);

  WGPUCommandBuffer commands = wgpuCommandEncoderFinish(encoder, nullptr);
  wgpuCommandEncoderRelease(encoder);
  wgpuQueueSubmit(impl_->queue, 1, &commands);
  wgpuCommandBufferRelease(commands);
  wgpuTextureViewRelease(view);

  wgpuSurfacePresent(impl_->surface);
  wgpuTextureRelease(surface_texture.texture);
  return true;
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
