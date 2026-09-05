#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "gen/material.hpp"
#include "render/rhi.hpp"

namespace inf::app {

// The surface tile library on the app side (T0019 WP6): loads the CC0
// sets fetched into assets/textures/<material>/ (colour, normal,
// roughness, ao, height) on a background thread, packs them into the
// renderer's two RGBA layouts, and falls back to the procedural tile
// generator for any material without a complete set — so the game never
// depends on the assets being present. Uploads happen on the main
// thread through poll().
class MaterialLibrary {
 public:
  MaterialLibrary();
  ~MaterialLibrary();

  // Starts loading; `assets_dir` may be empty (procedural only).
  void start(const std::string& assets_dir, std::uint32_t size);

  // Uploads finished tiles (a few per call). Returns true the first time
  // every material is resident.
  bool poll(render::Rhi& rhi);

  bool complete() const { return complete_; }
  std::uint32_t size() const { return size_; }
  // kMaterialCount x 3 untinted mean albedos (registry means until the
  // tile arrives, measured means after).
  const float* mean_albedo_table() const { return means_.data(); }
  // Which materials came from image sets (the rest are procedural).
  std::uint32_t loaded_from_files() const { return from_files_; }

  // Pushes the per-material parameters for a planet's palette into the
  // renderer (tints, tile sizes, emissive, means).
  void apply_planet(render::Rhi& rhi, const gen::MaterialField& field) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  std::vector<float> means_;
  std::uint32_t size_{0};
  std::uint32_t from_files_{0};
  bool complete_{false};
};

// Where the assets live: --assets, INFINITY_ASSETS, <exe dir>/assets, or
// the source tree's assets/ directory (development convenience).
std::string find_assets_dir(const char* override_dir, const char* argv0);

}  // namespace inf::app
