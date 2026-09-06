#pragma once
#include "debug_state.hpp"
#include "render/rhi.hpp"
#include <memory>

namespace inf::app {
class DebugOverlay {
public:
  explicit DebugOverlay(render::Rhi &rhi);
  ~DebugOverlay();
  DebugOverlay(const DebugOverlay &) = delete;
  DebugOverlay &operator=(const DebugOverlay &) = delete;
  void build(std::vector<render::Rhi::DrawItem> &items, const DebugState &state,
             const DebugSnapshot &snapshot, int width, int height);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace inf::app
