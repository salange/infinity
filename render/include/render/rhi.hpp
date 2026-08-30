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

  // Human-readable adapter description ("<name> (<backend>)").
  const std::string& adapter_info() const;

 private:
  struct Impl;
  explicit Rhi(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace inf::render
