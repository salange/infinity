#include "gpu.hpp"

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

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

#if defined(__APPLE__)
extern "C" void* infinityMetalLayerForCocoaWindow(void* nsWindow);
#endif

namespace cb {

std::string to_string(WGPUStringView v) {
  if (v.data == nullptr) return {};
  if (v.length == WGPU_STRLEN) return std::string(v.data);
  return std::string(v.data, v.length);
}

void Texture::release() {
  if (view != nullptr) wgpuTextureViewRelease(view);
  if (texture != nullptr) wgpuTextureRelease(texture);
  view = nullptr;
  texture = nullptr;
}

namespace {

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
  *error = "unsupported platform";
  return nullptr;
#endif
}

struct AdapterRequest {
  WGPUAdapter adapter{nullptr};
  std::string message;
  bool done{false};
};
struct DeviceRequest {
  WGPUDevice device{nullptr};
  std::string message;
  bool done{false};
};

}  // namespace

bool Gpu::create(GLFWwindow* window, Gpu* g, std::string* error) {
  WGPUInstanceDescriptor idesc{};
  g->instance = wgpuCreateInstance(&idesc);
  if (g->instance == nullptr) {
    *error = "wgpuCreateInstance failed";
    return false;
  }
  g->surface = create_surface(g->instance, window, error);
  if (g->surface == nullptr) {
    if (error->empty()) *error = "surface creation failed";
    return false;
  }
  AdapterRequest ar;
  {
    WGPURequestAdapterOptions options{};
    options.compatibleSurface = g->surface;
    options.powerPreference = WGPUPowerPreference_HighPerformance;
    WGPURequestAdapterCallbackInfo cb{};
    cb.mode = WGPUCallbackMode_AllowProcessEvents;
    cb.callback = [](WGPURequestAdapterStatus status, WGPUAdapter adapter, WGPUStringView message,
                     void* u1, void*) {
      auto* r = static_cast<AdapterRequest*>(u1);
      if (status == WGPURequestAdapterStatus_Success) r->adapter = adapter;
      else r->message = to_string(message);
      r->done = true;
    };
    cb.userdata1 = &ar;
    wgpuInstanceRequestAdapter(g->instance, &options, cb);
    while (!ar.done) wgpuInstanceProcessEvents(g->instance);
  }
  if (ar.adapter == nullptr) {
    *error = "no GPU adapter: " + ar.message;
    return false;
  }
  g->adapter = ar.adapter;
  {
    WGPUAdapterInfo info{};
    if (wgpuAdapterGetInfo(g->adapter, &info) == WGPUStatus_Success) {
      g->adapter_name = to_string(info.device) + " (" + to_string(info.description) + ")";
      wgpuAdapterInfoFreeMembers(info);
    }
    WGPULimits limits{};
    if (wgpuAdapterGetLimits(g->adapter, &limits) == WGPUStatus_Success) {
      g->max_buffer_size = limits.maxBufferSize;
    }
  }
  DeviceRequest dr;
  {
    WGPULimits required{};
    if (wgpuAdapterGetLimits(g->adapter, &required) != WGPUStatus_Success) {
      *error = "adapter limits unavailable";
      return false;
    }
    WGPUDeviceDescriptor ddesc{};
    ddesc.requiredLimits = &required;  // ask for everything the adapter has
    ddesc.uncapturedErrorCallbackInfo.callback = [](WGPUDevice const*, WGPUErrorType type,
                                                    WGPUStringView message, void*, void*) {
      std::fprintf(stderr, "[wgpu] error (%d): %.*s\n", static_cast<int>(type),
                   static_cast<int>(message.length == WGPU_STRLEN ? std::strlen(message.data)
                                                                   : message.length),
                   message.data);
    };
    WGPURequestDeviceCallbackInfo cb{};
    cb.mode = WGPUCallbackMode_AllowProcessEvents;
    cb.callback = [](WGPURequestDeviceStatus status, WGPUDevice device, WGPUStringView message,
                     void* u1, void*) {
      auto* r = static_cast<DeviceRequest*>(u1);
      if (status == WGPURequestDeviceStatus_Success) r->device = device;
      else r->message = to_string(message);
      r->done = true;
    };
    cb.userdata1 = &dr;
    wgpuAdapterRequestDevice(g->adapter, &ddesc, cb);
    while (!dr.done) wgpuInstanceProcessEvents(g->instance);
  }
  if (dr.device == nullptr) {
    *error = "device request failed: " + dr.message;
    return false;
  }
  g->device = dr.device;
  g->queue = wgpuDeviceGetQueue(g->device);
  {
    WGPUSurfaceCapabilities caps{};
    if (wgpuSurfaceGetCapabilities(g->surface, g->adapter, &caps) != WGPUStatus_Success ||
        caps.formatCount == 0) {
      *error = "surface reports no formats";
      return false;
    }
    g->surface_format = caps.formats[0];
    for (std::size_t i = 0; i < caps.formatCount; ++i) {
      if (caps.formats[i] == WGPUTextureFormat_BGRA8UnormSrgb ||
          caps.formats[i] == WGPUTextureFormat_RGBA8UnormSrgb) {
        g->surface_format = caps.formats[i];
        break;
      }
    }
    wgpuSurfaceCapabilitiesFreeMembers(caps);
    g->surface_srgb = g->surface_format == WGPUTextureFormat_BGRA8UnormSrgb ||
                      g->surface_format == WGPUTextureFormat_RGBA8UnormSrgb;
  }
  int fw = 0, fh = 0;
  glfwGetFramebufferSize(window, &fw, &fh);
  g->width = static_cast<std::uint32_t>(std::max(fw, 1));
  g->height = static_cast<std::uint32_t>(std::max(fh, 1));
  g->configure_surface();
  return true;
}

