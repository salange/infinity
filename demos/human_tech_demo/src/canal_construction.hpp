#pragma once
#include "scene.hpp"

namespace cb {
struct CanalCrossing {
  Vec2 a, b;
  float width;
};

// The caller supplies the existing terrain chord and actual road/parcel
// exclusions. This keeps the seed's canal, flood line and public floors exact.
void build_canal_bank(Scene& scene, Vec2 a, Vec2 b, float land_sign,
                      float floor_y, const std::vector<CanalCrossing>& crossings,
                      const std::vector<std::vector<Vec2>>& exclusions);

// Replaces only road()'s high slab and guard. Asphalt, approaches and piers
// remain owned by the road builder. a/b are its original surface centreline.
void build_canal_bridge_edge(Scene& scene, Vec3 a, Vec3 b, float width,
                             bool arterial, bool arrival_bridge = false);
void build_arrival_bridge_abutments(Scene& scene);
}  // namespace cb
