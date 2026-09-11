#include "scene.hpp"
#include "canal_tower.hpp"
#include "sculpted_diagrid.hpp"
#include <bit>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <stdexcept>

namespace {
void quoted(std::ostream& out,const std::string& value) {
  out<<'"';for(unsigned char c:value) {
    if(c=='"'||c=='\\')out<<'\\'<<c;
    else if(c=='\n')out<<"\\n";
    else if(c<32)throw std::runtime_error("Unexpected control character in inventory field");
    else out<<c;
  }out<<'"';
}
void vec(std::ostream& out,cb::Vec3 p){out<<'['<<p.x<<','<<p.y<<','<<p.z<<']';}
void u32(std::ostream& out,std::uint32_t n){for(int j=0;j<4;++j)out.put(char(n>>(j*8)));}
void f32(std::ostream& out,float n){u32(out,std::bit_cast<std::uint32_t>(n));}
void vertex(std::ostream& out,const cb::Vertex& v) {
  for(float n:{v.position.x,v.position.y,v.position.z,v.normal.x,v.normal.y,v.normal.z,
               v.tangent.x,v.tangent.y,v.tangent.z,v.tangent.w,v.uv.x,v.uv.y})f32(out,n);
  u32(out,v.material);
  for(float n:{v.aux.x,v.aux.y,v.aux.z,v.aux.w})f32(out,n);
}
}

int main(int argc,char** argv) {
  if(argc!=4)return 2;
  const std::string asset=argv[1];const std::filesystem::path output(argv[3]);
  std::filesystem::create_directories(output);
  auto scene=asset=="sculpted-lattice-module"?cb::make_sculpted_diagrid_sample("oblique"):
    cb::make_tower_inventory_sample(asset=="canal-crown"?"canal-round":asset,argv[2]);
  if(asset=="canal-crown") {
    // Exact existing crown triangles, including its closed lower deck. This
    // is an inspection subset of the integral tower, not a runtime resource.
    cb::Mesh crown;std::map<std::uint32_t,std::uint32_t> vertices;
    const float bottom=cb::CanalTowerSpec{}.crown_floor-.401f;
    for(std::size_t i=0;i<scene.opaque.indices.size();i+=3) {
      bool keep=true;for(int k=0;k<3;++k)
        keep&=scene.opaque.vertices[scene.opaque.indices[i+k]].position.y>=bottom;
      if(!keep)continue;
      for(int k=0;k<3;++k) {
        const auto old=scene.opaque.indices[i+k];
        if(!vertices.contains(old))vertices[old]=crown.add_vertex(scene.opaque.vertices[old]);
        crown.indices.push_back(vertices.at(old));
      }
    }
    scene.opaque=std::move(crown);
  }
  std::ofstream binary(output/"mesh.bin",std::ios::binary),json(output/"mesh.json");
  json<<std::setprecision(9)<<"{\"schema_version\":1,\"asset\":";quoted(json,asset);
  json<<",\"binary\":\"mesh.bin\",\"encoding\":\"little-endian 68-byte native vertices and uint32 indices\",\"materials\":[";
  for(std::size_t i=0;i<scene.materials.size();++i) {
    if(i)json<<',';const auto& m=scene.materials[i];json<<"{\"name\":";quoted(json,m.name);
    json<<",\"color\":";vec(json,m.base_color);json<<",\"roughness\":"<<m.roughness<<",\"metallic\":"<<m.metallic
        <<",\"emissive\":"<<m.emissive<<",\"normal_strength\":"<<m.normal_strength<<",\"flags\":"<<m.flags<<",\"texture\":";
    quoted(json,m.albedo_set);json<<",\"uv_scale\":"<<m.uv_scale<<",\"tint\":";vec(json,m.tint2);
    json<<",\"room\":["<<m.room_w<<','<<m.room_h<<','<<m.room_d<<','<<m.lit_probability<<"]}";
  }
  json<<"],\"meshes\":[";
  std::vector<const cb::Mesh*> meshes;std::vector<std::string> names;
  std::map<std::uint32_t,std::size_t> resources;
  auto add=[&](const cb::Mesh& mesh,const std::string& name) {
    auto index=meshes.size();meshes.push_back(&mesh);names.push_back(name);return index;
  };
  const auto opaque=add(scene.opaque,"generated opaque geometry"),foliage=add(scene.foliage,"generated foliage geometry");
  for(const auto& instance:scene.asset_instances)if(!resources.contains(instance.resource)) {
    const auto& resource=scene.asset_library.resources.at(instance.resource);
    resources[instance.resource]=add(resource.mesh,resource.name);
  }
  std::uint64_t unique_triangles=0,rendered_triangles=scene.opaque.triangle_count()+scene.foliage.triangle_count();
  for(std::size_t i=0;i<meshes.size();++i) {
    const auto& mesh=*meshes[i];if(i)json<<',';
    json<<"{\"name\":";quoted(json,names[i]);json<<",\"vertex_offset\":"<<binary.tellp()<<",\"vertex_count\":"<<mesh.vertices.size();
    for(const auto& v:mesh.vertices)vertex(binary,v);
    json<<",\"index_offset\":"<<binary.tellp()<<",\"index_count\":"<<mesh.indices.size()<<'}';
    for(auto index:mesh.indices)u32(binary,index);
    unique_triangles+=mesh.triangle_count();
  }
  json<<"],\"instances\":[{\"mesh\":"<<opaque<<",\"translation\":[0,0,0],\"yaw\":0,\"scale\":[1,1,1]},"
        "{\"mesh\":"<<foliage<<",\"translation\":[0,0,0],\"yaw\":0,\"scale\":[1,1,1]}";
  for(const auto& instance:scene.asset_instances) {
    json<<",{\"mesh\":"<<resources.at(instance.resource)<<",\"translation\":";vec(json,instance.translation);
    json<<",\"yaw\":"<<instance.yaw<<",\"scale\":";vec(json,instance.scale);json<<",\"tint\":";vec(json,instance.tint);json<<'}';
    rendered_triangles+=scene.asset_library.resources.at(instance.resource).mesh.triangle_count();
  }
  json<<"],\"unique_triangles\":"<<unique_triangles<<",\"rendered_triangles\":"<<rendered_triangles<<"}\n";
  binary.flush();json.flush();return binary&&json?0:1;
}
