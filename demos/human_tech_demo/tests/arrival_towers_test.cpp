#include "arrival_tower_lots.hpp"
#include "arrival_towers.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <unordered_map>

using namespace cb;
namespace {
struct Triangle {Vec3 a,b,c;};
int cell(float p) {return static_cast<int>(std::floor(p/8));}
std::uint64_t key(int x,int z) {return std::uint64_t(std::uint32_t(x))<<32|std::uint32_t(z);}
bool contains(const Triangle& t,Vec3 p) {
  const float d=(t.b.z-t.c.z)*(t.a.x-t.c.x)+(t.c.x-t.b.x)*(t.a.z-t.c.z);
  if(std::abs(d)<1e-8f)return false;
  const float u=((t.b.z-t.c.z)*(p.x-t.c.x)+(t.c.x-t.b.x)*(p.z-t.c.z))/d;
  const float v=((t.c.z-t.a.z)*(p.x-t.c.x)+(t.a.x-t.c.x)*(p.z-t.c.z))/d;
  return u>=-.0001f&&v>=-.0001f&&u+v<=1.0001f;
}
struct Floors {
  std::vector<Triangle> triangles;
  std::unordered_map<std::uint64_t,std::vector<unsigned>> grid;
  void add(Triangle t) {
    if(std::abs(t.a.y-t.b.y)>.001f||std::abs(t.a.y-t.c.y)>.001f||cross(t.b-t.a,t.c-t.a).y<1e-8f)return;
    const auto id=static_cast<unsigned>(triangles.size());triangles.push_back(t);
    for(int z=cell(std::min({t.a.z,t.b.z,t.c.z}));z<=cell(std::max({t.a.z,t.b.z,t.c.z}));++z)
      for(int x=cell(std::min({t.a.x,t.b.x,t.c.x}));x<=cell(std::max({t.a.x,t.b.x,t.c.x}));++x)
        grid[key(x,z)].push_back(id);
  }
  bool supports(Vec3 p,float tolerance=.045f) const {
    const auto found=grid.find(key(cell(p.x),cell(p.z)));if(found==grid.end())return false;
    for(auto id:found->second)if(std::abs(triangles[id].a.y-p.y)<=tolerance&&contains(triangles[id],p))return true;
    return false;
  }
};
void require(bool value,const std::string& message) {if(!value)throw std::runtime_error(message);}
bool finite(Vec3 p) {return std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z);}
void check_mesh(const Mesh& mesh,std::size_t materials,const std::string& name) {
  require(mesh.indices.size()%3==0,name+": incomplete triangles");
  for(const auto& v:mesh.vertices) {
    const Vec3 tangent{v.tangent.x,v.tangent.y,v.tangent.z};
    require(finite(v.position)&&finite(v.normal)&&finite(tangent)&&
      std::isfinite(v.tangent.w+v.uv.x+v.uv.y+v.aux.x+v.aux.y+v.aux.z+v.aux.w),name+": nonfinite vertex");
    require(std::abs(length(v.normal)-1)<.002f&&std::abs(length(tangent)-1)<.002f&&
      std::abs(dot(v.normal,tangent))<.003f,name+": invalid normal/tangent frame");
    require(v.material<materials,name+": material remap outside palette");
  }
  for(std::size_t i=0;i<mesh.indices.size();i+=3) {
    for(int j=0;j<3;++j)require(mesh.indices[i+j]<mesh.vertices.size(),name+": invalid index");
    const auto& a=mesh.vertices[mesh.indices[i]];const auto& b=mesh.vertices[mesh.indices[i+1]];
    const auto& c=mesh.vertices[mesh.indices[i+2]];const auto n=cross(b.position-a.position,c.position-a.position);
    require(length(n)>1e-8f,name+": degenerate triangle");
    require(dot(n,a.normal+b.normal+c.normal)>=-1e-7f,name+": reversed geometric normal");
  }
}
void add_floors(Floors& floors,const Mesh& mesh,const AssetInstance* instance=nullptr) {
  auto position=[&](std::uint32_t i) {
    const auto p=mesh.vertices[i].position;return instance?asset_transform_point(*instance,p):p;
  };
  for(std::size_t i=0;i<mesh.indices.size();i+=3) {
    const auto a=position(mesh.indices[i]);if(a.y>13.3f||a.y<.59f)continue;
    floors.add({a,position(mesh.indices[i+1]),position(mesh.indices[i+2])});
  }
}
void check_room_coordinates(const cb::Scene& scene,const MeshResource& resource) {
  std::size_t occupied=0;
  for(const auto& v:resource.mesh.vertices)if(scene.materials[v.material].flags&kMatGlass) {
    ++occupied;
    require(std::abs(v.aux.y-v.position.y)<.002f,
      resource.name+": occupied room height is inverted or offset from actual local floors");
    if(std::abs(v.normal.y)<.9f) {
      const Vec3 tangent{v.tangent.x,v.tangent.y,v.tangent.z};
      const Vec3 upward=cross(v.normal,tangent)*v.tangent.w;
      require(upward.y>.1f,resource.name+": occupied facade tangent basis points downward");
    }
  }
  require(occupied>100,resource.name+": occupied coordinate audit found no facade");
}
void check_rooms(const cb::Scene& scene,const MeshResource& resource) {
  check_room_coordinates(scene,resource);
  struct Span {float u0=1e30f,u1=-1e30f,v0=1e30f,v1=-1e30f,y0=1e30f,y1=-1e30f;};
  std::map<std::uint32_t,Span> spans;
  for(const auto& v:resource.mesh.vertices)if(scene.materials[v.material].flags&kMatGlass) {
    auto& x=spans[v.material];x.u0=std::min(x.u0,v.aux.x);x.u1=std::max(x.u1,v.aux.x);
    x.v0=std::min(x.v0,v.aux.y);x.v1=std::max(x.v1,v.aux.y);
    x.y0=std::min(x.y0,v.position.y);x.y1=std::max(x.y1,v.position.y);
  }
  require(!spans.empty(),resource.name+": occupied glazing missing");
  float main_height=0,main_room=0;
  for(const auto& [id,x]:spans) {
    const auto& m=scene.materials[id];require(m.room_h>=3.5f&&m.room_h<=4.5f&&m.room_w>=2&&m.room_w<=5,
      resource.name+": occupied room dimensions lost during import");
    require(m.lit_probability>0&&m.lit_probability<1,resource.name+": occupied variation missing");
    const float h=x.y1-x.y0;
    if(h>main_height){main_height=h;main_room=m.room_h;}
    if(h>12) {
      require(x.u1-x.u0>m.room_w*8,resource.name+": circumference repeats one occupied room");
      require(std::abs((x.v1-x.v0)-h)<.10f,resource.name+": facade vertical UVs are not metre-scale");
    }
  }
  require(main_height>60,resource.name+": main occupied shaft absent");
  // Broad horizontal floors reveal the physical spacing independently of
  // the room recipe. Narrow crown/fascia rings cannot satisfy this area gate.
  std::map<int,double> horizontal_area;
  const auto& mesh=resource.mesh;
  for(std::size_t i=0;i<mesh.indices.size();i+=3) {
    const auto a=mesh.vertices[mesh.indices[i]].position,b=mesh.vertices[mesh.indices[i+1]].position,
               c=mesh.vertices[mesh.indices[i+2]].position;
    if(std::abs(a.y-b.y)>.001f||std::abs(a.y-c.y)>.001f||a.y<0)continue;
    const auto n=cross(b-a,c-a);if(n.y>0)horizontal_area[int(std::lround(a.y*1000))]+=n.y*.5;
  }
  const float expected_area=(mesh.bounds_max.x-mesh.bounds_min.x)*(mesh.bounds_max.z-mesh.bounds_min.z)*.19f;
  std::vector<float> levels;
  for(const auto& [millimetres,area]:horizontal_area)if(area>expected_area)levels.push_back(millimetres*.001f);
  int intervals=0;
  for(std::size_t i=0;i+1<levels.size();++i) {
    const float delta=levels[i+1]-levels[i];
    if(delta<3.5f||delta>4.5f)continue;
    require(std::abs(delta-main_room)<.055f,resource.name+": room height differs from actual floor plates");++intervals;
  }
  require(intervals>=12,resource.name+": insufficient actual occupied floors for room-spacing audit");
}
} // namespace