void Gpu::configure_surface() {
  WGPUSurfaceConfiguration config{};
  config.device = device;
  config.format = surface_format;
  config.usage = WGPUTextureUsage_RenderAttachment;
  config.width = width;
  config.height = height;
  config.alphaMode = WGPUCompositeAlphaMode_Auto;
  config.presentMode = WGPUPresentMode_Fifo;
  wgpuSurfaceConfigure(surface, &config);
}

void Gpu::resize(std::uint32_t w, std::uint32_t h) {
  if (w == 0 || h == 0) return;
  width = w;
  height = h;
  configure_surface();
}

WGPUTextureView Gpu::acquire_frame(WGPUTexture* out_texture) {
  WGPUSurfaceTexture st{};
  wgpuSurfaceGetCurrentTexture(surface, &st);
  const bool ok = st.status == WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal ||
                  st.status == WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal;
  if (!ok) {
    if (st.texture != nullptr) wgpuTextureRelease(st.texture);
    if (st.status == WGPUSurfaceGetCurrentTextureStatus_Outdated ||
        st.status == WGPUSurfaceGetCurrentTextureStatus_Lost) {
      configure_surface();
    }
    *out_texture = nullptr;
    return nullptr;
  }
  *out_texture = st.texture;
  WGPUTextureViewDescriptor vd{};
  vd.format = surface_format;
  vd.dimension = WGPUTextureViewDimension_2D;
  vd.mipLevelCount = 1;
  vd.arrayLayerCount = 1;
  vd.aspect = WGPUTextureAspect_All;
  return wgpuTextureCreateView(st.texture, &vd);
}

void Gpu::present() { wgpuSurfacePresent(surface); }

void Gpu::destroy() {
  if (queue != nullptr) wgpuQueueRelease(queue);
  if (device != nullptr) wgpuDeviceRelease(device);
  if (adapter != nullptr) wgpuAdapterRelease(adapter);
  if (surface != nullptr) {
    wgpuSurfaceUnconfigure(surface);
    wgpuSurfaceRelease(surface);
  }
  if (instance != nullptr) wgpuInstanceRelease(instance);
  queue = nullptr;
  device = nullptr;
  adapter = nullptr;
  surface = nullptr;
  instance = nullptr;
}

WGPUBuffer Gpu::create_buffer(WGPUBufferUsage usage, std::uint64_t size, const void* data,
                              const char* label) {
  WGPUBufferDescriptor bd{};
  bd.label = sv(label);
  bd.usage = usage | (data != nullptr ? WGPUBufferUsage_CopyDst : 0);
  bd.size = (size + 3) & ~std::uint64_t{3};
  WGPUBuffer b = wgpuDeviceCreateBuffer(device, &bd);
  if (data != nullptr && size > 0) wgpuQueueWriteBuffer(queue, b, 0, data, size);
  return b;
}

void Gpu::write_buffer(WGPUBuffer buffer, std::uint64_t offset, const void* data,
                       std::uint64_t size) {
  wgpuQueueWriteBuffer(queue, buffer, offset, data, size);
}

