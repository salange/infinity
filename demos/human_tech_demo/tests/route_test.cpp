// Regression for real route support, headroom and opaque wall crossings.
// Samples the authored structural geometry instead of restating its dimensions.
#include "city_routes.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <fstream>
#include <iomanip>
#include <vector>
using namespace cb;
struct Triangle {Vec3 a,b,c;};
constexpr float cell=4,support_offset=.25f;
float loX=0,loZ=0;
int nx=0,nz=0;
std::vector<std::vector<unsigned>> grid;
std::vector<Triangle> triangles;
int ix(float x) {return int(std::floor((x-loX)/cell));}
int iz(float z) {return int(std::floor((z-loZ)/cell));}
bool in_grid(float x,float z) {return ix(x)>=0&&ix(x)<nx&&iz(z)>=0&&iz(z)<nz;}
bool configure_grid(const std::vector<SceneRoute>& routes) {
 float minX=1e30f,minZ=1e30f,maxX=-1e30f,maxZ=-1e30f;
 std::size_t points=0;
 for(const auto& route:routes)for(const auto& waypoint:route.waypoints) {
  const auto p=waypoint.position;if(!std::isfinite(p.x)||!std::isfinite(p.z))return false;
  minX=std::min(minX,p.x);maxX=std::max(maxX,p.x);
  minZ=std::min(minZ,p.z);maxZ=std::max(maxZ,p.z);++points;
 }
 if(!points)return false;
 // Cover every route and its lateral probes, with two extra cells for the
 // neighbouring-cell wall-crossing search and boundary roundoff.
 constexpr float margin=2*cell+support_offset;
 loX=std::floor((minX-margin)/cell)*cell;loZ=std::floor((minZ-margin)/cell)*cell;
 nx=int(std::ceil((maxX+margin-loX)/cell))+1;nz=int(std::ceil((maxZ+margin-loZ)/cell))+1;
 grid.resize(std::size_t(nx)*nz);
 std::printf("Route grid covers x[%.2f,%.2f), z[%.2f,%.2f), %d by %d cells\n",loX,loX+nx*cell,loZ,loZ+nz*cell,nx,nz);
 return true;
}
void triangle(Vec3 a,Vec3 b,Vec3 c) {
 if(std::max({a.y,b.y,c.y})<0||std::min({a.y,b.y,c.y})>291)return;
 int x0=ix(std::min({a.x,b.x,c.x})),x1=ix(std::max({a.x,b.x,c.x}));
 int z0=iz(std::min({a.z,b.z,c.z})),z1=iz(std::max({a.z,b.z,c.z}));
 if(x1<0||z1<0||x0>=nx||z0>=nz)return;
 unsigned n=triangles.size();triangles.push_back({a,b,c});
 for(int z=std::max(z0,0);z<=std::min(z1,nz-1);++z)
  for(int x=std::max(x0,0);x<=std::min(x1,nx-1);++x)grid[z*nx+x].push_back(n);
}
std::vector<float> hits(float x,float z) {
 int a=ix(x),b=iz(z);std::vector<float> ys;
 if(a<0||a>=nx||b<0||b>=nz)return ys;
 for(auto n:grid[b*nx+a]) {
  const auto& t=triangles[n];float den=(t.b.z-t.c.z)*(t.a.x-t.c.x)+(t.c.x-t.b.x)*(t.a.z-t.c.z);
  if(std::abs(den)<1e-8f)continue;
  float u=((t.b.z-t.c.z)*(x-t.c.x)+(t.c.x-t.b.x)*(z-t.c.z))/den;
  float v=((t.c.z-t.a.z)*(x-t.c.x)+(t.a.x-t.c.x)*(z-t.c.z))/den;
  if(u>=-1e-5f&&v>=-1e-5f&&u+v<=1.00001f)ys.push_back(u*t.a.y+v*t.b.y+(1-u-v)*t.c.y);
 }
 return ys;
}
bool crossing(Vec3 a,Vec3 b) {
 int cx=ix((a.x+b.x)*.5f),cz=iz((a.z+b.z)*.5f);
 Vec3 d=b-a;
 if(length(d)<1e-7f)return false;
 for(int z=std::max(0,cz-1);z<=std::min(nz-1,cz+1);++z)for(int x=std::max(0,cx-1);x<=std::min(nx-1,cx+1);++x)
  for(unsigned n:grid[z*nx+x]) {
   const auto& t=triangles[n];Vec3 e1=t.b-t.a,e2=t.c-t.a,q=cross(d,e2);float det=dot(e1,q);
   if(std::abs(det)<1e-8f)continue;
   Vec3 r=a-t.a;float u=dot(r,q)/det;if(u<0||u>1)continue;
   Vec3 h=cross(r,e1);float v=dot(d,h)/det;if(v<0||u+v>1)continue;
   float distance=dot(e2,h)/det;if(distance>.0001f&&distance<.9999f)return true;
  }
 return false;
}
int main(int argc,char** argv) {
 SceneParams params;if(argc>1)params.asset_kit=argv[1];
 const auto routes=scene_routes(!params.asset_kit.empty());
 if(!configure_grid(routes)){std::fprintf(stderr,"Cannot derive route grid from empty or invalid waypoint bounds\n");return 2;}
 const cb::Scene sc=generate_scene(params);
 // Optional read-only proof output shares this generated scene; no second
 // generation is needed for renderer light-list saturation analysis.
 if(argc>2) {
  std::ofstream out(argv[2]);if(!out){std::fprintf(stderr,"Cannot open light proof output.\n");return 2;}
  out<<std::setprecision(9)<<"{\"lights\":[";bool first=true;
  for(const auto& light:sc.lights) {
   if(!first)out<<',';
   first=false;out<<"{\"position\":["<<light.position.x<<','<<light.position.y<<','<<light.position.z
    <<"],\"radius\":"<<light.radius<<",\"color\":["<<light.color.x<<','<<light.color.y<<','<<light.color.z
    <<"],\"intensity\":"<<light.intensity<<'}';
  }
  out<<"],\"cameras\":[";first=true;
  for(const char* name:{"aerial","galaxy","civic","street","garden","landing"}) {
   Vec3 p,t;shot_camera(name,p,t);if(!first)out<<',';first=false;
   out<<"{\"id\":\""<<name<<"\",\"position\":["<<p.x<<','<<p.y<<','<<p.z
    <<"],\"target\":["<<t.x<<','<<t.y<<','<<t.z<<"],\"vertical_fov_degrees\":"<<shot_fov_degrees(name)<<'}';
  }
  out<<"]}\n";
 }
 for(std::size_t i=0;i<sc.opaque.indices.size();i+=3)
  triangle(sc.opaque.vertices[sc.opaque.indices[i]].position,sc.opaque.vertices[sc.opaque.indices[i+1]].position,sc.opaque.vertices[sc.opaque.indices[i+2]].position);
 // Native architecture participates in the same support/clearance audit.
 // Botanical volumes are excluded from rigid collision; visual branch and
 // leaf clearance remains part of the captured route review.
 for(const auto& inst:sc.asset_instances) {
  const auto& r=sc.asset_library.resources[inst.resource];
  if(r.name.find("canopy")!=std::string::npos||r.name.find("palm")!=std::string::npos||r.name.find("tree_")!=std::string::npos||r.name.find("shrub")!=std::string::npos||r.name.find("fern")!=std::string::npos||r.name.find("phormium")!=std::string::npos||r.name.find("groundcover")!=std::string::npos||r.name.find("climber")!=std::string::npos)continue;
  float radius=length(r.mesh.bounds_max-r.mesh.bounds_min)*std::max({inst.scale.x,inst.scale.y,inst.scale.z});
  if(inst.translation.x+radius<loX||inst.translation.x-radius>loX+nx*cell||inst.translation.z+radius<loZ||inst.translation.z-radius>loZ+nz*cell||inst.translation.y-radius>291)continue;
  auto transform=[&](Vec3 v){v={v.x*inst.scale.x,v.y*inst.scale.y,v.z*inst.scale.z};float c=std::cos(inst.yaw),s=std::sin(inst.yaw);return inst.translation+Vec3{v.x*c+v.z*s,v.y,-v.x*s+v.z*c};};
  for(std::size_t i=0;i<r.mesh.indices.size();i+=3)triangle(transform(r.mesh.vertices[r.mesh.indices[i]].position),transform(r.mesh.vertices[r.mesh.indices[i+1]].position),transform(r.mesh.vertices[r.mesh.indices[i+2]].position));
 }
 std::printf("Indexed %zu local structural triangles\n",triangles.size());
 int failures=0;
 // The new market outriggers must actually bear in the existing level-23
 // floor. Exclude their own named steel geometry from this contact proof.
 for(float source_z:{84.f,84.f,97.f,113.f,129.f,132.f}) {
  const float z=source_z-20.f;
  bool contact=false;
  for(std::size_t i=0;i<sc.opaque.indices.size()&&!contact;i+=3) {
   const auto& va=sc.opaque.vertices[sc.opaque.indices[i]];
   if(sc.materials[va.material].name.find("market")!=std::string::npos)continue;
   Vec3 a=va.position,b=sc.opaque.vertices[sc.opaque.indices[i+1]].position,c=sc.opaque.vertices[sc.opaque.indices[i+2]].position;
   if(std::abs(a.y-23)>.025f||std::abs(b.y-23)>.025f||std::abs(c.y-23)>.025f||cross(b-a,c-a).y<=1e-7f)continue;
   const float den=(b.z-c.z)*(a.x-c.x)+(c.x-b.x)*(a.z-c.z);if(std::abs(den)<1e-8f)continue;
   const float u=((b.z-c.z)*(-326-c.x)+(c.x-b.x)*(z-c.z))/den;
   const float v=((c.z-a.z)*(-326-c.x)+(a.x-c.x)*(z-c.z))/den;
   contact=u>=-1e-5f&&v>=-1e-5f&&u+v<=1.00001f;
  }
  std::printf("Market upper bearing x-326 y23 z%.0f: %s\n",z,contact?"supported":"UNSUPPORTED");
  if(!contact)++failures;
 }
 bool bridge_route=false,hex_route=false,loggia_route=false,west_promenade=false,east_promenade=false;
 for(const auto& route:routes) {
  bridge_route|=route.id=="arrival_bridge_to_market";hex_route|=route.id=="canal_hex_podium_access";
  loggia_route|=route.id=="garden_floor_loggia";
  west_promenade|=route.id=="arrival_west_promenade";east_promenade|=route.id=="arrival_east_promenade";
  if(route.id=="garden_access") {std::printf("Retired east ground access must not remain in route metadata\n");++failures;}
  if(route.id=="garden_floor_loggia")for(const auto& p:route.waypoints)
   if(p.position.y<278.9f){std::printf("Garden loggia incorrectly claims ground access\n");++failures;}
  int points=0,unsupported=0,blocked=0,crossed=0,outside_grid=0;float minClear=1e9,maxClear=0;
  for(std::size_t segment=0;segment+1<route.waypoints.size();++segment) {
   Vec3 a=route.waypoints[segment].position,b=route.waypoints[segment+1].position;
   int count=std::max(1,int(std::ceil(length(b-a)/.25f)));
   for(int sample=0;sample<=count;++sample) {
    Vec3 p=lerp(a,b,float(sample)/count);++points;
    if(!in_grid(p.x-support_offset,p.z-support_offset)||!in_grid(p.x+support_offset,p.z+support_offset)) {
      if(outside_grid++<12)std::printf("OUTSIDE ROUTE GRID %s segment%zu xyz %.3f %.3f %.3f; structural support was not evaluated\n",route.id.c_str(),segment,p.x,p.y,p.z);
      continue;
    }
    if(sample)for(float height:{-.9f,0.f}) {
      Vec3 previous=lerp(a,b,float(sample-1)/count)+Vec3{0,height,0},current=p+Vec3{0,height,0};
      if(crossing(previous,current)) {
        if(crossed++<16)std::printf("CROSSING %s segment%zu xyz %.3f %.3f %.3f height%.2f\n",route.id.c_str(),segment,p.x,p.y,p.z,height);
      }
    }
    for(Vec2 offset:std::array<Vec2,5>{{{0,0},{support_offset,0},{-support_offset,0},{0,support_offset},{0,-support_offset}}}) {
     auto ys=hits(p.x+offset.x,p.z+offset.y);float floor=-1e9;
     bool obstacle=false;
     for(float y:ys) {
      if(y<=p.y-.9f)floor=std::max(floor,y);
      if(y>p.y-1.55f&&y<p.y+.25f)obstacle=true;
     }
     float clearance=p.y-floor;minClear=std::min(minClear,clearance);maxClear=std::max(maxClear,clearance);
     if(clearance<1.58f||clearance>2.11f) {
      if(unsupported++<12)std::printf("SUPPORT %s segment%zu xyz %.3f %.3f %.3f offset%.2f %.2f floor%.3f clearance%.3f\n",route.id.c_str(),segment,p.x,p.y,p.z,offset.x,offset.y,floor,clearance);
     }
     if(obstacle) {
      if(blocked++<12)std::printf("BLOCKED %s segment%zu xyz %.3f %.3f %.3f offset%.2f %.2f\n",route.id.c_str(),segment,p.x,p.y,p.z,offset.x,offset.y);
     }
    }
   }
  }
  std::printf("%s: %d samples, clearance[%.4f,%.4f], support_failures%d, clearance_obstructions%d, wall_crossings%d, outside_grid%d\n",route.id.c_str(),points,minClear,maxClear,unsupported,blocked,crossed,outside_grid);
  failures+=unsupported+blocked+crossed+outside_grid;
 }
 if(!bridge_route||!hex_route||!loggia_route||!west_promenade||!east_promenade){std::printf("Missing current Arrival access contract\n");++failures;}
 return failures?1:0;
}
