#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

namespace inf::app {

// UI state only. Debugging never participates in the simulation or HUD.
struct DebugState {
  bool visible{false};
  std::array<bool, 4> expanded{true, true, true, true};
  std::array<bool, 10> previous_digits{};
  bool previous_toggle{false};

  void toggle_section(std::size_t section) {
    if (section < expanded.size())
      expanded[section] = !expanded[section];
  }
  void toggle_all() {
    const bool open = std::any_of(expanded.begin(), expanded.end(),
                                  [](bool v) { return !v; });
    expanded.fill(open);
  }
  void keys(bool toggle, const std::array<bool, 10> &digits, bool focused) {
    if (focused && toggle && !previous_toggle)
      visible = !visible;
    if (focused && visible) {
      for (std::size_t i = 0; i < digits.size(); ++i) {
        if (digits[i] && !previous_digits[i]) {
          if (i == 0)
            toggle_all();
          else
            toggle_section(i - 1);
        }
      }
    }
    // Track even while hidden/unfocused, avoiding delayed shortcuts on return.
    previous_digits = digits;
    previous_toggle = toggle;
  }
};

struct DebugSnapshot {
  std::array<std::vector<std::string>, 4> sections;
};

// Debug proximity is a view policy, not a change to world scale or physics:
// the nearest surface must be within one body radius to have planet context.
inline bool debug_body_near(double gap, double radius) {
  return radius > 0.0 && std::isfinite(gap) && gap <= radius;
}

// A site is nearby within twice its extent from its centre. Surface-relative
// Euclidean distance includes altitude, so far-away/orbital cities disappear.
inline bool debug_site_near(double distance, double radius) {
  return radius > 0.0 && std::isfinite(distance) && distance <= 2.0 * radius;
}

struct DebugLayout {
  double left{0.0}, top{16.0}, width{360.0}, height{0.0}, line{20.0},
      scale{1.3};
  std::array<double, 4> headers{};

  DebugLayout(int screen_width, int screen_height, const DebugState &state,
              const DebugSnapshot &snapshot) {
    int rows = 2;
    for (std::size_t i = 0; i < headers.size(); ++i) {
      rows +=
          1 + (state.expanded[i] ? static_cast<int>(snapshot.sections[i].size())
                                 : 0);
    }
    const double ui_scale = std::max(1.0, screen_height / 720.0);
    top = 16.0 * ui_scale;
    line = std::clamp((screen_height - 160.0 * ui_scale) / std::max(rows, 1),
                      11.0 * ui_scale, 20.0 * ui_scale);
    scale = line * 0.065;
    width = std::min(360.0 * ui_scale,
                     std::max(180.0, screen_width - 32.0 * ui_scale));
    left = std::max(0.0, screen_width - width - 16.0 * ui_scale);
    double y = top + 2.0 * line;
    for (std::size_t i = 0; i < headers.size(); ++i) {
      headers[i] = y;
      y += line * (1 + (state.expanded[i] ? snapshot.sections[i].size() : 0));
    }
    height = y - top + 8.0;
  }
  int hit(double x, double y) const {
    if (x < left || x > left + width)
      return -1;
    for (std::size_t i = 0; i < headers.size(); ++i) {
      if (y >= headers[i] && y < headers[i] + line)
        return static_cast<int>(i);
    }
    return -1;
  }
};

} // namespace inf::app
