#include "city_render.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "gen/material.hpp"

namespace inf::app {

namespace {

// Texture set name of the city material table -> shared library id.
int layer_for(const std::string& set) {
  if (set.empty() || set == "flat") return -1;
  if (set == "soil") return gen::material_by_name("meadow");
  return gen::material_by_name(set.c_str());
}

// Bounding sphere of a vertex span.
void sphere_of(const std::vector<render::Rhi::CityVertex>& verts, std::size_t begin, std::size_t end, float* centre,
               float* radius) {
  float lo[3] = {1e30f, 1e30f, 1e30f};
  float hi[3] = {-1e30f, -1e30f, -1e30f};
  for (std::size_t v = begin; v < end; ++v) {
    for (int c = 0; c < 3; ++c) {
      lo[c] = std::min(lo[c], verts[v].position[c]);
      hi[c] = std::max(hi[c], verts[v].position[c]);
    }
  }
  float r2 = 0.0f;
  for (int c = 0; c < 3; ++c) {
    centre[c] = 0.5f * (lo[c] + hi[c]);
    r2 += 0.25f * (hi[c] - lo[c]) * (hi[c] - lo[c]);
  }
  *radius = std::sqrt(r2);
}

// The frustum planes of a column-major view-projection (WebGPU clip z
// in [0, 1]); the far plane of the reversed infinite projection is
// omitted.
struct Frustum {
  float planes[5][4];
  explicit Frustum(const render::Mat4& vp) {
    const float* m = vp.m;
    const auto row = [&](int i, float* out) {
      out[0] = m[i];
      out[1] = m[4 + i];
      out[2] = m[8 + i];
      out[3] = m[12 + i];
    };
    float r0[4], r1[4], r2[4], r3[4];
    row(0, r0);
    row(1, r1);
    row(2, r2);
    row(3, r3);
    for (int k = 0; k < 4; ++k) {
      planes[0][k] = r3[k] + r0[k];  // left
      planes[1][k] = r3[k] - r0[k];  // right
      planes[2][k] = r3[k] + r1[k];  // bottom
      planes[3][k] = r3[k] - r1[k];  // top
      planes[4][k] = r2[k];          // near (z >= 0)
    }
    for (auto& p : planes) {
      const float len = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
      if (len > 0.0f) {
        for (float& v : p) v /= len;
      }
    }
  }
  bool visible(const float* c, float r) const {
    for (const auto& p : planes) {
      if (p[0] * c[0] + p[1] * c[1] + p[2] * c[2] + p[3] < -r) return false;
    }
    return true;
  }
};

}  // namespace

void upload_city_materials(render::Rhi& rhi, const std::vector<city::MaterialDesc>& materials) {
  std::vector<render::Rhi::CityMaterial> table;
  table.reserve(materials.size());
  for (const city::MaterialDesc& d : materials) {
    render::Rhi::CityMaterial m;
    m.base_color[0] = d.base_color.x;
    m.base_color[1] = d.base_color.y;
    m.base_color[2] = d.base_color.z;
    m.base_color[3] = 0.5f;
    m.params[0] = d.roughness;
    m.params[1] = d.metallic;
    m.params[2] = d.emissive;
    m.params[3] = d.normal_strength;
    const int layer = layer_for(d.albedo_set);
    m.tex[0] = static_cast<float>(layer);
    m.tex[1] = static_cast<float>(layer);
    m.tex[2] = static_cast<float>(layer);
    m.tex[3] = d.uv_scale;
    m.misc[0] = static_cast<float>(d.flags);
    m.misc[1] = d.tint2.x;
    m.misc[2] = d.tint2.y;
    m.misc[3] = d.tint2.z;
    m.room[0] = d.room_w;
    m.room[1] = d.room_h;
    m.room[2] = d.room_d;
    m.room[3] = d.lit_probability;
    table.push_back(m);
  }
  rhi.set_city_materials(table.data(), table.size());
}

CityUploadData prepare_city_upload(const city::Scene& scene, const gen::SiteFrame& frame, double datum_m) {
  CityUploadData up;
  const double R = frame.radius_m;
  const double ex[3] = {frame.east.x.to_double(), frame.east.y.to_double(), frame.east.z.to_double()};
  const double ny[3] = {frame.north.x.to_double(), frame.north.y.to_double(), frame.north.z.to_double()};
  const double uz[3] = {frame.up.x.to_double(), frame.up.y.to_double(), frame.up.z.to_double()};
  const double origin_r = R + datum_m;
  up.origin[0] = uz[0] * origin_r;
  up.origin[1] = uz[1] * origin_r;
  up.origin[2] = uz[2] * origin_r;
  // Scene (x, y, z) -> site-local east = x, north = -z, up = y.
  const auto place = [&](double x, double y, double z, double* out) {
    const gen::Dir3 d = frame.to_dir(x, -z);
    const double r = R + datum_m + y;
    out[0] = d.x.to_double() * r - up.origin[0];
    out[1] = d.y.to_double() * r - up.origin[1];
    out[2] = d.z.to_double() * r - up.origin[2];
  };
  const auto rotate = [&](float x, float y, float z, float* out) {
    // east*x + north*(-z) + up*y
    out[0] = static_cast<float>(ex[0] * x - ny[0] * z + uz[0] * y);
    out[1] = static_cast<float>(ex[1] * x - ny[1] * z + uz[1] * y);
    out[2] = static_cast<float>(ex[2] * x - ny[2] * z + uz[2] * y);
  };
  const auto unit = [](float* v) {
    const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 1e-12f) {
      v[0] /= l;
      v[1] /= l;
      v[2] /= l;
    } else {
      v[0] = 0.0f;
      v[1] = 1.0f;
      v[2] = 0.0f;
    }
  };
  // Placed on the sphere, rotated by the frame, packed into the
  // renderer's 32-byte layout (T0022 B.1).
  const auto convert_vertex = [&](const city::Vertex& v, render::Rhi::CityVertex* o) {
    double p[3];
    place(v.position.x, v.position.y, v.position.z, p);
    const float position[3] = {static_cast<float>(p[0]), static_cast<float>(p[1]), static_cast<float>(p[2])};
    float normal[3];
    float tangent[4];
    rotate(v.normal.x, v.normal.y, v.normal.z, normal);
    rotate(v.tangent.x, v.tangent.y, v.tangent.z, tangent);
    unit(normal);
    unit(tangent);
    tangent[3] = v.tangent.w;
    const float uv[2] = {v.uv.x, v.uv.y};
    const float aux[4] = {v.aux.x, v.aux.y, v.aux.z, v.aux.w};
    *o = render::Rhi::pack_city_vertex(position, normal, tangent, uv, v.material, aux);
  };
  // Pieces: ranges are appended in index order with their own vertices,
  // so walking the ranges and cutting whenever the referenced vertex
  // span would exceed the cap yields contiguous spans, and every range
  // lies inside one piece.
  constexpr std::size_t kMaxPieceVertices = 3000000;  // ~92 MB of packed city vertices
  const auto span_of = [](const city::Mesh& mesh, std::uint32_t first, std::uint32_t count, std::uint32_t* lo,
                          std::uint32_t* hi) {
    for (std::uint32_t i = first; i < first + count; ++i) {
      *lo = std::min(*lo, mesh.indices[i]);
      *hi = std::max(*hi, mesh.indices[i]);
    }
  };
  const auto convert_range = [&](const city::DrawRange& d, const CityUploadData::Piece& piece, std::uint32_t piece_index,
                                 std::uint32_t index_first, std::uint32_t vmin, const city::Mesh& mesh) {
    CityUploadData::Range out;
    out.piece = piece_index;
    out.first = d.first - index_first;
    out.count = d.count;
    out.lod_group = d.lod_group;
    out.lod_level = d.lod_level;
    out.lod_max_distance = d.lod_max_distance;
    out.has_fine = d.has_fine;
    if (d.radius > 0.0f) {
      double c[3];
      place(d.centre.x, d.centre.y, d.centre.z, c);
      out.centre[0] = static_cast<float>(c[0]);
      out.centre[1] = static_cast<float>(c[1]);
      out.centre[2] = static_cast<float>(c[2]);
      out.radius = d.radius;
    } else {
      // Unbounded range: its own vertex span.
      std::uint32_t lo = 0xFFFFFFFFu;
      std::uint32_t hi = 0;
      span_of(mesh, d.first, d.count, &lo, &hi);
      sphere_of(piece.vertices, lo - vmin, static_cast<std::size_t>(hi - vmin) + 1, out.centre, &out.radius);
    }
    return out;
  };
  const auto convert_mesh = [&](const city::Mesh& mesh, const std::vector<city::DrawRange>& ranges,
                                const std::vector<city::DrawRange>& fine, bool foliage) {
    if (mesh.indices.empty()) return;
    std::vector<city::DrawRange> all = ranges;
    if (all.empty()) {
      city::DrawRange whole;
      whole.first = 0;
      whole.count = static_cast<std::uint32_t>(mesh.indices.size());
      all.push_back(whole);
    }
    std::size_t r = 0;
    std::size_t f = 0;  // cursor into the sorted fine ranges
    while (r < all.size()) {
      // Grow a piece over consecutive ranges.
      std::uint32_t vmin = 0xFFFFFFFFu;
      std::uint32_t vmax = 0;
      std::size_t end = r;
      for (; end < all.size(); ++end) {
        std::uint32_t lo = vmin;
        std::uint32_t hi = vmax;
        span_of(mesh, all[end].first, all[end].count, &lo, &hi);
        if (end > r && static_cast<std::size_t>(hi - lo) + 1 > kMaxPieceVertices) break;
        vmin = lo;
        vmax = hi;
      }
      CityUploadData::Piece piece;
      piece.foliage = foliage;
      piece.vertices.resize(static_cast<std::size_t>(vmax - vmin) + 1);
      for (std::uint32_t v = vmin; v <= vmax; ++v) convert_vertex(mesh.vertices[v], &piece.vertices[v - vmin]);
      const std::uint32_t index_first = all[r].first;
      const std::uint32_t index_end = all[end - 1].first + all[end - 1].count;
      piece.indices.resize(index_end - index_first);
      for (std::uint32_t i = index_first; i < index_end; ++i) piece.indices[i - index_first] = mesh.indices[i] - vmin;
      sphere_of(piece.vertices, 0, piece.vertices.size(), piece.centre, &piece.radius);
      const std::uint32_t piece_index = static_cast<std::uint32_t>(up.pieces.size());
      for (std::size_t k = r; k < end; ++k) up.ranges.push_back(convert_range(all[k], piece, piece_index, index_first, vmin, mesh));
      // The fine ranges that fall inside this piece (both lists are
      // sorted by first).
      for (; f < fine.size() && fine[f].first < index_end; ++f) {
        if (fine[f].first < index_first) continue;
        up.fine.push_back(convert_range(fine[f], piece, piece_index, index_first, vmin, mesh));
      }
      up.pieces.push_back(std::move(piece));
      r = end;
    }
  };
  convert_mesh(scene.opaque, scene.draws, scene.fine, false);
  convert_mesh(scene.foliage, {}, {}, true);
  up.triangles = static_cast<std::uint32_t>((scene.opaque.indices.size() + scene.foliage.indices.size()) / 3);
  for (const city::PointLight& l : scene.lights) {
    CityUploadData::Light out;
    double p[3];
    place(l.position.x, l.position.y, l.position.z, p);
    out.position[0] = p[0] + up.origin[0];
    out.position[1] = p[1] + up.origin[1];
    out.position[2] = p[2] + up.origin[2];
    out.radius = l.radius;
    out.color[0] = l.color.x;
    out.color[1] = l.color.y;
    out.color[2] = l.color.z;
    out.intensity = l.intensity;
    up.lights.push_back(out);
  }
  return up;
}

