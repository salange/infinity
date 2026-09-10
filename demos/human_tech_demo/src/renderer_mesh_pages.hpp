#pragma once
// Lossless triangle-ordered vertex pages. Index positions remain global so
// existing draw ranges and GPU occlusion arguments retain their identity.
#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

namespace cb::mesh_pages {
struct Page {
  std::uint32_t first{}, count{}, vertex_first{}, vertex_count{};
  // A triangle whose source vertices span more than one buffer can still be
  // represented exactly by three copied vertices, without a global remap map.
  bool isolated{};
  std::array<std::uint32_t, 3> sources{};
  std::uint32_t source(std::uint32_t local) const {
    return isolated ? sources.at(local) : vertex_first + local;
  }
  std::uint32_t index(std::uint32_t global, std::uint32_t source_index) const {
    return isolated ? global - first : source_index - vertex_first;
  }
};

inline void validate(const std::vector<std::uint32_t>& indices,std::size_t vertices) {
  if(indices.size()%3 || indices.size()>std::numeric_limits<std::uint32_t>::max())
    throw std::length_error("mesh requires uint32 triangle-list index positions");
  if(vertices>std::numeric_limits<std::uint32_t>::max())
    throw std::length_error("source vertex count exceeds uint32");
  for(auto i:indices)if(i>=vertices)
    throw std::length_error("mesh index references a missing source vertex");
}

inline std::vector<Page> partition(const std::vector<std::uint32_t>& indices,
                                   std::size_t vertices, std::uint64_t cap) {
  validate(indices,vertices);
  if(cap<3)throw std::length_error("vertex page must hold at least one triangle");
  if(indices.empty())return {};
  if(vertices<=cap)return {{0,static_cast<std::uint32_t>(indices.size()),0,
                           static_cast<std::uint32_t>(vertices),false,{}}};
  std::vector<Page> out;
  Page current{};
  auto flush=[&] {if(current.count)out.push_back(current);current={};};
  for(std::uint32_t first=0;first<indices.size();first+=3) {
    const auto a=indices[first],b=indices[first+1],c=indices[first+2];
    const auto lo=std::min({a,b,c}),hi=std::max({a,b,c});
    if(std::uint64_t(hi)-lo+1>cap) {
      flush();out.push_back({first,3,0,3,true,{a,b,c}});continue;
    }
    if(current.count) {
      const auto low=std::min(lo,current.vertex_first);
      const auto high=std::max(hi,current.vertex_first+current.vertex_count-1);
      if(std::uint64_t(high)-low+1>cap)flush();
      else {current.vertex_first=low;current.vertex_count=high-low+1;}
    }
    if(!current.count)current={first,0,lo,hi-lo+1,false,{}};
    current.count+=3;
  }
  flush();return out;
}

inline std::size_t page_at(const std::vector<Page>& pages,std::uint32_t first) {
  auto it=std::upper_bound(pages.begin(),pages.end(),first,
      [](std::uint32_t value,const Page& p){return value<p.first;});
  if(it==pages.begin())throw std::out_of_range("draw begins before vertex pages");
  --it;
  if(std::uint64_t(first)>=std::uint64_t(it->first)+it->count)
    throw std::out_of_range("draw begins after vertex pages");
  return static_cast<std::size_t>(it-pages.begin());
}

template<class Emit>
void spans(const std::vector<Page>& pages,std::uint32_t first,
           std::uint32_t count,Emit emit) {
  if((first%3)||(count%3))throw std::length_error("draw splits a triangle");
  if(!count)return;
  std::uint64_t end=std::uint64_t(first)+count;
  if(pages.empty()||end>std::uint64_t(pages.back().first)+pages.back().count)
    throw std::out_of_range("draw exceeds vertex pages");
  auto page=page_at(pages,first);
  while(std::uint64_t(first)<end) {
    const auto stop=std::min(end,std::uint64_t(pages[page].first)+pages[page].count);
    const auto n=static_cast<std::uint32_t>(stop-first);
    emit(page,first,n);first+=n;++page;
  }
}

template<class Emit>
void candidate_spans(const std::vector<Page>& pages,std::uint32_t first,
                     std::uint32_t count,std::uint64_t triangle_budget,Emit emit) {
  const bool direct=count/3>triangle_budget;
  spans(pages,first,count,[&](auto page,auto begin,auto n) {
    emit(page,begin,n,direct);
  });
}
} // namespace cb::mesh_pages
