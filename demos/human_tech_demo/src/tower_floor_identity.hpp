#pragma once
#include "city/towers.hpp"
#include <cstddef>
#include <vector>

namespace cb {
// Scene-owned copies keep each occupied shaft's room period independent of the
// shared palette, double-height lobbies and locally parameterized crowns.
class TowerFloorMaterials {
public:
  inf::city::Mat material(inf::city::Scene &scene, inf::city::Mat source,
                          float floor_height);

private:
  struct Entry {
    inf::city::Mat source, shaft;
    float floor_height;
    inf::city::MaterialDesc original;
  };
  std::vector<Entry> entries_;
};

struct TowerFloorSpan {
  std::size_t first_vertex{}, end_vertex{}, adjusted_vertices{};
  inf::city::Mat original{}, shaft{};
};

TowerFloorSpan build_floor_aligned_tower(inf::city::Scene &scene,
                                         TowerFloorMaterials &materials,
                                         inf::city::TowerSpec spec,
                                         inf::city::Vec2 centre, float base_y,
                                         inf::city::Rng rng, int detail);
} // namespace cb
