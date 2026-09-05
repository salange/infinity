#include "ibl.hpp"

#include <stb_image.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <atomic>
#include <functional>
#include <thread>

namespace cb {

namespace {

struct Equirect {
  std::vector<float> rgb;  // w*h*3, linear
  int w{0}, h{0};
  Vec3 sample_dir(Vec3 d) const {  // nearest-ish bilinear
    const float u = std::atan2(d.x, -d.z) / (2.0f * kPi) + 0.5f;
    const float v = std::acos(clampf(d.y, -1.0f, 1.0f)) / kPi;
    return sample_uv(u, v);
  }
  Vec3 sample_uv(float u, float v) const {
    float x = u * static_cast<float>(w) - 0.5f, y = v * static_cast<float>(h) - 0.5f;
    x = std::fmod(x + static_cast<float>(w) * 4.0f, static_cast<float>(w));
    y = clampf(y, 0.0f, static_cast<float>(h - 1));
    const int x0 = static_cast<int>(x), y0 = static_cast<int>(y);
    const int x1 = (x0 + 1) % w, y1 = std::min(y0 + 1, h - 1);
    const float fx = x - static_cast<float>(x0), fy = y - static_cast<float>(y0);
    auto at = [&](int xi, int yi) {
      const float* p = &rgb[(static_cast<std::size_t>(yi) * w + xi) * 3];
      return Vec3{p[0], p[1], p[2]};
    };
    return lerp(lerp(at(x0, y0), at(x1, y0), fx), lerp(at(x0, y1), at(x1, y1), fx), fy);
  }
  Vec3 dir_of(int x, int y) const {
    const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(w);
    const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(h);
    const float phi = (u - 0.5f) * 2.0f * kPi, theta = v * kPi;
    return Vec3{std::sin(theta) * std::sin(phi), std::cos(theta), -std::sin(theta) * std::cos(phi)};
  }
  float solid_angle(int y) const {
    const float theta = (static_cast<float>(y) + 0.5f) / static_cast<float>(h) * kPi;
    return std::sin(theta) * (kPi / static_cast<float>(h)) * (2.0f * kPi / static_cast<float>(w));
  }
  Equirect downsample() const {
    Equirect o;
    o.w = std::max(1, w / 2);
    o.h = std::max(1, h / 2);
    o.rgb.resize(static_cast<std::size_t>(o.w) * o.h * 3);
    for (int y = 0; y < o.h; ++y)
      for (int x = 0; x < o.w; ++x)
        for (int c = 0; c < 3; ++c) {
          const int x0 = std::min(2 * x, w - 1), x1 = std::min(2 * x + 1, w - 1);
          const int y0 = std::min(2 * y, h - 1), y1 = std::min(2 * y + 1, h - 1);
          o.rgb[(static_cast<std::size_t>(y) * o.w + x) * 3 + c] =
              0.25f * (rgb[(static_cast<std::size_t>(y0) * w + x0) * 3 + c] + rgb[(static_cast<std::size_t>(y0) * w + x1) * 3 + c] +
                       rgb[(static_cast<std::size_t>(y1) * w + x0) * 3 + c] + rgb[(static_cast<std::size_t>(y1) * w + x1) * 3 + c]);
        }
    return o;
  }
};

float luminance(Vec3 c) { return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z; }

// Standard cubemap face directions (+X,-X,+Y,-Y,+Z,-Z), s,t in [-1,1], t down.
Vec3 face_dir(int face, float s, float t) {
  switch (face) {
    case 0: return normalize(Vec3{1, -t, -s});
    case 1: return normalize(Vec3{-1, -t, s});
    case 2: return normalize(Vec3{s, 1, t});
    case 3: return normalize(Vec3{s, -1, -t});
    case 4: return normalize(Vec3{s, -t, 1});
    default: return normalize(Vec3{-s, -t, -1});
  }
}

Vec3 rotate_y(Vec3 d, float yaw) {
  const float c = std::cos(yaw), s = std::sin(yaw);
  return Vec3{d.x * c + d.z * s, d.y, -d.x * s + d.z * c};
}

void parallel_for(int n, const std::function<void(int)>& fn) {
  const int threads = std::max(1, static_cast<int>(std::thread::hardware_concurrency()));
  std::vector<std::thread> pool;
  std::atomic<int> next{0};
  for (int t = 0; t < threads; ++t) {
    pool.emplace_back([&] {
      for (int i = next++; i < n; i = next++) fn(i);
    });
  }
  for (auto& th : pool) th.join();
}

// Hammersley point i of n.
Vec2 hammersley(std::uint32_t i, std::uint32_t n) {
  std::uint32_t bits = i;
  bits = (bits << 16u) | (bits >> 16u);
  bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
  bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
  bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
  bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
  return Vec2{static_cast<float>(i) / static_cast<float>(n), static_cast<float>(bits) * 2.3283064365386963e-10f};
}

// Uploads a float RGB cube face set as RGBA16F.
void upload_cube_level(Gpu& gpu, const Texture& tex, int mip, int size, const std::vector<Vec3>& faces) {
  std::vector<std::uint16_t> half(static_cast<std::size_t>(size) * size * 4);
  for (int f = 0; f < 6; ++f) {
    for (int i = 0; i < size * size; ++i) {
      const Vec3 c = faces[static_cast<std::size_t>(f) * size * size + i];
      half[i * 4 + 0] = float_to_half(c.x);
      half[i * 4 + 1] = float_to_half(c.y);
      half[i * 4 + 2] = float_to_half(c.z);
      half[i * 4 + 3] = float_to_half(1.0f);
    }
    gpu.upload_level(tex, static_cast<std::uint32_t>(f), static_cast<std::uint32_t>(mip), static_cast<std::uint32_t>(size),
                     static_cast<std::uint32_t>(size), half.data(), 8);
  }
}

Environment build_environment(Gpu& gpu, Equirect env, float yaw, std::uint32_t bg_size, std::uint32_t spec_size,
                              bool verbose) {
  Environment out;
  // ---- sun extraction --------------------------------------------------
  // Brightest texel of a 4x-downsampled copy, then integrate the radiance
  // within a small cone around it that exceeds 20% of the peak.
  Equirect small = env.downsample().downsample();
  int bx = 0, by = 0;
  float best = -1.0f;
  for (int y = 0; y < small.h; ++y)
    for (int x = 0; x < small.w; ++x) {
      const float* p = &small.rgb[(static_cast<std::size_t>(y) * small.w + x) * 3];
      const float l = 0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2];
      if (l > best) {
        best = l;
        bx = x;
        by = y;
      }
    }
  const Vec3 sun_dir_raw = small.dir_of(bx, by);
  // Mean radiance excluding the sun region as the sky level.
  const float cone_cos = std::cos(radians(4.0f));
  double sky_sum = 0.0, sky_w = 0.0;
  Vec3 sun_irr{0, 0, 0};
  for (int y = 0; y < env.h; ++y) {
    const float dw = env.solid_angle(y);
    for (int x = 0; x < env.w; ++x) {
      const Vec3 d = env.dir_of(x, y);
      const float* p = &env.rgb[(static_cast<std::size_t>(y) * env.w + x) * 3];
      const Vec3 c{p[0], p[1], p[2]};
      if (dot(d, sun_dir_raw) > cone_cos) {
        sun_irr += c * dw;
      } else if (d.y > 0.0f) {
        sky_sum += static_cast<double>(luminance(c)) * dw;
        sky_w += dw;
      }
    }
  }
  const float sky_mean = sky_w > 0.0 ? static_cast<float>(sky_sum / sky_w) : 0.1f;
  // Sun counts only if the cone carries clearly more than the sky would.
  const float cone_solid = 2.0f * kPi * (1.0f - cone_cos);
  const float sky_in_cone = sky_mean * cone_solid;
  out.has_sun = luminance(sun_irr) > 6.0f * sky_in_cone && sun_dir_raw.y > 0.02f;
  Vec3 sun_irr_net = sun_irr - Vec3{sky_mean, sky_mean, sky_mean} * cone_solid;
  if (out.has_sun) {
    // Remove the sun from the map for IBL: clamp cone texels to the local
    // surroundings (ring just outside the cone).
    Vec3 ring{0, 0, 0};
    float ring_n = 0.0f;
    const float ring_cos = std::cos(radians(7.0f));
    for (int y = 0; y < env.h; ++y)
      for (int x = 0; x < env.w; ++x) {
        const Vec3 d = env.dir_of(x, y);
        const float dd = dot(d, sun_dir_raw);
        if (dd <= cone_cos && dd > ring_cos) {
          const float* p = &env.rgb[(static_cast<std::size_t>(y) * env.w + x) * 3];
          ring += Vec3{p[0], p[1], p[2]};
          ring_n += 1.0f;
        }
      }
    if (ring_n > 0) ring = ring * (1.0f / ring_n);
    for (int y = 0; y < env.h; ++y)
      for (int x = 0; x < env.w; ++x) {
        const Vec3 d = env.dir_of(x, y);
        if (dot(d, sun_dir_raw) > cone_cos) {
          float* p = &env.rgb[(static_cast<std::size_t>(y) * env.w + x) * 3];
          p[0] = std::min(p[0], ring.x);
          p[1] = std::min(p[1], ring.y);
          p[2] = std::min(p[2], ring.z);
        }
      }
  } else {
    sun_irr_net = Vec3{0, 0, 0};
  }
  out.sun_dir = rotate_y(sun_dir_raw, yaw);
  out.sun_color = Vec3{std::max(0.0f, sun_irr_net.x), std::max(0.0f, sun_irr_net.y), std::max(0.0f, sun_irr_net.z)};
  out.sky_luminance = sky_mean;
  // Exposure: a mid-grey surface lit by sun + sky lands near 0.4 pre-tonemap.
  const float ground_radiance = 0.35f * (luminance(out.sun_color) * 0.8f + kPi * sky_mean) / kPi;
  const float key = out.has_sun ? 0.42f : 0.022f;  // night: keep the scene dark, let emissives carry it
  out.exposure = ground_radiance > 1e-6f ? key / ground_radiance : 1.0f;
  if (verbose) {
    std::printf("  environment: sun %s dir (%.2f %.2f %.2f) irradiance %.1f, sky mean %.3f, exposure %.3f\n",
                out.has_sun ? "found" : "none", out.sun_dir.x, out.sun_dir.y, out.sun_dir.z,
                luminance(out.sun_color), sky_mean, out.exposure);
  }

