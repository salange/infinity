#include "renderer_lights.hpp"
#include <iostream>

using namespace inf::city;
namespace {
void require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}
bool contains(const cb::lighting::List &list, std::uint32_t tile,
              std::uint32_t source) {
  const auto begin = list.words[2 * tile], count = list.words[2 * tile + 1];
  return std::find(list.words.begin() + begin,
                   list.words.begin() + begin + count,
                   source) != list.words.begin() + begin + count;
}
} // namespace

int main() {
  try {
    constexpr std::uint64_t limit = 64 * 1024 * 1024;
    constexpr std::uint32_t w = 1280, h = 720, tile = 22 * 80 + 40;
    const auto view = Mat4::identity();
    auto projection = perspective(kPi / 3, float(w) / h, .2f, 10000.f);
    std::vector<PointLight> sources;
    for (int i = 0; i < 64; ++i)
      sources.push_back({{0, 0, -10.f - i * .01f}, 2, {1, 1, 1}, 1});
    sources.push_back({{0, 0, -98}, 5, {1, .7f, .4f}, 1});
    auto lists =
        cb::lighting::build(sources, view, projection, w, h, .2f, limit);
    require(lists.words[tile * 2 + 1] == 65 && contains(lists, tile, 64),
            "reaching light lost to closer non-reaching sources");
    std::reverse(sources.begin(), sources.end());
    auto reverse =
        cb::lighting::build(sources, view, projection, w, h, .2f, limit);
    require(reverse.words[tile * 2 + 1] == 65 && contains(reverse, tile, 0),
            "source order changes list reach");
    require(!cb::lighting::sphere_bounds({500, 0, -100}, 1, view, projection, w,
                                         h, .2f)
                 .visible,
            "fully offscreen source pollutes border tiles");
    require(
        !cb::lighting::sphere_bounds({0, 0, 10}, 1, view, projection, w, h, .2f)
             .visible,
        "source behind near clip retained");
    const auto near = cb::lighting::sphere_bounds({0, 0, -.3f}, 1, view,
                                                  projection, w, h, .2f);
    require(near.visible && near.x0 == 0 && near.y0 == 0 && near.x1 == 79 &&
                near.y1 == 44,
            "near-plane sphere fallback loses pixels");
    bool rejected = false;
    try {
      (void)cb::lighting::build(sources, view, projection, w, h, .2f, 1);
    } catch (const std::length_error &) {
      rejected = true;
    }
    require(rejected, "adapter size limit was ignored");
    const auto empty =
        cb::lighting::build({}, view, projection, w, h, .2f, limit);
    require(empty.words.size() == 80 * 45 * 2 && empty.max_count == 0,
            "empty list header is invalid");
    std::size_t samples = 0;
    // Independently project actual sphere points. Exercise arbitrary off-axis
    // extent, both TAA jitter extremes and the reflected view's handedness.
    for (float jitter : {-.5f, .5f}) {
      projection.at(0, 2) = 2 * jitter / w;
      projection.at(1, 2) = -2 * jitter / h;
      for (bool mirror : {false, true}) {
        auto camera = view;
        if (mirror)
          camera.at(1, 1) = -1;
        for (float z : {.21f, 1.f, 7.f, 30.f, 100.f})
          for (float x : {-1.2f, -.4f, 0.f, .4f, 1.2f})
            for (float y : {-.8f, 0.f, .8f})
              for (float radius : {.1f, .4f * z, 1.1f * z}) {
                const Vec3 centre{x * z, y * z, -z};
                const auto b = cb::lighting::sphere_bounds(
                    centre, radius, camera, projection, w, h, .2f);
                for (int j = 0; j < 128; ++j) {
                  const float elevation = 1 - 2 * (j + .5f) / 128;
                  const float angle = j * kPi * (3 - std::sqrt(5.f));
                  const float rr = std::sqrt(1 - elevation * elevation);
                  const auto point =
                      centre + Vec3{rr * std::cos(angle), elevation,
                                    rr * std::sin(angle)} *
                                   (radius * .999f);
                  const auto camera_point = mul(camera, Vec4{point, 1});
                  if (-camera_point.z <= .2f)
                    continue;
                  const auto clip = mul(projection, camera_point);
                  const float px = (clip.x / clip.w + 1) * w / 2;
                  const float py = (1 - clip.y / clip.w) * h / 2;
                  if (px < 0 || py < 0 || px >= w || py >= h)
                    continue;
                  ++samples;
                  require(b.visible && std::uint32_t(px) / 16 >= b.x0 &&
                              std::uint32_t(px) / 16 <= b.x1 &&
                              std::uint32_t(py) / 16 >= b.y0 &&
                              std::uint32_t(py) / 16 <= b.y1,
                          "conservative sphere bounds omitted a visible "
                          "reaching point");
                }
              }
      }
    }
    std::cout << "light lists passed: untruncated65-source fixture, order, "
                 "offscreen/near clip, "
                 "empty/overflow guards, "
              << samples << " jittered/reflected sphere points\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
