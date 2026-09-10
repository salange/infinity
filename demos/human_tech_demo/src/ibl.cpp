#include "ibl.hpp"

#include <stb_image.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <functional>
#include <thread>

namespace cb {

namespace {

struct Equirect {
  std::vector<float> rgb; // w*h*3, linear
  int w{0}, h{0};
  Vec3 sample_dir(Vec3 d) const { // nearest-ish bilinear
    const float u = std::atan2(d.x, -d.z) / (2.0f * kPi) + 0.5f;
    const float v = std::acos(clampf(d.y, -1.0f, 1.0f)) / kPi;
    return sample_uv(u, v);
  }
  Vec3 sample_uv(float u, float v) const {
    float x = u * static_cast<float>(w) - 0.5f,
          y = v * static_cast<float>(h) - 0.5f;
    x = std::fmod(x + static_cast<float>(w) * 4.0f, static_cast<float>(w));
    y = clampf(y, 0.0f, static_cast<float>(h - 1));
    const int x0 = static_cast<int>(x), y0 = static_cast<int>(y);
    const int x1 = (x0 + 1) % w, y1 = std::min(y0 + 1, h - 1);
    const float fx = x - static_cast<float>(x0),
                fy = y - static_cast<float>(y0);
    auto at = [&](int xi, int yi) {
      const float *p = &rgb[(static_cast<std::size_t>(yi) * w + xi) * 3];
      return Vec3{p[0], p[1], p[2]};
    };
    return lerp(lerp(at(x0, y0), at(x1, y0), fx),
                lerp(at(x0, y1), at(x1, y1), fx), fy);
  }
  Vec3 dir_of(int x, int y) const {
    const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(w);
    const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(h);
    const float phi = (u - 0.5f) * 2.0f * kPi, theta = v * kPi;
    return Vec3{std::sin(theta) * std::sin(phi), std::cos(theta),
                -std::sin(theta) * std::cos(phi)};
  }
  float solid_angle(int y) const {
    const float theta =
        (static_cast<float>(y) + 0.5f) / static_cast<float>(h) * kPi;
    return std::sin(theta) * (kPi / static_cast<float>(h)) *
           (2.0f * kPi / static_cast<float>(w));
  }
  Equirect downsample() const {
    Equirect o;
    o.w = std::max(1, w / 2);
    o.h = std::max(1, h / 2);
    o.rgb.resize(static_cast<std::size_t>(o.w) * o.h * 3);
    for (int y = 0; y < o.h; ++y)
      for (int x = 0; x < o.w; ++x)
        for (int c = 0; c < 3; ++c) {
          const int x0 = std::min(2 * x, w - 1),
                    x1 = std::min(2 * x + 1, w - 1);
          const int y0 = std::min(2 * y, h - 1),
                    y1 = std::min(2 * y + 1, h - 1);
          o.rgb[(static_cast<std::size_t>(y) * o.w + x) * 3 + c] =
              0.25f * (rgb[(static_cast<std::size_t>(y0) * w + x0) * 3 + c] +
                       rgb[(static_cast<std::size_t>(y0) * w + x1) * 3 + c] +
                       rgb[(static_cast<std::size_t>(y1) * w + x0) * 3 + c] +
                       rgb[(static_cast<std::size_t>(y1) * w + x1) * 3 + c]);
        }
    return o;
  }
};

float luminance(Vec3 c) {
  return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z;
}

// Standard cubemap face directions (+X,-X,+Y,-Y,+Z,-Z), s,t in [-1,1], t down.
Vec3 face_dir(int face, float s, float t) {
  switch (face) {
  case 0:
    return normalize(Vec3{1, -t, -s});
  case 1:
    return normalize(Vec3{-1, -t, s});
  case 2:
    return normalize(Vec3{s, 1, t});
  case 3:
    return normalize(Vec3{s, -1, -t});
  case 4:
    return normalize(Vec3{s, -t, 1});
  default:
    return normalize(Vec3{-s, -t, -1});
  }
}

Vec3 rotate_y(Vec3 d, float yaw) {
  const float c = std::cos(yaw), s = std::sin(yaw);
  return Vec3{d.x * c + d.z * s, d.y, -d.x * s + d.z * c};
}

void parallel_for(int n, const std::function<void(int)> &fn) {
  const int threads =
      std::max(1, static_cast<int>(std::thread::hardware_concurrency()));
  std::vector<std::thread> pool;
  std::atomic<int> next{0};
  for (int t = 0; t < threads; ++t) {
    pool.emplace_back([&] {
      for (int i = next++; i < n; i = next++)
        fn(i);
    });
  }
  for (auto &th : pool)
    th.join();
}

// Hammersley point i of n.
Vec2 hammersley(std::uint32_t i, std::uint32_t n) {
  std::uint32_t bits = i;
  bits = (bits << 16u) | (bits >> 16u);
  bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
  bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
  bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
  bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
  return Vec2{static_cast<float>(i) / static_cast<float>(n),
              static_cast<float>(bits) * 2.3283064365386963e-10f};
}

// Uploads a float RGB cube face set as RGBA16F.
void upload_cube_level(Gpu &gpu, const Texture &tex, int mip, int size,
                       const std::vector<Vec3> &faces) {
  std::vector<std::uint16_t> half(static_cast<std::size_t>(size) * size * 4);
  for (int f = 0; f < 6; ++f) {
    for (int i = 0; i < size * size; ++i) {
      const Vec3 c = faces[static_cast<std::size_t>(f) * size * size + i];
      half[i * 4 + 0] = float_to_half(c.x);
      half[i * 4 + 1] = float_to_half(c.y);
      half[i * 4 + 2] = float_to_half(c.z);
      half[i * 4 + 3] = float_to_half(1.0f);
    }
    gpu.upload_level(tex, static_cast<std::uint32_t>(f),
                     static_cast<std::uint32_t>(mip),
                     static_cast<std::uint32_t>(size),
                     static_cast<std::uint32_t>(size), half.data(), 8);
  }
}

Environment build_environment(Gpu &gpu, Equirect env, float yaw,
                              std::uint32_t bg_size, std::uint32_t spec_size,
                              bool verbose, bool extract_sun = true) {
  Environment out;
  // ---- sun extraction --------------------------------------------------
  // Brightest texel of a 4x-downsampled copy, then integrate the radiance
  // within a small cone around it that exceeds 20% of the peak.
  Equirect small = env.downsample().downsample();
  int bx = 0, by = 0;
  float best = -1.0f;
  for (int y = 0; y < small.h; ++y)
    for (int x = 0; x < small.w; ++x) {
      const float *p =
          &small.rgb[(static_cast<std::size_t>(y) * small.w + x) * 3];
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
      const float *p = &env.rgb[(static_cast<std::size_t>(y) * env.w + x) * 3];
      const Vec3 c{p[0], p[1], p[2]};
      if (dot(d, sun_dir_raw) > cone_cos) {
        sun_irr += c * dw;
      } else if (d.y > 0.0f) {
        sky_sum += static_cast<double>(luminance(c)) * dw;
        sky_w += dw;
      }
    }
  }
  const float sky_mean =
      sky_w > 0.0 ? static_cast<float>(sky_sum / sky_w) : 0.1f;
  // Sun counts only if the cone carries clearly more than the sky would.
  const float cone_solid = 2.0f * kPi * (1.0f - cone_cos);
  const float sky_in_cone = sky_mean * cone_solid;
  out.has_sun = extract_sun && luminance(sun_irr) > 6.0f * sky_in_cone &&
                sun_dir_raw.y > 0.02f;
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
          const float *p =
              &env.rgb[(static_cast<std::size_t>(y) * env.w + x) * 3];
          ring += Vec3{p[0], p[1], p[2]};
          ring_n += 1.0f;
        }
      }
    if (ring_n > 0)
      ring = ring * (1.0f / ring_n);
    for (int y = 0; y < env.h; ++y)
      for (int x = 0; x < env.w; ++x) {
        const Vec3 d = env.dir_of(x, y);
        if (dot(d, sun_dir_raw) > cone_cos) {
          float *p = &env.rgb[(static_cast<std::size_t>(y) * env.w + x) * 3];
          p[0] = std::min(p[0], ring.x);
          p[1] = std::min(p[1], ring.y);
          p[2] = std::min(p[2], ring.z);
        }
      }
  } else {
    sun_irr_net = Vec3{0, 0, 0};
  }
  out.sun_dir = rotate_y(sun_dir_raw, yaw);
  out.sun_color =
      Vec3{std::max(0.0f, sun_irr_net.x), std::max(0.0f, sun_irr_net.y),
           std::max(0.0f, sun_irr_net.z)};
  out.sky_luminance = sky_mean;
  // Exposure: a mid-grey surface lit by sun + sky lands near 0.4 pre-tonemap.
  const float ground_radiance =
      0.35f * (luminance(out.sun_color) * 0.8f + kPi * sky_mean) / kPi;
  const float key =
      out.has_sun
          ? 0.42f
          : 0.022f; // night: keep the scene dark, let emissives carry it
  out.exposure = ground_radiance > 1e-6f ? key / ground_radiance : 1.0f;
  if (verbose) {
    std::printf("  environment: sun %s dir (%.2f %.2f %.2f) irradiance %.1f, "
                "sky mean %.3f, exposure %.3f\n",
                out.has_sun ? "found" : "none", out.sun_dir.x, out.sun_dir.y,
                out.sun_dir.z, luminance(out.sun_color), sky_mean,
                out.exposure);
  }

  // ---- source mip pyramid (sun removed) for filtered importance sampling
  std::vector<Equirect> pyramid;
  pyramid.push_back(env);
  while (pyramid.back().w > 8)
    pyramid.push_back(pyramid.back().downsample());
  auto sample_lod = [&](Vec3 d, float lod) {
    lod = clampf(lod, 0.0f, static_cast<float>(pyramid.size() - 1));
    const int l0 = static_cast<int>(lod),
              l1 = std::min(l0 + 1, static_cast<int>(pyramid.size()) - 1);
    const float f = lod - static_cast<float>(l0);
    const Vec3 d_env = rotate_y(d, -yaw);
    return lerp(pyramid[static_cast<std::size_t>(l0)].sample_dir(d_env),
                pyramid[static_cast<std::size_t>(l1)].sample_dir(d_env), f);
  };

  // ---- background cube (with the sun: the original map) --------------
  // We re-add the sun for the visible sky by sampling the pre-removal data;
  // simplest: keep a copy before removal. (env was modified in place, so
  // rebuild from the sun cone: paint the sun back as a disc.)
  {
    std::uint32_t mips = 1;
    while ((bg_size >> mips) >= 1)
      ++mips;
    out.background = gpu.create_texture(
        bg_size, bg_size, WGPUTextureFormat_RGBA16Float,
        WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst, mips, 6, 1,
        "sky-cube", true);
    int size = static_cast<int>(bg_size);
    const float sun_disc_cos = std::cos(radians(0.53f));
    const float disc_solid = 2.0f * kPi * (1.0f - sun_disc_cos);
    const Vec3 sun_radiance =
        out.has_sun ? out.sun_color * (1.0f / disc_solid) : Vec3{0, 0, 0};
    for (std::uint32_t mip = 0; mip < mips; ++mip) {
      std::vector<Vec3> faces(static_cast<std::size_t>(6) * size * size);
      const float lod = std::log2(static_cast<float>(env.w) /
                                  (4.0f * static_cast<float>(size)));
      parallel_for(6 * size, [&](int row) {
        const int f = row / size, y = row % size;
        for (int x = 0; x < size; ++x) {
          const float s =
              2.0f * (static_cast<float>(x) + 0.5f) / static_cast<float>(size) -
              1.0f;
          const float t =
              2.0f * (static_cast<float>(y) + 0.5f) / static_cast<float>(size) -
              1.0f;
          const Vec3 d = face_dir(f, s, t);
          Vec3 c = sample_lod(d, std::max(0.0f, lod));
          if (out.has_sun && mip == 0) {
            const float dd = dot(d, out.sun_dir);
            if (dd > sun_disc_cos)
              c = sun_radiance;
            else if (dd > std::cos(radians(1.2f)))
              c += sun_radiance * 0.05f *
                   smoothstep(std::cos(radians(1.2f)), sun_disc_cos, dd);
          }
          faces[(static_cast<std::size_t>(f) * size + y) * size + x] = c;
        }
      });
      upload_cube_level(gpu, out.background, static_cast<int>(mip), size,
                        faces);
      size = std::max(1, size / 2);
    }
  }

  // ---- GGX prefiltered specular cube ------------------------------------
  {
    const std::uint32_t mips = 6;
    out.specular = gpu.create_texture(
        spec_size, spec_size, WGPUTextureFormat_RGBA16Float,
        WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst, mips, 6, 1,
        "specular-cube", true);
    int size = static_cast<int>(spec_size);
    const float src_texel_solid =
        4.0f * kPi / (static_cast<float>(env.w) * static_cast<float>(env.h)) *
        1.5f;
    for (std::uint32_t mip = 0; mip < mips; ++mip) {
      const float roughness =
          static_cast<float>(mip) / static_cast<float>(mips - 1);
      const float a = std::max(roughness * roughness, 1e-3f);
      const std::uint32_t samples = mip == 0 ? 1u : (mip == 1 ? 96u : 160u);
      std::vector<Vec3> faces(static_cast<std::size_t>(6) * size * size);
      parallel_for(6 * size, [&](int row) {
        const int f = row / size, y = row % size;
        for (int x = 0; x < size; ++x) {
          const float s =
              2.0f * (static_cast<float>(x) + 0.5f) / static_cast<float>(size) -
              1.0f;
          const float t =
              2.0f * (static_cast<float>(y) + 0.5f) / static_cast<float>(size) -
              1.0f;
          const Vec3 n = face_dir(f, s, t);
          Vec3 acc{0, 0, 0};
          float wsum = 0.0f;
          if (mip == 0) {
            acc = sample_lod(n, std::log2(static_cast<float>(env.w) /
                                          (4.0f * static_cast<float>(size))));
            wsum = 1.0f;
          } else {
            Vec3 tx, bx2;
            basis(n, tx, bx2);
            for (std::uint32_t i = 0; i < samples; ++i) {
              const Vec2 xi = hammersley(i, samples);
              const float phi = 2.0f * kPi * xi.x;
              const float cos_th =
                  std::sqrt((1.0f - xi.y) / (1.0f + (a * a - 1.0f) * xi.y));
              const float sin_th =
                  std::sqrt(std::max(0.0f, 1.0f - cos_th * cos_th));
              const Vec3 h = tx * (sin_th * std::cos(phi)) +
                             bx2 * (sin_th * std::sin(phi)) + n * cos_th;
              const Vec3 l = h * (2.0f * dot(n, h)) - n;
              const float nl = dot(n, l);
              if (nl <= 0.0f)
                continue;
              // pdf of l = D(h) * (n.h) / (4 (v.h)), v = n
              const float nh = cos_th;
              const float d =
                  a * a /
                  (kPi * std::pow(nh * nh * (a * a - 1.0f) + 1.0f, 2.0f));
              const float pdf = d * nh / (4.0f * nh) + 1e-5f;
              const float sample_solid =
                  1.0f / (static_cast<float>(samples) * pdf);
              const float lod =
                  0.5f * std::log2(sample_solid / src_texel_solid) + 1.0f;
              acc += sample_lod(l, lod) * nl;
              wsum += nl;
            }
          }
          faces[(static_cast<std::size_t>(f) * size + y) * size + x] =
              wsum > 0 ? acc * (1.0f / wsum) : Vec3{0, 0, 0};
        }
      });
      upload_cube_level(gpu, out.specular, static_cast<int>(mip), size, faces);
      size = std::max(1, size / 2);
    }
  }

  // ---- SH9 irradiance -----------------------------------------------------
  {
    const Equirect &src = pyramid[std::min<std::size_t>(2, pyramid.size() - 1)];
    double sh[9][3] = {};
    for (int y = 0; y < src.h; ++y) {
      const float dw = src.solid_angle(y);
      for (int x = 0; x < src.w; ++x) {
        const Vec3 d = rotate_y(src.dir_of(x, y), yaw);
        const float *p =
            &src.rgb[(static_cast<std::size_t>(y) * src.w + x) * 3];
        const float basis_fn[9] = {0.282095f,
                                   0.488603f * d.y,
                                   0.488603f * d.z,
                                   0.488603f * d.x,
                                   1.092548f * d.x * d.y,
                                   1.092548f * d.y * d.z,
                                   0.315392f * (3.0f * d.z * d.z - 1.0f),
                                   1.092548f * d.x * d.z,
                                   0.546274f * (d.x * d.x - d.y * d.y)};
        for (int i = 0; i < 9; ++i)
          for (int c = 0; c < 3; ++c)
            sh[i][c] += static_cast<double>(basis_fn[i] * p[c] * dw);
      }
    }
    const float A[9] = {
        kPi,        2.0f * kPi / 3.0f, 2.0f * kPi / 3.0f, 2.0f * kPi / 3.0f,
        kPi / 4.0f, kPi / 4.0f,        kPi / 4.0f,        kPi / 4.0f,
        kPi / 4.0f};
    for (int i = 0; i < 9; ++i)
      for (int c = 0; c < 3; ++c)
        out.sh[i][c] = static_cast<float>(sh[i][c]) * A[i];
  }
  out.ok = true;
  return out;
}

} // namespace

