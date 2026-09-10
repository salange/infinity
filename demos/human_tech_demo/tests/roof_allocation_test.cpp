// Roof programs must stand on the actual constructed slabs and remain clear
// of upper occupied floors. This catches the old plants-under-towers failure.
#include "scene.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>
using namespace cb;
namespace {
struct Triangle {Vec3 a,b,c;};
std::vector<Triangle> floors;
std::unordered_map<std::uint64_t,std::vector<unsigned>> cells;
int cell(float p) {return int(std::floor(p/16));}
std::uint64_t key(int x,int z) {return std::uint64_t(std::uint32_t(x))<<32|std::uint32_t(z);}
bool supported(Vec3 p) {
  const auto found=cells.find(key(cell(p.x),cell(p.z)));if(found==cells.end())return false;
  for(auto index:found->second) {
    const auto& t=floors[index];if(std::fabs(t.a.y-p.y)>.045f)continue;
    const float denominator=(t.b.z-t.c.z)*(t.a.x-t.c.x)+(t.c.x-t.b.x)*(t.a.z-t.c.z);
    if(std::fabs(denominator)<1e-8f)continue;
    const float u=((t.b.z-t.c.z)*(p.x-t.c.x)+(t.c.x-t.b.x)*(p.z-t.c.z))/denominator;
    const float v=((t.c.z-t.a.z)*(p.x-t.c.x)+(t.a.x-t.c.x)*(p.z-t.c.z))/denominator;
    if(u>=-1e-4f&&v>=-1e-4f&&u+v<=1.0001f)return true;
  }
  return false;
}
}
int main(int argc,char** argv) {
  if(argc!=2){std::fprintf(stderr,"The production roof audit requires the native asset kit path.\n");return 2;}
  SceneParams parameters;parameters.asset_kit=argv[1];const cb::Scene scene=generate_scene(parameters);
  float highest=0;for(const auto& roof:scene.authored_roofs)highest=std::max(highest,roof.y);
  for(std::size_t i=0;i<scene.opaque.indices.size();i+=3) {
    Vec3 a=scene.opaque.vertices[scene.opaque.indices[i]].position;
    Vec3 b=scene.opaque.vertices[scene.opaque.indices[i+1]].position;
    Vec3 c=scene.opaque.vertices[scene.opaque.indices[i+2]].position;
    if(a.y<3||a.y>highest+.1f||std::fabs(a.y-b.y)>.001f||std::fabs(a.y-c.y)>.001f)continue;
    if(cross(b-a,c-a).y<1e-7f)continue;
    const unsigned index=static_cast<unsigned>(floors.size());floors.push_back({a,b,c});
    for(int z=cell(std::min({a.z,b.z,c.z}));z<=cell(std::max({a.z,b.z,c.z}));++z)
      for(int x=cell(std::min({a.x,b.x,c.x}));x<=cell(std::max({a.x,b.x,c.x}));++x)cells[key(x,z)].push_back(index);
  }
  const std::set<std::string> original{"roof_vent","roof_solar_panel","roof_irrigation","roof_service_cabinet"};
  struct Volume {const RoofObstruction* shape;Vec2 lo,hi;};
  std::vector<Volume> volumes;
  for(const auto& obstruction:scene.roof_obstructions) {
    Vec2 lo,hi;plan_bounds(obstruction.polygon,&lo,&hi);volumes.push_back({&obstruction,lo,hi});
  }
  std::size_t instances=0,contacts=0,unsupported=0,unallocated=0,intersections=0;
  for(const auto& instance:scene.asset_instances) {
    const auto& resource=scene.asset_library.resources[instance.resource];
    if(!resource.name.starts_with("roof_")||original.contains(resource.name))continue;
    ++instances;
    const AuthoredRoof* surface=nullptr;
    for(const auto& roof:scene.authored_roofs)
      if(std::fabs(roof.y-instance.translation.y)<.02f&&point_in_polygon(roof.polygon,{instance.translation.x,instance.translation.z})) {surface=&roof;break;}
    if(!surface){if(unallocated++<8)std::printf("Unallocated roof assembly %s at %.3f %.3f %.3f\n",resource.name.c_str(),instance.translation.x,instance.translation.y,instance.translation.z);continue;}
    std::set<std::pair<int,int>> seen;
    for(const auto& vertex:resource.mesh.vertices) {
      const Vec3 p=asset_transform_point(instance,vertex.position);
      if(vertex.position.y<=resource.mesh.bounds_min.y+.015f&&seen.insert({int(std::lround(p.x*1000)),int(std::lround(p.z*1000))}).second) {
        ++contacts;if(!supported(p)) {
          if(unsupported++<12)std::printf("Unsupported %s foot %.3f %.3f %.3f\n",resource.name.c_str(),p.x,p.y,p.z);
        }
      }
    }
    // Assembly bounds and transformed vertices were fixture-tested by the
    // helper; this integration check uses the final city's occupied volumes.
    Vec3 minimum{1e30f,1e30f,1e30f},maximum{-1e30f,-1e30f,-1e30f};
    for(float x:{resource.mesh.bounds_min.x,resource.mesh.bounds_max.x})
      for(float y:{resource.mesh.bounds_min.y,resource.mesh.bounds_max.y})
        for(float z:{resource.mesh.bounds_min.z,resource.mesh.bounds_max.z}) {
          const Vec3 p=asset_transform_point(instance,{x,y,z});minimum=vmin(minimum,p);maximum=vmax(maximum,p);
        }
    for(const auto& volume:volumes) {
      const auto& obstruction=*volume.shape;
      if(volume.hi.x<minimum.x||volume.lo.x>maximum.x||volume.hi.y<minimum.z||volume.lo.y>maximum.z)continue;
      if(obstruction.top<=instance.translation.y+.05f||obstruction.bottom>=instance.translation.y+resource.mesh.bounds_max.y*instance.scale.y)continue;
      bool collision=false;
      for(const auto& vertex:resource.mesh.vertices) {
        const Vec3 p=asset_transform_point(instance,vertex.position);
        if(p.y>obstruction.bottom+.02f&&p.y<obstruction.top-.02f&&point_in_polygon(obstruction.polygon,{p.x,p.z})) {collision=true;break;}
      }
      if(collision) {
        if(intersections++<8)std::printf("Roof assembly enters an occupied upper floor: %s at %.2f %.2f %.2f\n",resource.name.c_str(),instance.translation.x,instance.translation.y,instance.translation.z);
        break;
      }
    }
  }
  std::printf("Roofs %zu; staged assemblies %zu; actual slab contacts %zu; unsupported %zu; unallocated %zu; upper-floor intersections %zu\n",scene.authored_roofs.size(),instances,contacts,unsupported,unallocated,intersections);
  return unsupported||unallocated||intersections||instances<20?1:0;
}
