#include "arrival_tower_lots.hpp"
#include "city/flora.hpp"
#include "city/towers.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace cb {
namespace {
constexpr float ground=1.2f,roof=13.2f,lot_yaw=-.153776f;
const std::array<ArrivalTowerPlacement,6> placements{{
  {ArrivalLotId::Hex,"arrival_hex",{73.69f,82.40f},{19.71f,18.60f},{-3.07f,-1.43f},134.03f,-.27f},
  {ArrivalLotId::Blade,"arrival_blade",{52.32f,184.89f},{13.78f,4.92f},{-3.90f,-1.82f},127.21f,1.30f},
  {ArrivalLotId::Ribbon,"arrival_ribbon",{95.51f,207.87f},{20.34f,14.74f},{-1.64f,-.77f},153.83f,-.27f},
  {ArrivalLotId::Hero,"arrival_hero",{72.82f,301.39f},{28.80f,16.74f},{-2.50f,-1.17f},195.0f,-.27f},
  {ArrivalLotId::Bronze,"arrival_bronze",{145.65f,314.38f},{21.18f,16.94f},{-1.89f,-.88f},86.70f,-.27f},
  {ArrivalLotId::Diamond,"arrival_diamond",{271.17f,253.20f},{22.46f,20.05f},{-7.90f,-3.69f},150.35f,-.27f}
}};
struct Lot {Vec2 half;float corner,yaw;};
const std::array<Lot,6> lots{{
  {{33,28},6,lot_yaw},{{21,24},5,lot_yaw},{{30,29},6,lot_yaw},
  {{36.5f,26},8.2f,-.27f},{{27,29},6,lot_yaw},{{35,33},7,lot_yaw}
}};
std::size_t index(ArrivalLotId id) {return static_cast<std::size_t>(id);}
Vec2 rotate(Vec2 p,float a) {return {p.x*std::cos(a)-p.y*std::sin(a),p.x*std::sin(a)+p.y*std::cos(a)};}
bool overlap(const std::vector<Vec2>& a,const std::vector<Vec2>& b) {
  if(a.size()<3||b.size()<3)return false;
  auto separated=[&](const auto& p) {
    for(std::size_t i=0;i<p.size();++i) {
      const Vec2 d=p[(i+1)%p.size()]-p[i],axis{-d.y,d.x};
      float lo,hi,x,y;plan_extent(a,axis,&lo,&hi);plan_extent(b,axis,&x,&y);
      if(hi<x||y<lo)return true;
    }
    return false;
  };
  return !separated(a)&&!separated(b);
}
std::vector<Vec2> inset(std::vector<Vec2> p,float d) {
  const auto original=p;const float sign=plan_area(p)>0?1.f:-1.f;
  for(std::size_t i=0;i<original.size()&&p.size()>=3;++i) {
    const Vec2 e=original[(i+1)%original.size()]-original[i];
    if(length(e)<.001f)continue;
    const Vec2 n=normalize(Vec2{-e.y,e.x})*sign;
    p=clip_halfplane(p,original[i]+n*d,n);
  }
  bool changed=true;
  while(changed&&p.size()>3) {
    changed=false;
    for(std::size_t i=0;i<p.size();++i) {
      const Vec2 a=p[(i+p.size()-1)%p.size()],b=p[i],c=p[(i+1)%p.size()];
      const Vec2 u=b-a,v=c-b;
      if(length(u)<.002f||length(v)<.002f||std::abs(u.x*v.y-u.y*v.x)<.0005f*(length(u)+length(v))) {
        p.erase(p.begin()+static_cast<std::ptrdiff_t>(i));changed=true;break;
      }
    }
  }
  return p;
}
std::vector<Vec2> intersect(std::vector<Vec2> p,const std::vector<Vec2>& clip) {
  const float sign=plan_area(clip)>0?1.f:-1.f;
  for(std::size_t i=0;i<clip.size()&&p.size()>=3;++i) {
    const Vec2 d=clip[(i+1)%clip.size()]-clip[i];
    p=clip_halfplane(p,clip[i],Vec2{-d.y,d.x}*sign);
  }
  return p.size()>=3?inset(std::move(p),0):std::vector<Vec2>{};
}
std::vector<std::vector<Vec2>> subtract(std::vector<Vec2> p,const std::vector<Vec2>& obstacle) {
  if(!overlap(p,obstacle))return {std::move(p)};
  std::vector<std::vector<Vec2>> result;const float sign=plan_area(obstacle)>0?1.f:-1.f;
  for(std::size_t i=0;i<obstacle.size()&&p.size()>=3;++i) {
    const Vec2 d=obstacle[(i+1)%obstacle.size()]-obstacle[i],n=Vec2{-d.y,d.x}*sign;
    auto outside=inset(clip_halfplane(p,obstacle[i],n*-1.f),0);
    if(outside.size()>=3&&std::abs(plan_area(outside))>.30f)result.push_back(std::move(outside));
    p=clip_halfplane(p,obstacle[i],n);
  }
  return result;
}
float segment_distance(Vec2 p,Vec2 a,Vec2 b) {
  const Vec2 d=b-a;return length(p-(a+d*std::clamp(dot(p-a,d)/std::max(.001f,dot(d,d)),0.f,1.f)));
}
bool walk_clear(Vec2 p,float radius) {
  const auto& path=arrival_tower_public_walk();
  for(std::size_t i=0;i+1<path.size();++i)
    if(segment_distance(p,path[i],path[i+1])<radius+2)return false;
  return true;
}
bool shaft_clear(Vec2 p,float margin) {
  for(const auto& t:placements) {
    // Include the occupied envelope over the first two tree-height storeys.
    const Vec2 q=rotate(p-t.centre,-t.yaw);
    const float a=t.half_axes.x+margin+.6f,b=t.half_axes.y+margin+.6f;
    if(q.x*q.x/(a*a)+q.y*q.y/(b*b)<1)return false;
  }
  return true;
}
Mat material(Scene& s,Mat original,const char* name,Vec3 color,float roughness,float metallic=0) {
  auto m=s.materials[original];m.name=name;m.base_color=color;m.roughness=roughness;m.metallic=metallic;
  s.materials.push_back(m);return static_cast<Mat>(s.materials.size()-1);
}
struct Palette {
  Mat stone,fascia,bronze,glass,paving,soil,joint;
  explicit Palette(Scene& s) {
    stone=material(s,M_MARBLE_WHITE,"arrival district pale limestone",{.82f,.78f,.66f},.58f);
    fascia=material(s,M_WHITE_METAL,"arrival district formed ivory cornice",{.91f,.87f,.75f},.35f,.06f);
    bronze=material(s,M_BRONZE,"arrival district recessed bronze frames",{.21f,.14f,.075f},.34f,.56f);
    glass=material(s,M_GLASS_BRONZE,"arrival district warm occupied shopfronts",{.35f,.27f,.18f},.13f,.28f);
    s.materials[glass].room_h=4;s.materials[glass].room_w=3.8f;s.materials[glass].lit_probability=.47f;
    paving=material(s,M_TERRAZZO,"arrival district warm open terrace paving",{.77f,.74f,.64f},.70f);
    soil=material(s,M_SOIL,"arrival district deep planted soil",{.115f,.085f,.042f},.95f);
    joint=material(s,M_CONCRETE_DARK,"arrival district recessed stone joints",{.18f,.17f,.135f},.82f);
  }
};
bool has_resource(const Scene& s,std::string_view name) {
  return std::any_of(s.asset_library.resources.begin(),s.asset_library.resources.end(),
                    [&](const auto& r){return r.name==name;});
}
void rail(Scene& s,const std::vector<Vec2>& p,float y,Mat metal) {
  const auto edge=plan_sample(p,2.3f);
  for(std::size_t i=0;i<edge.points.size();++i) {
    const auto a=P3(edge.points[i],y),b=P3(edge.points[(i+1)%edge.points.size()],y);
    Emit(&s.opaque,metal).tube(a,b,.025f,6,true);
    Emit(&s.opaque,metal).tube(a+Vec3{0,1.05f,0},b+Vec3{0,1.05f,0},.032f,6,true);
    Emit(&s.opaque,metal).tube(a,a+Vec3{0,1.05f,0},.027f,6,true);
  }
}
void occupied_socket(Scene& s,const ArrivalTowerPlacement& t,const Palette& p,Rng rng) {
  const auto base=arrival_tower_lot_footprint(t.id);
  // The district terrain tops at .65 m. The foundation penetrates it by
  // five centimetres; facade/floor datums remain at the surveyed 1.2 m.
  slab(s.opaque,base,ground,ground-.60f,p.stone);
  // A founded service core and perimeter piers support every stepped floor.
  Emit(&s.opaque,p.stone).box(P3(t.centre,7.2f),{3.4f,6,3.4f});
  for(int floor=0;floor<3;++floor) {
    const float y=ground+floor*4;
    const auto outline=arrival_tower_lot_footprint(t.id,floor*1.2f);
    const auto skin=inset(outline,.38f),window=inset(outline,.57f);
    Emit pane(&s.opaque,p.glass);pane.element_random=rng.child(floor).next();
    pane.wall(window,y+.50f,y+3.55f,true);
    Emit(&s.opaque,p.bronze).wall(skin,y+.13f,y+.52f,true);
    Emit(&s.opaque,p.bronze).wall(skin,y+3.49f,y+3.79f,true);
    const auto bays=plan_sample(skin,3.6f);
    for(std::size_t bay=0;bay<bays.points.size();++bay) {
      const auto q=bays.points[bay],n=bays.normals[bay];const Vec3 across{-n.y,0,n.x},normal{n.x,0,n.y};
      Emit(&s.opaque,bay%3==0?p.stone:p.bronze).box(P3(q,y+2),
          {bay%3==0?.15f:.055f,2,.18f},across,{0,1,0},normal);
      if(bay%3==0) {
        const auto b=q+n*.16f;
        Emit(&s.opaque,M_LOBBY_LIGHT).box(P3(b,y+3.31f),{.31f,.026f,.045f},across,{0,1,0},normal);
      }
    }
    slab(s.opaque,outline,y+4,.32f,p.stone);
    Emit(&s.opaque,p.fascia).wall(outline,y+3.76f,y+4.04f,true);
    slab(s.opaque,inset(outline,.11f),y+4.015f,.025f,p.paving);
    s.roof_obstructions.push_back({outline,y-.01f,y+4.04f});
  }
  const auto upper=arrival_tower_lot_footprint(t.id,2.7f);
  rail(s,upper,roof+.04f,p.bronze);
  ++s.stats_blocks;++s.stats_standards;
}
void low_plant(Scene& s,Rng rng,Vec3 position,float size,bool flowering) {
  const char* name=flowering?"shrub_flowering":rng.chance(.24f)?"fern_arching":"groundcover";
  if(has_resource(s,name))add_asset_instance(s,name,position,rng.range(0,2*kPi),size);
  else {
    Emit leaf(&s.foliage,M_HEDGE);
    for(int stem=0;stem<6;++stem) {
      const float a=stem*2.39996f;const Vec3 d{std::cos(a),.5f,std::sin(a)},side{-d.z,0,d.x};
      const Vec3 end=position+d*size;
      leaf.quad_metric(position,position+side*size*.12f,end,end-side*size*.12f);
      leaf.quad_metric(position,end-side*size*.12f,end,position+side*size*.12f);
    }
  }
}
void tree(Scene& s,Rng rng,Vec3 position,float height,bool multi) {
  const char* name=multi?"tree_multistem":"canopy_broadleaf";
  if(has_resource(s,name)) {
    const auto& mesh=s.asset_library.resources[asset_resource(s.asset_library,name)].mesh;
    add_asset_instance(s,name,position,rng.range(0,2*kPi),height/std::max(.1f,mesh.bounds_max.y));
  } else gen_tree(s,rng,position,height);
}
void garden_bed(Scene& s,Rng rng,const std::vector<Vec2>& shape,float y,const Palette& p,bool detailed) {
  if(shape.size()<3||std::abs(plan_area(shape))<2)return;
  const auto soil=inset(shape,.18f);if(soil.size()<3)return;
  slab(s.opaque,shape,y+.38f,.38f,p.stone);slab(s.opaque,soil,y+.401f,.035f,p.soil);
  if(!detailed) {slab(s.opaque,inset(shape,.3f),y+.415f,.014f,M_GRASS);return;}
  Vec2 lo,hi;plan_bounds(soil,&lo,&hi);const auto roots=inset(soil,.15f);
  int row=0;
  for(float z=lo.y+.50f;z<hi.y-.3f;z+=1.0f,++row) {
    int column=0;
    for(float x=lo.x+.50f;x<hi.x-.3f;x+=1.1f,++column) {
      auto r=rng.child(1,row,column);const Vec2 q{x+r.range(-.16f,.16f),z+r.range(-.16f,.16f)};
      if(!point_in_polygon(roots,q)||!shaft_clear(q,1.55f)||!walk_clear(q,.6f))continue;
      low_plant(s,r.child(1),P3(q,y+.414f),r.range(.52f,.88f),(row+column)%17==0);
    }
  }
  const Vec2 c=plan_centroid(soil);
  if(plan_inradius(soil)>1.6f&&shaft_clear(c,5.0f)&&walk_clear(c,3.0f))
    tree(s,rng.child(2),P3(c,y+.414f),rng.child(3).range(4.8f,6.9f),rng.child(4).chance(.44f));
}
void roof_gardens(Scene& s,const ArrivalTowerPlacement& t,const Palette& p,Rng rng,bool detailed) {
  if(t.id==ArrivalLotId::Hero)return; // Its editable socket owns the clean roof apron.
  const auto upper=arrival_tower_lot_footprint(t.id,3.5f);
  const auto cfg=lots[index(t.id)];const Vec2 half=cfg.half-Vec2{4.7f,4.7f};
  std::vector<std::vector<Vec2>> beds;
  auto add=[&](Vec2 c,Vec2 h,float r) {
    auto shape=plan_transform(plan_rounded_rect(h.x,h.y,r,6),t.centre+rotate(c,cfg.yaw),cfg.yaw);
    shape=intersect(std::move(shape),upper);if(shape.size()<3)return;
    std::vector<std::vector<Vec2>> pieces{std::move(shape)};
    for(const auto& tower:placements) {
      const auto occupied=plan_superellipse(tower.half_axes.x+1.3f,tower.half_axes.y+1.3f,
                                            2,72,tower.centre,tower.yaw);
      std::vector<std::vector<Vec2>> next;
      for(auto& piece:pieces)for(auto part:subtract(std::move(piece),occupied))next.push_back(std::move(part));
      pieces=std::move(next);
    }
    for(auto& piece:pieces)beds.push_back(std::move(piece));
  };
  // Long planted edges leave the roof centre and each tower approach clear.
  for(float sign:{-1.f,1.f}) {
    add({0,sign*(half.y-1.3f)},{std::max(2.f,half.x-7.0f),1.25f},1.1f);
    add({sign*(half.x-1.25f),0},{1.15f,std::max(2.f,half.y-7.0f)},1.0f);
    for(float z:{-1.f,1.f})add({sign*(half.x-4.0f),z*(half.y-4.0f)},{2.5f,2.5f},1.5f);
  }
  for(std::size_t i=0;i<beds.size();++i)garden_bed(s,rng.child(i),beds[i],roof+.04f,p,detailed);
  // Sparse real paving joints remain on the open roof and stop at bed edges.
  Vec2 lo,hi;plan_bounds(upper,&lo,&hi);
  for(float z=lo.y+2;z<hi.y;z+=4)for(float x=lo.x+2;x<hi.x;x+=4) {
    const Vec2 q{x,z};if(!point_in_polygon(upper,q)||!shaft_clear(q,.6f))continue;
    bool planted=false;for(const auto& bed:beds)if(point_in_polygon(bed,q)){planted=true;break;}
    if(planted)continue;
    const auto line=intersect(plan_rect(.013f,1.4f,q),upper);
    if(line.size()>=3)slab(s.opaque,line,roof+.044f,.009f,p.joint);
  }
}
} // namespace

