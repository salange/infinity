#include "asset_pipeline.hpp"
#include "scene.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
namespace cb {
namespace {
[[noreturn]] void invalid(const std::string& message) { throw std::runtime_error("Human tech asset: " + message); }
std::string sha256(const std::vector<std::uint8_t>& input, std::size_t begin) {
  static constexpr std::uint32_t k[64]={
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
  std::array<std::uint32_t,8> h={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
  std::vector<std::uint8_t> d(input.begin()+static_cast<std::ptrdiff_t>(begin),input.end());
  const std::uint64_t bits=d.size()*8ull; d.push_back(0x80); while(d.size()%64!=56)d.push_back(0);
  for(int i=7;i>=0;--i)d.push_back(static_cast<std::uint8_t>(bits>>(i*8)));
  for(std::size_t off=0;off<d.size();off+=64){
    std::uint32_t w[64]{};for(int i=0;i<16;++i)for(int j=0;j<4;++j)w[i]=(w[i]<<8)|d[off+i*4+j];
    for(int i=16;i<64;++i){const auto s0=std::rotr(w[i-15],7)^std::rotr(w[i-15],18)^(w[i-15]>>3);
      const auto s1=std::rotr(w[i-2],17)^std::rotr(w[i-2],19)^(w[i-2]>>10);w[i]=w[i-16]+s0+w[i-7]+s1;}
    auto a=h[0],b=h[1],c=h[2],d0=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
    for(int i=0;i<64;++i){auto s1=std::rotr(e,6)^std::rotr(e,11)^std::rotr(e,25);auto ch=(e&f)^(~e&g);
      auto t1=hh+s1+ch+k[i]+w[i];auto s0=std::rotr(a,2)^std::rotr(a,13)^std::rotr(a,22);
      auto maj=(a&b)^(a&c)^(b&c);auto t2=s0+maj;hh=g;g=f;f=e;e=d0+t1;d0=c;c=b;b=a;a=t1+t2;}
    h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d0;h[4]+=e;h[5]+=f;h[6]+=g;h[7]+=hh;
  }
  std::ostringstream result;result<<std::hex<<std::setfill('0');for(auto v:h)result<<std::setw(8)<<v;return result.str();
}
struct Reader {
  std::vector<std::uint8_t> bytes; std::size_t pos{0};
  void need(std::size_t n) { if(n>bytes.size()-pos)invalid("truncated resource"); }
  std::uint32_t u32(){need(4);std::uint32_t v=0;for(int i=0;i<4;++i)v|=std::uint32_t(bytes[pos++])<<(8*i);return v;}
  float f32(){auto x=std::bit_cast<float>(u32());if(!std::isfinite(x))invalid("non-finite mesh or material component");return x;}
  Vec3 v3(){return{f32(),f32(),f32()};}
  std::string fixed(std::size_t n){need(n);std::string r(reinterpret_cast<const char*>(bytes.data()+pos),n);pos+=n;return r;}
  std::string str(){auto n=u32();if(n>4096)invalid("oversized string");return fixed(n);}
};
}
AssetLibrary load_asset_library(const std::filesystem::path& path, std::uint32_t material_base) {
  std::ifstream stream(path,std::ios::binary|std::ios::ate);
  if(!stream)invalid("required kit missing at " + path.string() + "; run tools/build-assets.py");
  auto size=stream.tellg();if(size<148||size>512*1024*1024)invalid("invalid file size");
  Reader r;r.bytes.resize(static_cast<std::size_t>(size));stream.seekg(0);stream.read(reinterpret_cast<char*>(r.bytes.data()),size);
  if(!stream)invalid("failed to read resource");
  if(r.fixed(8)!=std::string("HTKIT02\0",8)||r.u32()!=2)invalid("unsupported kit version");
  const auto nm=r.u32(),nr=r.u32();if(!nm||nm>256||!nr||nr>512)invalid("invalid material/resource count");
  AssetLibrary out;out.source_sha256=r.fixed(64);auto expected=r.fixed(64);
  if(sha256(r.bytes,r.pos)!=expected)invalid("SHA-256 integrity mismatch");
  for(std::uint32_t i=0;i<nm;++i){MaterialDesc m;m.name=r.str();m.base_color=r.v3();m.roughness=r.f32();m.metallic=r.f32();
    m.emissive=r.f32();m.normal_strength=r.f32();m.flags=r.u32();m.tint2=r.v3();m.room_w=r.f32();m.room_h=r.f32();
    m.room_d=r.f32();m.lit_probability=r.f32();m.albedo_set=r.str();m.uv_scale=r.f32();
    if(m.roughness<0||m.roughness>1||m.metallic<0||m.metallic>1||m.uv_scale<=0)invalid("invalid PBR material range");
    if((m.flags&128u)&&(m.room_w<1||m.room_w>2.5f||m.room_h<0||m.room_h>1||m.room_d<0||m.room_d>1||m.lit_probability<=0))invalid("invalid glass optical factors");
    out.materials.push_back(std::move(m));}
  std::uint64_t total_vertices=0,total_indices=0;
  for(std::uint32_t j=0;j<nr;++j){MeshResource resource;resource.name=r.str();
    if(resource.name.empty()||std::any_of(out.resources.begin(),out.resources.end(),[&](const auto& v){return v.name==resource.name;}))invalid("duplicate/empty resource name");
    auto nv=r.u32(),ni=r.u32();total_vertices+=nv;total_indices+=ni;
    if(nv<3||ni<3||ni%3||total_vertices>4000000||total_indices>18000000)invalid("invalid geometry count");
    r.need(std::size_t(nv)*68+std::size_t(ni)*4);resource.mesh.vertices.reserve(nv);resource.mesh.indices.reserve(ni);
    for(std::uint32_t i=0;i<nv;++i){Vertex v;v.position=r.v3();v.normal=r.v3();v.tangent={r.f32(),r.f32(),r.f32(),r.f32()};
      v.uv={r.f32(),r.f32()};auto m=r.u32();if(m>=nm||m>std::numeric_limits<std::uint32_t>::max()-material_base)invalid("invalid vertex material");v.material=m+material_base;
      v.aux={r.f32(),r.f32(),r.f32(),r.f32()};if(length(v.normal)<0.99f||length(v.normal)>1.01f)invalid("unnormalized vertex normal");resource.mesh.add_vertex(v);}
    for(std::uint32_t i=0;i<ni;++i){auto idx=r.u32();if(idx>=nv)invalid("index out of bounds");resource.mesh.indices.push_back(idx);}
    out.resources.push_back(std::move(resource));}
  if(r.pos!=r.bytes.size()) invalid("unexpected trailing data");
  return out;
}
std::uint32_t asset_resource(const AssetLibrary& library,std::string_view name){
  for(std::uint32_t i=0;i<library.resources.size();++i)if(library.resources[i].name==name)return i;
  invalid("required named resource missing: "+std::string(name));
}
Vec3 asset_transform_point(const AssetInstance& v,Vec3 p){
  p={p.x*v.scale.x,p.y*v.scale.y,p.z*v.scale.z};const float c=std::cos(v.yaw),s=std::sin(v.yaw);
  return {c*p.x+s*p.z+v.translation.x,p.y+v.translation.y,-s*p.x+c*p.z+v.translation.z};
}
Vec3 asset_transform_normal(const AssetInstance& v,Vec3 n){
  n={n.x/v.scale.x,n.y/v.scale.y,n.z/v.scale.z};const float c=std::cos(v.yaw),s=std::sin(v.yaw);
  return normalize(Vec3{c*n.x+s*n.z,n.y,-s*n.x+c*n.z});
}
void add_asset_instance(Scene& scene,std::string_view name,Vec3 position,float yaw,Vec3 scale,Vec3 tint){
  auto finite=[](Vec3 v){return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z);};
  if(!finite(position)||!finite(scale)||!finite(tint)||scale.x<=0||scale.y<=0||scale.z<=0||!std::isfinite(yaw))
    invalid("instance transform and tint must be finite, with positive scale");
  scene.asset_instances.push_back({asset_resource(scene.asset_library,name),position,yaw,scale,tint});
}
}  // namespace cb
