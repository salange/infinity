#include "voxel_coverage.hpp"
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <random>
#include <set>
#include <stdexcept>
#include <string>

using namespace cb;
namespace vc = cb::voxel_coverage;
namespace {
using D = std::array<double, 3>;
using Triangle = std::array<Vec3, 3>;
using Index = std::array<int, 3>;
D convert(Vec3 p) { return {p.x,p.y,p.z}; }
D sub(D a,D b) {return {a[0]-b[0],a[1]-b[1],a[2]-b[2]};}
D cross_d(D a,D b) {return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};}
double dot_d(D a,D b) {return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];}
void require(bool condition,const std::string& message) {
  if(!condition)throw std::runtime_error(message);
}

// Independent separating-axis oracle: three box normals, triangle normal,
// and nine edge/box-axis cross products. No polygon clipping or depth ranges.
bool oracle(const Triangle& triangle,const vc::Grid& grid,Index index) {
  D centre{double(grid.origin.x)+(index[0]+.5)*grid.cell,
           double(grid.origin.y)+(index[1]+.5)*grid.cell,
           double(grid.origin.z)+(index[2]+.5)*grid.cell};
  std::array<D,3> p;
  for(int i=0;i<3;++i)p[i]=sub(convert(triangle[i]),centre);
  const std::array<D,3> edges{sub(p[1],p[0]),sub(p[2],p[1]),sub(p[0],p[2])};
  const D normal=cross_d(edges[0],sub(p[2],p[0]));
  if(dot_d(normal,normal)==0)return false;
  const std::array<D,3> axes{{{1,0,0},{0,1,0},{0,0,1}}};
  auto separates=[&](D axis) {
    const double radius=grid.cell*.5*(std::abs(axis[0])+std::abs(axis[1])+std::abs(axis[2]));
    const double a=dot_d(axis,p[0]),b=dot_d(axis,p[1]),c=dot_d(axis,p[2]);
    // Numerical tolerance is only for this independent double-precision oracle
    // at exactly coincident faces; the production helper does not inflate boxes.
    const double error=32*std::numeric_limits<double>::epsilon()*
        std::max({std::abs(a),std::abs(b),std::abs(c),radius,1.});
    return std::min({a,b,c})>radius+error||std::max({a,b,c})<-radius-error;
  };
  for(D axis:axes)if(separates(axis))return false;
  if(separates(normal))return false;
  for(D edge:edges)for(D axis:axes)if(separates(cross_d(edge,axis)))return false;
  return true;
}

std::size_t checked_triangles=0,checked_cells=0,emitted=0;
std::set<Index> check(const Triangle& triangle,const vc::Grid& grid,const char* name) {
  std::set<Index> actual;
  const D pa=convert(triangle[0]),pb=convert(triangle[1]),pc=convert(triangle[2]);
  const D ab=sub(pb,pa),ac=sub(pc,pa),normal=cross_d(ab,ac);
  const double normal_length=std::sqrt(dot_d(normal,normal));
  const double scale=std::max({1.,std::sqrt(dot_d(ab,ab)),std::sqrt(dot_d(ac,ac)),
                              std::abs(pa[0]),std::abs(pa[1]),std::abs(pa[2])});
  const auto count=vc::triangle_cells(grid,triangle[0],triangle[1],triangle[2],
      [&](int x,int y,int z,Vec3 point) {
    const Index at{x,y,z};
    require(actual.insert(at).second,std::string(name)+": duplicate cell");
    const D p=convert(point),origin=convert(grid.origin);
    for(int axis=0;axis<3;++axis) {
      require(at[axis]>=0&&at[axis]<grid.dimensions[axis],std::string(name)+": cell outside grid");
      const double low=origin[axis]+at[axis]*double(grid.cell),high=low+grid.cell;
      const double error=4*std::numeric_limits<float>::epsilon()*std::max({1.,std::abs(low),std::abs(high)});
      require(std::isfinite(p[axis])&&p[axis]>=low-error&&p[axis]<=high+error,
              std::string(name)+": representative point outside cell");
    }
    require(std::abs(dot_d(normal,sub(p,pa)))<=normal_length*scale*4e-7,
            std::string(name)+": representative point outside triangle plane");
    // Each directed triangle edge bounds a half-space in its plane.
    const std::array<D,3> vertex{pa,pb,pc};
    for(int edge=0;edge<3;++edge) {
      D e=sub(vertex[(edge+1)%3],vertex[edge]);
      const double inward=dot_d(cross_d(e,sub(p,vertex[edge])),normal);
      const double error=normal_length*std::sqrt(dot_d(e,e))*scale*4e-7;
      require(inward>=-error,std::string(name)+": representative point outside triangle edges");
    }
  });
  require(count==actual.size(),std::string(name)+": count mismatch");
  for(int z=0;z<grid.dimensions[2];++z)
    for(int y=0;y<grid.dimensions[1];++y)
      for(int x=0;x<grid.dimensions[0];++x) {
        const Index at{x,y,z};const bool expected=oracle(triangle,grid,at);
        if(expected!=(actual.count(at)!=0))
          throw std::runtime_error(std::string(name)+(expected?": missed oracle cell ":": false cell ")+
                                   std::to_string(x)+","+std::to_string(y)+","+std::to_string(z));
        ++checked_cells;
      }
  ++checked_triangles;emitted+=count;return actual;
}
} // namespace

