#include "market_staging.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace cb {
namespace {
constexpr float kFloor=1.2f;
using Material=std::uint32_t;

Material named(const Scene& scene,std::string_view name) {
  for(std::uint32_t i=0;i<scene.materials.size();++i)
    if(scene.materials[i].name==name)return i;
  throw std::runtime_error("Market staging requires material: "+std::string(name));
}
Material surface(Scene& scene,const char* name,Vec3 colour,float roughness) {
  for(std::uint32_t i=0;i<scene.materials.size();++i)
    if(scene.materials[i].name==name)return i;
  MaterialDesc m;m.name=name;m.base_color=colour;m.roughness=roughness;
  scene.materials.push_back(m);return static_cast<Material>(scene.materials.size()-1);
}
struct Palette {
  Material wood,bronze,stone,ceramic,soil,leaf,paper,ink,orange,red,green,bread,
      cork,amber,glass,light,cloth;
  explicit Palette(Scene& scene):
    wood(named(scene,"wood_oiled")),bronze(named(scene,"bronze_satin")),
    stone(named(scene,"stone_warm")),ceramic(named(scene,"ceramic_ivory")),
    soil(named(scene,"soil_mulch")),leaf(named(scene,"leaf_middle")),
    paper(surface(scene,"market uncoated paper",{.63f,.54f,.38f},.86f)),
    ink(surface(scene,"market printed olive",{.055f,.073f,.027f},.75f)),
    orange(surface(scene,"market citrus peel",{.62f,.17f,.012f},.57f)),
    red(surface(scene,"market ripe red fruit",{.37f,.025f,.014f},.34f)),
    green(surface(scene,"market green pears",{.24f,.31f,.048f},.48f)),
    bread(surface(scene,"market baked crust",{.39f,.19f,.059f},.80f)),
    cork(surface(scene,"market natural cork",{.24f,.135f,.066f},.87f)),
    amber(surface(scene,"market amber preserve",{.26f,.105f,.022f},.29f)),
    glass(surface(scene,"market clear container",{.94f,.96f,.87f},.11f)),
    light(surface(scene,"market warm shelf diffuser",{1,.73f,.42f},.4f)),
    cloth(surface(scene,"market woven linen",{.44f,.37f,.26f},.93f)) {
    auto& pane=scene.materials[glass];pane.flags=128;pane.room_w=1.5f;
    pane.room_h=.88f;pane.room_d=.94f;pane.lit_probability=.004f;
    auto& diffuser=scene.materials[light];diffuser.flags=kMatEmissive;
    diffuser.emissive=1.8f;diffuser.tint2={1,.73f,.42f};
  }
};

void ellipsoid(Mesh& mesh,Material material,Vec3 centre,Vec3 radius,int rings=10,int sides=16) {
  const auto first=mesh.vertices.size();Emit(&mesh,material).sphere({0,0,0},1,rings,sides);
  for(std::size_t i=first;i<mesh.vertices.size();++i) {
    auto& v=mesh.vertices[i];
    v.position=centre+Vec3{v.position.x*radius.x,v.position.y*radius.y,v.position.z*radius.z};
    v.normal=normalize(Vec3{v.normal.x/radius.x,v.normal.y/radius.y,v.normal.z/radius.z});
    Vec3 t=v.tangent.xyz();t={t.x*radius.x,t.y*radius.y,t.z*radius.z};
    v.tangent={normalize(t-v.normal*dot(t,v.normal)),1};
  }
}

// Closed surfaces of revolution with analytic normals at curved shoulders.
// Profile points are (height,radius); zero-radius poles are cleaned on storage.
void vessel(Mesh& mesh,Material material,const std::vector<Vec2>& profile,int sides=24) {
  const auto first=static_cast<std::uint32_t>(mesh.vertices.size());
  for(std::size_t ring=0;ring<profile.size();++ring) {
    const Vec2 tangent=profile[std::min(ring+1,profile.size()-1)]-profile[ring?ring-1:0];
    const Vec2 normal=normalize(Vec2{tangent.x,-tangent.y});
    for(int i=0;i<=sides;++i) {
      float angle=2*kPi*i/sides,c=std::cos(angle),s=std::sin(angle);
      Vertex v;v.position={c*profile[ring].y,profile[ring].x,s*profile[ring].y};
      v.normal={c*normal.x,normal.y,s*normal.x};v.tangent={-s,0,c,1};
      v.uv={angle*.08f,profile[ring].x};v.material=material;v.aux={v.uv.x,v.uv.y,.5f,1};
      mesh.add_vertex(v);
    }
  }
  for(std::uint32_t ring=0;ring+1<profile.size();++ring)for(int i=0;i<sides;++i) {
    const auto a=first+ring*(sides+1)+i,b=a+sides+1;
    mesh.add_triangle(a,b,a+1);mesh.add_triangle(a+1,b,b+1);
  }
}

void resource(Scene& scene,const char* name,const std::function<void(Mesh&)>& build) {
  for(const auto& r:scene.asset_library.resources)if(r.name==name)return;
  MeshResource r;r.name=name;build(r.mesh);
  std::vector<std::uint32_t> indices;indices.reserve(r.mesh.indices.size());
  for(std::size_t i=0;i<r.mesh.indices.size();i+=3) {
    auto a=r.mesh.indices[i],b=r.mesh.indices[i+1],c=r.mesh.indices[i+2];
    if(length(cross(r.mesh.vertices[b].position-r.mesh.vertices[a].position,
                    r.mesh.vertices[c].position-r.mesh.vertices[a].position))>1e-10f)
      indices.insert(indices.end(),{a,b,c});
  }
  r.mesh.indices=std::move(indices);r.mesh.bounds_min={1e30f,1e30f,1e30f};
  r.mesh.bounds_max={-1e30f,-1e30f,-1e30f};
  for(const auto& v:r.mesh.vertices){r.mesh.bounds_min=vmin(r.mesh.bounds_min,v.position);r.mesh.bounds_max=vmax(r.mesh.bounds_max,v.position);}
  scene.asset_library.resources.push_back(std::move(r));
}

void make_resources(Scene& scene,const Palette& p) {
  resource(scene,"market_preserve_jar",[&](Mesh& mesh) {
    vessel(mesh,p.glass,{{0,0},{0,.062f},{.018f,.068f},{.18f,.068f},{.204f,.060f},{.21f,0}});
    vessel(mesh,p.amber,{{.012f,0},{.012f,.062f},{.16f,.062f},{.17f,0}});
    Emit e(&mesh,p.bronze);e.tube({0,.202f,0},{0,.227f,0},.071f,24,true);
    e.torus({0,.225f,0},{0,1,0},.065f,.006f,24,6);
    Emit(&mesh,p.paper).tube({0,.065f,0},{0,.145f,0},.069f,24,false);
    Emit(&mesh,p.ink).box({.0695f,.112f,0},{.0008f,.011f,.026f});
    Emit(&mesh,p.ink).box({.0695f,.087f,0},{.0008f,.002f,.019f});
  });
  resource(scene,"market_oil_bottle",[&](Mesh& mesh) {
    vessel(mesh,p.glass,{{0,0},{0,.044f},{.02f,.050f},{.25f,.050f},{.275f,.024f},{.335f,.022f},{.337f,0}});
    vessel(mesh,p.amber,{{.015f,0},{.015f,.045f},{.24f,.045f},{.25f,0}});
    Emit(&mesh,p.cork).tube({0,.328f,0},{0,.358f,0},.023f,16,true);
    Emit(&mesh,p.paper).tube({0,.085f,0},{0,.195f,0},.051f,24,false);
    Emit(&mesh,p.ink).box({.0514f,.143f,0},{.0008f,.020f,.020f});
    Emit(&mesh,p.bronze).torus({0,.29f,0},{0,1,0},.024f,.004f,18,6);
  });
  resource(scene,"market_paper_carton",[&](Mesh& mesh) {
    Emit e(&mesh,p.paper);e.box({0,.125f,0},{.061f,.125f,.09f});
    e.box({0,.252f,0},{.060f,.003f,.089f});
    Emit(&mesh,p.ink).box({.062f,.14f,0},{.001f,.035f,.047f});
    Emit(&mesh,p.bread).box({.0625f,.195f,0},{.001f,.014f,.024f});
    Emit(&mesh,p.ink).box({.062f,.06f,0},{.001f,.002f,.035f});
  });
  resource(scene,"market_cafe_cup",[&](Mesh& mesh) {
    vessel(mesh,p.ceramic,{{0,0},{0,.038f},{.075f,.045f},{.095f,.044f},{.095f,.038f},{.018f,.032f},{.018f,0}},24);
    Emit(&mesh,p.ceramic).torus({.049f,.052f,0},{0,0,1},.025f,.006f,18,8);
    Emit(&mesh,p.soil).tube({0,.077f,0},{0,.078f,0},.037f,20,true);
    Emit(&mesh,p.ceramic).tube({0,-.007f,0},{0,0,0},.075f,28,true);
  });
  resource(scene,"market_canvas_bag",[&](Mesh& mesh) {
    Emit e(&mesh,p.cloth);e.box({0,.18f,0},{.115f,.18f,.18f});
    for(float x:{-.08f,.08f}) {
      Emit(&mesh,p.cork).tube({x,.35f,-.09f},{x,.51f,-.07f},.013f,8,true);
      Emit(&mesh,p.cork).tube({x,.51f,-.07f},{x,.51f,.07f},.013f,8,true);
      Emit(&mesh,p.cork).tube({x,.51f,.07f},{x,.35f,.09f},.013f,8,true);
    }
    Emit(&mesh,p.ink).box({.116f,.205f,0},{.001f,.025f,.076f});
  });
  for(int kind=0;kind<4;++kind) {
    const std::string name="market_produce_crate_"+std::to_string(kind);
    resource(scene,name.c_str(),[&,kind](Mesh& mesh) {
      Emit wood(&mesh,p.wood),metal(&mesh,p.bronze);
      for(int i=0;i<7;++i)wood.box({0,.025f,-.48f+i*.16f},{.32f,.024f,.065f});
      for(float x:{-.325f,.325f})for(int row=0;row<3;++row)
        wood.box({x,.08f+row*.082f,0},{.018f,.031f,.57f});
      for(float z:{-.57f,.57f})for(int row=0;row<3;++row)
        wood.box({0,.08f+row*.082f,z},{.325f,.031f,.018f});
      for(float x:{-.30f,.30f})for(float z:{-.535f,.535f}){
        wood.box({x,.155f,z},{.023f,.155f,.023f});metal.sphere({x,.263f,z},.008f,4,8);
      }
      Rng rng=root_rng("market produce").child(kind);
      for(int layer=0;layer<2;++layer)for(int x=0;x<3;++x)for(int z=0;z<6;++z) {
        Vec3 centre{-.21f+x*.205f+layer*.02f,.13f+layer*.12f,-.45f+z*.18f};
        centre+=Vec3{rng.range(-.019f,.019f),rng.range(-.011f,.013f),rng.range(-.02f,.02f)};
        if(kind==3) {
          ellipsoid(mesh,p.bread,centre,{.082f,.055f,.076f},8,12);
          for(int cut=0;cut<3;++cut)Emit(&mesh,p.paper).box(centre+Vec3{-.038f+cut*.038f,.051f,0},{.006f,.002f,.039f});
        } else {
          Material mat=kind==0?p.orange:kind==1?p.red:p.green;
          ellipsoid(mesh,mat,centre,{.075f,kind==2?.087f:.07f,.073f},8,14);
          if(kind==2)ellipsoid(mesh,mat,centre+Vec3{0,.066f,0},{.04f,.057f,.037f},7,12);
          Emit(&mesh,p.cork).tube(centre+Vec3{0,kind==2?.11f:.061f,0},centre+Vec3{.01f,kind==2?.14f:.083f,0},.007f,6,true);
        }
      }
      wood.box({.35f,.20f,0},{.012f,.067f,.125f});
      Emit(&mesh,p.paper).box({.363f,.20f,0},{.001f,.047f,.108f});
      Emit(&mesh,p.ink).box({.365f,.211f,0},{.001f,.004f,.065f});
    });
  }
  resource(scene,"market_shelf_frame",[&](Mesh& mesh) {
    Emit wood(&mesh,p.wood),metal(&mesh,p.bronze),light(&mesh,p.light);
    wood.box({-.30f,1.72f,0},{.045f,1.72f,1.78f});
    for(float z:{-1.78f,1.78f})metal.box({.01f,1.75f,z},{.33f,1.75f,.025f});
    for(int shelf=0;shelf<6;++shelf) {
      const float y=.18f+shelf*.53f;wood.box({.01f,y,0},{.34f,.028f,1.80f});
      metal.box({.354f,y+.02f,0},{.018f,.018f,1.80f});
      light.box({.28f,y-.030f,0},{.033f,.006f,1.70f});
    }
    metal.box({0,3.48f,0},{.36f,.023f,1.81f});
  });
  resource(scene,"market_espresso_machine",[&](Mesh& mesh) {
    Emit metal(&mesh,p.bronze),stone(&mesh,p.stone),dark(&mesh,p.ink);
    stone.box({0,.036f,0},{.30f,.036f,.45f});metal.box({-.09f,.29f,0},{.22f,.23f,.43f});
    dark.box({.145f,.29f,0},{.018f,.17f,.365f});
    for(float z:{-.23f,.23f}) {
      metal.tube({.16f,.42f,z},{.26f,.36f,z},.044f,16,true);
      metal.tube({.26f,.36f,z},{.27f,.26f,z},.014f,10,true);
      metal.tube({.22f,.33f,z},{.39f,.33f,z},.022f,10,true);
      Emit(&mesh,p.cork).tube({.34f,.33f,z},{.45f,.33f,z},.024f,10,true);
    }
    for(int i=0;i<12;++i)metal.box({.23f,.087f,-.37f+i*.066f},{.10f,.006f,.013f});
    metal.torus({.153f,.49f,0},{1,0,0},.06f,.012f,24,8);
    Emit(&mesh,p.paper).tube({.154f,.49f,0},{.16f,.49f,0},.055f,24,true);
    dark.beam({.162f,.49f,0},{.162f,.525f,.018f},.006f,.005f);
  });
}

void shelf(Scene& scene,const Palette& p,Rng rng,Vec3 base) {
  add_asset_instance(scene,"market_shelf_frame",base);
  const std::array<const char*,3> products{{"market_preserve_jar","market_oil_bottle","market_paper_carton"}};
  for(int row=0;row<6;++row)for(int column=0;column<14;++column) {
    if((row*17+column)%19==3)continue;
    float z=-1.59f+column*.244f;
    const char* product=products[(row+column/4)%3];
    const float scale=rng.range(.86f,1.05f);
    add_asset_instance(scene,product,base+Vec3{.10f,.209f+row*.53f,z},
                       rng.range(-.13f,.13f),{scale,scale,scale},
                       {rng.range(.93f,1.04f),rng.range(.92f,1.03f),rng.range(.90f,1.01f)});
    if(row%2==0&&column%3==0)add_asset_instance(scene,product,base+Vec3{-.14f,.209f+row*.53f,z+.035f},.08f,.9f);
  }
  scene.lights.push_back({base+Vec3{.58f,2.68f,0},4.5f,{1,.72f,.40f},2.2f});
  Emit(&scene.opaque,p.bronze).box(base+Vec3{.36f,3.25f,0},{.017f,.065f,.58f});
}

void produce_stand(Scene& scene,const Palette& p,Vec3 base,int variant) {
  Emit wood(&scene.opaque,p.wood),bronze(&scene.opaque,p.bronze);
  for(float x:{-.39f,.39f})for(float z:{-1.65f,1.65f})
    wood.box(base+Vec3{x,.44f,z},{.041f,.44f,.041f});
  wood.box(base+Vec3{0,.83f,0},{.49f,.050f,1.90f});
  bronze.box(base+Vec3{.51f,.84f,0},{.022f,.023f,1.92f});
  wood.box(base+Vec3{0,.16f,0},{.43f,.035f,1.76f});
  for(int i=0;i<3;++i) {
    const std::string crate="market_produce_crate_"+std::to_string((i+variant)%4);
    add_asset_instance(scene,crate,base+Vec3{0,.88f,-1.22f+i*1.22f},0,1.f);
    if(i!=1)add_asset_instance(scene,"market_canvas_bag",base+Vec3{.10f,.20f,-1.2f+i*1.2f},.16f,.8f);
  }
}

void pot(Scene& scene,const Palette& p,Rng& rng,Vec3 base,bool tall) {
  Emit ceramic(&scene.opaque,p.ceramic),soil(&scene.opaque,p.soil);
  const float radius=tall?.38f:.28f,height=tall?.66f:.42f;
  ceramic.frustum(base,base+Vec3{0,height,0},radius*.73f,radius,28,false);
  ceramic.torus(base+Vec3{0,height,0},{0,1,0},radius,.025f,28,8);
  soil.polygon(plan_circle(radius-.027f,28,{base.x,base.z}),base.y+height-.045f,true);
  if(tall) {
    const auto& mesh=scene.asset_library.resources[asset_resource(scene.asset_library,"palm_fan")].mesh;
    const float scale=3.5f/mesh.bounds_max.y;
    add_asset_instance(scene,"palm_fan",base+Vec3{0,height-.045f,0},rng.range(-kPi,kPi),scale);
  } else {
    add_asset_instance(scene,"fern_arching",base+Vec3{0,height-.045f,0},rng.range(-kPi,kPi),.62f);
    add_asset_instance(scene,"climber_cascade",base+Vec3{radius*.6f,height-.02f,0},.8f,{.38f,.10f,.38f});
  }
}

void cafe(Scene& scene,const Palette& p,Rng rng) {
  for(float z:{116.f,121.f,126.f}) {
    Vec3 table{-140.2f,kFloor,z};add_asset_instance(scene,"street_table",table);
    for(float side:{-1.f,1.f})add_asset_instance(scene,"street_seat",table+Vec3{side*1.05f,0,.08f},side<0?kPi*.5f:-kPi*.5f);
    for(Vec3 offset:{Vec3{.20f,.807f,.18f},Vec3{-.23f,.807f,-.16f}})
      add_asset_instance(scene,"market_cafe_cup",table+offset,rng.range(-kPi,kPi));
    Emit(&scene.opaque,p.paper).box(table+Vec3{.10f,.815f,-.30f},{.11f,.014f,.15f});
    add_asset_instance(scene,"market_canvas_bag",table+Vec3{-.84f,0,.54f},-.2f);
    add_asset_instance(scene,"pendant_lamp",table+Vec3{0,5.39f,0});
    scene.lights.push_back({table+Vec3{0,3.9f,0},6.f,{1,.73f,.43f},3.4f});
  }
  Vec3 counter{-132.6f,kFloor,120.2f};
  add_asset_instance(scene,"market_counter",counter,kPi*.5f);
  add_asset_instance(scene,"market_espresso_machine",counter+Vec3{0,1.16f,0});
  for(int i=0;i<4;++i)add_asset_instance(scene,"market_preserve_jar",counter+Vec3{.04f,1.16f,-1.44f+i*.20f});
  for(int i=0;i<3;++i)add_asset_instance(scene,"market_cafe_cup",counter+Vec3{.12f,1.167f,.66f+i*.28f},.2f);
  pot(scene,p,rng,{-133.3f,kFloor,148.6f},true);
  pot(scene,p,rng,{-144.3f,kFloor,114.f},true);
  pot(scene,p,rng,{-124.8f,kFloor,124.5f},false);
  pot(scene,p,rng,{-124.8f,kFloor,149.f},false);
}
}  // namespace

