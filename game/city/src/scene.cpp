#include "city/scene.hpp"

#include <algorithm>

namespace inf::city {

void Scene::register_range(std::uint32_t first, std::uint32_t end, Vec3 centre, float radius, int lod_group, int lod_level,
                           float lod_max_distance, bool has_fine) {
  if (end <= first) return;
  DrawRange r;
  r.first = first;
  r.count = end - first;
  r.centre = centre;
  r.radius = radius;
  r.lod_group = lod_group;
  r.lod_level = lod_level;
  r.lod_max_distance = lod_max_distance;
  r.has_fine = has_fine;
  draws.push_back(r);
}

void Scene::register_fine(std::uint32_t first, std::uint32_t end, Vec3 centre, float radius) {
  if (end <= first) return;
  DrawRange r;
  r.first = first;
  r.count = end - first;
  r.centre = centre;
  r.radius = radius;
  fine.push_back(r);
}

void Scene::finalize_draws() {
  std::sort(draws.begin(), draws.end(), [](const DrawRange& a, const DrawRange& b) { return a.first < b.first; });
  std::sort(fine.begin(), fine.end(), [](const DrawRange& a, const DrawRange& b) { return a.first < b.first; });
  std::vector<DrawRange> out;
  std::uint32_t cursor = 0;
  const std::uint32_t total = static_cast<std::uint32_t>(opaque.indices.size());
  for (const DrawRange& r : draws) {
    if (r.first > cursor) {
      DrawRange gap;
      gap.first = cursor;
      gap.count = r.first - cursor;
      out.push_back(gap);
    }
    out.push_back(r);
    cursor = std::max(cursor, r.first + r.count);
  }
  if (cursor < total) {
    DrawRange gap;
    gap.first = cursor;
    gap.count = total - cursor;
    out.push_back(gap);
  }
  draws.swap(out);
}

}  // namespace inf::city