int main() {
  try {
    const vc::Grid grid{{0,-2,0},.5f,{8,12,8}};
    Triangle slope{{{0,.3f,0},{100,100.3f,0},{0,.3f,100}}};
    const auto result=check(slope,grid,"long sloped floor");
    require(result.count({0,4,0})!=0,"original missing-depthcell reproduction was not covered");
    // Axis permutations, signs and winding exercise every choice of major axis.
    const std::array<std::array<int,3>,6> permutations{{{0,1,2},{0,2,1},{1,0,2},{1,2,0},{2,0,1},{2,1,0}}};
    for(const auto& permutation:permutations)for(int signs=0;signs<8;++signs) {
      Triangle t;
      for(int i=0;i<3;++i) {
        const D p=convert(slope[i]);D q;
        for(int axis=0;axis<3;++axis)q[axis]=p[permutation[axis]]*((signs&(1<<axis))?-1:1);
        t[i]={float(q[0]),float(q[1]),float(q[2])};
      }
      const vc::Grid g{{-2,-2,-2},.5f,{8,8,8}};
      check(t,g,"oriented long slope");std::swap(t[1],t[2]);check(t,g,"reversed long slope");
    }
    const vc::Grid unit{{-2,-2,-2},.5f,{8,8,8}};
    check({{{-100,.13f,.11f},{100,.13f,.11f},{-100,.130001f,.11f}}},unit,"long thin valid triangle");
    check({{{0,0,0},{1,1,1},{2,2,2}}},unit,"collinear degenerate");
    check({{{0,0,0},{0,0,0},{0,0,0}}},unit,"point degenerate");
    check({{{0,0,0},{2,0,0},{0,0,2}}},unit,"coincident cell faces");
    check({{{2,2,2},{3,2,2},{2,3,2}}},unit,"touching grid corner");
    check({{{2.1f,2.1f,2.1f},{3,2.1f,2.1f},{2.1f,3,2.1f}}},unit,"outside grid");
    std::mt19937 rng(671298);
    std::uniform_real_distribution<float> coordinate(-3.f,3.f);
    for(float cell:{.125f,.5f,8.f})for(int sample=0;sample<220;++sample) {
      const vc::Grid g{{-2*cell,-2*cell,-2*cell},cell,{5,5,5}};
      Triangle t;
      for(auto& point:t)point={coordinate(rng)*cell,coordinate(rng)*cell,coordinate(rng)*cell};
      check(t,g,"random SAT oracle");
    }
    // The real fields are translated by hundreds of metres. Keep oracle
    // agreement there too, including inputs spanning far beyond a local grid.
    for(Vec3 origin:std::array<Vec3,3>{{{-768,-32,-768},{-448,247,339},{2710,0,2235}}})
      for(int sample=0;sample<100;++sample) {
        const vc::Grid g{origin,.5f,{5,5,5}};Triangle t;
        const float reach=sample%3==0?100.f:2.f;
        for(auto& point:t)point=origin+Vec3{coordinate(rng)*reach,coordinate(rng)*reach,coordinate(rng)*reach};
        check(t,g,"translated SAT oracle");
      }
    std::cout<<"PASS conservative triangle coverage: "<<checked_triangles<<" triangles, "
             <<checked_cells<<" independent triangle/box controls, "<<emitted
             <<" unique covered cells; exact slope, thin, boundary, winding and all-axis cases\n";
  } catch(const std::exception& error) {
    std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;
  }
}
