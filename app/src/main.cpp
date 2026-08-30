#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#include <GLFW/glfw3.h>

#include "core/version.hpp"
#include "render/rhi.hpp"

namespace {

struct AppState {
  inf::render::Rhi* rhi = nullptr;
};

void framebuffer_size_callback(GLFWwindow* window, int width, int height) {
  auto* state = static_cast<AppState*>(glfwGetWindowUserPointer(window));
  if (state != nullptr && state->rhi != nullptr && width > 0 && height > 0) {
    state->rhi->resize(static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height));
  }
}

}  // namespace

int main(int argc, char** argv) {
  // --frames N: render N frames then exit (0 = run until closed).
  // Used by ci/check.sh as a windowed smoke test.
  long max_frames = 0;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
      max_frames = std::strtol(argv[i + 1], nullptr, 10);
      ++i;
    }
  }

  if (glfwInit() != GLFW_TRUE) {
    std::fprintf(stderr, "glfwInit failed\n");
    return EXIT_FAILURE;
  }

  glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
  GLFWwindow* window =
      glfwCreateWindow(1280, 720, "infinity", nullptr, nullptr);
  if (window == nullptr) {
    std::fprintf(stderr, "glfwCreateWindow failed\n");
    glfwTerminate();
    return EXIT_FAILURE;
  }

  std::string error;
  std::unique_ptr<inf::render::Rhi> rhi = inf::render::Rhi::create(window, &error);
  if (rhi == nullptr) {
    std::fprintf(stderr, "RHI init failed: %s\n", error.c_str());
    glfwDestroyWindow(window);
    glfwTerminate();
    return EXIT_FAILURE;
  }
  std::printf("infinity %s (%s) — adapter: %s\n", inf::core::kVersion, inf::core::kGitHash,
              rhi->adapter_info().c_str());

  AppState state{rhi.get()};
  glfwSetWindowUserPointer(window, &state);
  glfwSetFramebufferSizeCallback(window, framebuffer_size_callback);

  long frame = 0;
  while (glfwWindowShouldClose(window) == GLFW_FALSE) {
    glfwPollEvents();
    if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
      glfwSetWindowShouldClose(window, GLFW_TRUE);
    }

    // Slow color sweep so a human (and a screenshot) can see it is alive.
    const double time = static_cast<double>(frame) / 120.0;
    const auto red = static_cast<float>(0.5 + 0.5 * std::sin(time));
    const auto green = static_cast<float>(0.5 + 0.5 * std::sin(time + 2.1));
    const auto blue = static_cast<float>(0.5 + 0.5 * std::sin(time + 4.2));
    rhi->render_clear(red, green, blue);

    ++frame;
    if (max_frames > 0 && frame >= max_frames) {
      glfwSetWindowShouldClose(window, GLFW_TRUE);
    }
  }

  rhi.reset();
  glfwDestroyWindow(window);
  glfwTerminate();
  return EXIT_SUCCESS;
}