Environment load_environment(Gpu &gpu, const std::string &path, float yaw,
                             std::uint32_t bg_size, std::uint32_t spec_size,
                             bool verbose, bool extract_sun) {
  Equirect env;
  int n = 0;
  float *data = stbi_loadf(path.c_str(), &env.w, &env.h, &n, 3);
  if (data == nullptr) {
    Environment e;
    e.ok = false;
    return e;
  }
  env.rgb.assign(data, data + static_cast<std::size_t>(env.w) * env.h * 3);
  stbi_image_free(data);
  // Cap the working resolution at 2048 wide for speed.
  while (env.w > 2048)
    env = env.downsample();
  return build_environment(gpu, std::move(env), yaw, bg_size, spec_size,
                           verbose,
                           extract_sun && stbi_is_hdr(path.c_str()) != 0);
}

Environment load_perspective_environment(Gpu &gpu, const std::string &path,
                                         Vec3 forward, float fov_y,
                                         float aspect, std::uint32_t bg_size,
                                         std::uint32_t spec_size, bool night,
                                         const std::string &cloud_path,
                                         float cloud_yaw) {
  int width = 0, height = 0, channels = 0;
  stbi_uc *image = stbi_load(path.c_str(), &width, &height, &channels, 3);
  if (!image)
    return Environment{};
  Equirect cloud;
  if (!cloud_path.empty()) {
    int source_channels = 0;
    stbi_uc *source = stbi_load(cloud_path.c_str(), &cloud.w, &cloud.h,
                               &source_channels, 3);
    if (!source || cloud.w < 2 || cloud.h < 2) {
      std::fprintf(stderr, "Required full-sky cloud source is invalid: %s\n",
                   cloud_path.c_str());
      stbi_image_free(source);
      stbi_image_free(image);
      return Environment{};
    }
    cloud.rgb.resize(std::size_t(cloud.w) * cloud.h * 3);
    for (std::size_t i = 0; i < cloud.rgb.size(); ++i) {
      const float value = float(source[i]) / 255.f;
      cloud.rgb[i] = value <= .04045f
                         ? value / 12.92f
                         : std::pow((value + .055f) / 1.055f, 2.4f);
    }
    stbi_image_free(source);
  }
  const Vec3 f = normalize(forward), right = normalize(cross(f, Vec3{0, 1, 0})),
             up = cross(right, f);
  Vec3 cloud_pole{};
  for (int x = 0; x < cloud.w; ++x)
    cloud_pole += Vec3{cloud.rgb[std::size_t(x) * 3],
                       cloud.rgb[std::size_t(x) * 3 + 1],
                       cloud.rgb[std::size_t(x) * 3 + 2]} * (1.f / cloud.w);
  auto sample_cloud = [&](Vec3 direction) {
    const Vec3 d = rotate_y(direction, cloud_yaw);
    const float u = std::atan2(d.x, -d.z) / (2.f * kPi) + .5f;
    const float v = std::acos(clampf(d.y, -1.f, 1.f)) / kPi;
    Vec3 color = cloud.sample_uv(u, v);
    // Spherical sampling joins the source edges across two degrees and makes
    // the top five degrees converge to one pole. Source pixels stay immutable.
    float seam = clampf(std::min(u, 1.f - u) * 180.f, 0.f, 1.f);
    seam = seam * seam * (3.f - 2.f * seam);
    color = lerp(color, cloud.sample_uv(1.f - u, v), .5f * (1.f - seam));
    float pole = clampf(v * 36.f, 0.f, 1.f);
    pole = pole * pole * (3.f - 2.f * pole);
    return lerp(cloud_pole, color, pole);
  };
  const float ty = std::tan(fov_y * .5f), tx = ty * aspect;
  Equirect env;
  env.w = std::max(4096, int(bg_size) * 4);
  env.h = env.w / 2;
  env.rgb.resize(std::size_t(env.w) * env.h * 3);
  auto sample = [&](float u, float v) {
    const float x = clampf(u, 0.f, 1.f) * (width - 1),
                y = clampf(v, 0.f, 1.f) * (height - 1);
    const int ix = int(x), iy = int(y), jx = std::min(ix + 1, width - 1),
              jy = std::min(iy + 1, height - 1);
    auto pixel = [&](int px, int py) {
      const auto i = (std::size_t(py) * width + px) * 3;
      auto linear = [](stbi_uc value) {
        const float x = float(value) / 255.f;
        return x <= .04045f ? x / 12.92f : std::pow((x + .055f) / 1.055f, 2.4f);
      };
      return Vec3{linear(image[i]), linear(image[i + 1]), linear(image[i + 2])};
    };
    return lerp(lerp(pixel(ix, iy), pixel(jx, iy), x - ix),
                lerp(pixel(ix, jy), pixel(jx, jy), x - ix), y - iy);
  };
  parallel_for(env.h, [&](int y) {
    for (int x = 0; x < env.w; ++x) {
      const Vec3 d = env.dir_of(x, y);
      Vec3 color = lerp(Vec3{.010f, .020f, .040f}, Vec3{.002f, .005f, .014f},
                        std::sqrt(std::max(d.y, 0.f)));
      if (d.y < 0)
        color = {.006f, .012f, .023f};
      if (!night) {
        color=lerp(Vec3{.25f,.28f,.31f},Vec3{.13f,.19f,.28f},std::sqrt(std::max(d.y,0.f)));
        if(d.y<0)color={.20f,.23f,.25f};
      }
      if (cloud.w && d.y > 0.f) {
        // The real city supplies the ground. Fade the cloud sphere into the
        // existing horizon across six degrees, without changing its lower half.
        float horizon = clampf(d.y / std::sin(radians(6.f)), 0.f, 1.f);
        horizon = horizon * horizon * (3.f - 2.f * horizon);
        color = lerp(color, sample_cloud(d), horizon);
      }
      const float z = dot(d, f);
      if (z > 0) {
        const float px = dot(d, right) / (z * tx), py = dot(d, up) / (z * ty);
        const float border = std::max(std::abs(px), std::abs(py));
        if (border < 1.45f) {
          // Preserve the complete authored field. Only its exterior dissolves
          // into the surrounding hemisphere; a narrow camera shift cannot reveal
          // an inset dark rectangular border.
          float edge = clampf((1.45f - border) / .45f, 0.f, 1.f);
          edge = edge * edge * (3 - 2 * edge);
          Vec3 patch = sample(px * .5f + .5f, .5f - py * .5f);
          if (border > 1) {
            Vec3 blurred{};
            const float radius = std::min((border - 1) * .35f, .10f);
            for (int j = 0; j < 12; ++j) {
              const float angle = j * (2 * kPi / 12);
              blurred += sample(px * .5f + .5f + std::cos(angle) * radius,
                                .5f - py * .5f + std::sin(angle) * radius);
            }
            patch = blurred * (1.f / 12);
          }
          color = lerp(color, patch, edge);
        }
      }
      const auto i = (std::size_t(y) * env.w + x) * 3;
      env.rgb[i] = color.x;
      env.rgb[i + 1] = color.y;
      env.rgb[i + 2] = color.z;
    }
  });
  stbi_image_free(image);
  if (cloud.w)
    std::printf("  full-sky clouds: %dx%d, fixed yaw %.1f deg, exact sRGB "
                "relative radiance; analytic ground retained\n",
                cloud.w, cloud.h, cloud_yaw * 180.f / kPi);
  std::printf("  environment: fixed directional plate, %.1f x %.1f degree "
              "field, %dx%d source; no solar extraction\n",
              2 * std::atan(tx) * 180 / kPi, fov_y * 180 / kPi, width, height);
  Environment result=build_environment(gpu, std::move(env), 0, bg_size, spec_size, false, false);
  result.display_referred_background = true;
  result.background_contains_atmosphere = true;
  if(!night){result.sun_dir=normalize(Vec3{.65f,.18f,-.4f});result.sun_color={1.8f,1.55f,1.25f};result.has_sun=true;result.exposure=1.f;}
  return result;
}

