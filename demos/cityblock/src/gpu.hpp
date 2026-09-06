#pragma once
// wgpu-native context and helpers for the demo renderer: device, surface,
// buffers, textures with CPU mip chains, shader modules from files,
// readback. Everything else (passes, pipelines) lives in renderer.cpp.
#include <webgpu/webgpu.h>
#include <webgpu/wgpu.h>

#include <cstdint>
#include <string>
#include <vector>

struct GLFWwindow;

namespace cb {

inline WGPUStringView sv(const char* s) { return WGPUStringView{s, WGPU_STRLEN}; }
std::string to_string(WGPUStringView v);

struct Texture {
  WGPUTexture texture{nullptr};
  WGPUTextureView view{nullptr};  // full view (all mips, all layers)
  WGPUTextureFormat format{WGPUTextureFormat_Undefined};
  std::uint32_t width{0}, height{0}, layers{1}, mips{1};
  void release();
};

class Gpu {
 public:
  static bool create(GLFWwindow* window, Gpu* out, std::string* error);
  void destroy();

  void resize(std::uint32_t w, std::uint32_t h);
  // Acquires the surface texture; returns nullptr when the frame must be
  // skipped (surface outdated mid-resize; already reconfigured).
  WGPUTextureView acquire_frame(WGPUTexture* out_texture);
  void present();
  void poll(bool wait) { wgpuDevicePoll(device, wait ? 1U : 0U, nullptr); }

  WGPUBuffer create_buffer(WGPUBufferUsage usage, std::uint64_t size, const void* data,
                           const char* label);
  void write_buffer(WGPUBuffer buffer, std::uint64_t offset, const void* data, std::uint64_t size);

  Texture create_texture(std::uint32_t w, std::uint32_t h, WGPUTextureFormat format,
                         WGPUTextureUsage usage, std::uint32_t mips = 1, std::uint32_t layers = 1,
                         std::uint32_t samples = 1, const char* label = "texture",
                         bool cube = false);
  // Uploads one layer of RGBA8 texels and generates the mip chain on the CPU.
  void upload_rgba8_mips(const Texture& tex, std::uint32_t layer, const std::uint8_t* rgba);
  // Uploads one layer/mip of RGBA16F (halfs) or RGBA32F data as-is.
  void upload_level(const Texture& tex, std::uint32_t layer, std::uint32_t mip, std::uint32_t w,
                    std::uint32_t h, const void* data, std::uint32_t bytes_per_pixel);
  WGPUTextureView create_view(const Texture& tex, std::uint32_t base_mip, std::uint32_t mip_count,
                              std::uint32_t base_layer, std::uint32_t layer_count,
                              WGPUTextureViewDimension dim);
  WGPUShaderModule load_shader(const std::string& path, std::string* error);
  WGPUSampler create_sampler(WGPUAddressMode mode, WGPUFilterMode filter, bool mips,
                             std::uint16_t anisotropy, WGPUCompareFunction compare, const char* label);

  // Synchronous readback of an RGBA8 texture (mip 0, layer 0) into rgba.
  bool read_rgba8(const Texture& tex, std::vector<std::uint8_t>* rgba);
  // Synchronous readback of a Depth32Float texture.
  bool read_depth32(const Texture& tex, std::vector<float>* depth);

  WGPUInstance instance{nullptr};
  WGPUSurface surface{nullptr};
  WGPUAdapter adapter{nullptr};
  WGPUDevice device{nullptr};
  WGPUQueue queue{nullptr};
  WGPUTextureFormat surface_format{WGPUTextureFormat_Undefined};
  bool surface_srgb{false};
  std::uint32_t width{0}, height{0};
  std::string adapter_name;
  std::uint64_t max_buffer_size{0};
  bool vsync{true};

 private:
  void configure_surface();
};

// Float → IEEE half.
std::uint16_t float_to_half(float f);

}  // namespace cb