CityUpload commit_city_upload(render::Rhi& rhi, CityUploadData&& data) {
  CityUpload up;
  std::memcpy(up.origin, data.origin, sizeof(up.origin));
  up.lights = std::move(data.lights);
  up.triangles = data.triangles;
  std::vector<std::uint32_t> piece_map(data.pieces.size(), 0xFFFFFFFFu);
  for (std::size_t i = 0; i < data.pieces.size(); ++i) {
    const CityUploadData::Piece& p = data.pieces[i];
    // Winding: the frame mapping (x, y, z) -> (east, up, north*-1) is a
    // proper rotation (east x north = up), so triangle orientation is
    // preserved.
    const std::uint32_t id = rhi.create_city_mesh(p.vertices.data(), p.vertices.size(), p.indices.data(), p.indices.size());
    if (id == 0) continue;
    piece_map[i] = static_cast<std::uint32_t>(up.pieces.size());
    up.pieces.push_back(CityUpload::Piece{id, p.foliage, static_cast<std::uint32_t>(p.indices.size() / 3)});
  }
  for (CityUploadData::Range& r : data.ranges) {
    if (piece_map[r.piece] == 0xFFFFFFFFu) continue;
    r.piece = piece_map[r.piece];
    up.ranges.push_back(r);
  }
  for (CityUploadData::Range& r : data.fine) {
    if (piece_map[r.piece] == 0xFFFFFFFFu) continue;
    r.piece = piece_map[r.piece];
    up.fine.push_back(r);
  }
  data.pieces.clear();
  data.ranges.clear();
  data.fine.clear();
  return up;
}

