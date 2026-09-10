#include "market_canopy.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace cb {
namespace {
constexpr float kTop=6.8f,kBacking=6.48f,kJoin=-124.f;
const std::array<Vec2,12> kReturn{{
  {0,6.64f},{.06f,6.49f},{.18f,6.34f},{.38f,6.24f},
  {.65f,6.21f},{.82f,6.21f},{1.f,6.30f},{1.15f,6.48f},
  {4.55f,6.48f},{4.72f,6.23f},{4.94f,6.23f},{5.f,6.48f}}};

std::vector<Vec2> cut_x(const std::vector<Vec2>& p,float x,bool greater) {
  std::vector<Vec2> result;if(p.empty())return result;
  auto a=p.back();bool ia=greater?a.x>=x:a.x<=x;
  for(auto b:p) {
    const bool ib=greater?b.x>=x:b.x<=x;
    if(ia!=ib)result.push_back({x,a.y+(b.y-a.y)*(x-a.x)/(b.x-a.x)});
    if(ib)result.push_back(b);
    a=b;ia=ib;
  }
  return result;
}
float front_x(const std::vector<Vec2>& plan,float z) {
  float x=-1e30f;
  for(std::size_t i=0;i<plan.size();++i) {
    const auto a=plan[i],b=plan[(i+1)%plan.size()];
    if(z<std::min(a.y,b.y)-.00001f||z>std::max(a.y,b.y)+.00001f)continue;
    if(std::abs(a.y-b.y)<.00001f)x=std::max({x,a.x,b.x});
    else x=std::max(x,a.x+(b.x-a.x)*std::clamp((z-a.y)/(b.y-a.y),0.f,1.f));
  }
  return x;
}
float underside(float depth) {
  depth=std::clamp(depth,0.f,5.f);
  for(std::size_t i=1;i<kReturn.size();++i)if(depth<=kReturn[i].x) {
    const Vec2 a=kReturn[i-1],b=kReturn[i];
    return a.y+(b.y-a.y)*(depth-a.x)/(b.x-a.x);
  }
  return kBacking;
}
void polygon(Scene& sc,std::uint32_t material,std::vector<Vec3> points,Vec3 outward) {
  if(points.size()<3)return;
  Emit e(&sc.opaque,material);
  for(std::size_t i=1;i+1<points.size();++i) {
    Vec3 a=points[0],b=points[i],c=points[i+1];
    const Vec3 normal=cross(b-a,c-a);
    if(dot(normal,normal)<1e-13f)continue;
    if(dot(normal,outward)<0)std::swap(b,c);
    e.triangle(a,b,c);
  }
}
void vertical_edge(Scene& sc,Vec2 a,Vec2 b,float ya,float yb,Vec3 outward) {
  polygon(sc,M_BRONZE,{{a.x,ya,a.y},{b.x,yb,b.y},{b.x,kTop,b.y},{a.x,kTop,a.y}},outward);
}
} // namespace

