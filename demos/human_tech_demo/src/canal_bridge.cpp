#include "canal_construction.hpp"
#include "canal_materials.hpp"
#include "riverfront_layout.hpp"
#include <algorithm>
#include <array>
#include <cmath>

namespace cb {
namespace {
using namespace canal_detail;
Vec3 at(Vec2 p, float y) { return {p.x, y, p.y}; }
struct ArrivalPalette {
  Material coping, diffuser;
  ArrivalPalette(Scene& scene, const Palette& p) {
    auto desc=scene.materials[p.stone];
    desc.name="arrival bridge pale cut limestone";
    desc.base_color=scene.reviewed_material_maps?Vec3{.98f,.95f,.82f}:Vec3{.72f,.70f,.63f};
    desc.roughness=.66f;desc.normal_strength=.18f;
    coping=material(scene,desc);
    desc=scene.materials[p.light];
    desc.name="arrival bridge recessed warm opal";
    desc.emissive=3.2f;desc.tint2={1,.74f,.40f};
    diffuser=material(scene,desc);
  }
};
template<std::size_t N>
void profile(Scene& scene, Material material, Vec3 a, Vec3 b, Vec3 outside,
             const std::array<Vec2,N>& section) {
  Emit emit(&scene.opaque,material);
  auto point=[&](Vec3 end,Vec2 p){return end+outside*p.x+kUp*p.y;};
  for(std::size_t i=0;i<N;++i) {
    const Vec2 p=section[i],q=section[(i+1)%N],d=q-p;
    const Vec3 normal=outside*-d.y+kUp*d.x;
    Vec3 aa=point(a,p),ab=point(a,q),ba=point(b,p),bb=point(b,q);
    if(dot(cross(ab-aa,bb-aa),normal)>0)emit.quad_metric(aa,ab,bb,ba);
    else emit.quad_metric(ab,aa,ba,bb);
  }
  for(int end=0;end<2;++end) {
    const Vec3 pos=end?b:a,normal=normalize(b-a)*(end?1.f:-1.f);
    for(std::size_t i=1;i+1<N;++i) {
      Vec3 p=point(pos,section[0]),q=point(pos,section[i]),r=point(pos,section[i+1]);
      if(dot(cross(q-p,r-p),normal)<0)std::swap(q,r);
      emit.triangle(p,q,r);
    }
  }
}
void arrival_parapet(Scene& scene,const Palette& p,Vec3 a,Vec3 b,float width) {
  const ArrivalPalette finish(scene,p);
  const Vec3 direction=normalize(b-a),side=normalize(cross(direction,kUp));
  const Vec3 slab_up=normalize(cross(side,direction));
  const bool use_x=std::abs(direction.x)>=std::abs(direction.z);
  const float start=use_x?a.x:a.z,end=use_x?b.x:b.z;
  Emit pale(&scene.opaque,finish.coping),bronze(&scene.opaque,p.bronze),
       dark(&scene.opaque,p.joints),light(&scene.opaque,finish.diffuser);
  // The knee is seated into the formed slab edge, including its outer return.
  // Its cap is only 0.39 m above the unchanged raised walking surface.
  const std::array<Vec2,6> knee{{{-.13f,-.08f},{.13f,-.08f},{.13f,.45f},
                                {.105f,.49f},{-.105f,.49f},{-.13f,.45f}}};
  const std::array<Vec2,6> cap{{{-.20f,.475f},{.20f,.475f},{.20f,.515f},
                               {.165f,.55f},{-.165f,.55f},{-.20f,.515f}}};
  for(float sign:{-1.f,1.f}) {
    const Vec3 offset=side*(sign*(width*.5f-.21f)),out=side*sign;
    profile(scene,finish.coping,a+offset,b+offset,out,knee);
    profile(scene,finish.coping,a+offset,b+offset,out,cap);
    // Real shield/opal/reveal layers face the walking side; the source is in
    // front of the diffuser and below its tiny hood, never inside the stone.
    for(int i=int(std::ceil(std::min(start,end)/4.8f));
        i<=int(std::floor(std::max(start,end)/4.8f));++i) {
      const float t=(i*4.8f-start)/(end-start);if(t<0||t>=1)continue;
      const Vec3 foot=lerp(a,b,t)+offset;
      const Vec3 inner=foot-out*.142f;
      dark.box(inner+kUp*.34f,{.65f,.075f,.014f},direction,slab_up,side);
      bronze.box(inner-out*.024f+kUp*.411f,{.69f,.021f,.056f},direction,slab_up,side);
      light.box(inner-out*.023f+kUp*.337f,{.54f,.035f,.010f},direction,slab_up,side);
      scene.lights.push_back({inner-out*.095f+kUp*.32f,3.2f,{1,.75f,.43f},2.6f});
    }
    // Slim intermediate verticals retain the open guard above the low knee.
    for(int i=int(std::ceil(std::min(start,end)/.6f));
        i<=int(std::floor(std::max(start,end)/.6f));++i) {
      if(i%4==0)continue; // Existing 2.4 m principal posts are unchanged.
      const float t=(i*.6f-start)/(end-start);if(t<0||t>=1)continue;
      const Vec3 foot=lerp(a,b,t)+side*(sign*(width*.5f-.12f));
      bronze.box(foot+kUp*.875f,{.014f,.335f,.016f},direction,slab_up,side);
    }
    // The inner curb has a rounded bronze nosing and seated drainage grates.
    const Vec3 curb=side*(sign*(width*.5f-2.36f));
    bronze.tube(a+curb+kUp*.165f,b+curb+kUp*.165f,.018f,8,true);
    for(int i=int(std::ceil(std::min(start,end)/7.2f));
        i<=int(std::floor(std::max(start,end)/7.2f));++i) {
      const float t=(i*7.2f-start)/(end-start);if(t<0||t>=1)continue;
      const Vec3 foot=lerp(a,b,t)+curb-side*(sign*.075f)+kUp*.009f;
      dark.box(foot,{.34f,.010f,.065f},direction,slab_up,side);
      for(int bar=0;bar<8;++bar)
        bronze.box(foot+direction*(-.30f+bar*.085f)+kUp*.010f,
                   {.016f,.009f,.058f},direction,slab_up,side);
    }
  }
  // Short diaphragms join both existing longitudinal ribs, with their upper
  // 25 mm seated in the unchanged slab. No central water pier is introduced.
  const float sa=riverfront::coordinates({a.x,a.z}).x;
  const float sb=riverfront::coordinates({b.x,b.z}).x;
  for(float station:{-18.f,-6.f,6.f,18.f}) {
    const float t=(station-sa)/(sb-sa);if(t<0||t>=1)continue;
    const Vec3 c=lerp(a,b,t)-kUp*.755f;
    pale.beam(c-side*(width*.31f+.18f),c+side*(width*.31f+.18f),.30f,.19f);
  }
}
void formed_edge(Scene& scene, Material material, Vec3 a, Vec3 b, Vec3 outside) {
  const std::array<Vec2, 6> profile{{{-.11f, -.035f}, {.065f, -.035f},
                                    {.11f, -.075f}, {.11f, -.16f},
                                    {.060f, -.205f}, {-.11f, -.205f}}};
  Emit emit(&scene.opaque, material);
  auto point = [&](Vec3 end, Vec2 p) { return end + outside * p.x + kUp * p.y; };
  for (std::size_t i = 0; i < profile.size(); ++i) {
    const Vec2 p = profile[i], q = profile[(i + 1) % profile.size()], d = q - p;
    const Vec3 normal = outside * -d.y + kUp * d.x;
    Vec3 aa = point(a, p), ab = point(a, q), ba = point(b, p), bb = point(b, q);
    if (dot(cross(ab - aa, bb - aa), normal) > 0) emit.quad_metric(aa, ab, bb, ba);
    else emit.quad_metric(ab, aa, ba, bb);
  }
  for (int end = 0; end < 2; ++end) {
    const Vec3 pos = end ? b : a, normal = normalize(b - a) * (end ? 1.f : -1.f);
    for (std::size_t i = 1; i + 1 < profile.size(); ++i) {
      Vec3 p = point(pos, profile[0]), q = point(pos, profile[i]), r = point(pos, profile[i + 1]);
      if (dot(cross(q - p, r - p), normal) < 0) std::swap(q, r);
      emit.triangle(p, q, r);
    }
  }
}
} // namespace

void build_canal_bridge_edge(Scene& scene, Vec3 a, Vec3 b, float width, bool arterial,
                             bool arrival_bridge) {
  const Palette p(scene);
  const Vec3 direction = normalize(b - a);
  const Vec3 side = normalize(cross(direction, kUp));
  const Vec3 slab_up = normalize(cross(side, direction));
  // Existing top is .035 m below asphalt and bottom .685 m below it.
  Emit stone(&scene.opaque, p.stone), bronze(&scene.opaque, p.bronze), dark(&scene.opaque, p.joints);
  // The formed edge replaces the outer 23 cm of the upper slab skin. Leaving
  // a full-width box behind it would bury its bevels and cause coplanar faces.
  stone.beam(a - kUp * .36f, b - kUp * .36f, width - .46f, .65f);
  stone.beam(a - kUp * .635f, b - kUp * .635f, width, .10f);
  const float walk_height=arrival_bridge?.16f:0.f;
  for (float sign : {-1.f, 1.f}) {
    const Vec3 offset = side * (sign * (width * .5f - .12f));
    const Vec3 backing = side * (sign * (width * .5f - .185f));
    dark.beam(a + backing - kUp * .385f, b + backing - kUp * .385f, .19f, .60f);
    // Layered edge stringers and their seated bronze nose remain within the
    // same slab depth. Longitudinal rails stay continuous across road pieces.
    bronze.beam(a + offset - kUp * .50f, b + offset - kUp * .50f, .16f, .22f);
    dark.beam(a + offset - kUp * .245f, b + offset - kUp * .245f, .18f, .065f);
    formed_edge(scene, p.stone, a + offset, b + offset, side * sign);
    if(arrival_bridge) {
      const Vec3 centre=side*(sign*(width*.5f-1.25f));
      stone.beam(a+centre+kUp*.08f,b+centre+kUp*.08f,2.2f,.16f);
      dark.beam(a+side*(sign*(width*.5f-2.36f))+kUp*.17f,
                b+side*(sign*(width*.5f-2.36f))+kUp*.17f,.065f,.024f);
      // A thin, continuous bearing rib keeps the span's underside readable.
      stone.beam(a+side*(sign*width*.31f)-kUp*.79f,
                 b+side*(sign*width*.31f)-kUp*.79f,.45f,.24f);
    }
    bronze.tube(a + offset + kUp * (1.08f+walk_height), b + offset + kUp * (1.08f+walk_height), .034f, 8, true);
    bronze.tube(a + offset + kUp * (.12f+walk_height), b + offset + kUp * (.12f+walk_height), .025f, 8, true);
    if (arterial)
      bronze.beam(a + offset + kUp * (.59f+walk_height), b + offset + kUp * (.59f+walk_height), .045f, .075f);
    else
      bronze.tube(a + offset + kUp * .55f, b + offset + kUp * .55f, .021f, 8, true);
    // Global dominant-axis stations prevent doubled posts at consecutive
    // 14 m slab seams. Half-open intervals assign each endpoint only once.
    const bool use_x = std::abs(direction.x) >= std::abs(direction.z);
    const float start = use_x ? a.x : a.z, end = use_x ? b.x : b.z;
    const float spacing = arterial ? 2.4f : 1.8f;
    const int first = static_cast<int>(std::ceil(std::min(start, end) / spacing));
    const int last = static_cast<int>(std::floor(std::max(start, end) / spacing));
    for (int i = first; i <= last; ++i) {
      const float t = (i * spacing - start) / (end - start);
      if (t < 0 || t >= 1) continue;
      const Vec3 base = lerp(a, b, t) + offset + kUp*walk_height;
      bronze.box(base + kUp * .025f, {.08f, .045f, .095f}, direction, slab_up, side);
      bronze.box(base + kUp * .55f, {.026f, .53f, .031f}, direction, slab_up, side);
      if (!arterial) {
        for (float delta : {-.42f, .42f})
          bronze.box(base + direction * delta + kUp * .59f, {.010f, .40f, .015f}, direction, slab_up, side);
      }
    }
    if(arrival_bridge) {
      for(int i=int(std::ceil(std::min(start,end)/1.8f));i<=int(std::floor(std::max(start,end)/1.8f));++i) {
        const float t=(i*1.8f-start)/(end-start);
        if(t<0||t>=1)continue;
        const Vec3 at=lerp(a,b,t)+side*(sign*(width*.5f-1.25f))+kUp*.171f;
        dark.box(at,{.012f,.004f,1.04f},direction,slab_up,side);
      }
      Emit light(&scene.opaque,p.light);
      for(int i=int(std::ceil(std::min(start,end)/18.f));i<=int(std::floor(std::max(start,end)/18.f));++i) {
        const float t=(i*18.f-start)/(end-start);
        if(t<0||t>=1)continue;
        const Vec3 foot=lerp(a,b,t)+side*(sign*(width*.5f-.38f))+kUp*walk_height;
        bronze.tube(foot,foot+kUp*3.65f,.036f,8,true);
        bronze.beam(foot+kUp*3.65f,foot+kUp*3.65f-side*(sign*.85f),.065f,.09f);
        const Vec3 diffuser=foot+kUp*3.59f-side*(sign*.66f);
        light.box(diffuser,{.12f,.025f,.19f},direction,slab_up,side);
        scene.lights.push_back({diffuser-kUp*.12f,9,{1,.76f,.46f},1.8f});
      }
    }
  }
  if(arrival_bridge)arrival_parapet(scene,p,a,b,width);
}
void build_arrival_bridge_abutments(Scene& scene) {
  const Palette p(scene);
  const ArrivalPalette finish(scene,p);
  Emit stone(&scene.opaque,p.stone),dark(&scene.opaque,p.joints),bronze(&scene.opaque,p.bronze);
  Emit pale(&scene.opaque,finish.coping),light(&scene.opaque,finish.diffuser);
  const Vec3 across{riverfront::across.x,0,riverfront::across.y};
  const Vec3 along{riverfront::along.x,0,riverfront::along.y};
  for(float sign:{-1.f,1.f}) {
    const Vec2 centre=riverfront::point(sign*24.f,0);
    stone.box(at(centre,1.8f),{2.45f,2.55f,10.1f},across,kUp,along);
    stone.box(at(centre,4.41f),{2.65f,.10f,10.3f},across,kUp,along);
    for(float side:{-6.2f,6.2f})
      bronze.box(at(centre,4.51f)+along*side,{1.9f,.075f,.35f},across,kUp,along);
    for(float y:{.45f,1.35f,2.25f,3.15f})
      dark.box(at(centre,y)-across*(sign*2.457f),{.008f,.014f,10.0f},across,kUp,along);
    // Face-mounted cut-stone ashlar courses dress the retained solid
    // abutment; shallow relief never narrows the canal's waterway.
    for(int row=0;row<4;++row)for(int bay=0;bay<6;++bay) {
      const float t=-8.30f+bay*3.32f;
      pale.box(at(centre,.89f+row*.9f)-across*(sign*2.46f)+along*t,
               {.027f,.414f,1.625f},across,kUp,along);
    }
    for(float t:{-8.7f,8.7f}) {
      const Vec3 face=at(centre,3.90f)-across*(sign*2.51f)+along*t;
      dark.box(face,{.025f,.22f,.32f},across,kUp,along);
      bronze.box(face+kUp*.24f-across*(sign*.02f),{.10f,.025f,.37f},across,kUp,along);
      light.box(face-across*(sign*.036f),{.012f,.13f,.22f},across,kUp,along);
      scene.lights.push_back({face-across*(sign*.14f),5,{1,.75f,.43f},3.2f});
    }
  }
}
} // namespace cb