CityUpload upload_city_scene(render::Rhi& rhi, const city::Scene& scene, const gen::SiteFrame& frame,
                             double datum_m) {
  return commit_city_upload(rhi, prepare_city_upload(scene, frame, datum_m));
}

void release_city_upload(render::Rhi& rhi, CityUpload* upload) {
  for (const CityUpload::Piece& piece : upload->pieces) rhi.destroy_mesh(piece.mesh);
  upload->pieces.clear();
  upload->ranges.clear();
  upload->fine.clear();
  upload->lights.clear();
}

void draw_city_upload(const CityUpload& upload, const render::Vec3& camera_pos,
                      const render::Mat4& view_projection, const CityDrawOptions& options,
                      std::vector<render::Rhi::DrawItem>* items, std::vector<render::Rhi::CityRange>* ranges,
                      CityDrawStats* stats) {
  if (upload.pieces.empty()) return;
  const render::Vec3 origin{upload.origin[0], upload.origin[1], upload.origin[2]};
  const render::Vec3 translation = origin - camera_pos;
  const render::Mat4 model = render::translate(translation);
  const render::Mat4 mvp = render::mul(view_projection, model);
  constexpr double kTilePeriod = 256.0;
  const Frustum frustum(view_projection);
  const auto centre_rel = [&](const CityUploadData::Range& r, float* c) {
    c[0] = static_cast<float>(translation.x) + r.centre[0];
    c[1] = static_cast<float>(translation.y) + r.centre[1];
    c[2] = static_cast<float>(translation.z) + r.centre[2];
  };
  // Level per group: the finest level whose switch distance the camera
  // is within (the demo's rule), clamped to the finest level the group
  // actually has (the geometry budget); the coarsest level; and the
  // shadow level — the coarsest REAL level (< 3; the far shell of level
  // 3 casts nothing useful).
  int groups = 0;
  for (const auto& r : upload.ranges) groups = std::max(groups, r.lod_group + 1);
  std::vector<int> chosen(static_cast<std::size_t>(groups), 99);
  std::vector<int> finest(static_cast<std::size_t>(groups), 99);
  std::vector<int> coarsest(static_cast<std::size_t>(groups), -1);
  std::vector<int> shadow_level(static_cast<std::size_t>(groups), -1);
  for (const auto& r : upload.ranges) {
    if (r.lod_group < 0) continue;
    float c[3];
    centre_rel(r, c);
    const float dist = std::sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
    const std::size_t g = static_cast<std::size_t>(r.lod_group);
    if (dist < r.lod_max_distance) chosen[g] = std::min(chosen[g], r.lod_level);
    finest[g] = std::min(finest[g], r.lod_level);
    coarsest[g] = std::max(coarsest[g], r.lod_level);
    if (r.lod_level < 3) shadow_level[g] = std::max(shadow_level[g], r.lod_level);
  }
  for (std::size_t g = 0; g < chosen.size(); ++g) {
    chosen[g] = std::min(std::max(chosen[g], finest[g]), coarsest[g]);
    if (shadow_level[g] < 0) shadow_level[g] = coarsest[g];
  }
  if (stats != nullptr) stats->resident_triangles += upload.triangles;
  // Ranges per piece, in piece order (one item each).
  std::vector<std::uint32_t> piece_first(upload.pieces.size(), 0);
  std::vector<std::uint32_t> piece_count(upload.pieces.size(), 0);
  std::vector<std::vector<render::Rhi::CityRange>> per_piece(upload.pieces.size());
  const auto emit = [&](const CityUploadData::Range& r, std::uint32_t flags, bool occludable) {
    render::Rhi::CityRange out;
    out.first = r.first;
    out.count = r.count;
    centre_rel(r, out.centre);
    out.radius = r.radius;
    out.flags = flags | (occludable && r.radius > 0.0f ? render::Rhi::kCityOcclude : 0u);
    per_piece[r.piece].push_back(out);
    if (stats != nullptr) {
      if ((flags & render::Rhi::kCityMain) != 0u) stats->drawn_triangles += r.count / 3;
      if ((flags & (render::Rhi::kCityCastNear | render::Rhi::kCityCastFar)) != 0u) stats->shadow_triangles += r.count / 3;
    }
  };
  const std::uint32_t cast_all = render::Rhi::kCityCastNear | render::Rhi::kCityCastFar;
  std::size_t fine_cursor = 0;  // the fine ranges are sorted by (piece, first), as the ranges are
  for (const auto& r : upload.ranges) {
    float c[3];
    centre_rel(r, c);
    const bool in_view = r.radius <= 0.0f || frustum.visible(c, r.radius);
    const float dist = std::sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
    bool main = false;
    std::uint32_t casts = 0;
    bool fine_expand = false;
    if (r.lod_group < 0) {
      main = in_view;
      casts = cast_all;
      // Far-cascade LOD: bounded ranges more than 350 m away (building
      // blocks, props) stay out of the far cascade.
      if (options.shadow_far_lod && r.radius > 0.0f && dist - r.radius > 350.0f) casts = render::Rhi::kCityCastNear;
      fine_expand = main && r.has_fine && options.fine_ranges;
    } else {
      const std::size_t g = static_cast<std::size_t>(r.lod_group);
      main = r.lod_level == chosen[g] && in_view;
      const bool has_shell = coarsest[g] == 3;
      if (r.lod_level == shadow_level[g]) {
        // The real geometry casts; with the far-cascade LOD the far
        // cascade takes the shell instead when the group has one.
        casts = (options.shadow_far_lod && has_shell) ? render::Rhi::kCityCastNear : cast_all;
      } else if (options.shadow_far_lod && r.lod_level == 3) {
        casts = render::Rhi::kCityCastFar;
      }
    }
    if (!main && casts == 0) continue;
    if (fine_expand) {
      // The block's buildings individually (frustum-tested) with the
      // gaps between them as unbounded ranges, so nothing is lost; the
      // block as a whole still casts its shadows in one range.
      if (casts != 0) emit(r, casts, false);
      const std::vector<CityUploadData::Range>& fine = upload.fine;
      while (fine_cursor < fine.size() &&
             (fine[fine_cursor].piece < r.piece || (fine[fine_cursor].piece == r.piece && fine[fine_cursor].first < r.first))) {
        ++fine_cursor;
      }
      std::uint32_t covered = r.first;
      std::size_t k = fine_cursor;
      for (; k < fine.size() && fine[k].piece == r.piece && fine[k].first < r.first + r.count; ++k) {
        const CityUploadData::Range& fr = fine[k];
        if (fr.first > covered) {
          CityUploadData::Range gap;
          gap.piece = r.piece;
          gap.first = covered;
          gap.count = fr.first - covered;
          emit(gap, render::Rhi::kCityMain, false);
        }
        float fc[3];
        centre_rel(fr, fc);
        if (frustum.visible(fc, fr.radius)) emit(fr, render::Rhi::kCityMain, true);
        covered = fr.first + fr.count;
      }
      if (covered < r.first + r.count) {
        CityUploadData::Range gap;
        gap.piece = r.piece;
        gap.first = covered;
        gap.count = r.first + r.count - covered;
        emit(gap, render::Rhi::kCityMain, false);
      }
      continue;
    }
    emit(r, (main ? render::Rhi::kCityMain : 0u) | casts, main);
  }
  for (std::size_t p = 0; p < upload.pieces.size(); ++p) {
    if (per_piece[p].empty()) continue;
    const CityUpload::Piece& piece = upload.pieces[p];
    piece_first[p] = static_cast<std::uint32_t>(ranges->size());
    piece_count[p] = static_cast<std::uint32_t>(per_piece[p].size());
    ranges->insert(ranges->end(), per_piece[p].begin(), per_piece[p].end());
    render::Rhi::DrawItem item;
    item.mesh = piece.mesh;
    item.mode = 8;
    item.city_range_first = piece_first[p];
    item.city_range_count = piece_count[p];
    item.shadow_caster = true;
    item.prepass = true;
    std::memcpy(item.mvp, mvp.m, sizeof(mvp.m));
    item.aux[0] = static_cast<float>(translation.x);
    item.aux[1] = static_cast<float>(translation.y);
    item.aux[2] = static_cast<float>(translation.z);
    item.extra[0] = static_cast<float>(std::fmod(upload.origin[0], kTilePeriod));
    item.extra[1] = static_cast<float>(std::fmod(upload.origin[1], kTilePeriod));
    item.extra[2] = static_cast<float>(std::fmod(upload.origin[2], kTilePeriod));
    item.bounds[3] = 0.0f;  // the ranges carry the bounds
    items->push_back(item);
    if (stats != nullptr) {
      ++stats->items;
      stats->ranges += piece_count[p];
    }
  }
}

