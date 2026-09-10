#pragma once
#include "scene.hpp"
#include "city/rng.hpp"
#include <array>
#include <string_view>

namespace cb {
enum class ArrivalLotId { Hex, Blade, Ribbon, Hero, Bronze, Diamond };
struct ArrivalTowerPlacement {
  ArrivalLotId id;
  std::string_view resource;
  Vec2 centre;
  Vec2 half_axes;
  Vec2 top_lean;
  float height;
  float yaw;
  float base_y{13.2f};
};
const std::array<ArrivalTowerPlacement,6>& arrival_tower_placements();
// Occupied podium outlines. Positive setback moves every edge inward.
std::vector<Vec2> arrival_tower_lot_footprint(ArrivalLotId id,float setback=0);
// The district replaces the old fragmented infill within these ground areas.
std::vector<std::vector<Vec2>> arrival_tower_cleanup_footprints();
bool arrival_tower_lot_overlap(const std::vector<Vec2>& polygon,float margin=0);
const std::vector<Vec2>& arrival_tower_public_walk();
// Hero occupied socket is supplied by its separate mesh resource. The other
// five sockets, their composed gardens and the supported public walk are here.
void build_arrival_tower_lots(Scene& scene,Rng rng,bool detailed=true,
    const std::vector<std::vector<Vec2>>& ground_exclusions={});
} // namespace cb
