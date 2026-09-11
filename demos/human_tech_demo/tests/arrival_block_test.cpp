#include "arrival_tower_lots.hpp"
#include "arrival_towers.hpp"
#include "riverfront_layout.hpp"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace cb;
namespace {
void require(bool value,const std::string& message) {
  if(!value)throw std::runtime_error(message);
}
float signed_area(const std::vector<Vec2>& polygon) {
  float area=0;
  for(std::size_t i=0;i<polygon.size();++i) {
    const auto a=polygon[i],b=polygon[(i+1)%polygon.size()];
    area+=a.x*b.y-a.y*b.x;
  }
  return area*.5f;
}
bool inside_block(Vec2 p,float margin=0) {
  const auto q=riverfront::coordinates(p);
  return q.x>=218-margin&&q.x<=538+margin&&q.y>=18-margin&&q.y<=338+margin;
}
bool separate(const std::vector<Vec2>& a,const std::vector<Vec2>& b) {
  for(const auto* outline:{&a,&b})for(std::size_t i=0;i<outline->size();++i) {
    const auto delta=(*outline)[(i+1)%outline->size()]-(*outline)[i];
    const auto n=normalize(Vec2{-delta.y,delta.x});
    float amin=1e30f,amax=-1e30f,bmin=1e30f,bmax=-1e30f;
    for(auto p:a){amin=std::min(amin,dot(p,n));amax=std::max(amax,dot(p,n));}
    for(auto p:b){bmin=std::min(bmin,dot(p,n));bmax=std::max(bmax,dot(p,n));}
    if(amax+.03f<bmin||bmax+.03f<amin)return true;
  }
  return false;
}
bool aligned(Vec2 direction) {
  direction=normalize(direction);
  return std::min(std::abs(dot(direction,riverfront::across)),
                  std::abs(dot(direction,riverfront::along)))<.0008f;
}
bool on_road(Vec2 p,const ArrivalBlockRoad& road,float tolerance=.025f) {
  const auto direction=road.b-road.a;const float distance=length(direction);
  const auto along=direction*(1.f/distance);const Vec2 normal{-along.y,along.x};
  const float t=dot(p-road.a,along);
  return t>=-tolerance&&t<=distance+tolerance&&std::abs(dot(p-road.a,normal))<=road.width*.5f+tolerance;
}
std::vector<Vec2> road_footprint(const ArrivalBlockRoad& road) {
  const auto direction=normalize(road.b-road.a);const Vec2 side{-direction.y*road.width*.5f,direction.x*road.width*.5f};
  return {road.a-side,road.b-side,road.b+side,road.a+side};
}
struct GroundTriangle {Vec2 a,b,c;};
bool contains(const GroundTriangle& t,Vec2 p) {
  const float d=(t.b.y-t.c.y)*(t.a.x-t.c.x)+(t.c.x-t.b.x)*(t.a.y-t.c.y);
  if(std::abs(d)<1e-8f)return false;
  const float u=((t.b.y-t.c.y)*(p.x-t.c.x)+(t.c.x-t.b.x)*(p.y-t.c.y))/d;
  const float v=((t.c.y-t.a.y)*(p.x-t.c.x)+(t.a.x-t.c.x)*(p.y-t.c.y))/d;
  return u>=-.0001f&&v>=-.0001f&&u+v<=1.0001f;
}
void clear_geometry(cb::Scene& scene) {
  scene.opaque={};scene.foliage={};scene.asset_instances.clear();
  scene.draws.clear();scene.fine.clear();scene.lights.clear();
  scene.authored_roofs.clear();scene.roof_obstructions.clear();
}
unsigned physical_base_edges(const Mesh& mesh,const AssetInstance* instance,const std::string& name) {
  auto position=[&](std::uint32_t index) {
    const auto p=mesh.vertices[index].position;
    return instance?asset_transform_point(*instance,p):p;
  };
  unsigned aligned_edges=0;
  for(std::size_t i=0;i<mesh.indices.size();i+=3) {
    Vec3 triangle[3]{position(mesh.indices[i]),position(mesh.indices[i+1]),position(mesh.indices[i+2])};
    for(auto p:triangle)
      require(inside_block({p.x,p.z},.03f),name+": actual base geometry leaves the square block");
    if(std::min({triangle[0].y,triangle[1].y,triangle[2].y})<=1.09f&&
       std::max({triangle[0].y,triangle[1].y,triangle[2].y})>=1.09f) {
      const std::vector<Vec2> foundation{{triangle[0].x,triangle[0].z},
        {triangle[1].x,triangle[1].z},{triangle[2].x,triangle[2].z}};
      for(const auto& road:arrival_tower_block_roads())
        require(separate(foundation,road_footprint(road)),name+": actual foundation crosses a carriageway");
    }
    const auto n=cross(triangle[1]-triangle[0],triangle[2]-triangle[0]);
    if(std::abs(n.y)>length(n)*.02f)continue;
    for(int edge=0;edge<3;++edge) {
      const auto a=triangle[edge],b=triangle[(edge+1)%3];
      // Inspect real straight foundation-wall edges. Rounded-corner chords
      // are shorter; triangulation diagonals do not have equal elevations.
      if(std::abs(a.y-b.y)>.002f||a.y<.59f||a.y>1.25f)continue;
      const Vec2 d{b.x-a.x,b.z-a.z};if(length(d)<12)continue;
      require(aligned(d),name+": actual foundation edge is oblique to its streets");
      ++aligned_edges;
    }
  }
  return aligned_edges;
}
} // namespace

