#include "commands_city.hpp"

#include <cmath>
#include <cstdio>
#include <cstdint>

#include "city/materials.hpp"
#include "city/rng.hpp"
#include "city/showcase.hpp"
#include "core/seed.hpp"

namespace inf::cli {

namespace {

// FNV-1a over centimetre-quantised geometry: the city generators are
// cosmetic float code (libm trig), so the golden tolerates last-bit
// differences but catches any change of shape, material or count.
struct Fnv {
  std::uint64_t h{0xcbf29ce484222325ULL};
  void feed(std::uint64_t v) {
    for (int i = 0; i < 8; ++i) {
      h ^= (v >> (i * 8)) & 0xffU;
      h *= 0x100000001b3ULL;
    }
  }
  void feed_cm(float v) { feed(static_cast<std::uint64_t>(static_cast<std::int64_t>(std::llround(v * 100.0f)))); }
};

std::uint64_t hash_scene(const city::Scene& sc) {
  Fnv f;
  for (const city::Mesh* m : {&sc.opaque, &sc.foliage}) {
    f.feed(m->vertices.size());
    f.feed(m->indices.size());
    for (const city::Vertex& v : m->vertices) {
      f.feed_cm(v.position.x);
      f.feed_cm(v.position.y);
      f.feed_cm(v.position.z);
      f.feed(v.material);
    }
    for (std::size_t i = 0; i < m->indices.size(); i += 97) f.feed(m->indices[i]);
  }
  f.feed(sc.lights.size());
  f.feed(sc.draws.size());
  return f.h;
}

}  // namespace

int cmd_hash_city() {
  const core::Seed128 seeds[2] = {core::Seed128{0, 1}, core::Seed128{0, 0x83}};
  for (const core::Seed128& seed : seeds) {
    city::Scene sc;
    sc.materials = city::make_materials();
    city::generate_showcase_small(sc, city::Rng(core::universe_key(seed)).child(0x51));
    std::printf("city-showcase seed=%s fnv=%016llx triangles=%zu\n", core::to_hex(seed).c_str(),
                static_cast<unsigned long long>(hash_scene(sc)),
                (sc.opaque.indices.size() + sc.foliage.indices.size()) / 3);
  }
  return 0;
}

}  // namespace inf::cli