int main(int argc,char** argv) {
  try {
    require(argc==2||argc==3,"Provide the native base kit path and optional arrival kit path");
    const std::filesystem::path base=argv[1];
    const auto arrival=argc==3?std::filesystem::path(argv[2]):base.parent_path()/"arrival_towers.htkit";
    cb::Scene scene;scene.materials=make_materials();
    scene.asset_library=load_asset_library(base,static_cast<std::uint32_t>(scene.materials.size()));
    scene.materials.insert(scene.materials.end(),scene.asset_library.materials.begin(),scene.asset_library.materials.end());
    append_asset_library(scene,arrival);
    build_arrival_tower_lots(scene,root_rng("83"),true);stage_arrival_towers(scene);
    check_mesh(scene.opaque,scene.materials.size(),"occupied arrival lots");
    require(scene.opaque.bounds_min.y<=.601f,"Ground foundations do not meet the .65m terrain");
    Floors floors,soil,foundations;add_floors(floors,scene.opaque);
    for(std::size_t i=0;i<scene.opaque.indices.size();i+=3) {
      const auto& a=scene.opaque.vertices[scene.opaque.indices[i]];
      const auto b=scene.opaque.vertices[scene.opaque.indices[i+1]].position,
                 c=scene.opaque.vertices[scene.opaque.indices[i+2]].position;
      if(a.position.y<=.651f&&a.position.y>=.59f&&cross(b-a.position,c-a.position).y<0)
        foundations.add({a.position,c,b});
      if(scene.materials[a.material].name=="arrival district deep planted soil")
        soil.add({a.position,b,c});
    }
    std::size_t ground_contacts=0;
    for(std::size_t i=0;i<scene.opaque.indices.size();i+=3) {
      const auto a=scene.opaque.vertices[scene.opaque.indices[i]].position,
                 b=scene.opaque.vertices[scene.opaque.indices[i+1]].position,
                 c=scene.opaque.vertices[scene.opaque.indices[i+2]].position;
      if(std::abs(a.y-1.2f)>.001f||std::abs(b.y-1.2f)>.001f||std::abs(c.y-1.2f)>.001f||cross(b-a,c-a).y<=0)continue;
      for(auto p:{a,b,c,(a+b+c)*(1.f/3)}) {
        p.y=.60f;++ground_contacts;
        require(foundations.supports(p,.051f),"Ground paving/occupied foundation floats above the terrain");
      }
    }
    unsigned socket_count=0,towers=0;
    for(const auto& instance:scene.asset_instances) {
      const auto& resource=scene.asset_library.resources[instance.resource];
      if(resource.name!="arrival_hero_socket")continue;
      ++socket_count;check_mesh(resource.mesh,scene.materials.size(),resource.name);
      check_room_coordinates(scene,resource);add_floors(floors,resource.mesh,&instance);
    }
    require(socket_count==1,"Hero requires exactly one occupied rounded socket resource");
    std::size_t feet=0,roots=0,bearings=0;
    for(const auto& instance:scene.asset_instances) {
      const auto& resource=scene.asset_library.resources[instance.resource];
      if(resource.name=="arrival_hero_socket")continue;
      if(resource.name.starts_with("arrival_")) {
        ++towers;check_mesh(resource.mesh,scene.materials.size(),resource.name);check_rooms(scene,resource);
        std::set<std::pair<int,int>> unique;
        for(const auto& vertex:resource.mesh.vertices) {
          if(vertex.position.y>resource.mesh.bounds_min.y+.40f)continue;
          auto p=asset_transform_point(instance,vertex.position);
          if(!unique.insert({int(std::lround(p.x*1000)),int(std::lround(p.z*1000))}).second)continue;
          // The slab underside penetrates its supporting podium slightly;
          // query the common actual floor plane, not an ellipse surrogate.
          p.y=instance.translation.y;++feet;
          require(floors.supports(p),resource.name+": actual imported shaft foot lacks a supporting top-floor triangle");
        }
        require(unique.size()>16,resource.name+": imported ground-contact ring missing");
      } else {
        ++roots;require(soil.supports(instance.translation,.03f),resource.name+": root does not contact actual planted soil");
      }
    }
    require(towers==6,"Expected six distinct staged tower resources");
    require(roots>500,"Composed planted terraces are missing");
    for(const auto& vertex:scene.opaque.vertices) {
      if(vertex.normal.y>-.999f)continue;
      bool level=false;for(float y:{1.2f,5.2f,9.2f})if(std::abs(vertex.position.y-y)<.001f)level=true;
      if(!level)continue;
      ++bearings;
      require(floors.supports(vertex.position),"An occupied socket pier lacks an actual bearing floor");
    }
    require(bearings>100,"Occupied socket bearing audit found no structural feet");
    const auto& route=arrival_tower_public_walk();
    for(std::size_t i=0;i+1<route.size();++i) {
      const auto delta=route[i+1]-route[i];const int n=static_cast<int>(std::ceil(length(delta)/.25f));
      for(int j=0;j<=n;++j)for(float lateral:{-.45f,0.f,.45f}) {
        const auto side=normalize(Vec2{-delta.y,delta.x});const auto q=route[i]+delta*(float(j)/n)+side*lateral;
        require(floors.supports(P3(q,1.2f)),"Public detour lacks actual paving support");
        for(const auto& t:arrival_tower_placements())
          require(!point_in_polygon(arrival_tower_lot_footprint(t.id),q),"Public detour enters occupied socket");
      }
    }
    std::cout<<"Six imported detailed towers and rounded hero socket: "<<feet<<" actual shaft feet, "
      <<bearings<<" pier bearings, "<<roots<<" planted root contacts, "<<ground_contacts
      <<" terrain foundation contacts; native attributes, occupied room/floor spacing and supported detour passed\n";
  } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
