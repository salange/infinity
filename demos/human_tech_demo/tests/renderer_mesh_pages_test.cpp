#include "renderer_mesh_pages.hpp"
#include <cstring>
#include <iostream>
#include <random>

namespace {
void require(bool b,const char* why) {if(!b)throw std::runtime_error(why);}
void check(const std::vector<std::uint32_t>& indices,std::size_t count,std::uint32_t cap) {
  using Record=std::array<std::uint32_t,8>;
  std::vector<Record> vertices(count);
  for(std::size_t i=0;i<count;++i)for(int c=0;c<8;++c)
    vertices[i][c]=static_cast<std::uint32_t>(i*179+c*7919);
  const auto pages=cb::mesh_pages::partition(indices,count,cap);
  std::vector<std::uint32_t> rebased(indices.size());
  std::vector<std::vector<Record>> buffers;
  std::uint64_t cursor=0;
  for(const auto& p:pages) {
    require(p.first==cursor&&p.count%3==0&&p.vertex_count<=cap,"page gap, triangle split or cap overflow");
    auto& b=buffers.emplace_back();
    for(std::uint32_t i=0;i<p.vertex_count;++i)b.push_back(vertices[p.source(i)]);
    for(auto i=p.first;i<p.first+p.count;++i)rebased[i]=p.index(i,indices[i]);
    cursor+=p.count;
  }
  require(cursor==indices.size(),"triangle positions changed");
  // Exercise complete, partial, cross-page and single-triangle draw spans.
  for(std::uint32_t first=0;first<indices.size();first+=3) {
    auto check_span=[&](std::uint32_t length) {
      auto at=first;
      cb::mesh_pages::spans(pages,first,length,[&](std::size_t page,std::uint32_t begin,std::uint32_t n) {
        require(begin==at,"draw ordering changed");
        for(auto i=begin;i<begin+n;++i) {
          require(rebased[i]<buffers[page].size(),"local index outside bound vertex page");
          require(std::memcmp(buffers[page][rebased[i]].data(),vertices[indices[i]].data(),sizeof(Record))==0,
                  "packed source attribute bytes changed");
        }
        at+=n;
      });
      require(at==first+length,"draw omitted indices");
    };
    check_span(3);
    check_span(static_cast<std::uint32_t>(indices.size())-first);
  }
}
template<class F> void rejected(F f) {bool bad=false;try{f();}catch(const std::exception&){bad=true;}require(bad,"invalid input accepted");}
}
int main() {
  try {
    std::mt19937 random(26);
    std::vector<std::uint32_t> contiguous,scattered;
    for(std::uint32_t i=0;i<180;i+=3) {contiguous.insert(contiguous.end(),{i,i+1,i+2});
      scattered.insert(scattered.end(),{static_cast<std::uint32_t>(random()%181),
          static_cast<std::uint32_t>(random()%181),static_cast<std::uint32_t>(random()%181)});}
    for(auto cap:{3u,4u,9u,17u,64u,180u,181u,256u}) {
      check(contiguous,181,cap);check(scattered,181,cap);
      check({100,0,100,0,180,7,17,18,19,18,19,20,19,18,17},181,cap);
    }
    check({},0,3);
    cb::mesh_pages::validate({0,1,2},3);
    rejected([]{cb::mesh_pages::validate({0,1},3);});
    rejected([]{cb::mesh_pages::validate({0,1,3},3);});
    rejected([]{cb::mesh_pages::partition({0,1},3,3);});
    rejected([]{cb::mesh_pages::partition({0,1,3},3,3);});
    rejected([]{cb::mesh_pages::partition({0,1,2},3,2);});
    const auto p=cb::mesh_pages::partition({0,1,2},3,3);
    rejected([&]{cb::mesh_pages::spans(p,0,6,[](auto,auto,auto){});});
    rejected([&]{cb::mesh_pages::spans(p,1,2,[](auto,auto,auto){});});
    // Page splitting must not change the pre-existing large-range Hi-Z bypass.
    // Both child ranges fit the draw budget although the original exceeds it.
    std::vector<cb::mesh_pages::Page> two{{0,450000,0,450000,false,{}},
                                        {450000,300003,450000,300003,false,{}}};
    int pieces=0;
    cb::mesh_pages::candidate_spans(two,0,750003,250000,[&](auto,auto,auto n,bool direct) {
      require(direct&&n/3<=250000,"paging changed an original direct culling decision");++pieces;
    });
    require(pieces==2,"large culling range was not split");
    cb::mesh_pages::candidate_spans(two,300000,450000,250000,[&](auto,auto,auto,bool direct) {
      require(!direct,"paging changed an original indirect culling decision");
    });
    std::cout<<"Lossless vertex pages passed: tiny caps, scattered/reused indices, packed attribute bytes, global triangle positions and split draw spans\n";
  }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