Texture Gpu::create_texture(std::uint32_t w, std::uint32_t h, WGPUTextureFormat format,
                            WGPUTextureUsage usage, std::uint32_t mips, std::uint32_t layers,
                            std::uint32_t samples, const char* label, bool cube) {
  Texture t;
  t.width = w;
  t.height = h;
  t.format = format;
  t.layers = layers;
  t.mips = mips;
  WGPUTextureDescriptor td{};
  td.label = sv(label);
  td.usage = usage;
  td.dimension = WGPUTextureDimension_2D;
  td.size = WGPUExtent3D{w, h, layers};
  td.format = format;
  td.mipLevelCount = mips;
  td.sampleCount = samples;
  t.texture = wgpuDeviceCreateTexture(device, &td);
  WGPUTextureViewDescriptor vd{};
  vd.format = format;
  vd.dimension = cube ? WGPUTextureViewDimension_Cube
                      : (layers > 1 ? WGPUTextureViewDimension_2DArray : WGPUTextureViewDimension_2D);
  vd.mipLevelCount = mips;
  vd.arrayLayerCount = layers;
  vd.aspect = WGPUTextureAspect_All;
  t.view = wgpuTextureCreateView(t.texture, &vd);
  return t;
}

WGPUTextureView Gpu::create_view(const Texture& tex, std::uint32_t base_mip,
                                 std::uint32_t mip_count, std::uint32_t base_layer,
                                 std::uint32_t layer_count, WGPUTextureViewDimension dim) {
  WGPUTextureViewDescriptor vd{};
  vd.format = tex.format;
  vd.dimension = dim;
  vd.baseMipLevel = base_mip;
  vd.mipLevelCount = mip_count;
  vd.baseArrayLayer = base_layer;
  vd.arrayLayerCount = layer_count;
  vd.aspect = WGPUTextureAspect_All;
  return wgpuTextureCreateView(tex.texture, &vd);
}

void Gpu::upload_level(const Texture& tex, std::uint32_t layer, std::uint32_t mip, std::uint32_t w,
                       std::uint32_t h, const void* data, std::uint32_t bpp) {
  WGPUTexelCopyTextureInfo dst{};
  dst.texture = tex.texture;
  dst.mipLevel = mip;
  dst.origin = WGPUOrigin3D{0, 0, layer};
  dst.aspect = WGPUTextureAspect_All;
  WGPUTexelCopyBufferLayout layout{};
  layout.offset = 0;
  layout.bytesPerRow = w * bpp;
  layout.rowsPerImage = h;
  const WGPUExtent3D extent{w, h, 1};
  wgpuQueueWriteTexture(queue, &dst, data, static_cast<std::size_t>(w) * h * bpp, &layout, &extent);
}

void Gpu::upload_rgba8_mips(const Texture& tex, std::uint32_t layer, const std::uint8_t* rgba) {
  std::uint32_t w = tex.width, h = tex.height;
  std::vector<std::uint8_t> level(rgba, rgba + static_cast<std::size_t>(w) * h * 4);
  for (std::uint32_t mip = 0; mip < tex.mips; ++mip) {
    upload_level(tex, layer, mip, w, h, level.data(), 4);
    if (w == 1 && h == 1) break;
    const std::uint32_t nw = std::max(1u, w / 2), nh = std::max(1u, h / 2);
    std::vector<std::uint8_t> next(static_cast<std::size_t>(nw) * nh * 4);
    for (std::uint32_t y = 0; y < nh; ++y) {
      for (std::uint32_t x = 0; x < nw; ++x) {
        for (int c = 0; c < 4; ++c) {
          const std::uint32_t x0 = std::min(2 * x, w - 1), x1 = std::min(2 * x + 1, w - 1);
          const std::uint32_t y0 = std::min(2 * y, h - 1), y1 = std::min(2 * y + 1, h - 1);
          const int s = level[(y0 * w + x0) * 4 + c] + level[(y0 * w + x1) * 4 + c] +
                        level[(y1 * w + x0) * 4 + c] + level[(y1 * w + x1) * 4 + c];
          next[(y * nw + x) * 4 + c] = static_cast<std::uint8_t>((s + 2) / 4);
        }
      }
    }
    level.swap(next);
    w = nw;
    h = nh;
  }
}

namespace {
std::string read_shader_source(const std::string& path, int depth, std::string* error) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    *error = "cannot read shader " + path;
    return {};
  }
  const std::string dir = path.substr(0, path.find_last_of("/\\") + 1);
  std::string out, line;
  while (std::getline(in, line)) {
    if (line.rfind("#include \"", 0) == 0 && depth < 8) {
      const std::size_t end = line.find('"', 10);
      const std::string inc = line.substr(10, end == std::string::npos ? std::string::npos : end - 10);
      out += read_shader_source(dir + inc, depth + 1, error);
      out += "\n";
    } else {
      out += line;
      out += "\n";
    }
  }
  return out;
}
}  // namespace

