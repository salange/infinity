#pragma once
#include "city/math.hpp"
#include <vector>

namespace cb::riverfront {
using inf::city::Vec2;
// One metric survey for the bridge, canal-side block and parallel terminal.
// The origin is the retained nearest bridge's canal centre-line crossing.
inline const Vec2 origin{-237.779667f,140.776341f};
inline const Vec2 across{.9881997f,-.153171f};
inline const Vec2 along{.153171f,.9881997f};
inline Vec2 point(float lateral,float longitudinal) {
  return origin+across*lateral+along*longitudinal;
}
inline Vec2 coordinates(Vec2 p) {
  const auto d=p-origin;
  return {inf::city::dot(d,across),inf::city::dot(d,along)};
}
inline std::vector<Vec2> rectangle(float s0,float s1,float t0,float t1) {
  return {point(s0,t0),point(s1,t0),point(s1,t1),point(s0,t1)};
}
inline constexpr float lot_west=35.f,lot_east=146.f;
inline constexpr float lot_north=18.f,lot_south=190.f;
inline constexpr float avenue_lateral=194.f,avenue_width=30.f;
inline constexpr float station_lateral=164.f,station_start=144.f,station_length=128.f;
inline constexpr float access_row=108.f,stair_bottom=171.f,stair_run=.32f;
inline constexpr int stair_steps=72;
inline constexpr float ground=1.2f,tower_floor=13.2f;
} // namespace cb::riverfront
