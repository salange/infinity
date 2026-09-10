#include "textures.hpp"

#include <stb_image.h>
#include <stb_image_resize2.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <future>

#include "math.hpp"
#include "rng.hpp"

namespace cb {

int MaterialArrays::layer_of(const std::string& name) const {
  for (std::size_t i = 0; i < names.size(); ++i)
    if (names[i] == name) return static_cast<int>(i);
  return 0;
}

bool MaterialArrays::has_file_set(const std::string& name) const {
  for (std::size_t i = 0; i < names.size(); ++i)
    if (names[i] == name) return i < file_sets.size() && file_sets[i];
  return false;
}

namespace {

struct Image {
  std::vector<std::uint8_t> rgba;
  std::uint32_t w{0}, h{0};
  bool ok() const { return w > 0; }
};

Image load_resized(const std::string& path, std::uint32_t size, bool srgb = false) {
  Image img;
  int w = 0, h = 0, n = 0;
  stbi_uc* data = stbi_load(path.c_str(), &w, &h, &n, 4);
  if (data == nullptr) return img;
  img.w = img.h = size;
  img.rgba.resize(static_cast<std::size_t>(size) * size * 4);
  if (static_cast<std::uint32_t>(w) == size && static_cast<std::uint32_t>(h) == size) {
    std::copy(data, data + img.rgba.size(), img.rgba.begin());
  } else {
    if (srgb)
      stbir_resize_uint8_srgb(data, w, h, 0, img.rgba.data(), static_cast<int>(size),
                              static_cast<int>(size), 0, STBIR_RGBA);
    else
      stbir_resize_uint8_linear(data, w, h, 0, img.rgba.data(), static_cast<int>(size),
                                static_cast<int>(size), 0, STBIR_RGBA);
  }
  stbi_image_free(data);
  return img;
}

// Tileable detail for the generated material library.
float vnoise(std::uint32_t seed, float x, float y, std::uint32_t period) {
  const int xi = static_cast<int>(std::floor(x)), yi = static_cast<int>(std::floor(y));
  const float fx = x - static_cast<float>(xi), fy = y - static_cast<float>(yi);
  auto h = [&](int i, int j) {
    const auto wrap=[&](int value){return static_cast<std::uint32_t>((value%int(period)+int(period))%int(period));};
    const std::uint32_t ui = wrap(i), uj = wrap(j);
    return hash01(ui, uj, seed);
  };
  const float sx = fx * fx * (3 - 2 * fx), sy = fy * fy * (3 - 2 * fy);
  const float a = h(xi, yi), b = h(xi + 1, yi), c = h(xi, yi + 1), d = h(xi + 1, yi + 1);
  return (a + (b - a) * sx) + ((c + (d - c) * sx) - (a + (b - a) * sx)) * sy;
}
float fbm(std::uint32_t seed, float x, float y, int octaves, std::uint32_t period) {
  float sum = 0.0f, amp = 0.5f, freq = 1.0f;
  for (int o = 0; o < octaves; ++o) {
    sum += amp * vnoise(seed + static_cast<std::uint32_t>(o) * 17u, x * freq, y * freq, period << o);
    amp *= 0.5f;
    freq *= 2.0f;
  }
  return sum;
}

void fallback_set(const TextureSetSpec& spec, std::uint32_t size, Image* albedo, Image* normal, Image* arm) {
  albedo->w = albedo->h = normal->w = normal->h = arm->w = arm->h = size;
  albedo->rgba.assign(static_cast<std::size_t>(size) * size * 4, 255);
  normal->rgba.assign(albedo->rgba.size(), 255);
  arm->rgba.assign(albedo->rgba.size(), 255);
  std::vector<float> height(static_cast<std::size_t>(size) * size);
  std::uint32_t seed = 2166136261u;
  for (unsigned char c : spec.name) { seed = (seed ^ c) * 16777619u; }
  const float period = 8.0f;
  for (std::uint32_t y = 0; y < size; ++y) {
    for (std::uint32_t x = 0; x < size; ++x) {
      const float u = static_cast<float>(x) / static_cast<float>(size) * period;
      const float v = static_cast<float>(y) / static_cast<float>(size) * period;
      float hgt = 0.5f, tint = 1.0f, roughness=spec.fallback_roughness, occlusion=1.0f;
      Vec3 color{spec.fallback_rgb[0],spec.fallback_rgb[1],spec.fallback_rgb[2]};
      switch (spec.fallback_pattern) {
        case 1: hgt = fbm(seed, u, v, 5, 8); tint = 0.94f + 0.12f * fbm(seed + 3, u * 0.5f, v * 0.5f, 3, 4); break;
        case 2: hgt = 0.5f + 0.1f * (fbm(seed, u * 8, v * 0.25f, 3, 64) - 0.5f); tint = 0.9f + 0.2f * hgt; break;
        case 3: {
          const float gx = std::fmod(u * 0.5f, 1.0f), gy = std::fmod(v * 0.5f, 1.0f);
          const float edge = std::min(std::min(gx, 1 - gx), std::min(gy, 1 - gy));
          hgt = std::min(1.0f, edge * 12.0f) * (0.7f + 0.3f * fbm(seed, u, v, 3, 8));
          tint = 0.96f + 0.08f * hash01(static_cast<std::uint32_t>(u * 0.5f), static_cast<std::uint32_t>(v * 0.5f), seed);
          break;
        }
        case 4: hgt = fbm(seed, u * 4, v * 4, 4, 32); tint = 0.7f + 0.6f * hgt; break;
        case 5: hgt = 0.5f + 0.15f * (fbm(seed, u * 6, v * 6, 4, 48) - 0.5f); tint = 0.85f + 0.3f * fbm(seed + 9, u, v, 3, 8); break;
        default: break;
      }
      // Material-specific structure at architectural scale. Wear is concentrated
      // in joints and pores; broad surfaces retain their maintained character.
      const float grain=hash01(x,y,seed+127);
      if(spec.name=="concrete_white") {
        const float pore=std::max(0.f,(grain-.982f)*55.f);
        hgt=.5f+.013f*(fbm(seed,u*3,v*3,4,24)-.5f)-pore*.016f;
        tint=.985f+.025f*fbm(seed,u,v,4,8)-pore*.024f;
        color={.92f,.914f,.89f};roughness=.34f+.035f*grain;
      } else if(spec.name=="paving_slabs"||spec.name=="pavement_light") {
        const float row=std::floor(v),gx=u*.5f+std::fmod(row,2.f)*.5f;
        const float fx=gx-std::floor(gx),fy=v-row;
        const float seam=std::min({fx,1-fx,fy*.5f,(1-fy)*.5f});
        const float bevel=clampf(seam/.012f,0,1);
        const float slab=hash01(std::uint32_t(std::floor(gx))%4,std::uint32_t(row),seed);
        const float stain=(1-clampf(seam/.065f,0,1))*fbm(seed+41,u*2,v*2,4,16);
        const float pit=std::max(0.f,(grain-.965f)*28.f)*.04f;
        hgt=.25f+.28f*bevel-pit;
        tint=(.91f+.11f*slab)*(1-.24f*stain)*(.58f+.42f*bevel);
        color=spec.name=="pavement_light"?Vec3{.72f,.713f,.685f}:Vec3{.56f,.57f,.55f};
        occlusion=.63f+.37f*bevel;
        roughness=.36f+.13f*grain+.2f*(1-bevel);
      } else if(spec.name=="soil") {
        const float clod=fbm(seed,u*4,v*4,5,32);
        const float grit=std::max(0.f,(grain-.85f)*5.f);
        const float mulch=std::pow(std::max(0.f,std::sin(u*83+v*19+fbm(seed,u,v,3,8)*6)),12.f)*(.4f+.6f*grain);
        hgt=.23f+.45f*clod+.12f*grit+.09f*mulch;
        tint=.63f+.58f*clod+.26f*grit;
        color={.31f+mulch*.12f,.235f+mulch*.07f,.155f+mulch*.024f};
        roughness=.9f+.08f*grain;occlusion=.77f+.23f*clod;
      } else if(spec.name=="bark") {
        const float warp=fbm(seed,u*2,v*.5f,4,16);
        const float ridge=std::pow(.5f+.5f*std::sin(u*16*kPi+warp*9),.35f);
        const float fissure=std::pow(1-ridge,1.6f);
        hgt=.2f+.55f*ridge+.06f*fbm(seed,u*8,v*3,3,64);
        tint=.61f+.49f*ridge-.3f*fissure;roughness=.85f+.1f*grain;
        color={.37f,.285f,.185f};occlusion=.65f+.35f*ridge;
      } else if(spec.name=="marble") {
        const float warp=fbm(seed,u,v,5,8);
        const float vein=std::pow(.5f+.5f*std::sin((u+v*.5f)*kPi*2+warp*8),24.f);
        hgt=.5f+grain*.004f;tint=.98f-vein*.16f;color={.84f,.84f,.815f};roughness=.22f+vein*.07f;
      } else if(spec.name=="terrazzo") {
        const float aggregate=hash01(x/3,y/3,seed);
        const float stone=aggregate>.7f?1.f:0.f;
        const float warm=aggregate>.86f?1.f:0.f;
        hgt=.5f+(grain-.5f)*.008f;tint=.94f+.08f*grain;
        color={.63f+stone*.17f+warm*.05f,.64f+stone*.15f,.62f+stone*.13f-warm*.06f};roughness=.27f+.09f*(1-stone);
      } else if(spec.name=="metal_silver"||spec.name=="metal_black") {
        const float brush=vnoise(seed,u*32,v*.25f,256);
        hgt=.5f+(brush-.5f)*.028f;tint=.97f+brush*.06f;
        roughness=spec.fallback_roughness+.09f*(brush-.5f);
      }
      height[y * size + x] = hgt;
      const std::size_t i = (static_cast<std::size_t>(y) * size + x) * 4;
      for (int c = 0; c < 3; ++c) {
        const float component=c==0?color.x:c==1?color.y:color.z;
        albedo->rgba[i + c] = static_cast<std::uint8_t>(std::clamp(component * tint, 0.0f, 1.0f) * 255.0f);
      }
      arm->rgba[i + 0] = static_cast<std::uint8_t>(occlusion*255);
      arm->rgba[i + 1] = static_cast<std::uint8_t>(std::clamp(roughness + 0.1f * (hgt - 0.5f), 0.0f, 1.0f) * 255.0f);
      arm->rgba[i + 2] = static_cast<std::uint8_t>(hgt * 255.0f);
    }
  }
  // Normal from height (tileable finite differences).
  const float strength = spec.fallback_pattern == 0 ? 0.0f : 2.0f;
  for (std::uint32_t y = 0; y < size; ++y) {
    for (std::uint32_t x = 0; x < size; ++x) {
      const float hl = height[y * size + (x + size - 1) % size], hr = height[y * size + (x + 1) % size];
      const float hd = height[((y + size - 1) % size) * size + x], hu = height[((y + 1) % size) * size + x];
      Vec3 n = normalize(Vec3{-(hr - hl) * strength, -(hu - hd) * strength, 1.0f});
      const std::size_t i = (static_cast<std::size_t>(y) * size + x) * 4;
      normal->rgba[i + 0] = static_cast<std::uint8_t>((n.x * 0.5f + 0.5f) * 255.0f);
      normal->rgba[i + 1] = static_cast<std::uint8_t>((n.y * 0.5f + 0.5f) * 255.0f);
      normal->rgba[i + 2] = static_cast<std::uint8_t>((n.z * 0.5f + 0.5f) * 255.0f);
    }
  }
}

struct LoadedSet {
  Image albedo, normal, arm;
  bool from_files{false};
};

LoadedSet load_set(const std::string& dir, const TextureSetSpec& spec, std::uint32_t size) {
  LoadedSet out;
  Image color = load_resized(dir + "/color.jpg", size, true);
  Image nrm = load_resized(dir + "/normal.jpg", size);
  Image rough = load_resized(dir + "/roughness.jpg", size);
  Image ao = load_resized(dir + "/ao.jpg", size);
  Image hgt = load_resized(dir + "/height.jpg", size);
  if (color.ok() && nrm.ok() && rough.ok()) {
    out.from_files = true;
    out.albedo = std::move(color);
    out.normal = std::move(nrm);
    out.arm.w = out.arm.h = size;
    out.arm.rgba.resize(static_cast<std::size_t>(size) * size * 4);
    for (std::size_t i = 0; i < static_cast<std::size_t>(size) * size; ++i) {
      out.arm.rgba[i * 4 + 0] = ao.ok() ? ao.rgba[i * 4] : 255;
      out.arm.rgba[i * 4 + 1] = rough.ok() ? rough.rgba[i * 4] : static_cast<std::uint8_t>(spec.fallback_roughness * 255);
      out.arm.rgba[i * 4 + 2] = hgt.ok() ? hgt.rgba[i * 4] : 128;
      out.arm.rgba[i * 4 + 3] = 255;
    }
    for (std::size_t i = 0; i < static_cast<std::size_t>(size) * size; ++i) out.albedo.rgba[i * 4 + 3] = 255;
  } else {
    fallback_set(spec, size, &out.albedo, &out.normal, &out.arm);
  }
  return out;
}

}  // namespace

MaterialArrays load_material_arrays(Gpu& gpu, const std::string& assets_dir,
                                    const std::vector<TextureSetSpec>& sets, std::uint32_t size,
                                    bool verbose) {
  MaterialArrays arrays;
  arrays.size = size;
  const std::uint32_t layers = static_cast<std::uint32_t>(sets.size());
  std::uint32_t mips = 1;
  while ((size >> mips) >= 1) ++mips;
  const WGPUTextureUsage usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
  arrays.albedo = gpu.create_texture(size, size, WGPUTextureFormat_RGBA8UnormSrgb, usage, mips, layers, 1, "albedo-array");
  arrays.normal = gpu.create_texture(size, size, WGPUTextureFormat_RGBA8Unorm, usage, mips, layers, 1, "normal-array");
  arrays.arm = gpu.create_texture(size, size, WGPUTextureFormat_RGBA8Unorm, usage, mips, layers, 1, "arm-array");
  std::vector<std::future<LoadedSet>> jobs;
  for (const TextureSetSpec& spec : sets) {
    arrays.names.push_back(spec.name);
    const std::string dir = assets_dir + "/textures/" + spec.name;
    jobs.push_back(std::async(std::launch::async, load_set, dir, spec, size));
  }
  for (std::uint32_t i = 0; i < layers; ++i) {
    LoadedSet set = jobs[i].get();
    arrays.file_sets.push_back(set.from_files);
    if (verbose) {
      std::printf("  material %-18s %s\n", sets[i].name.c_str(), set.from_files ? "files" : "generated surface library");
    }
    gpu.upload_rgba8_mips(arrays.albedo, i, set.albedo.rgba.data());
    gpu.upload_rgba8_mips(arrays.normal, i, set.normal.rgba.data());
    gpu.upload_rgba8_mips(arrays.arm, i, set.arm.rgba.data());
  }
  return arrays;
}

std::vector<std::uint8_t> make_leaf_texture(std::uint32_t size, std::uint32_t seed) {
  std::vector<std::uint8_t> rgba(static_cast<std::size_t>(size) * size * 4, 0);
  // A cluster of overlapping elliptical leaves around the centre, alpha =
  // coverage; colour varies per leaf; slightly translucent edges.
  struct Leaf { float cx, cy, rx, ry, rot, shade; };
  std::vector<Leaf> leaves;
  for (int i = 0; i < 60; ++i) {
    const float a = hash01(i, 1, seed) * 2.0f * kPi;
    const float r = std::sqrt(hash01(i, 2, seed)) * 0.42f;
    leaves.push_back(Leaf{0.5f + std::cos(a) * r, 0.5f + std::sin(a) * r, 0.05f + 0.06f * hash01(i, 3, seed),
                          0.025f + 0.03f * hash01(i, 4, seed), hash01(i, 5, seed) * kPi, 0.6f + 0.5f * hash01(i, 6, seed)});
  }
  for (std::uint32_t y = 0; y < size; ++y) {
    for (std::uint32_t x = 0; x < size; ++x) {
      const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(size);
      const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(size);
      float cover = 0.0f, shade = 0.0f;
      for (const Leaf& l : leaves) {
        const float dx = u - l.cx, dy = v - l.cy;
        const float c = std::cos(l.rot), s = std::sin(l.rot);
        const float lx = (dx * c + dy * s) / l.rx, ly = (-dx * s + dy * c) / l.ry;
        const float d = lx * lx + ly * ly;
        if (d < 1.0f) {
          const float a = std::min(1.0f, (1.0f - d) * 6.0f);
          if (a > cover) {
            cover = a;
            shade = l.shade * (0.85f + 0.3f * std::fabs(lx));
          }
        }
      }
      const std::size_t i = (static_cast<std::size_t>(y) * size + x) * 4;
      const float g = 0.26f * shade, r = 0.10f * shade + 0.04f * (1.0f - shade), b = 0.05f * shade;
      rgba[i + 0] = static_cast<std::uint8_t>(std::pow(std::clamp(r, 0.0f, 1.0f), 1.0f / 2.2f) * 255.0f);
      rgba[i + 1] = static_cast<std::uint8_t>(std::pow(std::clamp(g, 0.0f, 1.0f), 1.0f / 2.2f) * 255.0f);
      rgba[i + 2] = static_cast<std::uint8_t>(std::pow(std::clamp(b, 0.0f, 1.0f), 1.0f / 2.2f) * 255.0f);
      rgba[i + 3] = static_cast<std::uint8_t>(cover * 255.0f);
    }
  }
  return rgba;
}

}  // namespace cb
