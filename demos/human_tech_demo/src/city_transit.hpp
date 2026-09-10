#pragma once
#include "city/math.hpp"
#include <vector>

namespace cb {
inline const inf::city::Vec2 kTransitStart{-68,265};
inline const inf::city::Vec2 kTransitEnd{-19,366};
inline const inf::city::Vec2 kTransitAlong=inf::city::normalize(kTransitEnd-kTransitStart);
inline const inf::city::Vec2 kTransitLeft{-kTransitAlong.y,kTransitAlong.x};
inline const float kTransitLength=inf::city::length(kTransitEnd-kTransitStart);
inline constexpr float kTransitDeckY=17.2f;
inline constexpr float kTransitWalkY=18.0f;
inline inf::city::Vec3 transit_point(float along,float left,float y) {
  const auto p=kTransitStart+kTransitAlong*along+kTransitLeft*left;
  return {p.x,y,p.y};
}
inline std::vector<inf::city::Vec2> transit_plan(float u0,float u1,float v0,float v1) {
  std::vector<inf::city::Vec2> result;
  for(const auto& uv:std::vector<inf::city::Vec2>{{u0,v0},{u1,v0},{u1,v1},{u0,v1}})
    result.push_back(kTransitStart+kTransitAlong*uv.x+kTransitLeft*uv.y);
  return result;
}
inline std::vector<std::vector<inf::city::Vec2>> transit_reserved_plans() {
  return {transit_plan(-2,kTransitLength+2,-10.8f,10.8f),
          transit_plan(-7,17,9.5f,21)};
}
} // namespace cb