  // ---- source mip pyramid (sun removed) for filtered importance sampling
  std::vector<Equirect> pyramid;
  pyramid.push_back(env);
  while (pyramid.back().w > 8) pyramid.push_back(pyramid.back().downsample());
  auto sample_lod = [&](Vec3 d, float lod) {
    lod = clampf(lod, 0.0f, static_cast<float>(pyramid.size() - 1));
    const int l0 = static_cast<int>(lod), l1 = std::min(l0 + 1, static_cast<int>(pyramid.size()) - 1);
    const float f = lod - static_cast<float>(l0);
    const Vec3 d_env = rotate_y(d, -yaw);
    return lerp(pyramid[static_cast<std::size_t>(l0)].sample_dir(d_env), pyramid[static_cast<std::size_t>(l1)].sample_dir(d_env), f);
  };

  // ---- background cube (with the sun: the original map) --------------
  // We re-add the sun for the visible sky by sampling the pre-removal data;
  // simplest: keep a copy before removal. (env was modified in place, so
  // rebuild from the sun cone: paint the sun back as a disc.)
  {
    std::uint32_t mips = 1;
    while ((bg_size >> mips) >= 1) ++mips;
    out.background = gpu.create_texture(bg_size, bg_size, WGPUTextureFormat_RGBA16Float,
                                        WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst, mips, 6, 1,
                                        "sky-cube", true);
    int size = static_cast<int>(bg_size);
    const float sun_disc_cos = std::cos(radians(0.53f));
    const float disc_solid = 2.0f * kPi * (1.0f - sun_disc_cos);
    const Vec3 sun_radiance = out.has_sun ? out.sun_color * (1.0f / disc_solid) : Vec3{0, 0, 0};
    for (std::uint32_t mip = 0; mip < mips; ++mip) {
      std::vector<Vec3> faces(static_cast<std::size_t>(6) * size * size);
      const float lod = std::log2(static_cast<float>(env.w) / (4.0f * static_cast<float>(size)));
      parallel_for(6 * size, [&](int row) {
        const int f = row / size, y = row % size;
        for (int x = 0; x < size; ++x) {
          const float s = 2.0f * (static_cast<float>(x) + 0.5f) / static_cast<float>(size) - 1.0f;
          const float t = 2.0f * (static_cast<float>(y) + 0.5f) / static_cast<float>(size) - 1.0f;
          const Vec3 d = face_dir(f, s, t);
          Vec3 c = sample_lod(d, std::max(0.0f, lod));
          if (!out.has_sun) {
            // night maps: stars are sub-texel points that would bloom into
            // blobs at cube resolution — cap them at a few times the sky level
            const float cap = std::max(sky_mean * 6.0f, 0.3f);
            c = Vec3{std::min(c.x, cap), std::min(c.y, cap), std::min(c.z, cap)};
          }
          if (out.has_sun && mip == 0) {
            const float dd = dot(d, out.sun_dir);
            if (dd > sun_disc_cos) c = sun_radiance;
            else if (dd > std::cos(radians(1.2f))) c += sun_radiance * 0.05f * smoothstep(std::cos(radians(1.2f)), sun_disc_cos, dd);
          }
          faces[(static_cast<std::size_t>(f) * size + y) * size + x] = c;
        }
      });
      upload_cube_level(gpu, out.background, static_cast<int>(mip), size, faces);
      size = std::max(1, size / 2);
    }
  }

