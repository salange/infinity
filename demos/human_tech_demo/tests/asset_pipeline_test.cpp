#include "asset_pipeline.hpp"
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace cb;
int main(int argc,char** argv){
  try {
    if(argc!=2)throw std::runtime_error("provide compiled kit path");
    const auto kit=load_asset_library(argv[1],100);
    if(kit.resources.size()<53||kit.materials.size()<15)throw std::runtime_error("incomplete authored kit");
    for(const std::string species:{"canopy_broadleaf","canopy_columnar","tree_multistem","palm_fan","palm_feather",
                                    "shrub_flowering","fern_arching","phormium","climber_cascade","groundcover"}) {
      const auto& full=kit.resources.at(asset_resource(kit,species));
      const auto& medium=kit.resources.at(asset_resource(kit,species+"_mid"));
      if(medium.mesh.triangle_count()>full.mesh.triangle_count()*.4)throw std::runtime_error("medium resource exceeded simplification budget");
    }
    for(const auto& material:kit.materials)if(material.flags&128u)
      if(std::abs(material.room_w-1.5f)>.001f||std::abs(material.room_h-.86f)>.001f||std::abs(material.room_d-.18f)>.001f)
        throw std::runtime_error("authored glass optical factors were lost");
    const auto& node=kit.resources.at(asset_resource(kit,"ceramic_lattice_node"));
    if(node.mesh.bounds_max.y<6||node.mesh.bounds_max.z>1.2f)throw std::runtime_error("authoring metre/Y-up transform contract failed");
    for(const auto& mesh:kit.resources)for(const auto& v:mesh.mesh.vertices)
      if(v.material<100||v.material>=100+kit.materials.size())throw std::runtime_error("material remap failed");
    AssetInstance instance;instance.translation={10,20,30};instance.yaw=1.57079632679f;instance.scale={2,3,4};
    auto p=asset_transform_point(instance,{1,1,1});
    if(length(p-Vec3{14,23,28})>.0001f)throw std::runtime_error("instance transform failed");
    auto n=asset_transform_normal(instance,{0,0,1});
    if(length(n-Vec3{1,0,0})>.0001f)throw std::runtime_error("normal transform failed");
    bool missing=false;try{asset_resource(kit,"absent_resource");}catch(const std::runtime_error&){missing=true;}
    if(!missing)throw std::runtime_error("missing resource must fail");
    const auto corrupt=std::filesystem::temp_directory_path()/"human-tech-asset-corrupt.htkit";
    std::filesystem::copy_file(argv[1],corrupt,std::filesystem::copy_options::overwrite_existing);
    {std::fstream f(corrupt,std::ios::binary|std::ios::in|std::ios::out);f.seekp(-1,std::ios::end);f.put('\xff');}
    bool rejected=false;try{load_asset_library(corrupt);}catch(const std::runtime_error&){rejected=true;}
    std::filesystem::remove(corrupt);if(!rejected)throw std::runtime_error("corrupted SHA-256 payload accepted");
    std::cout<<kit.resources.size()<<" resources imported; metre/Y-up, material remap, instance transforms and integrity checked\n";
  }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