namespace {
float sky_hash(int x, int y) {
  std::uint32_t h = static_cast<std::uint32_t>(x) * 374761393u +
                    static_cast<std::uint32_t>(y) * 668265263u;
  h = (h ^ (h >> 13)) * 1274126177u;
  return static_cast<float>((h ^ (h >> 16)) & 0xffffffu) / 16777216.0f;
}
float sky_noise(float x, float y) {
  int ix = static_cast<int>(std::floor(x)),
      iy = static_cast<int>(std::floor(y));
  float fx = x - ix, fy = y - iy;
  fx = fx * fx * (3 - 2 * fx);
  fy = fy * fy * (3 - 2 * fy);
  return (sky_hash(ix, iy) * (1 - fx) + sky_hash(ix + 1, iy) * fx) * (1 - fy) +
         (sky_hash(ix, iy + 1) * (1 - fx) + sky_hash(ix + 1, iy + 1) * fx) * fy;
}
float sky_fbm(float x, float y) {
  float v = 0, a = 0.55f;
  for (int i = 0; i < 5; ++i) {
    v += a * sky_noise(x, y);
    x = x * 2.03f + 17;
    y = y * 2.03f - 11;
    a *= 0.48f;
  }
  return v;
}
} // namespace

Environment make_overcast_environment(Gpu& gpu,std::uint32_t bg_size,std::uint32_t spec_size) {
  Equirect env;env.w=2048;env.h=1024;env.rgb.resize(std::size_t(env.w)*env.h*3);
  parallel_for(env.h,[&](int y){for(int x=0;x<env.w;++x){
    const Vec3 d=env.dir_of(x,y);
    const float elevation=std::max(d.y,0.f);
    // CIE-style overcast distribution gives broad, neutral diffuse light.
    const float luminance=(1+2*elevation)/3;
    const float clouds=sky_fbm(d.x*3.2f/(elevation+.4f),d.z*3.2f/(elevation+.4f));
    Vec3 color=lerp(Vec3{.44f,.49f,.54f},Vec3{.57f,.64f,.74f},elevation)*
               (luminance*(.76f+.38f*clouds));
    if(d.y<0)color=Vec3{.13f,.15f,.17f};
    const auto i=(std::size_t(y)*env.w+x)*3;env.rgb[i]=color.x;env.rgb[i+1]=color.y;env.rgb[i+2]=color.z;
  }});
  auto result=build_environment(gpu,std::move(env),0,bg_size,spec_size,false,false);
  result.sun_dir=normalize(Vec3{.35f,.75f,-.32f});result.sun_color={.22f,.225f,.23f};
  result.has_sun=true;result.exposure=1.25f;result.lighting_scale=1.8f;
  result.background_contains_atmosphere=true;
  return result;
}

