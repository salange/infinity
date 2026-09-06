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

CityUpload upload_city_scene(render::Rhi& rhi, const city::Scene& scene, const gen::SiteFrame& frame,
                             double datum_m) {
  CityUpload up;
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
  const auto convert_vertex = [&](const city::Vertex& v, render::Rhi::CityVertex* o) {
    double p[3];
    place(v.position.x, v.position.y, v.position.z, p);
    o->position[0] = static_cast<float>(p[0]);
    o->position[1] = static_cast<float>(p[1]);
    o->position[2] = static_cast<float>(p[2]);
    rotate(v.normal.x, v.normal.y, v.normal.z, o->normal);
    rotate(v.tangent.x, v.tangent.y, v.tangent.z, o->tangent);
    o->tangent[3] = v.tangent.w;
    o->uv[0] = v.uv.x;
    o->uv[1] = v.uv.y;
    o->material = v.material;
    o->aux[0] = v.aux.x;
    o->aux[1] = v.aux.y;
    o->aux[2] = v.aux.z;
    o->aux[3] = v.aux.w;
  };
  // Pieces: triangles are appended lot by lot with their own vertices,
  // so walking the index buffer in order and cutting whenever the
  // referenced vertex span would exceed the cap yields contiguous spans.
  constexpr std::size_t kMaxPieceVertices = 1400000;  // ~95 MB of city vertices
  const auto convert = [&](const city::Mesh& mesh, bool foliage) {
    if (mesh.indices.empty()) return;
    std::size_t tri = 0;
    const std::size_t tri_count = mesh.indices.size() / 3;
    while (tri < tri_count) {
      std::uint32_t vmin = 0xFFFFFFFFu;
      std::uint32_t vmax = 0;
      std::size_t end = tri;
      for (; end < tri_count; ++end) {
        std::uint32_t lo = vmin;
        std::uint32_t hi = vmax;
        for (int k = 0; k < 3; ++k) {
          const std::uint32_t idx = mesh.indices[end * 3 + k];
          lo = std::min(lo, idx);
          hi = std::max(hi, idx);
        }
        if (end > tri && static_cast<std::size_t>(hi - lo) + 1 > kMaxPieceVertices) break;
        vmin = lo;
        vmax = hi;
      }
      std::vector<render::Rhi::CityVertex> verts(static_cast<std::size_t>(vmax - vmin) + 1);
      for (std::uint32_t v = vmin; v <= vmax; ++v) convert_vertex(mesh.vertices[v], &verts[v - vmin]);
      std::vector<std::uint32_t> indices((end - tri) * 3);
      for (std::size_t i = 0; i < indices.size(); ++i) indices[i] = mesh.indices[tri * 3 + i] - vmin;
      // Winding: the frame mapping (x, y, z) -> (east, up, north*-1) is a
      // proper rotation (east x north = up), so triangle orientation is
      // preserved.
      const std::uint32_t id = rhi.create_city_mesh(verts.data(), verts.size(), indices.data(), indices.size());
      if (id != 0) {
        CityUpload::Piece piece{id, foliage, static_cast<std::uint32_t>(end - tri), {0.0f, 0.0f, 0.0f}, 0.0f};
        float lo[3] = {1e30f, 1e30f, 1e30f};
        float hi[3] = {-1e30f, -1e30f, -1e30f};
        for (const render::Rhi::CityVertex& v : verts) {
          for (int c = 0; c < 3; ++c) {
            lo[c] = std::min(lo[c], v.position[c]);
            hi[c] = std::max(hi[c], v.position[c]);
          }
        }
        float r2 = 0.0f;
        for (int c = 0; c < 3; ++c) {
          piece.centre[c] = 0.5f * (lo[c] + hi[c]);
          r2 += 0.25f * (hi[c] - lo[c]) * (hi[c] - lo[c]);
        }
        piece.radius = std::sqrt(r2);
        up.pieces.push_back(piece);
      }
      tri = end;
    }
  };
  convert(scene.opaque, false);
  convert(scene.foliage, true);
  up.triangles = static_cast<std::uint32_t>((scene.opaque.indices.size() + scene.foliage.indices.size()) / 3);
  for (const city::PointLight& l : scene.lights) {
    CityUpload::Light out;
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
  up.draws = scene.draws;
  return up;
}

void release_city_upload(render::Rhi& rhi, CityUpload* upload) {
  for (const CityUpload::Piece& piece : upload->pieces) rhi.destroy_mesh(piece.mesh);
  upload->pieces.clear();
  upload->lights.clear();
  upload->draws.clear();
}

void draw_city_upload(const CityUpload& upload, const render::Vec3& camera_pos,
                      const render::Mat4& view_projection, std::vector<render::Rhi::DrawItem>* items) {
  const render::Vec3 origin{upload.origin[0], upload.origin[1], upload.origin[2]};
  const render::Vec3 translation = origin - camera_pos;
  const render::Mat4 model = render::translate(translation);
  const render::Mat4 mvp = render::mul(view_projection, model);
  constexpr double kTilePeriod = 256.0;
  for (const CityUpload::Piece& piece : upload.pieces) {
    render::Rhi::DrawItem item;
    item.mesh = piece.mesh;
    item.bounds[0] = static_cast<float>(translation.x) + piece.centre[0];
    item.bounds[1] = static_cast<float>(translation.y) + piece.centre[1];
    item.bounds[2] = static_cast<float>(translation.z) + piece.centre[2];
    item.bounds[3] = piece.radius;
    item.mode = 8;
    item.shadow_caster = true;
    std::memcpy(item.mvp, mvp.m, sizeof(mvp.m));
    item.aux[0] = static_cast<float>(translation.x);
    item.aux[1] = static_cast<float>(translation.y);
    item.aux[2] = static_cast<float>(translation.z);
    item.extra[0] = static_cast<float>(std::fmod(upload.origin[0], kTilePeriod));
    item.extra[1] = static_cast<float>(std::fmod(upload.origin[1], kTilePeriod));
    item.extra[2] = static_cast<float>(std::fmod(upload.origin[2], kTilePeriod));
    items->push_back(item);
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
