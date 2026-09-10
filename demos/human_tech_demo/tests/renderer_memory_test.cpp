#include "renderer_memory.hpp"
#include "scene_storage.hpp"
#include <array>
#include <cstring>
#include <iostream>

namespace {
void require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}
template <class F> void rejected(F f) {
  bool failed = false;
  try {
    f();
  } catch (const std::length_error &) {
    failed = true;
  }
  require(failed, "invalid size/range was accepted");
}
void check_texture(std::uint32_t w, std::uint32_t h, std::uint32_t d,
                   std::uint32_t bpp) {
  std::uint64_t cursor = 0;
  std::size_t chunks = 0;
  cb::memory::texture_chunks(w, h, d, bpp, [&](cb::memory::TextureChunk c) {
    require(c.offset == cursor && c.size > 0 &&
                c.size <= cb::memory::upload_bytes,
            "texture staging has a gap, overlap or unbounded chunk");
    require(c.y + c.height <= h && c.z + c.depth <= d,
            "texture chunk exceeds destination extent");
    require(c.depth == 1 || (c.y == 0 && c.height == h),
            "multi-slice chunk must contain full image rows");
    require(c.offset == (std::uint64_t(c.z) * h + c.y) * w * bpp &&
                c.size == std::uint64_t(w) * c.height * c.depth * bpp,
            "texture source stride differs from destination stride");
    cursor += c.size;
    ++chunks;
  });
  require(cursor == std::uint64_t(w) * h * d * bpp && chunks > 0,
          "texture upload omitted source texels");
}
} // namespace

int main() {
  try {
    using namespace cb::memory;
    const auto max = std::numeric_limits<std::uint64_t>::max();
    require(buffer_size(5, 8) == 8 && buffer_size(8, 8) == 8,
            "buffer alignment changed");
    rejected([&] { buffer_size(5, 7); });
    rejected([&] { buffer_size(max, max); });
    rejected([&] { bytes(max / 32 + 1, 32); });
    buffer_write(128, 0, 128);
    buffer_write(128, 128, 0);
    rejected([&] { buffer_write(128, 128, 4); });
    rejected([&] { buffer_write(128, 4, 128); });
    rejected([&] { buffer_write(max, max - 3, 8); });
    rejected([&] { buffer_write(128, 1, 4); });
    rejected([&] { buffer_write(128, 0, 3); });

    require(instance_capacity(1) == 1 && instance_capacity(2) == 8 &&
                instance_capacity(70414) == 492892,
            "seven-view instance capacity is incorrect");
    // All instances visible in every pass was the case missing from the old
    // six-list allocation. Simulate the exact worst-case count independently.
    std::uint64_t visible = 1;
    for (int view = 0; view < 7; ++view)
      visible += 70413;
    require(visible == instance_capacity(70414),
            "all-visible seventh pass exceeds allocation");
    const auto largest_source =
        (std::uint64_t(std::numeric_limits<std::uint32_t>::max()) - 1) / 7 + 1;
    (void)instance_capacity(largest_source);
    rejected([&] { instance_capacity(largest_source + 1); });
    rejected([&] { instance_capacity(0); });
    rejected([&] { instance_capacity(max); });

    // Simulate a vertex upload spanning two boundaries with unique records;
    // compare bytes with a direct copy, including the short final chunk.
    using Record = std::array<std::uint32_t, 8>;
    const std::size_t count = upload_bytes / sizeof(Record) * 2 + 13;
    std::vector<Record> source(count), destination(count);
    for (std::size_t i = 0; i < count; ++i)
      for (std::size_t c = 0; c < 8; ++c)
        source[i][c] = std::uint32_t(i * 17 + c);
    for (std::size_t first = 0; first < count;) {
      const auto n = chunk_records(count - first, sizeof(Record));
      require(n > 0 && n * sizeof(Record) <= upload_bytes,
              "record upload exceeded staging budget");
      std::memcpy(destination.data() + first, source.data() + first,
                  n * sizeof(Record));
      first += n;
    }
    require(source == destination, "chunked upload changed record bytes");
    check_texture(192, 192, 192, 8); // production transport volume
    check_texture(256, 256, 256, 8); // bounded assembly transport volume
    check_texture(4096, 2048, 1, 8); // wide sky mip split into rows
    check_texture(4096, 2048, 3, 8); // row splitting with multiple slices
    check_texture(1, 1, 1, 8);
    rejected([&] { check_texture(0, 1, 1, 8); });

    cb::Scene scene;
    scene.camera_position = {1, 2, 3};
    scene.stats_towers = 17;
    scene.city_size = "test";
    scene.materials.emplace_back();
    scene.asset_library.resources.push_back({"test-resource", {}});
    scene.asset_library.source_sha256 = "retained provenance";
    scene.asset_instances.emplace_back();
    scene.asset_instances.back().translation = {4, 5, 6};
    std::uint64_t expected = 0;
    for (auto *mesh : {&scene.opaque, &scene.foliage,
                       &scene.asset_library.resources[0].mesh}) {
      mesh->vertices.resize(3);
      mesh->vertices.reserve(29);
      mesh->indices = {0, 1, 2};
      mesh->indices.reserve(31);
      mesh->bounds_min = {-2, -3, -4};
      mesh->bounds_max = {2, 3, 4};
      expected += mesh->vertices.capacity() * sizeof(cb::Vertex) +
                  mesh->indices.capacity() * sizeof(std::uint32_t);
    }
    require(cb::release_scene_mesh_storage(scene) == expected,
            "CPU mesh release byte accounting differs from freed capacity");
    for (const auto *mesh : {&scene.opaque, &scene.foliage,
                             &scene.asset_library.resources[0].mesh})
      require(mesh->vertices.capacity() == 0 && mesh->indices.capacity() == 0 &&
                  mesh->bounds_min.x == -2 && mesh->bounds_max.z == 4,
              "CPU release retained capacity or changed mesh bounds");
    require(scene.materials.size() == 1 && scene.stats_towers == 17 &&
                scene.camera_position.y == 2 && scene.city_size == "test" &&
                scene.asset_library.resources[0].name == "test-resource" &&
                scene.asset_library.source_sha256 == "retained provenance" &&
                scene.asset_instances[0].translation.z == 6 &&
                cb::release_scene_mesh_storage(scene) == 0,
            "CPU mesh release damaged retained scene metadata");
    std::cout
        << "Renderer memory contracts passed: seven views, limits, "
           "write ranges, byte-preserving chunks and CPU mesh lifetimes\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