  // ---- GGX prefiltered specular cube ------------------------------------
  {
    const std::uint32_t mips = 6;
    out.specular = gpu.create_texture(spec_size, spec_size, WGPUTextureFormat_RGBA16Float,
                                      WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst, mips, 6, 1,
                                      "specular-cube", true);
    int size = static_cast<int>(spec_size);
    const float src_texel_solid = 4.0f * kPi / (static_cast<float>(env.w) * static_cast<float>(env.h)) * 1.5f;
    for (std::uint32_t mip = 0; mip < mips; ++mip) {
      const float roughness = static_cast<float>(mip) / static_cast<float>(mips - 1);
      const float a = std::max(roughness * roughness, 1e-3f);
      const std::uint32_t samples = mip == 0 ? 1u : (mip == 1 ? 96u : 160u);
      std::vector<Vec3> faces(static_cast<std::size_t>(6) * size * size);
      parallel_for(6 * size, [&](int row) {
        const int f = row / size, y = row % size;
        for (int x = 0; x < size; ++x) {
          const float s = 2.0f * (static_cast<float>(x) + 0.5f) / static_cast<float>(size) - 1.0f;
          const float t = 2.0f * (static_cast<float>(y) + 0.5f) / static_cast<float>(size) - 1.0f;
          const Vec3 n = face_dir(f, s, t);
          Vec3 acc{0, 0, 0};
          float wsum = 0.0f;
          if (mip == 0) {
            acc = sample_lod(n, std::log2(static_cast<float>(env.w) / (4.0f * static_cast<float>(size))));
            wsum = 1.0f;
          } else {
            Vec3 tx, bx2;
            basis(n, tx, bx2);
            for (std::uint32_t i = 0; i < samples; ++i) {
              const Vec2 xi = hammersley(i, samples);
              const float phi = 2.0f * kPi * xi.x;
              const float cos_th = std::sqrt((1.0f - xi.y) / (1.0f + (a * a - 1.0f) * xi.y));
              const float sin_th = std::sqrt(std::max(0.0f, 1.0f - cos_th * cos_th));
              const Vec3 h = tx * (sin_th * std::cos(phi)) + bx2 * (sin_th * std::sin(phi)) + n * cos_th;
              const Vec3 l = h * (2.0f * dot(n, h)) - n;
              const float nl = dot(n, l);
              if (nl <= 0.0f) continue;
              // pdf of l = D(h) * (n.h) / (4 (v.h)), v = n
              const float nh = cos_th;
              const float d = a * a / (kPi * std::pow(nh * nh * (a * a - 1.0f) + 1.0f, 2.0f));
              const float pdf = d * nh / (4.0f * nh) + 1e-5f;
              const float sample_solid = 1.0f / (static_cast<float>(samples) * pdf);
              const float lod = 0.5f * std::log2(sample_solid / src_texel_solid) + 1.0f;
              acc += sample_lod(l, lod) * nl;
              wsum += nl;
            }
          }
          faces[(static_cast<std::size_t>(f) * size + y) * size + x] = wsum > 0 ? acc * (1.0f / wsum) : Vec3{0, 0, 0};
        }
      });
      upload_cube_level(gpu, out.specular, static_cast<int>(mip), size, faces);
      size = std::max(1, size / 2);
    }
  }