void stage_cinematic_market(Scene& scene,Rng rng) {
  if(scene.asset_library.resources.empty())return;
  const Palette p(scene);make_resources(scene,p);
  // Keep the east front walk and the three entrance cross-aisles unobstructed.
  // Near shelves and stepped displays supply depth through real clear glazing.
  const std::array<float,11> bays{{70,76,88,94,100,112,118,124,136,142,148}};
  for(std::size_t i=0;i<bays.size();++i) {
    shelf(scene,p,rng.child(10+i),{-130.0f,kFloor,bays[i]});
    if(i<8)shelf(scene,p,rng.child(40+i),{-145.8f,kFloor,bays[i]});
  }
  produce_stand(scene,p,{-125.1f,kFloor,111.9f},0);
  produce_stand(scene,p,{-125.1f,kFloor,119.9f},2);
  // These two window displays sit at the actual shop threshold. Their front
  // edges remain behind the glazing, jambs and load-bearing street blades;
  // the existing cross-aisles and public promenade retain their clear width.
  produce_stand(scene,p,{-123.5f,kFloor,135.9f},0);
  produce_stand(scene,p,{-123.5f,kFloor,140.1f},2);
  for(float z:{91.f,115.f})produce_stand(scene,p,{-136.4f,kFloor,z},int(z)%4);
  cafe(scene,p,rng.child(80));
  // A folded linen runner and small checkout objects make the nearest counter
  // legible as an active shop without introducing people or moving assets.
  Emit(&scene.opaque,p.cloth).box({-124.585f,kFloor+.68f,118.4f},{.007f,.20f,.29f});
  for(float z:{111.8f,119.8f,135.8f,140.0f}) {
    add_asset_instance(scene,"pendant_lamp",{-124.8f,6.59f,z});
    scene.lights.push_back({{-124.8f,5.05f,z},6.f,{1,.73f,.40f},3.8f});
  }
}
}  // namespace cb