int main(int argc,char** argv) {
  try {
    require(argc==2,"Provide the native base kit path; the arrival kit must be adjacent");
    const auto block=arrival_tower_block_footprint();
    require(block.size()==4,"The district block must have exactly four square corners");
    require(std::abs(std::abs(signed_area(block))-320.f*320.f)<1,
            "The district footprint does not enclose the agreed 320m square");
    for(std::size_t i=0;i<block.size();++i) {
      const auto d=block[(i+1)%block.size()]-block[i];
      const auto next=block[(i+2)%block.size()]-block[(i+1)%block.size()];
      require(std::abs(length(d)-320)<.01f&&aligned(d)&&std::abs(dot(normalize(d),normalize(next)))<.0008f,
              "Block edges must be equal, perpendicular and parallel to the canal street survey");
      require(inside_block(block[i],.01f),"The block moved off its bridge/station survey anchor");
    }
    require(std::abs(arrival_tower_base_yaw()-std::atan2(riverfront::across.y,riverfront::across.x))<.0001f,
            "All occupied bases must use the street survey independently of shaft yaw");
    const auto& towers=arrival_tower_placements();
    const auto& roads=arrival_tower_block_roads();
    bool cross_street=false,hero_alley=false;
    for(const auto& road:roads) {
      require(road.width>0&&length(road.b-road.a)>1&&aligned(road.b-road.a),
              "A block street is degenerate or cuts diagonally through the survey");
      const auto a=riverfront::coordinates(road.a),b=riverfront::coordinates(road.b);
      if(std::abs(a.y-172)<.01f&&std::abs(b.y-172)<.01f) {
        require(std::abs(road.width-10)<.01f&&std::min(a.x,b.x)<=218&&std::max(a.x,b.x)>=538,
                "The group-separating street must cross the complete block");
        cross_street=true;
      }
      if(std::abs(a.x-325)<.01f&&std::abs(b.x-325)<.01f) {
        require(std::abs(road.width-6)<.01f&&std::min(a.y,b.y)<=172.01f&&std::max(a.y,b.y)>=351.99f,
                "The alley behind the graphite tower must join both cross streets");
        hero_alley=true;
      }
    }
    require(cross_street&&hero_alley,"The concept's group-separating street or parallel hero alley is missing");
    for(std::size_t i=0;i<towers.size();++i) {
      const auto footprint=arrival_tower_lot_footprint(towers[i].id);
      for(auto p:footprint)require(inside_block(p,.01f),std::string(towers[i].resource)+": base leaves the block");
      unsigned straight=0;
      for(std::size_t j=0;j<footprint.size();++j) {
        const auto d=footprint[(j+1)%footprint.size()]-footprint[j];
        if(length(d)>12){require(aligned(d),"A base retains an oblique clipped edge");++straight;}
      }
      require(straight==4,"Each uncut rounded rectangular base needs four straight street-aligned sides");
      for(std::size_t j=0;j<i;++j)
        require(separate(footprint,arrival_tower_lot_footprint(towers[j].id)),
                std::string(towers[i].resource)+" and "+std::string(towers[j].resource)+" have overlapping occupied bases");
      for(const auto& road:roads)
        require(separate(footprint,road_footprint(road)),std::string(towers[i].resource)+": a street passes through the occupied base");
      const auto centre=riverfront::coordinates(towers[i].centre);
      require(i<3?centre.y<172:centre.y>172,"The internal street no longer separates the left and right tower groups");
    }

    cb::Scene scene;scene.materials=make_materials();
    const std::filesystem::path kit=argv[1];
    scene.asset_library=load_asset_library(kit,static_cast<std::uint32_t>(scene.materials.size()));
    scene.materials.insert(scene.materials.end(),scene.asset_library.materials.begin(),scene.asset_library.materials.end());
    append_asset_library(scene,kit.parent_path()/"arrival_towers.htkit");
    unsigned foundation_edges=0;
    for(const auto& tower:towers) {
      clear_geometry(scene);
      build_arrival_tower_base(scene,root_rng("arrival-block-test").child(static_cast<int>(tower.id)),tower.id,false);
      if(tower.id==ArrivalLotId::Hero)stage_arrival_towers(scene);
      unsigned edges=physical_base_edges(scene.opaque,nullptr,std::string(tower.resource));
      unsigned imported_sockets=0;
      for(const auto& instance:scene.asset_instances) {
        const auto& resource=scene.asset_library.resources[instance.resource];
        if(resource.name=="arrival_hero_socket") {
          ++imported_sockets;edges+=physical_base_edges(resource.mesh,&instance,resource.name);
        }
      }
      if(tower.id==ArrivalLotId::Hero)require(imported_sockets==1,"The actual Blender socket must participate in the alignment check");
      require(edges>=4,std::string(tower.resource)+": no actual aligned foundation perimeter found");
      foundation_edges+=edges;
    }
    clear_geometry(scene);
    build_arrival_tower_lots(scene,root_rng("arrival-block-walk"),false);
    for(const auto& draw:scene.draws)for(std::size_t i=draw.first;i<draw.first+draw.count;++i)
      require(length(scene.opaque.vertices[scene.opaque.indices[i]].position-draw.centre)<=draw.radius+.02f,
              "The block's actual geometry exceeds its culling sphere");
    std::vector<GroundTriangle> floors;
    std::vector<GroundTriangle> asphalt;
    for(std::size_t i=0;i<scene.opaque.indices.size();i+=3) {
      const auto a=scene.opaque.vertices[scene.opaque.indices[i]].position,
                 b=scene.opaque.vertices[scene.opaque.indices[i+1]].position,
                 c=scene.opaque.vertices[scene.opaque.indices[i+2]].position;
      if(scene.opaque.vertices[scene.opaque.indices[i]].material==M_ASPHALT&&cross(b-a,c-a).y>0) {
        require(std::abs(a.y-1.09f)<.005f&&std::abs(b.y-1.09f)<.005f&&std::abs(c.y-1.09f)<.005f,
                "A block road no longer meets the existing street elevation");
        const std::vector<Vec2> triangle{{a.x,a.z},{b.x,b.z},{c.x,c.z}};
        for(const auto& tower:towers)
          require(separate(triangle,arrival_tower_lot_footprint(tower.id)),"Actual asphalt triangles pass underneath an occupied base");
        for(auto point:{a,b,c,(a+b+c)*(1.f/3),(a+b)*.5f,(b+c)*.5f,(c+a)*.5f}) {
          bool declared=false;for(const auto& road:roads)if(on_road({point.x,point.z},road)){declared=true;break;}
          require(declared,"Actual asphalt leaves the four perimeter streets and two intended internal lanes");
        }
        asphalt.push_back({{a.x,a.z},{b.x,b.z},{c.x,c.z}});
      }
      if(std::abs(a.y-1.2f)>.005f||std::abs(b.y-1.2f)>.005f||std::abs(c.y-1.2f)>.005f||cross(b-a,c-a).y<=0)continue;
      floors.push_back({{a.x,a.z},{b.x,b.z},{c.x,c.z}});
    }
    unsigned street_samples=0;
    for(const auto& road:roads) {
      const auto delta=road.b-road.a;const int steps=static_cast<int>(std::ceil(length(delta)/1.0f));
      const auto side=normalize(Vec2{-delta.y,delta.x});
      for(int i=0;i<=steps;++i)for(float width:{-.45f,0.f,.45f}) {
        const auto p=road.a+delta*(float(i)/steps)+side*(road.width*width);
        bool supported=false;for(const auto& triangle:asphalt)if(contains(triangle,p)){supported=true;break;}
        require(supported,"Declared street has a gap in its actual carriageway triangles");++street_samples;
      }
    }
    const auto& walk=arrival_tower_public_walk();require(walk.size()>=3,"Square block public walk is missing");
    unsigned walk_samples=0;
    for(std::size_t i=0;i+1<walk.size();++i) {
      const auto delta=walk[i+1]-walk[i];require(length(delta)>.001f,"Duplicate public walk point");
      require(aligned(delta),"A retained diagonal path cuts the square block");
      const auto side=normalize(Vec2{-delta.y,delta.x});const int steps=static_cast<int>(std::ceil(length(delta)/.5f));
      for(int step=0;step<=steps;++step)for(float lateral:{-.45f,0.f,.45f}) {
        const auto p=walk[i]+delta*(float(step)/steps)+side*lateral;
        bool supported=false;for(const auto& floor:floors)if(contains(floor,p)){supported=true;break;}
        require(supported,"Aligned public walk has no actual ground paving support");
        for(const auto& tower:towers)
          require(!point_in_polygon(arrival_tower_lot_footprint(tower.id),p),"Public walk enters an occupied base");
        ++walk_samples;
      }
    }
    std::cout<<"320m street-aligned square, six contained/disjoint rounded bases, "<<foundation_edges
             <<" actual aligned foundation edges, "<<street_samples<<" continuous street samples and "
             <<walk_samples<<" supported public walk samples passed\n";
  } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