WGPUShaderModule Gpu::load_shader(const std::string& path, std::string* error) {
  const std::string code = read_shader_source(path, 0, error);
  if (!error->empty()) return nullptr;
  WGPUShaderSourceWGSL wgsl{};
  wgsl.chain.sType = WGPUSType_ShaderSourceWGSL;
  wgsl.code = WGPUStringView{code.data(), code.size()};
  WGPUShaderModuleDescriptor md{};
  md.nextInChain = &wgsl.chain;
  md.label = sv(path.c_str());
  return wgpuDeviceCreateShaderModule(device, &md);
}

WGPUSampler Gpu::create_sampler(WGPUAddressMode mode, WGPUFilterMode filter, bool mips,
                                std::uint16_t anisotropy, WGPUCompareFunction compare,
                                const char* label) {
  WGPUSamplerDescriptor sd{};
  sd.label = sv(label);
  sd.addressModeU = sd.addressModeV = sd.addressModeW = mode;
  sd.magFilter = sd.minFilter = filter;
  sd.mipmapFilter = mips ? WGPUMipmapFilterMode_Linear : WGPUMipmapFilterMode_Nearest;
  sd.lodMinClamp = 0.0f;
  sd.lodMaxClamp = 32.0f;
  sd.compare = compare;
  sd.maxAnisotropy = anisotropy;
  return wgpuDeviceCreateSampler(device, &sd);
}

bool Gpu::read_rgba8(const Texture& tex, std::vector<std::uint8_t>* rgba) {
  const std::uint32_t bpr = (tex.width * 4 + 255) & ~255u;
  const std::uint64_t size = static_cast<std::uint64_t>(bpr) * tex.height;
  WGPUBufferDescriptor bd{};
  bd.label = sv("readback");
  bd.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead;
  bd.size = size;
  WGPUBuffer buf = wgpuDeviceCreateBuffer(device, &bd);
  WGPUCommandEncoderDescriptor ed{};
  WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(device, &ed);
  WGPUTexelCopyTextureInfo src{};
  src.texture = tex.texture;
  src.aspect = WGPUTextureAspect_All;
  WGPUTexelCopyBufferInfo dst{};
  dst.buffer = buf;
  dst.layout.bytesPerRow = bpr;
  dst.layout.rowsPerImage = tex.height;
  const WGPUExtent3D extent{tex.width, tex.height, 1};
  wgpuCommandEncoderCopyTextureToBuffer(enc, &src, &dst, &extent);
  WGPUCommandBufferDescriptor cd{};
  WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, &cd);
  wgpuQueueSubmit(queue, 1, &cmd);
  wgpuCommandBufferRelease(cmd);
  wgpuCommandEncoderRelease(enc);
  struct MapState {
    bool done{false};
    bool ok{false};
  } state;
  WGPUBufferMapCallbackInfo mi{};
  mi.mode = WGPUCallbackMode_AllowProcessEvents;
  mi.callback = [](WGPUMapAsyncStatus status, WGPUStringView, void* u1, void*) {
    auto* s = static_cast<MapState*>(u1);
    s->ok = status == WGPUMapAsyncStatus_Success;
    s->done = true;
  };
  mi.userdata1 = &state;
  wgpuBufferMapAsync(buf, WGPUMapMode_Read, 0, size, mi);
  while (!state.done) wgpuDevicePoll(device, 1U, nullptr);
  if (!state.ok) {
    wgpuBufferRelease(buf);
    return false;
  }
  const auto* mapped = static_cast<const std::uint8_t*>(wgpuBufferGetConstMappedRange(buf, 0, size));
  rgba->resize(static_cast<std::size_t>(tex.width) * tex.height * 4);
  for (std::uint32_t y = 0; y < tex.height; ++y) {
    std::memcpy(rgba->data() + static_cast<std::size_t>(y) * tex.width * 4, mapped + static_cast<std::size_t>(y) * bpr,
                static_cast<std::size_t>(tex.width) * 4);
  }
  wgpuBufferUnmap(buf);
  wgpuBufferRelease(buf);
  return true;
}

std::uint16_t float_to_half(float f) {
  std::uint32_t x;
  std::memcpy(&x, &f, 4);
  const std::uint32_t sign = (x >> 16) & 0x8000u;
  std::int32_t exp = static_cast<std::int32_t>((x >> 23) & 0xff) - 127 + 15;
  std::uint32_t mant = x & 0x7fffffu;
  if (exp <= 0) {
    if (exp < -10) return static_cast<std::uint16_t>(sign);
    mant |= 0x800000u;
    const std::uint32_t shift = static_cast<std::uint32_t>(14 - exp);
    return static_cast<std::uint16_t>(sign | (mant >> shift));
  }
  if (exp >= 31) return static_cast<std::uint16_t>(sign | 0x7c00u);
  return static_cast<std::uint16_t>(sign | (static_cast<std::uint32_t>(exp) << 10) | (mant >> 13));
}

}  // namespace cb
