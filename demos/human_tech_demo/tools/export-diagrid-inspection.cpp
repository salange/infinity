#include "sculpted_diagrid.hpp"
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <stdexcept>

int main(int argc,char**argv) {
  if(argc!=2)return 2;
  const std::filesystem::path output(argv[1]);
  std::filesystem::create_directories(output);
  cb::Scene scene;scene.materials=cb::make_materials();
  const auto material=cb::sculpted_diagrid_ceramic(scene);
  cb::SculptedDiagridSpec spec;
  spec.first_column=2;spec.column_count=3;spec.first_level=13;spec.level_count=2;
  cb::build_sculpted_diagrid(scene,spec,[&](float){return material;});
  std::ofstream mesh(output/"diagrid.obj");mesh<<std::setprecision(9);
  for(const auto&v:scene.opaque.vertices)mesh<<"v "<<v.position.x<<' '<<v.position.y<<' '<<v.position.z<<'\n';
  for(const auto&v:scene.opaque.vertices)mesh<<"vn "<<v.normal.x<<' '<<v.normal.y<<' '<<v.normal.z<<'\n';
  std::uint32_t prior=~0u;
  for(std::size_t i=0;i<scene.opaque.indices.size();i+=3) {
    const auto mat=scene.opaque.vertices[scene.opaque.indices[i]].material;
    if(mat!=prior){mesh<<"g material_"<<mat<<'\n';prior=mat;}
    mesh<<'f';for(int j=0;j<3;++j){auto id=scene.opaque.indices[i+j]+1;mesh<<' '<<id<<"//"<<id;}mesh<<'\n';
  }
  std::ofstream metadata(output/"materials.json");metadata<<std::setprecision(9)<<'[';
  for(std::size_t i=0;i<scene.materials.size();++i) {
    const auto&m=scene.materials[i];if(i)metadata<<',';
    metadata<<"{\"name\":\""<<m.name<<"\",\"color\":["<<m.base_color.x<<','<<m.base_color.y<<','<<m.base_color.z<<"],\"metallic\":"<<m.metallic<<",\"roughness\":"<<m.roughness<<'}';
  }
  metadata<<"]\n";
  mesh.flush();
  metadata.flush();
  return mesh&&metadata?0:1;
}