Environment make_analytic_environment(Gpu &gpu, Vec3 sun_dir,
                                      std::uint32_t bg_size,
                                      std::uint32_t spec_size) {
  Equirect env;
  env.w = 2048;
  env.h = 1024;
  env.rgb.resize(static_cast<std::size_t>(env.w) * env.h * 3);
  const bool dusk = sun_dir.y < .15f;
  sun_dir = normalize(sun_dir);
  const Vec3 band_n = normalize(Vec3{.30f, .87f, -.39f});
  const Vec3 moon = normalize(Vec3{.04f, .30f, .95f});
  parallel_for(env.h, [&](int y) {
    for (int x = 0; x < env.w; ++x) {
      const Vec3 d = env.dir_of(x, y);
      const float up = std::max(d.y, 0.f),
                  sun_dot = std::max(dot(d, sun_dir), 0.f);
      const float glow = std::pow(sun_dot, 24.f);
      Vec3 sky = lerp(Vec3{.72f, .66f, .56f}, Vec3{.065f, .16f, .31f},
                      std::pow(up, .42f));
      sky += Vec3{1.7f, .68f, .20f} * (glow * std::exp(-up * 1.3f));
      const float px = d.x / (up + .14f), pz = d.z / (up + .14f);
      // Three separate cloud decks: a ragged storm front, illuminated cumulus
      // edge and fine cirrus. Domain warping gives curled edges, not noise
      // tiles.
      const float warp = sky_fbm(px * .7f + 4, pz * .7f - 6);
      const float broad =
          sky_fbm(px * 1.55f + warp * 2.2f, pz * 1.55f - warp * 1.8f);
      const float detail = sky_fbm(px * 5.7f - 3, pz * 5.7f + 9);
      const float storm_side = clampf(.55f - d.x * .55f + d.z * .22f, 0.f, 1.f);
      const float threshold = .43f - storm_side * .065f + glow * .13f;
      float cloud =
          clampf((broad + detail * .13f - threshold) * 6.8f, 0.f, 1.f);
      cloud *= clampf(up * 15.f, 0.f, 1.f);
      const float rim = clampf((sky_fbm(px * 1.55f + warp * 2.2f + .14f,
                                        pz * 1.55f - warp * 1.8f - .09f) -
                                broad) *
                                       12.f +
                                   .25f,
                               0.f, 1.f);
      Vec3 cloud_colour =
          lerp(Vec3{.105f, .145f, .205f}, Vec3{.53f, .60f, .67f},
               clampf(detail * .9f + rim * .45f, 0.f, 1.f));
      cloud_colour += Vec3{1.6f, .77f, .30f} * (rim * std::pow(sun_dot, 4.f));
      cloud_colour =
          lerp(cloud_colour, Vec3{1.8f, 1.28f, .75f}, glow * rim * .7f);
      sky = lerp(sky, cloud_colour, cloud * .96f);
      const float cirrus =
          std::pow(clampf(sky_fbm(px * .8f, pz * 6.f) - .4f, 0.f, 1.f), 1.5f) *
          .22f;
      sky += Vec3{.33f, .36f, .40f} * (cirrus * (1 - cloud));
      if (dusk) {
        sky = lerp(Vec3{.018f, .032f, .055f}, Vec3{.0028f, .006f, .014f},
                   std::pow(up, .34f));
        const float latitude = dot(d, band_n);
        const float along = std::atan2(d.z, d.x);
        const float fine = sky_fbm(along * 13.f, latitude * 29.f);
        const float clumps =
            sky_fbm(along * 4.1f + fine * .7f, latitude * 12.f);
        const float glow_band = std::exp(-latitude * latitude / .013f);
        const float diffuse_band = std::exp(-latitude * latitude / .065f);
        const float dust =
            clampf((sky_fbm(along * 9.f + 2, latitude * 35.f - 4) - .41f) * 5.f,
                   0.f, 1.f);
        const float dust_lane = std::exp(-std::pow(
            (latitude + .035f + .028f * std::sin(along * 7.f)) / .045f, 2.f));
        const float stars_cloud = glow_band * (.24f + .9f * clumps) *
                                  (.3f + .7f * fine) *
                                  (1 - .85f * dust * dust_lane);
        sky += Vec3{.70f, .61f, .47f} * stars_cloud +
               Vec3{.022f, .028f, .047f} * diffuse_band;
        sky += Vec3{.035f, .010f, .021f} *
               (glow_band * std::pow(clampf(fine - .4f, 0.f, 1.f), 2.f));
        // Deterministic directional stars. The angular lattice is never tied to
        // the screen, so each point stays fixed under camera movement.
        const float az = std::atan2(d.z, d.x),
                    el = std::asin(clampf(d.y, -1, 1));
        const int sx = int(std::floor((az + kPi) * 590)),
                  sy = int(std::floor((el + kPi * .5f) * 590));
        const float h = sky_hash(sx, sy);
        if (h > .9975f && up > .04f) {
          const float fx = (az + kPi) * 590 - sx,
                      fy = (el + kPi * .5f) * 590 - sy;
          const float ox = sky_hash(sx + 17, sy - 9),
                      oy = sky_hash(sx - 4, sy + 21);
          const float star =
              std::exp(-((fx - ox) * (fx - ox) + (fy - oy) * (fy - oy)) *
                       38.f) *
              (.15f + 2.f * std::pow((h - .9975f) / .0025f, 4.f));
          sky += lerp(Vec3{.66f, .79f, 1.f}, Vec3{1.f, .75f, .49f},
                      sky_hash(sx + 4, sy)) *
                 star;
        }
        // The sky pass draws a geometric crescent from the same moon direction.
        const Vec3 night_cloud =
            Vec3{.012f, .023f, .044f} + Vec3{.025f, .031f, .038f} * rim;
        // Low banks leave the high galaxy clear and obscure its stars
        // correctly.
        const float cover = cloud * std::exp(-up * 4.5f) * .8f;
        sky = lerp(sky, night_cloud, cover);
      }
      if (d.y < 0)
        sky = dusk ? Vec3{.004f, .009f, .015f} : Vec3{.065f, .075f, .070f};
      if (!dusk && sun_dot > std::cos(radians(.56f)))
        sky = Vec3{21000, 15000, 8500};
      float *out = &env.rgb[(static_cast<std::size_t>(y) * env.w + x) * 3];
      out[0] = sky.x;
      out[1] = sky.y;
      out[2] = sky.z;
    }
  });
  Environment out =
      build_environment(gpu, std::move(env), 0, bg_size, spec_size, false);
  if (dusk) {
    out.exposure = 1.35f;
    out.has_sun = true;
    out.sun_dir = moon;
    out.moon_dir = moon;
    out.moon_visible = true;
    out.sun_color = {.065f, .083f, .12f};
  } else {
    out.exposure = .86f;
    out.has_sun = true;
    out.sun_dir = sun_dir;
    out.sun_color = {4.8f, 3.45f, 2.18f};
  }
  return out;
}

} // namespace cb
