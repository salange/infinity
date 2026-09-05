#include "material_library.hpp"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <thread>

#include <stb_image.h>

#include "tex/tiles.hpp"

namespace inf::app {

namespace {

struct Image {
  int w{0};
  int h{0};
  int channels{0};
  std::vector<std::uint8_t> data;  // always 4 channels after load
  bool ok() const { return w > 0 && h > 0; }
};

Image load_image(const std::string& path) {
  Image img;
  int w = 0;
  int h = 0;
  int n = 0;
  stbi_uc* pixels = stbi_load(path.c_str(), &w, &h, &n, 4);
  if (pixels == nullptr) {
    return img;
  }
  img.w = w;
  img.h = h;
  img.channels = 4;
  img.data.assign(pixels, pixels + static_cast<std::size_t>(w) * h * 4);
  stbi_image_free(pixels);
  return img;
}

// Samples channel c of an image at tile-space (u, v) in [0,1) with a box
// filter matched to the output size (downscale) or bilinear (upscale).
std::uint8_t sample_channel(const Image& img, int c, std::uint32_t x, std::uint32_t y,
                            std::uint32_t out_size) {
  if (static_cast<std::uint32_t>(img.w) >= out_size) {
    const std::uint32_t box = static_cast<std::uint32_t>(img.w) / out_size;
    const std::uint32_t box_y = static_cast<std::uint32_t>(img.h) / out_size;
    std::uint32_t sum = 0;
    std::uint32_t count = 0;
    for (std::uint32_t by = 0; by < box_y; ++by) {
      for (std::uint32_t bx = 0; bx < box; ++bx) {
        const std::uint32_t sx = x * box + bx;
        const std::uint32_t sy = y * box_y + by;
        sum += img.data[(static_cast<std::size_t>(sy) * img.w + sx) * 4 + c];
        ++count;
      }
    }
    return static_cast<std::uint8_t>((sum + count / 2) / (count > 0 ? count : 1));
  }
  const double fx = (x + 0.5) / out_size * img.w - 0.5;
  const double fy = (y + 0.5) / out_size * img.h - 0.5;
  const int x0 = static_cast<int>(std::floor(fx));
  const int y0 = static_cast<int>(std::floor(fy));
  const double tx = fx - x0;
  const double ty = fy - y0;
  const auto at = [&](int sx, int sy) {
    sx = ((sx % img.w) + img.w) % img.w;
    sy = ((sy % img.h) + img.h) % img.h;
    return static_cast<double>(img.data[(static_cast<std::size_t>(sy) * img.w + sx) * 4 + c]);
  };
  const double v = (at(x0, y0) * (1 - tx) + at(x0 + 1, y0) * tx) * (1 - ty) +
                   (at(x0, y0 + 1) * (1 - tx) + at(x0 + 1, y0 + 1) * tx) * ty;
  return static_cast<std::uint8_t>(v + 0.5);
}

// Builds a tile from an image set directory; returns false if the colour
// map is missing (then the procedural generator is used).
bool tile_from_files(const std::string& dir, std::uint32_t size, const gen::MaterialInfo& info,
                     tex::Tile* out) {
  const Image color = load_image(dir + "/color.jpg");
  if (!color.ok()) {
    return false;
  }
  const Image normal = load_image(dir + "/normal.jpg");
  const Image rough = load_image(dir + "/roughness.jpg");
  const Image ao = load_image(dir + "/ao.jpg");
  const Image height = load_image(dir + "/height.jpg");
  out->size = size;
  const std::size_t count = static_cast<std::size_t>(size) * size;
  out->albedo.resize(count * 4);
  out->normal.resize(count * 4);
  out->emissive = info.emissive;
  double sum[3] = {0.0, 0.0, 0.0};
  for (std::uint32_t y = 0; y < size; ++y) {
    for (std::uint32_t x = 0; x < size; ++x) {
      const std::size_t i = (static_cast<std::size_t>(y) * size + x) * 4;
      for (int c = 0; c < 3; ++c) {
        out->albedo[i + c] = sample_channel(color, c, x, y, size);
        sum[c] += out->albedo[i + c] / 255.0;
      }
      // Height: the displacement map, else luminance as a stand-in.
      if (height.ok()) {
        out->albedo[i + 3] = sample_channel(height, 0, x, y, size);
      } else {
        out->albedo[i + 3] = static_cast<std::uint8_t>(
            (out->albedo[i] * 77 + out->albedo[i + 1] * 150 + out->albedo[i + 2] * 29) / 256);
      }
      if (normal.ok()) {
        out->normal[i + 0] = sample_channel(normal, 0, x, y, size);
        out->normal[i + 1] = sample_channel(normal, 1, x, y, size);  // NormalGL: +y up
      } else {
        out->normal[i + 0] = 128;
        out->normal[i + 1] = 128;
      }
      out->normal[i + 2] = rough.ok() ? sample_channel(rough, 0, x, y, size)
                                      : static_cast<std::uint8_t>(info.roughness * 255.0f);
      if (info.emissive) {
        // Emissive mask from brightness (lava cracks, crystal tips).
        const int lum = (out->albedo[i] * 77 + out->albedo[i + 1] * 150 + out->albedo[i + 2] * 29) / 256;
        out->normal[i + 3] = static_cast<std::uint8_t>(lum > 140 ? (lum - 140) * 255 / 115 : 0);
      } else {
        out->normal[i + 3] = ao.ok() ? sample_channel(ao, 0, x, y, size) : 255;
      }
    }
  }
  for (int c = 0; c < 3; ++c) {
    out->mean_albedo[c] = static_cast<float>(sum[c] / static_cast<double>(count));
  }
  if (!normal.ok()) {
    tex::finish_tile_from_height(*out, static_cast<float>(size) * 0.012f, info.roughness, 0.0f);
  }
  return true;
}

}  // namespace

struct MaterialLibrary::Impl {
  std::thread worker;
  std::mutex mutex;
  std::atomic<bool> quit{false};
  struct Done {
    std::uint32_t id;
    tex::Tile tile;
    bool from_file;
  };
  std::vector<Done> done;
  std::uint32_t uploaded{0};
  bool created{false};
  ~Impl() {
    quit.store(true);
    if (worker.joinable()) {
      worker.join();
    }
  }
};

MaterialLibrary::MaterialLibrary() : impl_(std::make_unique<Impl>()) {
  means_.resize(gen::kMaterialCount * 3);
  for (std::uint32_t id = 0; id < gen::kMaterialCount; ++id) {
    const gen::MaterialInfo& info = gen::material_info(static_cast<gen::Material>(id));
    for (int c = 0; c < 3; ++c) {
      means_[id * 3 + c] = info.albedo[c];
    }
  }
}

MaterialLibrary::~MaterialLibrary() = default;

void MaterialLibrary::start(const std::string& assets_dir, std::uint32_t size) {
  size_ = size;
  Impl* impl = impl_.get();
  impl->worker = std::thread([impl, assets_dir, size] {
    for (std::uint32_t id = 1; id < gen::kMaterialCount; ++id) {
      if (impl->quit.load()) {
        return;
      }
      const gen::MaterialInfo& info = gen::material_info(static_cast<gen::Material>(id));
      Impl::Done done;
      done.id = id;
      done.from_file = false;
      if (!assets_dir.empty()) {
        const std::string dir = assets_dir + "/textures/" + info.name;
        done.from_file = tile_from_files(dir, size, info, &done.tile);
      }
      if (!done.from_file) {
        done.tile = tex::generate_tile(info.name, size, 0x51ED0000ULL + id);
      }
      const std::lock_guard<std::mutex> lock(impl->mutex);
      impl->done.push_back(std::move(done));
    }
  });
}

bool MaterialLibrary::poll(render::Rhi& rhi) {
  if (complete_ || size_ == 0) {
    return false;
  }
  if (!impl_->created) {
    rhi.create_material_library(size_, gen::kMaterialCount);
    impl_->created = true;
  }
  std::vector<Impl::Done> ready;
  {
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    // A few per frame: each upload also builds a mip chain on the CPU.
    const std::size_t take = impl_->done.size() < 3 ? impl_->done.size() : 3;
    ready.assign(std::make_move_iterator(impl_->done.begin()),
                 std::make_move_iterator(impl_->done.begin() + static_cast<std::ptrdiff_t>(take)));
    impl_->done.erase(impl_->done.begin(), impl_->done.begin() + static_cast<std::ptrdiff_t>(take));
  }
  for (Impl::Done& d : ready) {
    rhi.upload_material_layer(d.id, d.tile.albedo.data(), d.tile.normal.data());
    for (int c = 0; c < 3; ++c) {
      means_[d.id * 3 + c] = d.tile.mean_albedo[c];
    }
    from_files_ += d.from_file ? 1 : 0;
    ++impl_->uploaded;
  }
  if (impl_->uploaded >= gen::kMaterialCount - 1) {
    complete_ = true;
    if (impl_->worker.joinable()) {
      impl_->worker.join();
    }
    std::printf("materials: %u tiles resident at %ux%u (%u from image sets, %u procedural)\n",
                impl_->uploaded, size_, size_, from_files_, impl_->uploaded - from_files_);
    return true;
  }
  return false;
}

void MaterialLibrary::apply_planet(render::Rhi& rhi, const gen::MaterialField& field) const {
  for (std::uint32_t id = 0; id < gen::kMaterialCount; ++id) {
    const auto material = static_cast<gen::Material>(id);
    const gen::MaterialInfo& info = gen::material_info(material);
    render::Rhi::MaterialParams params;
    field.tint(material, params.tint);
    params.tile_m = info.tile_m;
    params.roughness = info.roughness;
    params.normal_strength = 1.35f;
    params.emissive = 0.0f;
    if (info.emissive) {
      params.emissive = material == gen::Material::LavaRock
                            ? 2.5f
                            : 1.5f * field.palette().emissive;
    }
    for (int c = 0; c < 3; ++c) {
      params.mean[c] = means_[id * 3 + c];
    }
    rhi.set_material_params(id, params);
  }
}

std::string find_assets_dir(const char* override_dir, const char* argv0) {
  namespace fs = std::filesystem;
  const auto has_manifest = [](const fs::path& dir) {
    std::error_code ec;
    return fs::is_regular_file(dir / "manifest.json", ec);
  };
  if (override_dir != nullptr && has_manifest(override_dir)) {
    return override_dir;
  }
  if (const char* env = std::getenv("INFINITY_ASSETS"); env != nullptr && has_manifest(env)) {
    return env;
  }
  if (argv0 != nullptr) {
    std::error_code ec;
    const fs::path exe = fs::weakly_canonical(fs::path(argv0), ec);
    if (!ec) {
      const fs::path beside = exe.parent_path() / "assets";
      if (has_manifest(beside)) {
        return beside.string();
      }
    }
  }
#ifdef INFINITY_SOURCE_DIR
  if (has_manifest(fs::path(INFINITY_SOURCE_DIR) / "assets")) {
    return (fs::path(INFINITY_SOURCE_DIR) / "assets").string();
  }
#endif
  return std::string();
}

}  // namespace inf::app
