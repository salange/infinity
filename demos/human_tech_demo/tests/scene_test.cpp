// Headless validation of the authored set and its renderer-facing draw data.
#include <cmath>
#include <cstdio>
#include <vector>
#include "scene.hpp"

int main() {
  const cb::Scene sc=cb::generate_scene({});
  auto fail=[](const char* message) {std::fprintf(stderr,"%s\n",message);return 1;};
  for (const auto* mesh : {&sc.opaque,&sc.foliage}) {
    if(mesh->vertices.empty() || mesh->indices.size()%3) return fail("empty or incomplete mesh");
    for(const auto& v:mesh->vertices) {
      if(!std::isfinite(v.position.x+v.position.y+v.position.z+v.normal.x+v.normal.y+v.normal.z+v.uv.x+v.uv.y) || v.material>=sc.materials.size())
        return fail("invalid vertex or material");
    }
    for(auto index:mesh->indices) if(index>=mesh->vertices.size()) return fail("out of bounds mesh index");
  }
  std::vector<unsigned> levels(sc.lod_groups,0);
  std::size_t end=0;
  for(const auto& draw:sc.draws) {
    if(draw.first!=end || draw.count%3 || draw.first+draw.count>sc.opaque.indices.size()) return fail("draw ranges must partition the index buffer");
    end=draw.first+draw.count;
    if(draw.lod_group>=0) {
      if(draw.lod_group>=sc.lod_groups || draw.lod_level<0 || draw.lod_level>3) return fail("invalid LOD group");
      unsigned bit=1u<<draw.lod_level;
      if(levels[draw.lod_group]&bit) return fail("duplicate detail level");
      levels[draw.lod_group]|=bit;
    }
  }
  if(end!=sc.opaque.indices.size()) return fail("unregistered geometry");
  for(auto mask:levels) if(mask!=15) return fail("missing tower detail level");
  if(sc.stats_towers<100 || sc.stats_standards<500 || sc.lights.size()>64) return fail("city density or light budget regression");
  for(const char* shot:{"aerial","civic","street","terrace"}) {
    cb::Vec3 p,t;
    if(!cb::shot_camera(shot,p,t) || p.y<1.8f || cb::length(t-p)<1) return fail("invalid capture camera");
  }
  cb::Vec3 p,t;
  if(cb::shot_camera("missing",p,t)) return fail("unknown camera accepted");
  std::printf("valid set: %zu opaque triangles, %d towers, %d buildings, %zu draw ranges\n",sc.opaque.triangle_count(),sc.stats_towers,sc.stats_standards,sc.draws.size());
}