  // ---- SH9 irradiance -----------------------------------------------------
  {
    const Equirect& src = pyramid[std::min<std::size_t>(2, pyramid.size() - 1)];
    double sh[9][3] = {};
    for (int y = 0; y < src.h; ++y) {
      const float dw = src.solid_angle(y);
      for (int x = 0; x < src.w; ++x) {
        const Vec3 d = rotate_y(src.dir_of(x, y), yaw);
        const float* p = &src.rgb[(static_cast<std::size_t>(y) * src.w + x) * 3];
        const float basis_fn[9] = {0.282095f, 0.488603f * d.y, 0.488603f * d.z, 0.488603f * d.x,
                                   1.092548f * d.x * d.y, 1.092548f * d.y * d.z, 0.315392f * (3.0f * d.z * d.z - 1.0f),
                                   1.092548f * d.x * d.z, 0.546274f * (d.x * d.x - d.y * d.y)};
        for (int i = 0; i < 9; ++i)
          for (int c = 0; c < 3; ++c) sh[i][c] += static_cast<double>(basis_fn[i] * p[c] * dw);
      }
    }
    const float A[9] = {kPi, 2.0f * kPi / 3.0f, 2.0f * kPi / 3.0f, 2.0f * kPi / 3.0f, kPi / 4.0f, kPi / 4.0f, kPi / 4.0f,
                        kPi / 4.0f, kPi / 4.0f};
    for (int i = 0; i < 9; ++i)
      for (int c = 0; c < 3; ++c) out.sh[i][c] = static_cast<float>(sh[i][c]) * A[i];
  }
  out.ok = true;
  return out;
}

}  // namespace

