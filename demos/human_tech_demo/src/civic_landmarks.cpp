#include "civic_landmarks.hpp"
#include "city/towers.hpp"

#include <array>
#include <cmath>
#include <string>
#include <vector>

namespace cb {
namespace {
using Material = std::uint32_t;
Vec3 X3(Vec2 p) { return {p.x,0,p.y}; }

Sampled evenly_spaced(const std::vector<Vec2>& plan, int count) {
  Sampled result;
  const float perimeter=plan_perimeter(plan);
  std::size_t edge=0;float start=0;
  for(int i=0;i<count;++i) {
    const float distance=perimeter*i/count;
    while(edge+1<plan.size()&&start+length(plan[(edge+1)%plan.size()]-plan[edge])<distance) {
      start+=length(plan[(edge+1)%plan.size()]-plan[edge]);++edge;
    }
    const Vec2 delta=plan[(edge+1)%plan.size()]-plan[edge];
    const Vec2 direction=normalize(delta);
    result.points.push_back(plan[edge]+direction*(distance-start));
    result.normals.push_back({direction.y,-direction.x});
    result.arclen.push_back(distance);
  }
  return result;
}

Material surface(Scene& scene, const char* name, Vec3 color, float roughness,
                 float metallic = 0, float emission = 0, std::uint32_t flags = 0) {
  for (std::size_t i = 0; i < scene.materials.size(); ++i)
    if (scene.materials[i].name == name) return static_cast<Material>(i);
  MaterialDesc m;
  m.name = name; m.base_color = color; m.roughness = roughness;
  m.metallic = metallic; m.emissive = emission; m.flags = flags;
  m.tint2 = {1, .73f, .40f};
  scene.materials.push_back(m);
  return static_cast<Material>(scene.materials.size() - 1);
}

Material glazing(Scene& scene, const char* name, Vec3 tint, float roughness,
                 float transmission, float thickness) {
  Material id = surface(scene, name, tint, roughness, 0, 0, 128u);
  auto& m = scene.materials[id];
  m.room_w = 1.52f; m.room_h = transmission; m.room_d = .97f;
  m.lit_probability = thickness;
  return id;
}

struct Palette {
  Material ceramic, stone, bronze, steel, dark, plaster, wood, light, glass, roof_glass;
  explicit Palette(Scene& scene)
      : ceramic(surface(scene, "civic glazed ivory ceramic", {.82f,.79f,.70f}, .28f)),
        stone(surface(scene, "civic honed warm limestone", {.68f,.65f,.58f}, .49f)),
        bronze(surface(scene, "civic satin bronze", {.47f,.29f,.105f}, .24f, .88f)),
        steel(surface(scene, "civic brushed silver shell", {.46f,.50f,.54f}, .25f, .96f)),
        dark(surface(scene, "civic bronze recess", {.095f,.069f,.041f}, .42f, .7f)),
        plaster(surface(scene, "civic interior plaster", {.78f,.745f,.67f}, .75f)),
        wood(surface(scene, "civic walnut joinery", {.19f,.09f,.033f}, .43f)),
        light(surface(scene, "civic warm luminaire", {1,.84f,.64f}, .35f, 0, 4.2f, kMatEmissive)),
        glass(glazing(scene, "civic laminated clear glazing", {.91f,.96f,.94f}, .075f, .94f, .024f)),
        roof_glass(glazing(scene, "civic curved solar glazing", {.19f,.31f,.48f}, .10f, .32f, .045f)) {
    // Continuous solar-control roof panes have full physical coverage. The
    // clear occupied drum windows keep their independent optical recipe.
    // The renderer currently evaluates the nearest pane, not both sides of
    // a closed hemisphere; this tint is an explicit single-pane material.
    scene.materials[roof_glass].room_d=1.f;
    scene.materials[light].tint2={1,.84f,.64f};
    // Albedo arrays are sRGB textures. Compensate in linear space for each
    // supported map's reference colour, preserving the intended base hue.
    // Physical panel geometry carries the large features; these maps add pores,
    // honed veins and brushed metal variation without world-scale colour noise.
    auto mapped_linear=[&](Material id,const char* set,Vec3 reference,float metres,float normal) {
      auto& material=scene.materials[id];
      material.base_color={material.base_color.x/reference.x,
                           material.base_color.y/reference.y,
                           material.base_color.z/reference.z};
      material.albedo_set=set;material.uv_scale=metres;material.normal_strength=normal;
    };
    auto mapped=[&](Material id,const char* set,Vec3 reference,float metres,float normal) {
      auto linear=[](float value) {return value<=.04045f?value/12.92f:std::pow((value+.055f)/1.055f,2.4f);};
      mapped_linear(id,set,{linear(reference.x),linear(reference.y),linear(reference.z)},metres,normal);
    };
    // Idempotent palette construction: the ring and dome share these entries.
    if(scene.materials[ceramic].albedo_set.empty())mapped(ceramic,"concrete_white",{.92f,.914f,.89f},1.8f,.16f);
    if(scene.materials[stone].albedo_set.empty()) {
      if(scene.reviewed_material_maps) {
        // Reviewed Marble012 statistics are linear albedo and linear roughness.
        // Preserve the honed finish after the renderer's 55% ARM-map blend.
        mapped_linear(stone,"marble",{.420374f,.425492f,.473587f},2.4f,.12f);
        scene.materials[stone].roughness=.49f/(.45f+.55f*.0658778f/.6f);
      } else {
        mapped(stone,"marble",{.84f,.84f,.815f},2.4f,.12f);
      }
    }
    if(scene.materials[bronze].albedo_set.empty())mapped(bronze,"metal_silver",{.80f,.80f,.82f},.45f,.20f);
    if(scene.materials[steel].albedo_set.empty())mapped(steel,"metal_silver",{.80f,.80f,.82f},.55f,.15f);
    if(scene.materials[dark].albedo_set.empty())mapped(dark,"metal_silver",{.80f,.80f,.82f},.45f,.13f);
  }
};

void solid(Mesh& mesh, Material material, const std::vector<Vec2>& plan,
           float bottom, float top) {
  Emit e(&mesh, material);
  e.polygon(plan, top, true); e.polygon(plan, bottom, false);
  e.wall(plan, bottom, top, true);
}

// Smooth curved faces retain analytic normals instead of making the panel
// tessellation visible in reflected light. The surface has real back/edge faces.
void smooth_quad(Mesh& mesh, Material material, const std::array<Vec3,4>& p,
                 const std::array<Vec3,4>& n, float u0, float u1,
                 float v0, float v1, float variation = .5f) {
  const std::array<Vec2,4> uv{{{u0,v0},{u1,v0},{u1,v1},{u0,v1}}};
  const std::uint32_t start = static_cast<std::uint32_t>(mesh.vertices.size());
  for (int i = 0; i < 4; ++i) {
    Vec3 tangent = p[1] - p[0]; tangent -= n[i] * dot(n[i], tangent);
    if (length(tangent) < 1e-5f) tangent = cross(n[i], Vec3{0,0,1});
    Vertex vertex;
    vertex.position = p[i]; vertex.normal = n[i];
    vertex.tangent = {normalize(tangent),1}; vertex.uv = uv[i];
    vertex.material = material; vertex.aux = {uv[i].x,uv[i].y,variation,1};
    mesh.add_vertex(vertex);
  }
  if (dot(cross(p[1]-p[0],p[3]-p[0]),n[0]+n[1]+n[2]+n[3]) > 0) {
    mesh.add_triangle(start,start+1,start+2); mesh.add_triangle(start,start+2,start+3);
  } else {
    mesh.add_triangle(start,start+2,start+1); mesh.add_triangle(start,start+3,start+2);
  }
}

void guard(Scene& scene, const Palette& m, const std::vector<Vec2>& plan,
           float floor, bool glass) {
  const auto sampled = evenly_spaced(plan, std::max(8,int(std::ceil(plan_perimeter(plan)/2.5f))));
  Emit frame(&scene.opaque,m.bronze),pane(&scene.opaque,m.glass);
  for (std::size_t i=0;i<sampled.points.size();++i) {
    const Vec2 a=sampled.points[i],b=sampled.points[(i+1)%sampled.points.size()];
    frame.tube(P3(a,floor+.13f),P3(a,floor+1.10f),.026f,8,true);
    frame.tube(P3(a,floor+1.10f),P3(b,floor+1.10f),.029f,8,true);
    if (glass) {
      const Vec3 aa=P3(a,floor+.19f),bb=P3(b,floor+.19f);
      pane.quad_metric(aa,bb,bb+Vec3{0,.84f,0},aa+Vec3{0,.84f,0});
      pane.quad_metric(bb,aa,aa+Vec3{0,.84f,0},bb+Vec3{0,.84f,0});
    }
  }
}

void chamfered_column(Mesh& mesh, Material material, Vec3 base, float height,
                       Vec3 along, Vec3 outward, float width, float depth) {
  const float bevel=std::min(width,depth)*.17f;
  const std::array<Vec2,8> section{{{-width+bevel,-depth},{width-bevel,-depth},
    {width,-depth+bevel},{width,depth-bevel},{width-bevel,depth},
    {-width+bevel,depth},{-width,depth-bevel},{-width,-depth+bevel}}};
  Emit e(&mesh,material);
  for (int i=0;i<8;++i) {
    const int j=(i+1)%8;
    Vec3 a=base+along*section[i].x+outward*section[i].y;
    Vec3 b=base+along*section[j].x+outward*section[j].y;
    e.quad_metric(b,a,a+Vec3{0,height,0},b+Vec3{0,height,0});
    e.triangle(base,a,b); e.triangle(base+Vec3{0,height,0},b+Vec3{0,height,0},a+Vec3{0,height,0});
  }
}
}  // namespace

void build_cinematic_ring(Scene& scene, Vec2 centre, float base_y,
                          float radius, float facing_radians) {
  const Palette m(scene);
  const Vec3 axis{std::cos(facing_radians),0,std::sin(facing_radians)};
  const Vec3 side=normalize(cross(axis,Vec3{0,1,0}));
  constexpr float stretch=1.9f;
  // Broaden the blade toward its opening. The exterior ellipse, height and
  // axis depth retain their established city-wide silhouette and anchors.
  const float axis_half_width=radius*.06f;
  const float radial_half_width=radius*.108f;
  const float blade_radius=radius+axis_half_width-radial_half_width;
  const Vec3 origin=P3(centre,base_y+(radius+1.3f)*stretch);
  const auto start=static_cast<std::uint32_t>(scene.opaque.indices.size());

  // A shallow manufactured crown gives each broad silver face a changing
  // physical normal. The maximum axis depth and radial silhouette stay inside
  // the existing envelope; the separate inner bronze skin is retained.
  const std::array<Vec2,10> section{{{1,0},{.9483f,.3017f},{.819f,.44f},
    {-.819f,.44f},{-.9483f,.3017f},{-1,0},{-.9483f,-.3017f},
    {-.819f,-.44f},{.819f,-.44f},{.9483f,-.3017f}}};
  auto point=[&](float angle,Vec2 section_point) {
    return origin+side*(std::cos(angle)*(blade_radius+section_point.x*radial_half_width))+
      Vec3{0,std::sin(angle)*(blade_radius+section_point.x*radial_half_width)*stretch,0}+
      axis*(section_point.y*axis_half_width);
  };
  auto normal=[&](float angle,Vec2 position,Vec2 tangent) {
    const Vec3 along=side*(-std::sin(angle)*(blade_radius+position.x*radial_half_width))+
      Vec3{0,std::cos(angle)*(blade_radius+position.x*radial_half_width)*stretch,0};
    const Vec3 across=side*(std::cos(angle)*tangent.x*radial_half_width)+
      Vec3{0,std::sin(angle)*tangent.x*radial_half_width*stretch,0}+
      axis*(tangent.y*axis_half_width);
    Vec3 n=normalize(cross(along,across));
    const Vec3 outward=side*(std::cos(angle)*position.x)+
      Vec3{0,std::sin(angle)*position.x/stretch,0}+axis*position.y;
    return dot(n,outward)<0?-n:n;
  };
  constexpr int panels=112,subdivisions=4;
  for (int panel=0;panel<panels;++panel) {
    const float begin=2*kPi*panel/panels,end=2*kPi*(panel+1)/panels;
    const float gap=.022f/(radius*1.4f);
    for (int face=0;face<10;++face) {
      const Vec2 a=section[face],b=section[(face+1)%10];
      const bool inner=face>=3&&face<=6;
      const Material finish=inner?m.bronze:m.steel;
      const bool crowned=face==2||face==7;
      const int across_steps=crowned?16:1;
      auto profile=[&](float t) {
        Vec2 p=a+(b-a)*t;
        if(crowned)p.y=(face==2?1.f:-1.f)*(.44f+.1318f*std::sin(kPi*t));
        return p;
      };
      auto derivative=[&](float t) {
        Vec2 d=b-a;
        if(crowned)d.y=(face==2?1.f:-1.f)*.1318f*kPi*std::cos(kPi*t);
        return d;
      };
      for(int across=0;across<across_steps;++across) {
        const float s0=float(across)/across_steps,s1=float(across+1)/across_steps;
        const Vec2 pa=profile(s0),pb=profile(s1),da=derivative(s0),db=derivative(s1);
        const float section_length=std::hypot((b.x-a.x)*radial_half_width,(b.y-a.y)*axis_half_width);
        const float v0=s0*section_length,v1=s1*section_length;
        for (int step=0;step<subdivisions;++step) {
          const float t0=begin+gap+(end-begin-2*gap)*step/subdivisions;
          const float t1=begin+gap+(end-begin-2*gap)*(step+1)/subdivisions;
          smooth_quad(scene.opaque,finish,
              {point(t0,pa),point(t1,pa),point(t1,pb),point(t0,pb)},
              {normal(t0,pa,da),normal(t1,pa,da),normal(t1,pb,db),normal(t0,pb,db)},
              t0*radius,t1*radius,v0,v1);
        }
        // Each curved panel return meets its matching recessed joint backing.
        // The same profile function supplies both contact edges.
        Emit joint(&scene.opaque,m.dark),returns(&scene.opaque,finish);
        joint.quad_metric(point(begin-gap,pa*.985f),point(begin+gap,pa*.985f),
                          point(begin+gap,pb*.985f),point(begin-gap,pb*.985f));
        for (float angle:{begin+gap,end-gap})
          returns.quad_metric(point(angle,pa),point(angle,pb),
                              point(angle,pb*.985f),point(angle,pa*.985f));
      }
    }
    // Warm metal reveals and a recessed luminous line remain actual geometry.
    Emit strip(&scene.opaque,m.light);
    for (int step=0;step<subdivisions;++step) {
      const float t0=begin+(end-begin)*step/subdivisions;
      const float t1=begin+(end-begin)*(step+1)/subdivisions;
      const Vec2 a{-.94f,.32f},b{-.93f,.35f};
      strip.quad_metric(point(t0,a),point(t1,a),point(t1,b),point(t0,b));
    }
  }

  // The original foundation footprint is retained; the blade seats into a
  // continuous bronze saddle instead of leaning on disconnected thin pylons.
  solid(scene.opaque,m.stone,plan_circle(radius*.75f+1.2f,128,centre),base_y,base_y+.76f);
  solid(scene.opaque,m.ceramic,plan_circle(radius*.75f,128,centre),base_y+.76f,base_y+1.9f);
  Emit rings(&scene.opaque,m.bronze),stone(&scene.opaque,m.stone);
  rings.torus(P3(centre,base_y+1.88f),{0,1,0},radius*.75f-.24f,.055f,160,8);
  for (int i=0;i<64;++i) {
    float a=i*2*kPi/64;
    rings.beam(P3(centre+Vec2{std::cos(a),std::sin(a)}*(radius*.75f-2),base_y+1.906f),
               P3(centre+Vec2{std::cos(a),std::sin(a)}*(radius*.75f-.3f),base_y+1.906f),.018f,.014f);
  }
  for (float sign:{-1.f,1.f}) {
    Vec3 at=P3(centre,base_y+1.9f)+side*(sign*radius*.17f);
    chamfered_column(scene.opaque,m.bronze,at,.58f,side,axis,radius*.105f,axis_half_width*.82f);
    for (float offset:{-1.f,1.f}) {
      Vec3 lamp=at+axis*(offset*(axis_half_width+1.1f));
      rings.box(lamp+Vec3{0,.12f,0},{.42f,.12f,.28f},side,{0,1,0},axis);
      Emit(&scene.opaque,m.light).box(lamp+Vec3{0,.245f,0},{.32f,.02f,.20f},side,{0,1,0},axis);
      scene.lights.push_back({lamp+Vec3{0,.5f,0},12,{1,.68f,.32f},4.5f});
    }
  }
  scene.register_range(start,static_cast<std::uint32_t>(scene.opaque.indices.size()),origin,radius*2.1f);
}

void build_cinematic_dome(Scene& scene, Vec2 centre, float base_y,
                          float half, float yaw, Rng rng) {
  const Palette m(scene);
  const auto first=static_cast<std::uint32_t>(scene.opaque.indices.size());
  constexpr int floors=5;
  constexpr float foundation=3.2f,storey=5.2f;
  const float floor0=base_y+foundation,roof=floor0+floors*storey+.6f;
  const float dome_radius=half*.6f;
  const Vec3 dome_centre=P3(centre,roof);
  const Vec3 xaxis{std::cos(yaw),0,-std::sin(yaw)},zaxis{std::sin(yaw),0,std::cos(yaw)};
  auto local=[&](float x,float y,float z) {return P3(centre,y)+xaxis*x+zaxis*z;};
  auto plan=[&](float x,float z,float exponent=4.f) {
    return plan_transform(plan_superellipse(x,z,exponent,96),centre,yaw);
  };
  const auto body=plan(half*.78f,half*.62f);
  const auto terrace=plan_transform(plan_rounded_rect(half*1.25f,half*1.05f,4,12),centre,yaw);
  solid(scene.opaque,m.stone,terrace,base_y,floor0);
  // The old +Z stairs extended 8.4m beyond the plinth. Solid individual risers
  // preserve that accessible approach and close the previously hollow steps.
  for (int step=0;step<20;++step) {
    const float top=floor0-step*.16f;
    Emit(&scene.opaque,m.stone).box(local(0,(base_y+top)*.5f,half*1.05f+.21f+step*.42f),
      {half*.875f,(top-base_y)*.5f,.21f},xaxis,{0,1,0},zaxis);
    Emit(&scene.opaque,m.bronze).box(local(0,top+.004f,half*1.05f+.40f+step*.42f),
      {half*.875f,.004f,.012f},xaxis,{0,1,0},zaxis);
  }
  for (float sign:{-1.f,1.f}) {
    Emit(&scene.opaque,m.ceramic).box(local(sign*(half*.875f+.4f),base_y+foundation*.5f,half*1.05f+4.2f),
      {.4f,foundation*.5f,4.2f},xaxis,{0,1,0},zaxis);
  }

  // Real floor plates, bronze reveals, mullions, and a continuous colonnade.
  const auto column_plan=plan_offset(body,3.35f);
  const auto columns=evenly_spaced(column_plan,std::max(12,int(std::ceil(plan_perimeter(column_plan)/5.5f))));
  for (std::size_t i=0;i<columns.points.size();++i) {
    Vec3 outward=X3(columns.normals[i]),along={outward.z,0,-outward.x};
    const Vec3 foot=P3(columns.points[i],floor0);
    for (int floor=0;floor<floors;++floor) {
      chamfered_column(scene.opaque,m.ceramic,foot+Vec3{0,floor*storey+.035f,0},storey-.07f,along,outward,.40f,.62f);
      Emit(&scene.opaque,m.bronze).box(foot+Vec3{0,floor*storey+.025f,0},{.405f,.025f,.625f},along,{0,1,0},outward);
    }
    chamfered_column(scene.opaque,m.ceramic,foot+Vec3{0,floors*storey-.6f,0},.6f,along,outward,.56f,.74f);
  }
  for (int floor=0;floor<=floors;++floor) {
    const float y=floor0+floor*storey;
    if (floor<floors) solid(scene.opaque,m.stone,plan_offset(body,.8f),y-.27f,y);
    else {
      // The dome opens onto its chamber, rather than enclosing a hidden lower
      // hemisphere inside the occupied floors.
      const auto outer=plan_offset(body,4.5f),inner=plan_circle(dome_radius-.12f,96,centre);
      Emit roof_skin(&scene.opaque,m.ceramic);
      roof_skin.ring_cap(outer,inner,roof);
      roof_skin.ring_cap(inner,outer,roof-.32f);
      roof_skin.wall(outer,roof-.32f,roof,true);
      roof_skin.wall(inner,roof-.32f,roof,true,false);
      // A real pale soffit lines the occupied ring below the dome opening.
      Emit lining(&scene.opaque,m.plaster);
      lining.ring_cap(inner,outer,roof-.342f);
      lining.wall(outer,roof-.342f,roof-.32f,true);
      lining.wall(inner,roof-.342f,roof-.32f,true,false);
      continue;
    }
    if(floor+1<floors) {
      // Finished ceiling lining touches the actual slab underside. It does
      // not invent a diffuse/emissive room image behind the windows.
      solid(scene.opaque,m.plaster,plan_offset(body,.72f),
            y+storey-.295f,y+storey-.27f);
    }
    const auto glass_plan=plan_offset(body,-.5f);
    const auto bays=evenly_spaced(glass_plan,std::max(16,int(std::ceil(plan_perimeter(glass_plan)/3.2f))));
    for (std::size_t i=0;i<bays.points.size();++i) {
      Vec2 a=bays.points[i],b=bays.points[(i+1)%bays.points.size()];
      const Vec2 direction=normalize(b-a);
      Vec3 outward{direction.y,0,-direction.x},along={outward.z,0,-outward.x};
      const Vec3 bottom=P3(a,y+.15f),top=bottom+Vec3{0,storey-.42f,0};
      const float front=dot(X3((a+b)*.5f-centre),zaxis),across=dot(X3((a+b)*.5f-centre),xaxis);
      const bool doorway=floor==0&&front>half*.50f&&std::fabs(across)<3.3f;
      if (!doorway) {
        Emit glass(&scene.opaque,m.glass);
        const Vec3 middle=P3((a+b)*.5f,y+(storey-.12f)*.5f);
        glass.box(middle,{length(b-a)*.5f,(storey-.42f)*.5f,.012f},along,{0,1,0},outward);
      }
      Emit frame(&scene.opaque,m.bronze);
      if(!doorway)frame.box((bottom+top)*.5f,{.055f,(storey-.42f)*.5f,.095f},along,{0,1,0},outward);
      frame.beam(top,top+(P3(b,0)-P3(a,0)),.095f,.075f);
      if(!doorway)frame.beam(bottom+Vec3{0,1.12f,0},P3(b,y+1.27f),.042f,.040f);
    }
    if(floor==0) {
      // The entrance has two real jambs and a head, rather than the regular
      // curtain-grid mullion bisecting its walking aisle.
      Emit frame(&scene.opaque,m.bronze);
      for(float sign:{-1.f,1.f})frame.box(local(sign*3.3f,y+1.56f,half*.62f-.5f),
          {.07f,1.56f,.12f},xaxis,{0,1,0},zaxis);
      frame.box(local(0,y+3.12f,half*.62f-.5f),{3.37f,.07f,.12f},xaxis,{0,1,0},zaxis);
    }
    // Large, legible occupied rooms behind the glazing. The permanent joinery
    // and furniture are visible from every side, including the night view.
    const auto room_edge=evenly_spaced(glass_plan,12);
    for (int room=0;room<12;++room) {
      Vec2 p,n; // Sample a point on the actual rounded footprint.
      p=room_edge.points[std::size_t(room)%room_edge.points.size()];
      n=room_edge.normals[std::size_t(room)%room_edge.normals.size()];
      const Vec3 outward=X3(n),along={outward.z,0,-outward.x};
      const Vec3 at=P3(p,y)-outward*3.0f;
      Emit timber(&scene.opaque,m.wood),seat(&scene.opaque,m.plaster),metal(&scene.opaque,m.bronze);
      const bool entry_aisle=floor==0&&dot(X3(p-centre),zaxis)>half*.48f&&std::fabs(dot(X3(p-centre),xaxis))<7.5f;
      if(!entry_aisle) {
      timber.box(at+Vec3{0,.77f,0},{1.05f,.055f,.52f},along,{0,1,0},outward);
      for (float side:{-1.f,1.f}) {
        metal.box(at+along*(side*.75f)+Vec3{0,.36f,0},{.04f,.36f,.28f},along,{0,1,0},outward);
        for (float front_sign:{-1.f,1.f}) {
          Vec3 chair=at+along*(side*.72f)+outward*(front_sign*1.02f);
          seat.box(chair+Vec3{0,.45f,0},{.35f,.10f,.33f},along,{0,1,0},outward);
          seat.box(chair+outward*(front_sign*.28f)+Vec3{0,.89f,0},{.35f,.43f,.07f},along,{0,1,0},outward);
          for (float leg:{-1.f,1.f}) metal.box(chair+along*(leg*.25f)+Vec3{0,.18f,0},{.025f,.18f,.24f},along,{0,1,0},outward);
        }
      }
      const Vec3 shelf=at-outward*2.0f;
      timber.box(shelf+Vec3{0,1.35f,0},{1.45f,1.35f,.21f},along,{0,1,0},outward);
      for (int level=0;level<4;++level) {
        timber.box(shelf+outward*.27f+Vec3{0,.3f+level*.58f,0},{1.48f,.035f,.28f},along,{0,1,0},outward);
        for (int book=0;book<9;++book)
          seat.box(shelf+outward*.32f+along*(-1.1f+book*.27f)+Vec3{0,.55f+level*.58f,0},
                   {.075f,.19f+rng.range(0,.06f),.13f},along,{0,1,0},outward);
      }
      }
      const Vec3 luminaire=at+Vec3{0,storey-.55f,0};
      const float attachment_y=floor+1<floors?y+storey-.295f:roof-.39f;
      // Upper-room rails cantilever from the actual cornice. Lower fixtures
      // suspend from the physical ceiling lining, with visible hanger rods.
      if(floor+1==floors) {
        metal.beam(P3(p,attachment_y)+outward*3.8f,
                   Vec3{at.x,attachment_y,at.z},.16f,.18f);
        const Vec3 crossbar{at.x,attachment_y,at.z};
        metal.beam(crossbar-along*2.65f,crossbar+along*2.65f,.065f,.08f);
      }
      metal.box(luminaire+Vec3{0,.028f,0},{2.95f,.044f,.25f},along,{0,1,0},outward);
      Emit(&scene.opaque,m.light).box(luminaire-Vec3{0,.018f,0},
                                    {2.88f,.020f,.20f},along,{0,1,0},outward);
      for(float sign:{-1.f,1.f}) {
        Vec3 hanger=luminaire+along*(sign*2.3f)+Vec3{0,.070f,0};
        metal.tube(hanger,{hanger.x,attachment_y,hanger.z},.014f,8,true);
      }
      // Overlapping real fixtures light the furniture, pale ceilings and
      // occupied perimeter. The values are renderer intensity units.
      scene.lights.push_back({luminaire-Vec3{0,.14f,0},12.f,{1,.84f,.64f},28.f});
    }
    // Central service core supplies actual back walls and connects the rooms.
    const auto core=plan(5.5f,4.5f,3.f);
    Emit(&scene.opaque,m.plaster).wall(core,y,y+storey-.3f,true);
    for (float sign:{-1.f,1.f})
      Emit(&scene.opaque,m.bronze).box(local(sign*2.3f,y+1.25f,4.56f),{.85f,1.25f,.04f},xaxis,{0,1,0},zaxis);
  }
  // Sheltered entrance and upper cornice form two distinct horizontal scales.
  const auto canopy=plan_transform(plan_rounded_rect(14,5,3,12,{0,half*.62f+4.3f}),centre,yaw);
  solid(scene.opaque,m.ceramic,canopy,floor0+4.9f,floor0+5.27f);
  for (float sign:{-1.f,1.f}) {
    Vec3 at=local(sign*12,floor0,half*.62f+7.2f);
    chamfered_column(scene.opaque,m.bronze,at,4.9f,xaxis,zaxis,.10f,.12f);
    Emit(&scene.opaque,m.light).box(at+Vec3{0,4.78f,0},{.4f,.03f,1.25f},xaxis,{0,1,0},zaxis);
  }
  const auto cornice=plan_offset(body,4.7f);
  Emit eave(&scene.opaque,m.ceramic);
  eave.wall(cornice,roof-.65f,roof+.10f,true);
  Emit(&scene.opaque,m.bronze).wall(plan_offset(cornice,.012f),roof-.18f,roof-.13f,true);
  guard(scene,m,plan_offset(body,2.6f),roof,false);

  // Twenty-four independently curved solar-glass gores. Four construction
  // bands, bronze caps and an inner lining give the shell real material depth.
  constexpr int gores=24,bands=4,across_steps=5,up_steps=6;
  for (int gore=0;gore<gores;++gore) for (int band=0;band<bands;++band) {
    const float az0=gore*2*kPi/gores+.0025f,az1=(gore+1)*2*kPi/gores-.0025f;
    const float el0=band*(kPi*.5f-.055f)/bands+.0012f;
    const float el1=(band+1)*(kPi*.5f-.055f)/bands-.0012f;
    auto normal_at=[](float az,float el){return Vec3{std::cos(az)*std::cos(el),std::sin(el),std::sin(az)*std::cos(el)};};
    auto shell=[&](float az,float el,float radius){return dome_centre+normal_at(az,el)*radius;};
    for (int u=0;u<across_steps;++u) for (int v=0;v<up_steps;++v) {
      const float a=az0+(az1-az0)*u/across_steps,b=az0+(az1-az0)*(u+1)/across_steps;
      const float c=el0+(el1-el0)*v/up_steps,d=el0+(el1-el0)*(v+1)/up_steps;
      const std::array<Vec3,4> normals{{normal_at(a,c),normal_at(b,c),normal_at(b,d),normal_at(a,d)}};
      smooth_quad(scene.opaque,m.roof_glass,{shell(a,c,dome_radius),shell(b,c,dome_radius),shell(b,d,dome_radius),shell(a,d,dome_radius)},
                  normals,a*dome_radius,b*dome_radius,c*dome_radius,d*dome_radius);
      std::array<Vec3,4> inward;for(int i=0;i<4;++i)inward[i]=-normals[i];
      smooth_quad(scene.opaque,m.roof_glass,{shell(a,c,dome_radius-.045f),shell(b,c,dome_radius-.045f),shell(b,d,dome_radius-.045f),shell(a,d,dome_radius-.045f)},
                  inward,a*dome_radius,b*dome_radius,c*dome_radius,d*dome_radius);
    }
    Emit edge(&scene.opaque,m.dark);
    for(int u=0;u<across_steps;++u) {
      float a=az0+(az1-az0)*u/across_steps,b=az0+(az1-az0)*(u+1)/across_steps;
      for(float elevation:{el0,el1})
        edge.quad_metric(shell(a,elevation,dome_radius),shell(b,elevation,dome_radius),shell(b,elevation,dome_radius-.045f),shell(a,elevation,dome_radius-.045f));
    }
    for(int v=0;v<up_steps;++v) {
      const float a=el0+(el1-el0)*v/up_steps,b=el0+(el1-el0)*(v+1)/up_steps;
      for(float azimuth:{az0,az1})
        edge.quad_metric(shell(azimuth,a,dome_radius),shell(azimuth,b,dome_radius),shell(azimuth,b,dome_radius-.045f),shell(azimuth,a,dome_radius-.045f));
    }
  }
  Emit ribs(&scene.opaque,m.bronze),ceramic(&scene.opaque,m.ceramic);
  for(int gore=0;gore<gores;++gore) {
    const float az=gore*2*kPi/gores;
    for(int step=0;step<40;++step) {
      auto point_at=[&](float el){return dome_centre+Vec3{std::cos(az)*std::cos(el),std::sin(el),std::sin(az)*std::cos(el)}*(dome_radius-.16f);};
      ribs.tube(point_at(step*kPi*.5f/40),point_at((step+1)*kPi*.5f/40),.14f,10,true);
    }
  }
  for(int band=0;band<bands;++band) {
    const float elevation=band*(kPi*.5f-.055f)/bands;
    ribs.torus(dome_centre+Vec3{0,std::sin(elevation)*(dome_radius-.12f),0},{0,1,0},
               std::cos(elevation)*(dome_radius-.12f),band==0?.20f:.095f,160,8);
  }
  // Small glazed lantern preserves the existing apex marker at y=64.3 for
  // the canonical42m half-size, without putting facade rooms on a sphere.
  const float apex=roof+dome_radius;
  solid(scene.opaque,m.bronze,plan_circle(1.55f,48,centre),apex-.22f,apex+.22f);
  Emit lantern(&scene.opaque,m.glass);
  const auto lantern_plan=plan_circle(1.28f,32,centre);
  lantern.wall(lantern_plan,apex+.22f,apex+2.7f,true);
  for(int i=0;i<8;++i) {
    float angle=i*2*kPi/8;Vec3 p=P3(centre,apex+.22f)+Vec3{std::cos(angle)*1.29f,0,std::sin(angle)*1.29f};
    ribs.tube(p,p+Vec3{0,2.48f,0},.045f,8,true);
  }
  const auto lantern_roof_first=scene.opaque.vertices.size();
  ceramic.frustum(P3(centre,apex+2.7f),P3(centre,apex+3.25f),1.48f,.48f,32,true);
  // The generic tapered primitive supplies an axial tangent; project it onto
  // this steep ceramic cap so its surface map has an orthonormal basis.
  for(std::size_t i=lantern_roof_first;i<scene.opaque.vertices.size();++i) {
    auto& vertex=scene.opaque.vertices[i];
    const Vec3 tangent=vertex.tangent.xyz()-vertex.normal*dot(vertex.normal,vertex.tangent.xyz());
    vertex.tangent={normalize(tangent),vertex.tangent.w};
  }
  ribs.tube(P3(centre,apex+3.25f),P3(centre,apex+6.65f),.09f,10,true);
  Emit(&scene.opaque,m.light).tube(P3(centre,apex+6.54f),P3(centre,apex+6.9f),.12f,12,true);
  scene.register_range(first,static_cast<std::uint32_t>(scene.opaque.indices.size()),
                       P3(centre,(base_y+apex+6.9f)*.5f),half*1.9f);
}
}  // namespace cb