void city_lights_for_frame(const CityUpload& upload, const render::Vec3& camera_pos, bool night,
                           std::vector<render::Rhi::CityLight>* out) {
  if (!night) return;
  const std::vector<const CityUpload*> one{&upload};
  city_lights_select(one, camera_pos, out);
}

void city_lights_select(const std::vector<const CityUpload*>& uploads, const render::Vec3& camera_pos,
                        std::vector<render::Rhi::CityLight>* out) {
  struct Ranked {
    double d2;
    const CityUpload::Light* light;
  };
  std::vector<Ranked> ranked;
  for (const CityUpload* upload : uploads) {
    for (const CityUpload::Light& l : upload->lights) {
      const double dx = l.position[0] - camera_pos.x;
      const double dy = l.position[1] - camera_pos.y;
      const double dz = l.position[2] - camera_pos.z;
      ranked.push_back(Ranked{dx * dx + dy * dy + dz * dz, &l});
    }
  }
  std::sort(ranked.begin(), ranked.end(), [](const Ranked& a, const Ranked& b) { return a.d2 < b.d2; });
  for (std::size_t i = 0; i < ranked.size() && out->size() < 64; ++i) {
    const CityUpload::Light& l = *ranked[i].light;
    render::Rhi::CityLight g;
    g.pos_radius[0] = static_cast<float>(l.position[0] - camera_pos.x);
    g.pos_radius[1] = static_cast<float>(l.position[1] - camera_pos.y);
    g.pos_radius[2] = static_cast<float>(l.position[2] - camera_pos.z);
    g.pos_radius[3] = l.radius;
    g.color_int[0] = l.color[0];
    g.color_int[1] = l.color[1];
    g.color_int[2] = l.color[2];
    g.color_int[3] = l.intensity;
    out->push_back(g);
  }
}

}  // namespace inf::app
