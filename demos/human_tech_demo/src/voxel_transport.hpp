#pragma once
// Runtime transport representation, rebuilt from the actual visible resources.
// This coarse volume complements geometric shadows and planar reflections. It
// never stores world data, replaces geometry, or depends on the current camera.
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

#include "ibl.hpp"
#include "point_visibility.hpp"
#include "scene.hpp"
#include "voxel_coverage.hpp"
namespace cb {
struct VoxelTransport {
  int nx = 192, ny = 80, nz = 192;
  float cell = 8.0f;
  Vec3 origin{-768.0f, -32.0f, -768.0f};
  struct Cell {
    std::uint16_t material{65535};
    std::int16_t nx{}, ny{}, nz{};
    point_visibility::Surface surface;
  };
  std::vector<Cell> cells;
  std::vector<MaterialDesc> materials;
  std::uint32_t occupied{};
  std::size_t index(int x, int y, int z) const {
    return (static_cast<std::size_t>(z) * ny + y) * nx + x;
  }
  bool inside(int x, int y, int z) const {
    return x >= 0 && y >= 0 && z >= 0 && x < nx && y < ny && z < nz;
  }
  bool solid(Vec3 p) const {
    const int x = int(std::floor((p.x - origin.x) / cell)),
              y = int(std::floor((p.y - origin.y) / cell)),
              z = int(std::floor((p.z - origin.z) / cell));
    return inside(x, y, z) && cells[index(x, y, z)].material != 65535;
  }
  void build(const Scene &scene) {
    const auto start = std::chrono::steady_clock::now();
    cells.assign(static_cast<std::size_t>(nx) * ny * nz, {});
    materials = scene.materials;
    occupied = 0;
    auto record = [&](int x, int y, int z, Vec3 p, Vec3 n,
                      Vec3 geometric_normal, std::uint32_t material) {
      if (!inside(x, y, z) || material >= materials.size() || material >= 65535)
        return;
      // Water and clear glazing transmit light. Opaque glazing remains a thin
      // diffuse/emissive transport surface; the main renderer retains its BRDF.
      if ((materials[material].flags & (64u | 128u)) != 0)
        return;
      auto &c = cells[index(x, y, z)];
      if (c.material == 65535)
        ++occupied;
      c.material = static_cast<std::uint16_t>(material);
      c.nx = static_cast<std::int16_t>(n.x * 32767);
      c.ny = static_cast<std::int16_t>(n.y * 32767);
      c.nz = static_cast<std::int16_t>(n.z * 32767);
      const Vec3 centre =
          origin + Vec3{(x + .5f) * cell, (y + .5f) * cell, (z + .5f) * cell};
      point_visibility::include_surface(c.surface, p, geometric_normal, centre,
                                        cell);
    };
    auto mesh = [&](const Mesh &m, const AssetInstance *instance,
                    std::uint32_t first, std::uint32_t count) {
      const float cosine = instance ? std::cos(instance->yaw) : 1.f,
                  sine = instance ? std::sin(instance->yaw) : 0.f;
      auto transform = [&](Vec3 p) {
        p = {p.x * instance->scale.x, p.y * instance->scale.y,
             p.z * instance->scale.z};
        return Vec3{cosine * p.x + sine * p.z + instance->translation.x,
                    p.y + instance->translation.y,
                    -sine * p.x + cosine * p.z + instance->translation.z};
      };
      for (std::uint32_t i = first; i + 2 < first + count; i += 3) {
        const Vertex &a = m.vertices[m.indices[i]],
                     &b = m.vertices[m.indices[i + 1]],
                     &c = m.vertices[m.indices[i + 2]];
        Vec3 p = a.position, q = b.position, r = c.position,
             n = normalize(a.normal + b.normal + c.normal);
        if (instance) {
          p = transform(p);
          q = transform(q);
          r = transform(r);
          n = {n.x / instance->scale.x, n.y / instance->scale.y,
               n.z / instance->scale.z};
          n = normalize(
              Vec3{cosine * n.x + sine * n.z, n.y, -sine * n.x + cosine * n.z});
        }
        const Vec3 low = vmin(p, vmin(q, r)), high = vmax(p, vmax(q, r));
        const Vec3 cross_normal = cross(q - p, r - p);
        const float normal_scale =
            std::max({std::abs(cross_normal.x), std::abs(cross_normal.y),
                      std::abs(cross_normal.z)});
        if (normal_scale == 0)
          continue;
        const Vec3 geometric_normal =
            normalize(cross_normal * (1.f / normal_scale));
        const Vec3 end = origin + Vec3{nx * cell, ny * cell, nz * cell};
        if (high.x < origin.x || high.y < origin.y || high.z < origin.z ||
            low.x > end.x || low.y > end.y || low.z > end.z)
          continue;
        const voxel_coverage::Grid grid{origin, cell, {nx, ny, nz}};
        voxel_coverage::triangle_cells(
            grid, p, q, r, [&](int x, int y, int z, Vec3 surface_point) {
              record(x, y, z, surface_point, n, geometric_normal, a.material);
            });
      }
    };
    auto overlaps = [&](Vec3 centre, float radius) {
      const Vec3 end = origin + Vec3{nx * cell, ny * cell, nz * cell};
      return centre.x + radius >= origin.x && centre.y + radius >= origin.y &&
             centre.z + radius >= origin.z && centre.x - radius <= end.x &&
             centre.y - radius <= end.y && centre.z - radius <= end.z;
    };
    if (scene.draws.empty())
      mesh(scene.opaque, nullptr, 0,
           static_cast<std::uint32_t>(scene.opaque.indices.size()));
    else {
      // Exactly one geometry representation per LOD group. Level 0 preserves
      // recessed floors and balconies in this camera-independent field.
      for (const auto &d : scene.draws)
        if ((d.lod_group < 0 || d.lod_level == 0) &&
            (d.radius <= 0 || overlaps(d.centre, d.radius)))
          mesh(scene.opaque, nullptr, d.first, d.count);
    }
    mesh(scene.foliage, nullptr, 0,
         static_cast<std::uint32_t>(scene.foliage.indices.size()));
    struct ResourceBounds {
      Vec3 centre;
      float radius;
    };
    std::vector<ResourceBounds> bounds;
    for (const auto &resource : scene.asset_library.resources) {
      Vec3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
      for (const auto &v : resource.mesh.vertices) {
        lo = vmin(lo, v.position);
        hi = vmax(hi, v.position);
      }
      bounds.push_back({(lo + hi) * .5f, length(hi - lo) * .5f});
    }
    for (const auto &inst : scene.asset_instances)
      if (inst.resource < scene.asset_library.resources.size()) {
        const auto &bound = bounds[inst.resource];
        const Vec3 centre = asset_transform_point(inst, bound.centre);
        const float radius = bound.radius * std::max({std::abs(inst.scale.x),
                                                      std::abs(inst.scale.y),
                                                      std::abs(inst.scale.z)});
        if (!overlaps(centre, radius))
          continue;
        const auto &m = scene.asset_library.resources[inst.resource].mesh;
        mesh(m, &inst, 0, static_cast<std::uint32_t>(m.indices.size()));
      }
    std::printf(
        "  transport: %u occupied cells, %.1f MiB geometry, %.2f s runtime "
        "voxelisation (%.1f m cells, %.0f x %.0f x "
        "%.0f m)\n",
        occupied, double(cells.size() * sizeof(Cell)) / 1048576.0,
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
            .count(),
        cell, nx * cell, ny * cell, nz * cell);
  }
  bool blocked(Vec3 p, Vec3 d, int steps,
               const VoxelTransport *farther = nullptr) const {
    for (int i = 0; i < steps; ++i) {
      p += d * (cell * .85f);
      const Vec3 relative = (p - origin) * (1.f / cell);
      if (!inside(int(std::floor(relative.x)), int(std::floor(relative.y)),
                  int(std::floor(relative.z))))
        return farther ? farther->blocked(p, d, 256) : false;
      if (solid(p))
        return true;
    }
    return false;
  }
  Texture upload_visibility(Gpu &gpu) const {
    std::vector<std::uint32_t> packed(cells.size() * 2);
    std::size_t ambiguous = 0;
    for (std::size_t i = 0; i < cells.size(); ++i) {
      const auto &surface = cells[i].surface;
      packed[i * 2] = surface.normal;
      packed[i * 2 + 1] = std::uint16_t(surface.lower) |
                          (std::uint32_t(std::uint16_t(surface.upper)) << 16);
      ambiguous += (surface.normal & 0x80000000u) != 0;
    }
    Texture t;
    t.width = nx;
    t.height = ny;
    t.layers = nz;
    t.mips = 1;
    t.format = WGPUTextureFormat_RG32Uint;
    WGPUTextureDescriptor td{};
    td.label = sv("point-visibility-surface-slabs");
    td.dimension = WGPUTextureDimension_3D;
    td.size = {std::uint32_t(nx), std::uint32_t(ny), std::uint32_t(nz)};
    td.format = t.format;
    td.mipLevelCount = 1;
    td.sampleCount = 1;
    td.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
    if (!gpu.poll(false))
      throw GpuUnavailable(gpu.failure_message());
    t.texture = wgpuDeviceCreateTexture(gpu.device, &td);
    WGPUTextureViewDescriptor vd{};
    vd.dimension = WGPUTextureViewDimension_3D;
    vd.format = t.format;
    vd.mipLevelCount = 1;
    vd.arrayLayerCount = 1;
    t.view = wgpuTextureCreateView(t.texture, &vd);
    gpu.upload_volume_level(t, 0, nx, ny, nz, packed.data(), 8);
    if (!gpu.healthy()) {
      t.release();
      throw GpuUnavailable(gpu.failure_message());
    }
    std::printf(
        "  point surface field: %.3f m cells, %u occupied, %zu conservative "
        "multi-surface cells; %.1f MiB GPU\n",
        cell, occupied, ambiguous,
        double(packed.size() * sizeof(std::uint32_t)) / 1048576.);
    return t;
  }
  Texture illuminate(Gpu &gpu, const Environment &env, float night,
                     const std::vector<PointLight> &lights,
                     const VoxelTransport *farther = nullptr,
                     bool point_shadows = true) const {
    const auto start = std::chrono::steady_clock::now();
    std::vector<float> data(cells.size() * 4, 0);
    auto grid = [](const VoxelTransport &field) {
      return point_visibility::Grid{
          field.origin, field.cell, {field.nx, field.ny, field.nz}};
    };
    const auto coarse_grid = grid(farther ? *farther : *this),
               local_grid = grid(*this);
    auto occupied_at = [](const VoxelTransport &field, int x, int y, int z,
                          Vec3 a, Vec3 b) {
      const Vec3 centre =
          field.origin + Vec3{(x + .5f) * field.cell, (y + .5f) * field.cell,
                              (z + .5f) * field.cell};
      return point_visibility::surface_blocks(
          field.cells[field.index(x, y, z)].surface, centre, field.cell, a, b);
    };
    std::uint64_t point_segments = 0, point_blocked = 0, point_partial = 0;
    const std::array<Vec3, 4> sky_dirs = {
        normalize(Vec3{1, 2, 1}), normalize(Vec3{-1, 2, 1}),
        normalize(Vec3{1, 2, -1}), normalize(Vec3{-1, 2, -1})};
    auto sh = [&](Vec3 n) {
      const float b[9] = {.282095f,
                          .488603f * n.y,
                          .488603f * n.z,
                          .488603f * n.x,
                          1.092548f * n.x * n.y,
                          1.092548f * n.y * n.z,
                          .315392f * (3 * n.z * n.z - 1),
                          1.092548f * n.x * n.z,
                          .546274f * (n.x * n.x - n.y * n.y)};
      Vec3 c{};
      for (int i = 0; i < 9; ++i)
        c += Vec3{env.sh[i][0], env.sh[i][1], env.sh[i][2]} * b[i];
      return Vec3{std::max(c.x, 0.f), std::max(c.y, 0.f), std::max(c.z, 0.f)};
    };
    for (int z = 0; z < nz; ++z)
      for (int y = 0; y < ny; ++y)
        for (int x = 0; x < nx; ++x) {
          const auto i = index(x, y, z);
          const auto &c = cells[i];
          if (c.material == 65535)
            continue;
          const auto &m = materials[c.material];
          const Vec3 n = normalize(Vec3{float(c.nx), float(c.ny), float(c.nz)});
          const Vec3 p = origin + Vec3{(x + .5f) * cell, (y + .5f) * cell,
                                       (z + .5f) * cell};
          const Vec3 start_p = p + n * (cell * 1.25f);
          const float sun_vis =
              blocked(start_p, env.sun_dir, std::max({nx, ny, nz}) * 3, farther)
                  ? 0.f
                  : 1.f;
          float sky_vis = .12f;
          for (Vec3 d : sky_dirs)
            if (!blocked(start_p, d, std::max({nx, ny, nz}) * 2, farther))
              sky_vis += .22f;
          Vec3 irradiance =
              sh(n) * (sky_vis * env.lighting_scale) +
              env.sun_color * (std::max(0.f, dot(n, env.sun_dir)) * sun_vis);
          for (const auto &l : lights) {
            const Vec3 delta = l.position - p;
            const float d2 = dot(delta, delta), r2 = l.radius * l.radius;
            if (d2 >= r2 || d2 < 1e-3f)
              continue;
            const float incident = dot(n, normalize(delta));
            const bool botanical = (m.flags & (8u | 256u)) != 0;
            const float diffuse_cosine =
                botanical ? clampf((incident + .4f) / 1.4f, 0.f, 1.f) *
                                (1.f + std::max(-incident, 0.f) * .65f)
                          : std::max(incident, 0.f);
            // Opaque surfaces receive no back-hemisphere fill. Leaf wrap and
            // transmission retain their explicit material scattering model.
            if (diffuse_cosine <= 0 || l.intensity == 0)
              continue;
            if (point_shadows) {
              const auto visibility = point_visibility::segment(
                  p, n, l.position, coarse_grid,
                  [&](int x, int y, int z, Vec3 a, Vec3 b) {
                    return occupied_at(farther ? *farther : *this, x, y, z, a,
                                       b);
                  },
                  farther ? &local_grid : nullptr,
                  [&](int x, int y, int z, Vec3 a, Vec3 b) {
                    return occupied_at(*this, x, y, z, a, b);
                  });
              ++point_segments;
              point_partial += visibility.covered < .99999f;
              point_blocked += visibility.blocked;
              if (visibility.blocked)
                continue;
            }
            const float win = std::max(0.f, 1.f - d2 * d2 / (r2 * r2));
            irradiance += l.color * (l.intensity * (.12f + .88f * night) * win *
                                     win / (d2 + 1) * diffuse_cosine);
          }
          Vec3 radiance = m.base_color * irradiance * ((1 - m.metallic) / kPi);
          if ((m.flags & 2u) != 0)
            radiance += m.tint2 * (m.emissive * .9f / env.exposure *
                                   ((m.flags & 32u) != 0 ? night : 1.f));
          if ((m.flags & 1u) != 0) {
            // Occupied glass base RGB is transmission, not diffuse albedo.
            // This isotropic field stores only the angularly averaged coating
            // response and room output; it cannot store a directional BRDF.
            const Vec3 transmission{clampf(m.base_color.x, .001f, 1.f),
                                    clampf(m.base_color.y, .001f, 1.f),
                                    clampf(m.base_color.z, .001f, 1.f)};
            const float maximum =
                std::max({transmission.x, transmission.y, transmission.z});
            const float coating = clampf(m.metallic, 0.f, 1.f);
            const Vec3 f0 = Vec3{.04f, .04f, .04f} * (1.f - coating) +
                            transmission * (.32f * coating / maximum);
            radiance = (f0 + transmission * .02f) * irradiance * (1.f / kPi);
            // Spatially averaged room emission mirrors the shader's continuous
            // occupancy, ambient and display-radiance factors at dusk.
            const float floor_probability =
                clampf(m.lit_probability * (.58f + .20f * night), 0.f, 1.f);
            const float lit = .725f * (.10f + .75f * floor_probability);
            const float ambient = .10f - .092f * night;
            const float room = (ambient + (.75f - ambient) * lit) * .8f;
            // Same unresolved RGB and triangular mullion coverage as WGSL.
            const Vec3 mean_light =
                Vec3{1.f, .86f, .68f} * .86f + Vec3{.78f, .88f, 1.f} * .14f;
            const Vec3 mean_room = Vec3{.42f, .40f, .37f} * mean_light;
            constexpr float mullion_half_width = .04f;
            const float grid_mean =
                mullion_half_width / m.room_w + mullion_half_width / m.room_h -
                4.f * mullion_half_width * mullion_half_width /
                    (3.f * m.room_w * m.room_h);
            radiance += m.tint2 * transmission * (Vec3{1, 1, 1} - f0) *
                        mean_room *
                        (room * .9f * (.65f + 2.05f * night) / env.exposure);
            radiance =
                radiance * (1.f - .85f * grid_mean) +
                Vec3{.02f, .02f, .022f} * irradiance * (.85f * grid_mean / kPi);
          }
          data[i * 4] = std::min(radiance.x, 32.f);
          data[i * 4 + 1] = std::min(radiance.y, 32.f);
          data[i * 4 + 2] = std::min(radiance.z, 32.f);
          data[i * 4 + 3] = 1.f;
        }
    Texture t;
    std::printf("  point visibility: %llu/%llu blocked injection segments, "
                "%llu partially outside known volume; enabled=%d\n",
                static_cast<unsigned long long>(point_blocked),
                static_cast<unsigned long long>(point_segments),
                static_cast<unsigned long long>(point_partial), point_shadows);
    t.width = nx;
    t.height = ny;
    t.layers = nz;
    t.mips = 1;
    for (int dimension = std::max({nx, ny, nz}); dimension > 1; dimension >>= 1)
      ++t.mips;
    t.format = WGPUTextureFormat_RGBA16Float;
    WGPUTextureDescriptor td{};
    td.label = sv("scene-radiance-volume");
    td.dimension = WGPUTextureDimension_3D;
    td.size = {static_cast<std::uint32_t>(nx), static_cast<std::uint32_t>(ny),
               static_cast<std::uint32_t>(nz)};
    td.format = t.format;
    td.mipLevelCount = t.mips;
    td.sampleCount = 1;
    td.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
    if (!gpu.poll(false))
      throw GpuUnavailable(gpu.failure_message());
    t.texture = wgpuDeviceCreateTexture(gpu.device, &td);
    WGPUTextureViewDescriptor vd{};
    vd.dimension = WGPUTextureViewDimension_3D;
    vd.format = t.format;
    vd.mipLevelCount = t.mips;
    vd.arrayLayerCount = 1;
    t.view = wgpuTextureCreateView(t.texture, &vd);
    int width = nx, height = ny, depth = nz;
    std::size_t uploaded = 0;
    for (std::uint32_t mip = 0; mip < t.mips; ++mip) {
      std::vector<std::uint16_t> half(data.size());
      std::transform(data.begin(), data.end(), half.begin(), float_to_half);
      if (!gpu.poll(false)) {
        t.release();
        throw GpuUnavailable(gpu.failure_message());
      }
      gpu.upload_volume_level(t, mip, width, height, depth, half.data(), 8);
      if (!gpu.healthy()) {
        t.release();
        throw GpuUnavailable(gpu.failure_message());
      }
      uploaded += half.size() * 2;
      if (mip + 1 == t.mips)
        break;
      const int next_w = std::max(1, width / 2),
                next_h = std::max(1, height / 2),
                next_d = std::max(1, depth / 2);
      std::vector<float> next(std::size_t(next_w) * next_h * next_d * 4, 0.f);
      // Radiance is stored premultiplied by occupied coverage. Averaging both
      // together yields a cone footprint that conserves surface energy.
      for (int z = 0; z < next_d; ++z)
        for (int y = 0; y < next_h; ++y)
          for (int x = 0; x < next_w; ++x)
            for (int dz = 0; dz < 2; ++dz)
              for (int dy = 0; dy < 2; ++dy)
                for (int dx = 0; dx < 2; ++dx) {
                  const auto src =
                      ((std::size_t(std::min(z * 2 + dz, depth - 1)) * height +
                        std::min(y * 2 + dy, height - 1)) *
                           width +
                       std::min(x * 2 + dx, width - 1)) *
                      4;
                  const auto dest =
                      ((std::size_t(z) * next_h + y) * next_w + x) * 4;
                  for (int c = 0; c < 4; ++c)
                    next[dest + c] += data[src + c] * .125f;
                }
      data = std::move(next);
      width = next_w;
      height = next_h;
      depth = next_d;
    }
    std::printf(
        "  transport: evening factor %.2f one-bounce radiance updated in %.2f "
        "s, %.1f MiB\n",
        night,
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
            .count(),
        double(uploaded) / 1048576.0);
    return t;
  }
};
} // namespace cb
