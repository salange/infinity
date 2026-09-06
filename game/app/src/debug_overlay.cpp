#include "debug_overlay.hpp"

#include <algorithm>
#include <array>
#include <cstring>

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include <stb_easy_font.h>
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

namespace inf::app {
using render::Rhi;
namespace {
Rhi::DrawItem screen_item(std::uint32_t mesh, double x, double y, double sx,
                          double sy, int width, int height, bool text) {
  Rhi::DrawItem item;
  item.mesh = mesh;
  item.overlay = true;
  item.mvp[0] = static_cast<float>(2.0 * sx / width);
  item.mvp[5] = static_cast<float>(-2.0 * sy / height);
  item.mvp[10] = 0.00001f;
  item.mvp[12] = static_cast<float>(2.0 * x / width - 1.0);
  item.mvp[13] = static_cast<float>(1.0 - 2.0 * y / height);
  item.mvp[14] = text ? 0.99999f : 0.9999f;
  item.mvp[15] = 1.0f;
  item.color[0] = text ? 0.78f : 0.025f;
  item.color[1] = text ? 0.92f : 0.04f;
  item.color[2] = text ? 0.88f : 0.055f;
  item.color[3] = 1.0f;
  return item;
}
} // namespace

struct DebugOverlay::Impl {
  struct Line {
    std::string text;
    std::uint32_t mesh{0};
  };
  Rhi &rhi;
  std::vector<Line> lines;
  std::uint32_t background{0};
  explicit Impl(Rhi &renderer) : rhi(renderer) {
    const float vertices[] = {0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 1,
                              1, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1,
                              1, 1, 0, 0, 0, 1, 0, 1, 0, 0, 0, 1};
    background = rhi.create_mesh(vertices, std::size(vertices));
  }
  ~Impl() {
    for (const auto &line : lines)
      if (line.mesh)
        rhi.destroy_mesh(line.mesh);
    rhi.destroy_mesh(background);
  }
  void text(std::size_t index, std::string value, double x, double y,
            const DebugLayout &layout, int width, int height,
            std::vector<Rhi::DrawItem> &items) {
    if (lines.size() <= index)
      lines.resize(index + 1);
    auto &line = lines[index];
    // Fit labels inside the panel; full identifiers remain in the world data.
    const int max_chars = std::max(
        12, static_cast<int>((layout.width - 24) / (6 * layout.scale)));
    if (value.size() > static_cast<std::size_t>(max_chars))
      value = value.substr(0, max_chars - 3) + "...";
    if (line.text != value) {
      if (line.mesh)
        rhi.destroy_mesh(line.mesh);
      line.mesh = 0;
      line.text = value;
      alignas(float) std::array<char, 24000> buffer{};
      unsigned char white[]{255, 255, 255, 255};
      const int quads =
          stb_easy_font_print(0, 0, value.data(), white, buffer.data(),
                              static_cast<int>(buffer.size()));
      const auto *raw = reinterpret_cast<const float *>(buffer.data());
      std::vector<float> vertices;
      for (int q = 0; q < quads; ++q) {
        for (int v : {0, 1, 2, 0, 2, 3}) {
          const int offset = (q * 4 + v) * 4;
          vertices.insert(vertices.end(),
                          {raw[offset], raw[offset + 1], 0, 0, 0, 1});
        }
      }
      if (!vertices.empty())
        line.mesh = rhi.create_mesh(vertices.data(), vertices.size());
    }
    if (line.mesh)
      items.push_back(screen_item(line.mesh, x, y, layout.scale, layout.scale,
                                  width, height, true));
  }
};

DebugOverlay::DebugOverlay(Rhi &rhi) : impl_(std::make_unique<Impl>(rhi)) {}
DebugOverlay::~DebugOverlay() = default;
void DebugOverlay::build(std::vector<Rhi::DrawItem> &items,
                         const DebugState &state, const DebugSnapshot &snapshot,
                         int width, int height) {
  if (!state.visible || width <= 0 || height <= 0)
    return;
  const DebugLayout layout(width, height, state, snapshot);
  items.push_back(screen_item(impl_->background, layout.left, layout.top,
                              layout.width, layout.height, width, height,
                              false));
  std::size_t line = 0;
  const double x = layout.left + 12;
  impl_->text(line++, "DEBUG  [F3] hide  [0] fold all", x, layout.top + 5,
              layout, width, height, items);
  impl_->text(line++, "Hold Alt for pointer / click sections", x,
              layout.top + layout.line + 3, layout, width, height, items);
  constexpr const char *names[] = {"CITY", "PLANET / MOON", "PLANETARY SYSTEM",
                                   "GALAXY"};
  for (std::size_t i = 0; i < state.expanded.size(); ++i) {
    const std::string heading = "[" + std::to_string(i + 1) + "] " +
                                (state.expanded[i] ? "- " : "+ ") + names[i];
    impl_->text(line++, heading, x, layout.headers[i] + 3, layout, width,
                height, items);
    if (!state.expanded[i])
      continue;
    double y = layout.headers[i] + layout.line;
    for (const auto &value : snapshot.sections[i]) {
      impl_->text(line++, value, x + 4, y + 3, layout, width, height, items);
      y += layout.line;
    }
  }
  // Drop hidden/stale text meshes as context shrinks or sections fold.
  while (impl_->lines.size() > line) {
    if (impl_->lines.back().mesh)
      impl_->rhi.destroy_mesh(impl_->lines.back().mesh);
    impl_->lines.pop_back();
  }
}
} // namespace inf::app
