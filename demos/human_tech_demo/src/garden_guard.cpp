#include "garden_guard.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>

namespace cb {
namespace {
constexpr float kJoinTolerance = .0005f;
constexpr float kMaximumPostSpacing = 2.8f;

std::uint32_t guard_glass(Scene &scene) {
  for (std::uint32_t i = 0; i < scene.materials.size(); ++i)
    if (scene.materials[i].name == "laminated balcony glazing")
      return i;
  auto material = scene.materials.at(M_GLASS_CLEAR);
  material.name = "laminated balcony glazing";
  material.flags = 128u;
  material.base_color = {.93f, .97f, .95f};
  material.roughness = .07f;
  material.albedo_set = "";
  material.emissive = 0;
  material.metallic = 0;
  material.room_w = 1.5f;
  material.room_h = .98f;
  material.room_d = .96f;
  material.lit_probability = .012f;
  scene.materials.push_back(material);
  return static_cast<std::uint32_t>(scene.materials.size() - 1);
}
} // namespace

GardenGuardPlan plan_garden_guard(const std::vector<GardenGuardSpan> &edges) {
  GardenGuardPlan plan;
  std::vector<Vec3> nodes;
  std::vector<std::pair<std::size_t, std::size_t>> links;
  std::set<std::pair<std::size_t, std::size_t>> unique;
  auto node = [&](Vec3 p) {
    if (!std::isfinite(p.x + p.y + p.z))
      throw std::runtime_error("Invalid garden guard endpoint");
    for (std::size_t i = 0; i < nodes.size(); ++i)
      if (length(nodes[i] - p) <= kJoinTolerance)
        return i;
    nodes.push_back(p);
    return nodes.size() - 1;
  };
  for (const auto &[a, b] : edges) {
    if (length(b - a) <= kJoinTolerance)
      continue;
    auto first = node(a), second = node(b);
    if (first == second)
      continue;
    if (unique.insert(std::minmax(first, second)).second)
      links.push_back({first, second});
  }
  std::vector<std::vector<std::size_t>> adjacent(nodes.size());
  for (std::size_t i = 0; i < links.size(); ++i) {
    const auto [a, b] = links[i];
    adjacent[a].push_back(i);
    adjacent[b].push_back(i);
    plan.spans.push_back({nodes[a], nodes[b]});
  }
  std::vector<bool> visited(links.size());
  std::vector<std::vector<Vec3>> chains;
  auto walk = [&](std::size_t start, std::size_t edge) {
    std::vector<Vec3> chain{nodes[start]};
    auto current = start;
    while (!visited[edge]) {
      visited[edge] = true;
      const auto [a, b] = links[edge];
      current = current == a ? b : a;
      chain.push_back(nodes[current]);
      if (adjacent[current].size() != 2)
        break;
      const auto next = adjacent[current][0] == edge ? adjacent[current][1]
                                                     : adjacent[current][0];
      if (visited[next])
        break;
      edge = next;
    }
    chains.push_back(std::move(chain));
  };
  for (std::size_t node = 0; node < nodes.size(); ++node)
    if (adjacent[node].size() != 2)
      for (auto edge : adjacent[node])
        if (!visited[edge])
          walk(node, edge);
  for (std::size_t edge = 0; edge < links.size(); ++edge)
    if (!visited[edge])
      walk(links[edge].first, edge);
  auto post = [&](Vec3 p) {
    for (auto old : plan.posts)
      if (length(old - p) <= kJoinTolerance)
        return;
    plan.posts.push_back(p);
  };
  for (const auto &chain : chains) {
    std::vector<float> distance{0};
    for (std::size_t i = 1; i < chain.size(); ++i)
      distance.push_back(distance.back() + length(chain[i] - chain[i - 1]));
    const float total = distance.back();
    const bool closed = length(chain.front() - chain.back()) <= kJoinTolerance;
    auto at = [&](float along) {
      along = std::fmod(along, total);
      if (along < 0)
        along += total;
      if (!closed && along == 0)
        return chain.front();
      auto upper = std::upper_bound(distance.begin(), distance.end(), along);
      const auto i =
          std::min(chain.size() - 1, std::size_t(upper - distance.begin()));
      return lerp(chain[i - 1], chain[i],
                  (along - distance[i - 1]) / (distance[i] - distance[i - 1]));
    };
    std::vector<float> stations;
    if (!closed)
      stations.push_back(0);
    for (std::size_t i = 0; i + 1 < chain.size(); ++i) {
      if (!closed && i == 0)
        continue;
      const auto previous = i ? i - 1 : chain.size() - 2;
      const auto incoming = normalize(chain[i] - chain[previous]);
      const auto outgoing = normalize(chain[i + 1] - chain[i]);
      // Real plan corners receive a support. Small curve chords do not create
      // repeated posts at every tessellation vertex.
      if (dot(incoming, outgoing) < std::cos(radians(35.f)))
        stations.push_back(distance[i]);
    }
    if (!closed)
      stations.push_back(total);
    if (stations.empty())
      stations.push_back(0);
    const auto spans = closed ? stations.size() : stations.size() - 1;
    for (std::size_t i = 0; i < spans; ++i) {
      const float a = stations[i];
      const float b =
          i + 1 < stations.size() ? stations[i + 1] : stations.front() + total;
      const int count =
          std::max(1, int(std::ceil((b - a) / kMaximumPostSpacing)));
      for (int step = 0; step < count; ++step)
        post(at(a + (b - a) * step / count));
    }
    if (!closed)
      post(chain.back());
  }
  return plan;
}

void build_garden_guard(Scene &scene,
                        const std::vector<GardenGuardSpan> &edges) {
  const auto plan = plan_garden_guard(edges);
  const auto glass_id = guard_glass(scene);
  Emit metal(&scene.opaque, M_BRONZE), glass(&scene.opaque, glass_id);
  for (const auto &[a, b] : plan.spans) {
    metal.tube(a + Vec3{0, 1.08f, 0}, b + Vec3{0, 1.08f, 0}, .034f, 6);
    metal.tube(a + Vec3{0, .12f, 0}, b + Vec3{0, .12f, 0}, .025f, 5);
    const auto side = normalize(cross(b - a, Vec3{0, 1, 0})) * .018f;
    glass.quad_metric(a + Vec3{0, .18f, 0} + side, b + Vec3{0, .18f, 0} + side,
                      b + Vec3{0, 1.02f, 0} + side,
                      a + Vec3{0, 1.02f, 0} + side);
  }
  for (auto p : plan.posts)
    metal.tube(p, p + Vec3{0, 1.08f, 0}, .032f, 5);
}
} // namespace cb