Environment load_environment(Gpu& gpu, const std::string& path, float yaw, std::uint32_t bg_size,
                             std::uint32_t spec_size, bool verbose) {
  Equirect env;
  int n = 0;
  float* data = stbi_loadf(path.c_str(), &env.w, &env.h, &n, 3);
  if (data == nullptr) {
    Environment e;
    e.ok = false;
    return e;
  }
  env.rgb.assign(data, data + static_cast<std::size_t>(env.w) * env.h * 3);
  stbi_image_free(data);
  // Cap the working resolution at 2048 wide for speed.
  while (env.w > 2048) env = env.downsample();
  return build_environment(gpu, std::move(env), yaw, bg_size, spec_size, verbose);
}

Environment make_analytic_environment(Gpu& gpu, Vec3 sun_dir, std::uint32_t bg_size, std::uint32_t spec_size) {
  Equirect env;
  env.w = 512;
  env.h = 256;
  env.rgb.resize(static_cast<std::size_t>(env.w) * env.h * 3);
  sun_dir = normalize(sun_dir);
  for (int y = 0; y < env.h; ++y)
    for (int x = 0; x < env.w; ++x) {
      const Vec3 d = env.dir_of(x, y);
      const float up = clampf(d.y, -1.0f, 1.0f);
      Vec3 sky = lerp(Vec3{0.75f, 0.82f, 0.95f}, Vec3{0.18f, 0.32f, 0.75f}, std::pow(std::max(up, 0.0f), 0.5f)) * 1.2f;
      if (up < 0.0f) sky = Vec3{0.25f, 0.23f, 0.2f} * (0.6f + 0.4f * (1.0f + up));
      const float dd = dot(d, sun_dir);
      sky += Vec3{1.0f, 0.9f, 0.7f} * (0.35f * std::pow(std::max(dd, 0.0f), 8.0f));
      if (dd > std::cos(radians(0.53f))) sky = Vec3{60000.0f, 55000.0f, 48000.0f};
      float* p = &env.rgb[(static_cast<std::size_t>(y) * env.w + x) * 3];
      p[0] = sky.x;
      p[1] = sky.y;
      p[2] = sky.z;
    }
  return build_environment(gpu, std::move(env), 0.0f, bg_size, spec_size, false);
}

}  // namespace cb