void build_market_canopy(Scene& sc) {
  const auto canopy=plan_rounded_rect(25,49,10,14,{-144,108});
  const auto rear=cut_x(canopy,kJoin,false);
  // One continuous retained upper surface. The old flat underside is removed;
  // its replacement is the formed shell and actual recessed panel backing.
  Emit(&sc.opaque,M_BRONZE).polygon(canopy,kTop,true);
  Emit(&sc.opaque,M_BRONZE).polygon(rear,kBacking,false);
  std::vector<float> zs;
  for(auto p:canopy)zs.push_back(p.y);
  for(std::size_t i=0;i<canopy.size();++i) {
    const Vec2 a=canopy[i],b=canopy[(i+1)%canopy.size()];
    if(std::abs(a.x-b.x)<.00001f)continue;
    for(Vec2 profile:kReturn) {
      const float x=kJoin+profile.x;
      if(x>=std::min(a.x,b.x)&&x<=std::max(a.x,b.x))
        zs.push_back(a.y+(b.y-a.y)*(x-a.x)/(b.x-a.x));
    }
  }
  std::sort(zs.begin(),zs.end());
  zs.erase(std::unique(zs.begin(),zs.end(),[](float a,float b){return std::abs(a-b)<.00001f;}),zs.end());
  for(std::size_t z=1;z<zs.size();++z) {
    const float za=zs[z-1],zb=zs[z],xa=front_x(canopy,za),xb=front_x(canopy,zb);
    if((xa+xb)*.5f<=kJoin||zb-za<.00001f)continue;
    for(std::size_t d=1;d<kReturn.size();++d) {
      const float da=kReturn[d-1].x,db=kReturn[d].x;
      auto patch=cut_x({{xa-da,za},{xb-da,zb},{xb-db,zb},{xa-db,za}},kJoin,true);
      std::vector<Vec3> bottom;
      for(auto p:patch)bottom.push_back({p.x,underside(front_x(canopy,p.y)-p.x),p.y});
      polygon(sc,M_BRONZE,std::move(bottom),{0,-1,0});
    }
    // The clipped front shell meets the rear backing without an open step.
    const float ya=underside(xa-kJoin),yb=underside(xb-kJoin);
    const float difference=(ya+yb)*.5f-kBacking;
    if(std::abs(difference)>.00001f)
      polygon(sc,M_BRONZE,{{kJoin,kBacking,za},{kJoin,kBacking,zb},
                           {kJoin,yb,zb},{kJoin,ya,za}},
              {difference>0?1.f:-1.f,0,0});
  }
  float area=0;for(std::size_t i=0;i<canopy.size();++i) {
    auto a=canopy[i],b=canopy[(i+1)%canopy.size()];area+=a.x*b.y-b.x*a.y;
  }
  for(std::size_t i=0;i<canopy.size();++i) {
    Vec2 a=canopy[i],b=canopy[(i+1)%canopy.size()];
    Vec3 outward{(b.y-a.y)*(area>0?1.f:-1.f),0,(a.x-b.x)*(area>0?1.f:-1.f)};
    if((a.x<kJoin)!=(b.x<kJoin)) {
      Vec2 join{kJoin,a.y+(b.y-a.y)*(kJoin-a.x)/(b.x-a.x)};
      vertical_edge(sc,a,join,a.x<kJoin?kBacking:kReturn[0].y,a.x<kJoin?kBacking:kReturn[0].y,outward);
      vertical_edge(sc,join,b,b.x<kJoin?kBacking:kReturn[0].y,b.x<kJoin?kBacking:kReturn[0].y,outward);
    } else vertical_edge(sc,a,b,a.x<kJoin?kBacking:kReturn[0].y,b.x<kJoin?kBacking:kReturn[0].y,outward);
  }
  Emit bronze(&sc.opaque,M_BRONZE),steel(&sc.opaque,M_DARK_METAL);
  // Six main bays and their secondary joists frame actual recessed ceiling
  // panels. A continuous beam transfers the bay loads into the ten diagonals.
  bronze.box({-121.6f,6.36f,108},{.15f,.12f,40});
  for(int station=0;station<=10;++station) {
    const float z=65+station*8.f;
    const float edge=std::min(front_x(canopy,z-.12f),front_x(canopy,z+.12f))-.95f;
    const float inner=-124.5f;
    bronze.box({(inner+edge)*.5f,6.35f,z},{(edge-inner)*.5f,.13f,station%2?.065f:.12f});
  }
  for(int bay=0;bay<5;++bay)for(float sign:{-1.f,1.f}) {
    const float t=(6.48f-1.8f)/(22.76f-1.8f);
    const float z=65+bay*16.f+(sign>0?.42f+15.16f*t:15.58f-15.16f*t);
    for(float side:{-1.f,1.f})
      bronze.box({-121.6f+side*.335f,6.345f,z},{.075f,.135f,.49f});
    bronze.box({-121.6f,6.48f,z},{.41f,.025f,.49f});
  }
  // Existing deep slats stop at the cornice joint. Their rail hangers reach
  // the real backing instead of floating below the former slab underside.
  for(int slat=0;slat<94;++slat)
    bronze.box({-141.5f,6.24f,65+slat*.92f},{17.5f,.035f,.026f});
  for(float x:{-155.f,-139.f,-124.15f}) {
    steel.box({x,6.3275f,108},{.045f,.0625f,43});
    for(int mount=0;mount<22;++mount) {
      const float z=66+mount*4.f;
      bronze.box({x,6.295f,z},{.07f,.065f,.07f});
      steel.box({x,6.415f,z},{.026f,.065f,.026f});
    }
  }
  // The cascading planting is rooted in supported soil at the actual edge.
  // Separate short channels leave the deck's longitudinal drainage open.
  Emit soil(&sc.opaque,M_SOIL);
  for(int i=0;i<12;++i) {
    const float z=72+i*6.f;
    bronze.box({-119.27f,6.85f,z},{.26f,.05f,1.30f});
    bronze.box({-119.50f,7.08f,z},{.03f,.18f,1.30f});
    for(float sign:{-1.f,1.f})
      bronze.box({-119.27f,7.08f,z+sign*1.27f},{.26f,.18f,.03f});
    // Two genuine filtered weep openings interrupt only the front wall.
    bronze.box({-119.04f,6.93f,z},{.03f,.03f,1.30f});
    bronze.box({-119.04f,7.15f,z},{.03f,.11f,1.30f});
    for(Vec2 interval:std::array<Vec2,3>{{{-1.30f,-.96f},{-.84f,.84f},{.96f,1.30f}}})
      bronze.box({-119.04f,7.f,z+(interval.x+interval.y)*.5f},
                 {.03f,.04f,(interval.y-interval.x)*.5f});
    for(float sign:{-1.f,1.f})for(float dz:{-.035f,.035f})
      steel.box({-119.032f,7.f,z+sign*.90f+dz},{.018f,.04f,.007f});
    soil.polygon(plan_rect(.20f,1.24f,{-119.27f,z}),7.20f,true);
  }
}
} // namespace cb
