#include "canal_tower.hpp"
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <cmath>
#include <cstdio>
#include <set>

int main(int argc,char** argv) {
  if(argc!=2)return 2;
  const std::filesystem::path output(argv[1]);std::filesystem::create_directories(output);
  cb::Scene scene;scene.materials=cb::make_materials();const cb::CanalTowerSpec spec;
  cb::build_canal_tower(scene,spec);
  int invalid=0,degenerate=0,opposed=0;
  std::set<int> occupied_room_columns;
  for(const auto& v:scene.opaque.vertices) {
    const cb::Vec3 tangent{v.tangent.x,v.tangent.y,v.tangent.z};
    if(!std::isfinite(v.position.x+v.position.y+v.position.z+v.normal.x+v.normal.y+v.normal.z+
                     v.uv.x+v.uv.y+v.aux.x+v.aux.y+v.aux.z+v.aux.w)||
       std::abs(cb::length(v.normal)-1)>.002f||std::abs(cb::length(tangent)-1)>.002f||
       std::abs(cb::dot(v.normal,tangent))>.003f||v.material>=scene.materials.size())++invalid;
    if(v.material<scene.materials.size()) {
      const auto& material=scene.materials[v.material];
      if(material.name=="canal tower graduated occupied glass")
        occupied_room_columns.insert(int(std::floor(v.aux.x/material.room_w)));
    }
  }
  if(occupied_room_columns.size()<2)++invalid;
  for(std::size_t i=0;i<scene.opaque.indices.size();i+=3) {
    const auto& a=scene.opaque.vertices[scene.opaque.indices[i]];
    const auto& b=scene.opaque.vertices[scene.opaque.indices[i+1]];
    const auto& c=scene.opaque.vertices[scene.opaque.indices[i+2]];
    const auto n=cb::cross(b.position-a.position,c.position-a.position);
    if(cb::length(n)<1e-8f)++degenerate;
    else if(cb::dot(n,a.normal+b.normal+c.normal)<-1e-7f)++opposed;
  }
  std::ofstream mesh(output/"canal-tower.obj");mesh<<std::setprecision(9);
  for(const auto& v:scene.opaque.vertices)mesh<<"v "<<v.position.x<<' '<<v.position.y<<' '<<v.position.z<<'\n';
  for(const auto& v:scene.opaque.vertices)mesh<<"vn "<<v.normal.x<<' '<<v.normal.y<<' '<<v.normal.z<<'\n';
  std::uint32_t previous=~0u;
  for(std::size_t i=0;i<scene.opaque.indices.size();i+=3) {
    const auto mat=scene.opaque.vertices[scene.opaque.indices[i]].material;
    if(mat!=previous){mesh<<"g material_"<<mat<<'\n';previous=mat;}
    mesh<<'f';for(int k=0;k<3;++k){auto n=scene.opaque.indices[i+k]+1;mesh<<' '<<n<<"//"<<n;}mesh<<'\n';
  }
  std::ofstream materials(output/"materials.json");materials<<std::setprecision(9)<<'[';
  for(std::size_t i=0;i<scene.materials.size();++i) {
    const auto& m=scene.materials[i];if(i)materials<<',';
    materials<<"{\"name\":\""<<m.name<<"\",\"color\":["<<m.base_color.x<<','<<m.base_color.y<<','<<m.base_color.z
      <<"],\"metallic\":"<<m.metallic<<",\"roughness\":"<<m.roughness<<'}';
  }
  materials<<"]\n";
  std::ofstream data(output/"geometry.json");data<<std::setprecision(9)
    <<"{\"native_origin_y_up\":["<<spec.centre.x<<",0,"<<spec.centre.y<<"],\"base_y\":"<<spec.base_y
    <<",\"base_radius\":"<<spec.base_radius<<",\"neck_radius\":"<<spec.neck_radius
    <<",\"crown_floor\":"<<spec.crown_floor<<",\"crown_radius\":"<<spec.crown_radius
    <<",\"crown_top\":"<<spec.crown_top<<",\"pavilion_radius\":"<<spec.pavilion_radius
    <<",\"pavilion_roof\":"<<spec.pavilion_roof<<",\"columns\":"<<spec.columns
    <<",\"cell_rows\":"<<spec.cell_rows<<",\"cell_height\":"<<spec.cell_height
    <<",\"vertices\":"<<scene.opaque.vertices.size()<<",\"triangles\":"<<scene.opaque.triangle_count()
    <<",\"occupied_room_columns\":"<<occupied_room_columns.size()
    <<",\"invalid_attributes\":"<<invalid<<",\"degenerate\":"<<degenerate<<",\"opposed\":"<<opposed<<"}\n";
  std::printf("Native tower: %zu triangles; invalid %d, degenerate %d, opposed %d\n",
      scene.opaque.triangle_count(),invalid,degenerate,opposed);
  mesh.flush();materials.flush();data.flush();
  return invalid||degenerate||opposed||!mesh||!materials||!data?1:0;
}
