#pragma once
#include "city/math.hpp"
#include "riverfront_layout.hpp"
#include <vector>

namespace cb {
inline const inf::city::Vec2 kTransitStart=riverfront::point(riverfront::station_lateral,riverfront::station_start);
inline const inf::city::Vec2 kTransitAlong=riverfront::along;
inline const inf::city::Vec2 kTransitEnd=kTransitStart+kTransitAlong*riverfront::station_length;
inline const inf::city::Vec2 kTransitLeft{-kTransitAlong.y,kTransitAlong.x};
inline constexpr float kTransitLength=riverfront::station_length;
inline constexpr float kTransitDeckY=17.2f;
inline constexpr float kTransitWalkY=18.0f;
inline constexpr float kTransitRoofEaveY=22.45f;
inline constexpr float kTransitRoofCrownY=25.20f;
inline constexpr float kTransitRoofHalfWidth=10.05f;
inline constexpr float kTransitRoofGap=4.0f;
inline constexpr float kTransitTerminalLength=26.0f;
inline constexpr float kTransitAccessOffset=80.0f;
inline constexpr float kTransitTurnU=kTransitLength-7.0f;
inline constexpr float kTransitEndPlatformStart=kTransitTurnU-1.5f;
inline constexpr float kTransitRailEnd=kTransitEndPlatformStart-.7f;
inline const float kTransitHallEnd=kTransitLength-kTransitTerminalLength-kTransitRoofGap;
inline const float kTransitTerminalStart=kTransitHallEnd+kTransitRoofGap;
inline inf::city::Vec3 transit_point(float along,float left,float y) {
  const auto p=kTransitStart+kTransitAlong*along+kTransitLeft*left;
  return {p.x,y,p.y};
}
inline inf::city::Vec3 transit_access_point(float along,float left,float y) {
  return transit_point(along+kTransitAccessOffset,left,y);
}
inline std::vector<inf::city::Vec2> transit_plan(float u0,float u1,float v0,float v1) {
  std::vector<inf::city::Vec2> result;
  for(const auto& uv:std::vector<inf::city::Vec2>{{u0,v0},{u1,v0},{u1,v1},{u0,v1}})
    result.push_back(kTransitStart+kTransitAlong*uv.x+kTransitLeft*uv.y);
  return result;
}
inline std::vector<inf::city::Vec2> transit_access_plan(float u0,float u1,float v0,float v1) {
  return transit_plan(u0+kTransitAccessOffset,u1+kTransitAccessOffset,v0,v1);
}
inline std::vector<std::vector<inf::city::Vec2>> transit_reserved_plans() {
  return {transit_plan(-2,kTransitLength+2,-10.8f,10.8f),
          transit_access_plan(-7,17,9.5f,21),
          riverfront::rectangle(175,178,129,222),
          riverfront::rectangle(142.5f,178,218.5f,221.5f)};
}
} // namespace cb
