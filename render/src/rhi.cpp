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

constexpr const char* kMeshShader = R"(
struct Uniforms { mvp: mat4x4<f32> };
@group(0) @binding(0) var<uniform> u: Uniforms;

struct VSOut {
  @builtin(position) pos: vec4<f32>,
  @location(0) normal: vec3<f32>,
};

@vertex
fn vs_main(@location(0) position: vec3<f32>, @location(1) normal: vec3<f32>) -> VSOut {
  var out: VSOut;
  out.pos = u.mvp * vec4<f32>(position, 1.0);
  out.normal = normal;
  return out;
}

@fragment
fn fs_main(in: VSOut) -> @location(0) vec4<f32> {
  let light = normalize(vec3<f32>(0.45, 0.75, 0.5));
  let n = normalize(in.normal);
  let ndl = max(dot(n, light), 0.0);
  let base = vec3<f32>(0.55, 0.52, 0.45);
  let color = base * (0.22 + 0.78 * ndl);
  return vec4<f32>(color, 1.0);
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

WGPUSurface create_surface(WGPUInstance instance, GLFWwindow* window, std::string* error) {
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
  WGPUBindGroupLayout bind_layout = nullptr;
  WGPUBindGroup bind_group = nullptr;
  WGPUBuffer uniform_buffer = nullptr;
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

    WGPUBindGroupLayoutEntry layout_entry{};
    layout_entry.binding = 0;
    layout_entry.visibility = WGPUShaderStage_Vertex;
    layout_entry.buffer.type = WGPUBufferBindingType_Uniform;
    layout_entry.buffer.hasDynamicOffset = 1U;
    layout_entry.buffer.minBindingSize = 64;
    WGPUBindGroupLayoutDescriptor layout_desc{};
    layout_desc.entryCount = 1;
    layout_desc.entries = &layout_entry;
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
    wgpuPipelineLayoutRelease(pipeline_layout);
    wgpuShaderModuleRelease(module);

    WGPUBufferDescriptor uniform_desc{};
    uniform_desc.label = sv("uniforms");
    uniform_desc.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
    uniform_desc.size = kUniformStride * kMaxDrawItems;
    uniform_buffer = wgpuDeviceCreateBuffer(device, &uniform_desc);

    WGPUBindGroupEntry bind_entry{};
    bind_entry.binding = 0;
    bind_entry.buffer = uniform_buffer;
    bind_entry.offset = 0;
    bind_entry.size = 64;
    WGPUBindGroupDescriptor bind_desc{};
    bind_desc.layout = bind_layout;
    bind_desc.entryCount = 1;
    bind_desc.entries = &bind_entry;
    bind_group = wgpuDeviceCreateBindGroup(device, &bind_desc);
  }

  ~Impl() {
    for (auto& [id, mesh] : meshes) {
      wgpuBufferRelease(mesh.buffer);
    }
    if (bind_group != nullptr) wgpuBindGroupRelease(bind_group);
    if (uniform_buffer != nullptr) wgpuBufferRelease(uniform_buffer);
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

bool Rhi::render_frame(float r, float g, float b, const DrawItem* items,
                       std::size_t item_count) {
  impl_->ensure_mesh_pipeline();

  // Upload all uniforms before the command buffer executes.
  const std::size_t count = item_count > kMaxDrawItems ? kMaxDrawItems : item_count;
  for (std::size_t i = 0; i < count; ++i) {
    wgpuQueueWriteBuffer(impl_->queue, impl_->uniform_buffer, i * kUniformStride,
                         items[i].mvp, sizeof(items[i].mvp));
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
  attachment.clearValue = WGPUColor{r, g, b, 1.0};
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
  wgpuRenderPassEncoderSetPipeline(pass, impl_->mesh_pipeline);
  for (std::size_t i = 0; i < count; ++i) {
    const auto it = impl_->meshes.find(items[i].mesh);
    if (it == impl_->meshes.end() || it->second.vertex_count == 0) {
      continue;
    }
    const std::uint32_t offset = static_cast<std::uint32_t>(i * kUniformStride);
    wgpuRenderPassEncoderSetBindGroup(pass, 0, impl_->bind_group, 1, &offset);
    wgpuRenderPassEncoderSetVertexBuffer(pass, 0, it->second.buffer, 0, WGPU_WHOLE_SIZE);
    wgpuRenderPassEncoderDraw(pass, it->second.vertex_count, 1, 0, 0);
  }
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

bool Rhi::render_clear(float r, float g, float b) { return render_frame(r, g, b, nullptr, 0); }

const std::string& Rhi::adapter_info() const { return impl_->adapter_info; }

}  // namespace inf::render
