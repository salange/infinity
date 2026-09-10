#include "point_visibility.hpp"
#include "voxel_transport.hpp"
#include <functional>
#include <iostream>
#include <random>
#include <stdexcept>
#include <vector>
using namespace cb;
namespace pv = cb::point_visibility;
namespace {
void require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}
struct Field {
  pv::Grid grid{{-2, -2, -2}, .125f, {32, 32, 32}};
  std::vector<bool> cells = std::vector<bool>(32 * 32 * 32);
  std::size_t index(int x, int y, int z) const { return (z * 32 + y) * 32 + x; }
  bool operator()(int x, int y, int z) const { return cells[index(x, y, z)]; }
};
} // namespace
int main() {
  try {
    Field field;
    for (int y = 0; y < 32; ++y)
      for (int z = 0; z < 32; ++z)
        field.cells[field.index(12, y, z)] = true;
    auto trace = [&](Vec3 a, Vec3 b) {
      return pv::segment(a, {}, b, field.grid, std::ref(field), nullptr,
                         std::ref(field));
    };
    require(trace({1, 0, 0}, {-1, 0, 0}).blocked,
            "axis ray crossed a thin wall");
    require(trace({1, 0, 0}, {0, 0, 0}).covered == 1 &&
                !trace({1, 0, 0}, {0, 0, 0}).blocked,
            "wall behind source blocks finite light segment");
    require(trace({1, 1, 1}, {-1, -1, -1}).blocked, "oblique ray crossed wall");
    require(trace({1, 0, 0}, {-3, 0, 0}).blocked &&
                std::abs(trace({1, 0, 0}, {-3, 0, 0}).covered - .75f) < 1e-6f,
            "outside segment coverage or boundary blocker lost");
    require(!trace({4, 0, 0}, {3, 0, 0}).blocked &&
                trace({4, 0, 0}, {3, 0, 0}).covered == 0,
            "unknown space claimed covered or blocked");
    require(!trace({-.45f, 0, 0}, {1, 0, 0}).blocked,
            "receiver endpoint cell self-occludes");
    require(!trace({1, 0, 0}, {-.45f, 0, 0}).blocked,
            "source endpoint cell self-occludes");
    Field fine;
    fine.grid = {{-1, -1, -1}, .0625f, {32, 32, 32}};
    auto nested =
        pv::segment(Vec3{1.5f, 0, 0}, {}, Vec3{-1.5f, 0, 0}, field.grid,
                    std::ref(field), &fine.grid, std::ref(fine));
    require(!nested.blocked && nested.covered == 1,
            "coarse occupied cell overrides known fine aperture");
    fine.cells[fine.index(8, 16, 16)] = true;
    nested = pv::segment(Vec3{1.5f, 0, 0}, {}, Vec3{-1.5f, 0, 0}, field.grid,
                         std::ref(field), &fine.grid, std::ref(fine));
    require(nested.blocked, "fine boundary traversal misses fine wall");
    for (float cell : {.125f, .5f, 8.f}) {
      pv::Surface surface;
      const Vec3 centre{0, cell * .5f, 0};
      pv::include_surface(surface, {}, {0, 1, 0}, centre, cell);
      require(!pv::surface_blocks(surface, centre, cell,
                                  {-cell * .5f, .002f, 0},
                                  {cell * .5f, .004f, 0}),
              "grazing ray above actual floor self-shadows occupied voxel");
      require(pv::surface_blocks(surface, centre, cell, {0, -.01f, 0},
                                 {0, .01f, 0}),
              "surface-plane confirmation loses real crossing");
      pv::include_surface(surface, {}, {1, 0, 0}, centre, cell);
      require(
          (surface.normal & 0x80000000u) != 0 &&
              pv::surface_blocks(surface, centre, cell, {0, .002f, 0},
                                 {0, .004f, 0}),
          "incompatible surface discarded instead of conservative fallback");
    }
    for (int sample = 0; sample < 10001; ++sample) {
      const float z = 1.f - 2.f * (sample + .5f) / 10001.f;
      const float angle = sample * 2.39996323f;
      const Vec3 n = sample == 10000
                         ? normalize(Vec3{.05f, .998749f, 0})
                         : Vec3{std::sqrt(1 - z * z) * std::cos(angle),
                                std::sqrt(1 - z * z) * std::sin(angle), z};
      const Vec3 tangent = normalize(
          cross(n, std::abs(n.y) < .9f ? Vec3{0, 1, 0} : Vec3{1, 0, 0}));
      for (float cell : {.125f, .5f, 8.f}) {
        pv::Surface surface;
        pv::include_surface(surface, {}, n, {}, cell);
        require(!pv::surface_blocks(surface, {}, cell,
                                    n * .002f - tangent * (cell * .4f),
                                    n * .002f + tangent * (cell * .4f)),
                "clear2mm sloped grazing ray thickened into a false shadow");
        require(pv::surface_blocks(surface, {}, cell, n * -.002f, n * .002f),
                "high precision normal loses a real plane crossing");
      }
    }
    {
      // Actual production construction, not a manually authored occupied-cell
      // array: the old projected-centre sampler omitted cell(0,0,0) entirely.
      cb::Scene scene;
      MaterialDesc material;
      material.flags = 0;
      scene.materials.push_back(material);
      const Vec3 points[] = {{0, .3f, 0}, {100, 100.3f, 0}, {0, .3f, 100}};
      const Vec3 normal =
          normalize(cross(points[1] - points[0], points[2] - points[0]));
      for (auto p : points) {
        Vertex vertex{};
        vertex.position = p;
        vertex.normal = normal;
        scene.opaque.vertices.push_back(vertex);
      }
      scene.opaque.indices = {0, 1, 2};
      VoxelTransport transport;
      transport.origin = {};
      transport.nx = transport.ny = transport.nz = 4;
      transport.cell = .5f;
      transport.build(scene);
      require(transport.cells[transport.index(0, 0, 0)].material != 65535,
              "actual long-triangle build omitted its crossed depth cell");
      const pv::Grid grid{transport.origin, transport.cell, {4, 4, 4}};
      auto surface = [&](int x, int y, int z, Vec3 a, Vec3 b) {
        return pv::surface_blocks(
            transport.cells[transport.index(x, y, z)].surface,
            Vec3{(x + .5f) * .5f, (y + .5f) * .5f, (z + .5f) * .5f}, .5f, a, b);
      };
      require(pv::segment(Vec3{.1f, -1, .25f}, {}, Vec3{.1f, 2, .25f}, grid,
                          surface, nullptr, surface)
                  .blocked,
              "built sloped triangle failed finite-segment plane confirmation");
    }
    // Compare DDA against independent intersections with every occupied voxel,
    // using the same explicitly excluded endpoint cells, not implementation
    // step sequences. Random rays include outside starts and all octants.
    std::mt19937 rng(9271);
    std::uniform_real_distribution<float> coordinate(-3.f, 3.f);
    for (auto bit = std::size_t{0}; bit < field.cells.size(); ++bit)
      field.cells[bit] = (rng() % 79) == 0;
    std::size_t rays = 0;
    for (; rays < 1600; ++rays) {
      const Vec3 a{coordinate(rng), coordinate(rng), coordinate(rng)},
          b{coordinate(rng), coordinate(rng), coordinate(rng)};
      const auto ac = pv::cell_at(field.grid, a),
                 bc = pv::cell_at(field.grid, b);
      bool brute = false;
      for (int z = 0; z < 32 && !brute; ++z)
        for (int y = 0; y < 32 && !brute; ++y)
          for (int x = 0; x < 32 && !brute; ++x) {
            const std::array<int, 3> xyz{x, y, z};
            if (!field(x, y, z) || xyz == ac || xyz == bc)
              continue;
            const pv::Grid box{field.grid.origin +
                                   Vec3{float(x), float(y), float(z)} *
                                       field.grid.cell,
                               field.grid.cell,
                               {1, 1, 1}};
            const auto range = pv::interval(box, a, b - a);
            brute = range[1] > range[0];
          }
      require(trace(a, b).blocked == brute,
              "DDA disagrees with independent occupied-cell intersections");
    }
    std::cout
        << "PASS point visibility: finite endpoints, local contact, "
           "thin/oblique wall, "
           "fine precedence, unknown coverage, 30003 sloped-grazing controls, "
        << rays << " independent ray/occupied-box controls\n";
  } catch (const std::exception &error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
