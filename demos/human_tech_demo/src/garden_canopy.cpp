#include "garden_canopy.hpp"
#include "rng.hpp"

#include <array>
#include <cmath>
#include <stdexcept>
#include <string_view>

namespace cb {
namespace {
using Material=std::uint32_t;
Material named(const Scene& scene,std::string_view name) {
  for(std::size_t i=scene.materials.size();i>0;--i)
    if(scene.materials[i-1].name==name)return static_cast<Material>(i-1);
  throw std::runtime_error("Garden hero plants require authored material: "+std::string(name));
}
bool present(const Scene& scene,std::string_view name) {
  for(const auto& r:scene.asset_library.resources)if(r.name==name)return true;
  return false;
}

// Smooth, closed tapered wood with a true curved centreline and fine geometric
// bark ridges. Each branch has an explicit load path into its parent limb.
void branch(Mesh& mesh,Material material,const std::vector<Vec3>& path,
            float root_radius,float tip_radius,int sides=10) {
  const auto first=static_cast<std::uint32_t>(mesh.vertices.size());
  Vec3 previous_side{};float distance=0;
  for(std::size_t ring=0;ring<path.size();++ring) {
    const Vec3 tangent=normalize(path[std::min(ring+1,path.size()-1)]-path[ring?ring-1:0]);
    Vec3 side;
    if(ring)side=normalize(previous_side-tangent*dot(previous_side,tangent));
    else side=normalize(cross(tangent,std::fabs(tangent.y)<.92f?Vec3{0,1,0}:Vec3{1,0,0}));
    const Vec3 up=cross(tangent,side);previous_side=side;
    if(ring)distance+=length(path[ring]-path[ring-1]);
    const float t=float(ring)/float(path.size()-1);
    const float radius=root_radius*std::pow(1-t,.78f)+tip_radius*t;
    for(int j=0;j<=sides;++j) {
      float angle=j*2*kPi/sides;
      const Vec3 radial=side*std::cos(angle)+up*std::sin(angle);
      const float ridge=1+.025f*std::sin(angle*5+t*3.2f);
      Vertex v;v.position=path[ring]+radial*(radius*ridge);v.normal=radial;
      v.tangent={normalize(-side*std::sin(angle)+up*std::cos(angle)),1};
      v.uv={angle*root_radius,distance};v.aux={v.uv.x,v.uv.y,.5f,1};v.material=material;
      mesh.add_vertex(v);
    }
  }
  for(std::uint32_t ring=0;ring+1<path.size();++ring)for(int j=0;j<sides;++j) {
    auto a=first+ring*(sides+1)+j,b=a+sides+1;
    mesh.add_triangle(a,a+1,b);mesh.add_triangle(a+1,b+1,b);
  }
  for(int end=0;end<2;++end) {
    const auto ring=end?path.size()-1:0;
    const Vec3 normal=normalize(end?path.back()-path[path.size()-2]:path.front()-path[1]);
    Vertex centre;centre.position=path[ring];centre.normal=normal;centre.tangent={normalize(cross(normal,std::fabs(normal.y)<.92f?Vec3{0,1,0}:Vec3{1,0,0})),1};
    centre.material=material;centre.aux={0,0,.5f,1};
    const auto c=mesh.add_vertex(centre);
    for(int j=0;j<sides;++j) {
      Vertex a=mesh.vertices[first+ring*(sides+1)+j],b=mesh.vertices[first+ring*(sides+1)+j+1];
      a.normal=normal;b.normal=normal;const auto ia=mesh.add_vertex(a),ib=mesh.add_vertex(b);
      if(end)mesh.add_triangle(c,ia,ib);else mesh.add_triangle(c,ib,ia);
    }
  }
}

std::vector<Vec3> curve(Vec3 a,Vec3 b,Vec3 c,Vec3 d,int steps) {
  std::vector<Vec3> result;result.reserve(steps+1);
  for(int i=0;i<=steps;++i) {
    const float t=float(i)/steps,u=1-t;
    result.push_back(a*(u*u*u)+b*(3*u*u*t)+c*(3*u*t*t)+d*(t*t*t));
  }
  return result;
}

// Each leaf is a fresh curved, folded blade with metric UVs and smooth normals.
// Explicit back faces preserve its silhouette in every renderer pass. No
// source leaf or entire crown is scaled nonuniformly to create the spread.
void leaf(Mesh& mesh,Material material,Vec3 base,Vec3 tip,float half_width,
          float curl,float roll,int segments=4,bool broad=false) {
  const Vec3 d=tip-base,axis=normalize(d);
  Vec3 side=cross(d,{0,1,0});if(length(side)<1e-5f)side={1,0,0};side=normalize(side);
  side=side*std::cos(roll)+cross(axis,side)*std::sin(roll);
  std::vector<Vertex> vertices;std::vector<std::uint32_t> indices;
  vertices.reserve((segments+1)*3);
  for(int j=0;j<=segments;++j) {
    const float t=float(j)/segments,arc=std::max(0.f,std::sin(kPi*t));
    const Vec3 centre=base+d*t+Vec3{0,arc*curl,0};
    const float width=std::pow(arc,broad?.60f:.85f)*half_width;
    for(int s=-1;s<=1;++s) {
      const float asymmetry=1+s*.08f*std::sin(kPi*t+.4f);
      Vertex v;v.position=centre+side*(width*s*asymmetry)+Vec3{0,-std::abs(s)*half_width*(broad?.32f:.18f)*arc,0};
      v.uv={length(d)*t,(s+1)*half_width};v.aux={v.uv.x,v.uv.y,.5f,1};v.material=material;
      vertices.push_back(v);
    }
  }
  auto face=[&](std::uint32_t a,std::uint32_t b,std::uint32_t c) {
    const Vec3 normal=cross(vertices[b].position-vertices[a].position,vertices[c].position-vertices[a].position);
    if(length(normal)<1e-10f)return;
    indices.insert(indices.end(),{a,b,c});vertices[a].normal+=normal;vertices[b].normal+=normal;vertices[c].normal+=normal;
  };
  for(int j=0;j<segments;++j)for(int s=0;s<2;++s) {
    const auto a=std::uint32_t(j*3+s),b=a+3;
    face(a,a+1,b+1);face(a,b+1,b);
  }
  for(auto& v:vertices) {
    v.normal=normalize(v.normal);v.tangent={normalize(axis-v.normal*dot(axis,v.normal)),1};
  }
  auto start=static_cast<std::uint32_t>(mesh.vertices.size());
  for(const auto& v:vertices)mesh.add_vertex(v);
  for(auto index:indices)mesh.indices.push_back(start+index);
  start=static_cast<std::uint32_t>(mesh.vertices.size());
  for(auto v:vertices){v.normal=-v.normal;v.tangent.w=-1;mesh.add_vertex(v);}
  for(std::size_t j=0;j<indices.size();j+=3)mesh.indices.insert(mesh.indices.end(),{start+indices[j],start+indices[j+2],start+indices[j+1]});
}

// The fig is a maintained garden specimen beside the ceramic outrigger.
// In its authored .30-radian orientation, this local-space clearance volume
// corresponds to the real near blade plus room for growing foliage. Branch
// centrelines route around it; entire fine sprays are pruned before geometry
// is created, so no blade or leaf is warped to fit the architecture.
bool enters_frame_clearance(const std::vector<Vec3>& path,float margin) {
  auto oriented=[](Vec3 p) {
    const float c=std::cos(.30f),s=std::sin(.30f);
    return Vec3{c*p.x+s*p.z,p.y,-s*p.x+c*p.z};
  };
  const Vec3 lo{10.3f-margin,1.f-margin,-3.4f-margin};
  const Vec3 hi{13.2f+margin,33.f+margin,1.f+margin};
  for(std::size_t i=1;i<path.size();++i) {
    const Vec3 a=oriented(path[i-1]),d=oriented(path[i])-a;
    float begin=0,end=1;
    auto axis=[&](float p,float delta,float low,float high) {
      if(std::fabs(delta)<1e-7f)return p>=low&&p<=high;
      float one=(low-p)/delta,two=(high-p)/delta;
      if(one>two)std::swap(one,two);
      begin=std::max(begin,one);end=std::min(end,two);
      return begin<=end;
    };
    if(axis(a.x,d.x,lo.x,hi.x)&&axis(a.y,d.y,lo.y,hi.y)&&
       axis(a.z,d.z,lo.z,hi.z))return true;
  }
  return false;
}

void fig(Scene& scene) {
  if(present(scene,"garden_fig_canopy"))return;
  const auto bark=named(scene,"bark_ridged");
  const std::array<Material,3> leaves{{named(scene,"leaf_deep"),named(scene,"leaf_middle"),named(scene,"leaf_sunlit")}};
  MeshResource result;result.name="garden_fig_canopy";auto& mesh=result.mesh;
  Rng rng=root_rng("83").child(0x464947u);
  const auto trunk=curve({0,0,0},{-.45f,1.4f,.1f},{.35f,3.1f,-.25f},{1.1f,4.6f,.1f},24);
  branch(mesh,bark,trunk,.54f,.21f,18);
  for(int i=0;i<7;++i) {
    const float a=i*2*kPi/7;const Vec3 foot{std::cos(a)*1.1f,.015f,std::sin(a)*1.1f};
    branch(mesh,bark,curve(foot,foot*.65f+Vec3{0,.17f,0},{0,.48f,0},{0,1.2f,0},9),.085f,.21f,10);
  }
  const std::array<Vec3,6> tips{{{10.659f,5.8f,6.112f},{9.5f,6.7f,5.4f},{8.5f,7.2f,-3.8f},
                                 {4.1f,8.0f,5.7f},{-3.8f,6.9f,2.7f},{-4.3f,6.1f,-3.6f}}};
  for(std::size_t limb=0;limb<tips.size();++limb) {
    Rng r=rng.child(limb+1);const Vec3 start=trunk[12+limb*2],end=tips[limb];
    const auto bough=curve(start,start+Vec3{(end.x-start.x)*.18f,1.5f,(end.z-start.z)*.18f},
                           end-Vec3{(end.x-start.x)*.27f,.15f,(end.z-start.z)*.27f},end,28);
    if(enters_frame_clearance(bough,limb<3?.29f:.22f))
      throw std::runtime_error("Garden fig bough enters ceramic-frame maintenance clearance");
    branch(mesh,bark,bough,limb<3?.29f:.22f,.034f,14);
    for(int twig=0;twig<24;++twig) {
      const float azimuth=twig*2.399963f+r.range(-.45f,.45f);
      const float radial=std::sqrt(r.range(.12f,1.f));
      // Sprays occupy overlapping volumes along the outer bough, not a
      // single flat terminal shelf. The irregular heights leave readable
      // limbs while forming a continuous asymmetric canopy envelope.
      const int branch_station=14+(twig*7)%15;
      const Vec3 crown=bough[branch_station]+Vec3{std::cos(azimuth)*2.65f*radial,
                            r.range(-1.10f,1.55f),std::sin(azimuth)*2.65f*radial};
      const Vec3 origin=bough[branch_station-5];
      if(length(crown-origin)<.40f)continue;
      auto secondary=curve(origin,lerp(origin,crown,.35f)+Vec3{0,.25f,0},
                             lerp(origin,crown,.78f)+Vec3{0,.12f,0},crown,8);
      if(enters_frame_clearance(secondary,.08f))continue;
      branch(mesh,bark,secondary,.037f,.009f,7);
      for(int shoot=0;shoot<9;++shoot) {
        const float angle=r.range(-kPi,kPi);
        const bool hanging=limb<3&&twig%3==0&&shoot%2==0;
        Vec3 axis=normalize(Vec3{std::cos(angle),hanging?r.range(-1.5f,-.65f):r.range(-.75f,.80f),std::sin(angle)});
        const Vec3 a=secondary[5+shoot%4];const float reach=hanging?r.range(.95f,1.65f):r.range(.65f,1.10f);
        const Vec3 b=a+axis*reach;
        const std::vector<Vec3> shoot_path{a,lerp(a,b,.5f)+Vec3{0,.06f,0},b};
        if(enters_frame_clearance(shoot_path,.34f))continue;
        branch(mesh,bark,shoot_path,.0039f,.001f,4);
        Vec3 side=normalize(cross(axis,{0,1,0}));
        for(int blade=0;blade<30;++blade) {
          const float t=.08f+blade*(.88f/29),sign=blade%2?1.f:-1.f;
          const Vec3 base=lerp(a,b,t)+Vec3{0,.06f*std::sin(kPi*t),0};
          const float size=r.range(.14f,.24f);
          const Vec3 direction=normalize(axis*.25f+side*(sign*.92f)+Vec3{0,r.range(-.35f,.25f),0});
          const float material_choice=r.next();const int shade=material_choice<.27f?0:material_choice>.91f?2:1;
          leaf(mesh,leaves[shade],base,base+direction*size,size*r.range(.27f,.36f),
               size*r.range(.10f,.19f),r.range(-.90f,.90f));
        }
      }
    }
  }
  scene.asset_library.resources.push_back(std::move(result));
}

void strelitzia(Scene& scene) {
  if(present(scene,"garden_strelitzia"))return;
  const auto middle=named(scene,"leaf_middle"),deep=named(scene,"leaf_deep");
  MeshResource result;result.name="garden_strelitzia";auto& mesh=result.mesh;
  Rng rng=root_rng("83").child(0x535452u);
  for(int i=0;i<15;++i) {
    const float a=i*2.399963f,reach=rng.range(.38f,.76f),height=rng.range(.63f,1.12f);
    const Vec3 base{std::cos(a)*.06f,0,std::sin(a)*.06f};
    const Vec3 start{std::cos(a)*reach,height,std::sin(a)*reach};
    branch(mesh,middle,curve(base,base+Vec3{0,height*.5f,0},start-Vec3{0,.26f,0},start,12),.020f,.009f,8);
    const float blade=rng.range(.72f,1.12f);
    const Vec3 direction=normalize(Vec3{std::cos(a)*.82f,rng.range(-.06f,.42f),std::sin(a)*.82f});
    leaf(mesh,i%4==0?deep:middle,start,start+direction*blade,blade*rng.range(.20f,.29f),.18f,rng.range(-.35f,.35f),12,true);
    branch(mesh,middle,curve(start,lerp(start,start+direction*blade,.35f)+Vec3{0,.18f,0},
                              lerp(start,start+direction*blade,.72f)+Vec3{0,.11f,0},start+direction*blade,12),.010f,.001f,6);
  }
  scene.asset_library.resources.push_back(std::move(result));
}
}  // namespace

void add_garden_hero_plants(Scene& scene) { fig(scene);strelitzia(scene); }
}  // namespace cb