const std::array<ArrivalTowerPlacement,6>& arrival_tower_placements() {return placements;}
std::vector<Vec2> arrival_tower_lot_footprint(ArrivalLotId id,float setback) {
  const auto& t=placements[index(id)];const auto& cfg=lots[index(id)];
  auto p=plan_transform(plan_rounded_rect(cfg.half.x,cfg.half.y,cfg.corner,12),t.centre,cfg.yaw);
  // A shared clear pedestrian cut separates the neighboring small sockets.
  const Vec2 direction=normalize(placements[2].centre-placements[1].centre);
  const Vec2 middle=placements[1].centre+(placements[2].centre-placements[1].centre)*.44f;
  if(id==ArrivalLotId::Blade)p=clip_halfplane(p,middle-direction*2,direction*-1.f);
  if(id==ArrivalLotId::Ribbon) {
    p=clip_halfplane(p,middle+direction*2,direction);
    p=clip_halfplane(p,{123,0},{-1,0});
  }
  if(id==ArrivalLotId::Bronze) {
    // Leave a real lane beyond the hero's complete rounded corner envelope.
    const Vec2 normal=normalize(t.centre-placements[3].centre);
    const auto hero=arrival_tower_lot_footprint(ArrivalLotId::Hero);
    float lo,hi;plan_extent(hero,normal,&lo,&hi);
    p=clip_halfplane(p,normal*(hi+3.0f),normal);
  }
  return inset(std::move(p),setback);
}
std::vector<std::vector<Vec2>> arrival_tower_cleanup_footprints() {
  std::vector<std::vector<Vec2>> result;
  for(const auto& t:placements)result.push_back(arrival_tower_lot_footprint(t.id,-2.5f));
  // One quiet paved court joins the blade and ribbon gardens. It replaces
  // fragmented infill only; its public route remains open and supported.
  result.push_back(plan_rounded_rect(58,47,8,12,{74,196}));
  return result;
}
bool arrival_tower_lot_overlap(const std::vector<Vec2>& polygon,float margin) {
  for(const auto& t:placements)if(overlap(polygon,arrival_tower_lot_footprint(t.id,-margin)))return true;
  return false;
}
const std::vector<Vec2>& arrival_tower_public_walk() {
  static const std::vector<Vec2> path{{24,106.5f},{24,119},{24,194},{24,252},{124,252},{124,213},{128,213}};
  return path;
}
void build_arrival_tower_lots(Scene& s,Rng rng,bool detailed) {
  const Palette p(s);
  for(const auto& t:placements) {
    const auto begin=static_cast<std::uint32_t>(s.opaque.indices.size());
    const auto outer=arrival_tower_lot_footprint(t.id,-2.3f);
    slab(s.opaque,outer,ground,ground-.60f,p.paving);
    if(t.id!=ArrivalLotId::Hero)occupied_socket(s,t,p,rng.child(100+index(t.id)));
    roof_gardens(s,t,p,rng.child(200+index(t.id)),detailed);
    s.register_range(begin,static_cast<std::uint32_t>(s.opaque.indices.size()),P3(t.centre,8),65);
  }
  const auto& walk=arrival_tower_public_walk();
  const auto begin=static_cast<std::uint32_t>(s.opaque.indices.size());
  for(std::size_t i=0;i+1<walk.size();++i) {
    const Vec2 a=walk[i],b=walk[i+1],n=normalize(Vec2{-(b-a).y,(b-a).x})*2;
    slab(s.opaque,{a-n,b-n,b+n,a+n},ground,ground-.60f,p.paving);
  }
  for(auto q:walk)slab(s.opaque,plan_circle(2,24,q),ground,ground-.60f,p.paving);
  s.register_range(begin,static_cast<std::uint32_t>(s.opaque.indices.size()),{70,2,184},100);
}
} // namespace cb
