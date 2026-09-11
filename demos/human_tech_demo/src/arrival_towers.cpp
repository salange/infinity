#include "arrival_towers.hpp"
#include "arrival_tower_lots.hpp"
#include <sstream>

namespace cb {
void stage_arrival_towers(Scene& scene) {
  for(const auto& tower:arrival_tower_placements()) {
    // Authoring specs rotate local +X toward +Z; native instances use the
    // opposite yaw convention. The imported geometry itself stays Y-up.
    add_asset_instance(scene,tower.resource,{tower.centre.x,tower.base_y,tower.centre.y},-tower.yaw);
    scene.roof_obstructions.push_back({arrival_tower_lot_footprint(tower.id),1.2f,tower.base_y+tower.height});
    ++scene.stats_towers;
  }
  const auto& hero=arrival_tower_placements()[3];
  add_asset_instance(scene,"arrival_hero_socket",{hero.centre.x,1.2f,hero.centre.y},-arrival_tower_base_yaw());
}
std::string arrival_towers_manifest() {
  std::ostringstream out;out<<"[";bool first=true;
  for(const auto& tower:arrival_tower_placements()) {
    if(!first)out<<',';
    first=false;
    out<<"{\"resource\":\""<<tower.resource<<"\",\"position\":["<<tower.centre.x<<','<<tower.base_y<<','<<tower.centre.y
       <<"],\"half_axes\":["<<tower.half_axes.x<<','<<tower.half_axes.y<<"],\"height\":"<<tower.height
       <<",\"instance_yaw\":"<<-tower.yaw<<",\"base_yaw\":"<<arrival_tower_base_yaw()
       <<",\"source\":\"Blender/glTF/native mesh\",\"socket_footprint\":[";
    const auto footprint=arrival_tower_lot_footprint(tower.id);
    for(std::size_t i=0;i<footprint.size();++i){if(i)out<<',';out<<'['<<footprint[i].x<<','<<footprint[i].y<<']';}
    out<<"]}";
  }
  out<<']';return out.str();
}
}
