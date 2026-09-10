#include "renderer.hpp"
#include "renderer_batch.hpp"
#include "renderer_lights.hpp"
#include "renderer_memory.hpp"
#include <functional>
#include <memory>

#include <stb_image_write.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "voxel_transport.hpp"

namespace cb {

namespace {

constexpr std::uint32_t kCascades = 3;
constexpr std::uint32_t kBloomLevels = 5;
constexpr std::uint64_t kUniformStride = 256;

struct FrameUniform {
  Mat4 view, proj, view_proj, inv_view_proj, inv_proj;
  Mat4 shadow[kCascades];
  Vec4 camera_pos, sun_dir, sun_color;
  Vec4 sh[9];
  Vec4 cascade, cascade_extent, cascade_depth, screen, params, params2;
  Mat4 reflection_vp, puddle_reflection_vp, pond_reflection_vp;
  Vec4 transport; // volume enabled, reflection enabled, water height, secondary
                  // pass
  Vec4 voxel_min, voxel_extent, fine_min, fine_extent;
  Vec4 moon_dir, moon_color, moon_shape;
  Vec4 point_visibility;
};

struct GpuMaterial {
  Vec4 base_color, params, tex, misc, room;
};
struct GpuInstance {
  Mat4 model;
  Vec4 normal0, normal1, normal2, tint;
};
struct GpuLight {
  Vec4 pos_radius, color_int;
};

WGPUBindGroupLayoutEntry buffer_entry(std::uint32_t binding,
                                      WGPUShaderStage vis,
                                      WGPUBufferBindingType type, bool dynamic,
                                      std::uint64_t min_size) {
  WGPUBindGroupLayoutEntry e{};
  e.binding = binding;
  e.visibility = vis;
  e.buffer.type = type;
  e.buffer.hasDynamicOffset = dynamic ? 1U : 0U;
  e.buffer.minBindingSize = min_size;
  return e;
}
WGPUBindGroupLayoutEntry texture_entry(std::uint32_t binding,
                                       WGPUTextureSampleType type,
                                       WGPUTextureViewDimension dim) {
  WGPUBindGroupLayoutEntry e{};
  e.binding = binding;
  e.visibility = WGPUShaderStage_Fragment;
  e.texture.sampleType = type;
  e.texture.viewDimension = dim;
  return e;
}
WGPUBindGroupLayoutEntry sampler_entry(std::uint32_t binding,
                                       WGPUSamplerBindingType type) {
  WGPUBindGroupLayoutEntry e{};
  e.binding = binding;
  e.visibility = WGPUShaderStage_Fragment;
  e.sampler.type = type;
  return e;
}
WGPUBindGroupEntry bg_buffer(std::uint32_t binding, WGPUBuffer b,
                             std::uint64_t size) {
  WGPUBindGroupEntry e{};
  e.binding = binding;
  e.buffer = b;
  e.size = size;
  return e;
}
WGPUBindGroupEntry bg_texture(std::uint32_t binding, WGPUTextureView v) {
  WGPUBindGroupEntry e{};
  e.binding = binding;
  e.textureView = v;
  return e;
}
WGPUBindGroupEntry bg_sampler(std::uint32_t binding, WGPUSampler s) {
  WGPUBindGroupEntry e{};
  e.binding = binding;
  e.sampler = s;
  return e;
}

// GPU vertex: 32 bytes (see common.wgsl).
struct PackedVertex {
  float px, py, pz;
  std::int16_t nx, ny;
  std::int16_t tx, ty;
  std::uint16_t u, v; // half floats
  std::uint8_t mat_lo, mat_hi, rnd, occ_sign;
  std::uint16_t ax, ay; // unorm16, 655.35 m full scale
};
static_assert(sizeof(PackedVertex) == 32);

std::int16_t snorm16(float f) {
  return static_cast<std::int16_t>(
      std::lround(clampf(f, -1.0f, 1.0f) * 32767.0f));
}
void oct_encode(Vec3 n, std::int16_t *ox, std::int16_t *oy) {
  const float l1 = std::fabs(n.x) + std::fabs(n.y) + std::fabs(n.z);
  float x = n.x / std::max(l1, 1e-9f), y = n.y / std::max(l1, 1e-9f);
  if (n.z < 0.0f) {
    const float sx = x >= 0.0f ? 1.0f : -1.0f, sy = y >= 0.0f ? 1.0f : -1.0f;
    const float nx = (1.0f - std::fabs(y)) * sx,
                ny = (1.0f - std::fabs(x)) * sy;
    x = nx;
    y = ny;
  }
  *ox = snorm16(x);
  *oy = snorm16(y);
}
PackedVertex pack_vertex(const Vertex &v) {
  PackedVertex p{};
  p.px = v.position.x;
  p.py = v.position.y;
  p.pz = v.position.z;
  oct_encode(normalize(v.normal), &p.nx, &p.ny);
  oct_encode(normalize(Vec3{v.tangent.x, v.tangent.y, v.tangent.z}), &p.tx,
             &p.ty);
  p.u = float_to_half(v.uv.x);
  p.v = float_to_half(v.uv.y);
  p.mat_lo = static_cast<std::uint8_t>(v.material & 0xff);
  p.mat_hi = static_cast<std::uint8_t>((v.material >> 8) & 0xff);
  p.rnd = static_cast<std::uint8_t>(clampf(v.aux.z, 0.0f, 1.0f) * 255.0f);
  p.occ_sign = static_cast<std::uint8_t>(
                   std::lround(clampf(v.aux.w, 0.0f, 1.0f) * 127.0f)) |
               (v.tangent.w < 0.0f ? 0x80 : 0);
  p.ax = static_cast<std::uint16_t>(clampf(v.aux.x / 655.35f, 0.0f, 1.0f) *
                                    65535.0f);
  p.ay = static_cast<std::uint16_t>(clampf(v.aux.y / 655.35f, 0.0f, 1.0f) *
                                    65535.0f);
  return p;
}

WGPUVertexBufferLayout vertex_layout(WGPUVertexAttribute *attrs) {
  const WGPUVertexFormat fmts[6] = {
      WGPUVertexFormat_Float32x3, WGPUVertexFormat_Snorm16x2,
      WGPUVertexFormat_Snorm16x2, WGPUVertexFormat_Float16x2,
      WGPUVertexFormat_Uint8x4,   WGPUVertexFormat_Unorm16x2};
  const std::uint64_t offs[6] = {0, 12, 16, 20, 24, 28};
  for (std::uint32_t i = 0; i < 6; ++i) {
    attrs[i] = WGPUVertexAttribute{};
    attrs[i].format = fmts[i];
    attrs[i].offset = offs[i];
    attrs[i].shaderLocation = i;
  }
  WGPUVertexBufferLayout l{};
  l.arrayStride = sizeof(PackedVertex);
  l.stepMode = WGPUVertexStepMode_Vertex;
  l.attributeCount = 6;
  l.attributes = attrs;
  return l;
}

struct PipelineOpts {
  WGPUShaderModule module{nullptr};
  const char *vs{"vs_main"};
  const char *fs{nullptr}; // nullptr = depth only
  WGPUPipelineLayout layout{nullptr};
  WGPUTextureFormat color{WGPUTextureFormat_Undefined};
  WGPUTextureFormat depth{WGPUTextureFormat_Undefined};
  bool depth_write{true};
  WGPUCompareFunction depth_compare{WGPUCompareFunction_Less};
  std::uint32_t samples{1};
  WGPUCullMode cull{WGPUCullMode_Back};
  bool alpha_to_coverage{false};
  bool vertices{true}; // false = fullscreen triangle
  bool blend_add{false};
};

WGPURenderPipeline make_pipeline(WGPUDevice device, const PipelineOpts &o) {
  WGPUVertexAttribute attrs[6];
  WGPUVertexBufferLayout vl = vertex_layout(attrs);
  WGPURenderPipelineDescriptor d{};
  d.layout = o.layout;
  d.vertex.module = o.module;
  d.vertex.entryPoint = sv(o.vs);
  d.vertex.bufferCount = o.vertices ? 1 : 0;
  d.vertex.buffers = o.vertices ? &vl : nullptr;
  d.primitive.topology = WGPUPrimitiveTopology_TriangleList;
  d.primitive.frontFace = WGPUFrontFace_CCW;
  d.primitive.cullMode = o.cull;
  WGPUDepthStencilState ds{};
  if (o.depth != WGPUTextureFormat_Undefined) {
    ds.format = o.depth;
    ds.depthWriteEnabled =
        o.depth_write ? WGPUOptionalBool_True : WGPUOptionalBool_False;
    ds.depthCompare = o.depth_compare;
    ds.stencilFront.compare = WGPUCompareFunction_Always;
    ds.stencilBack.compare = WGPUCompareFunction_Always;
    ds.stencilReadMask = 0xFFFFFFFF;
    ds.stencilWriteMask = 0xFFFFFFFF;
    d.depthStencil = &ds;
  }
  d.multisample.count = o.samples;
  d.multisample.mask = 0xFFFFFFFF;
  d.multisample.alphaToCoverageEnabled = o.alpha_to_coverage ? 1U : 0U;
  WGPUFragmentState fs{};
  WGPUColorTargetState ct{};
  WGPUBlendState blend{};
  if (o.fs != nullptr) {
    fs.module = o.module;
    fs.entryPoint = sv(o.fs);
    if (o.color != WGPUTextureFormat_Undefined) {
      ct.format = o.color;
      ct.writeMask = WGPUColorWriteMask_All;
      if (o.blend_add) {
        blend.color.srcFactor = WGPUBlendFactor_One;
        blend.color.dstFactor = WGPUBlendFactor_One;
        blend.color.operation = WGPUBlendOperation_Add;
        blend.alpha = blend.color;
        ct.blend = &blend;
      }
      fs.targetCount = 1;
      fs.targets = &ct;
    }
    d.fragment = &fs;
  }
  return wgpuDeviceCreateRenderPipeline(device, &d);
}

WGPUPipelineLayout
make_layout(WGPUDevice device,
            std::initializer_list<WGPUBindGroupLayout> groups) {
  std::vector<WGPUBindGroupLayout> v(groups.begin(), groups.end());
  WGPUPipelineLayoutDescriptor d{};
  d.bindGroupLayoutCount = v.size();
  d.bindGroupLayouts = v.data();
  return wgpuDeviceCreatePipelineLayout(device, &d);
}

WGPUBindGroupLayout make_bgl(WGPUDevice device,
                             std::vector<WGPUBindGroupLayoutEntry> entries) {
  WGPUBindGroupLayoutDescriptor d{};
  d.entryCount = entries.size();
  d.entries = entries.data();
  return wgpuDeviceCreateBindGroupLayout(device, &d);
}

WGPUBindGroup make_bg(WGPUDevice device, WGPUBindGroupLayout layout,
                      std::vector<WGPUBindGroupEntry> entries) {
  WGPUBindGroupDescriptor d{};
  d.layout = layout;
  d.entryCount = entries.size();
  d.entries = entries.data();
  return wgpuDeviceCreateBindGroup(device, &d);
}

struct MeshBuffers {
  WGPUBuffer vertices{nullptr};
  WGPUBuffer indices{nullptr};
  std::uint32_t index_count{0};
  void release() {
    if (vertices != nullptr)
      wgpuBufferRelease(vertices);
    if (indices != nullptr)
      wgpuBufferRelease(indices);
    vertices = indices = nullptr;
    index_count = 0;
  }
};

struct PostParams {
  Vec4 a, b;
};

// Frustum planes from a view-projection matrix (Gribb & Hartmann), as
// (a,b,c,d) with the inside being positive.
struct Frustum {
  Vec4 planes[6];
  static Frustum from(const Mat4 &m, bool reversed_z = false) {
    Frustum f;
    auto row = [&](int r) {
      return Vec4{m.at(r, 0), m.at(r, 1), m.at(r, 2), m.at(r, 3)};
    };
    const Vec4 r0 = row(0), r1 = row(1), r2 = row(2), r3 = row(3);
    auto add = [](Vec4 a, Vec4 b) {
      return Vec4{a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w};
    };
    auto sub = [](Vec4 a, Vec4 b) {
      return Vec4{a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w};
    };
    f.planes[0] = add(r3, r0); // left
    f.planes[1] = sub(r3, r0); // right
    f.planes[2] = add(r3, r1); // bottom
    f.planes[3] = sub(r3, r1); // top
    // standard Z: 0 <= z <= w; reversed Z swaps which side is near
    f.planes[4] = reversed_z ? sub(r3, r2) : r2; // near
    f.planes[5] = reversed_z ? r2 : sub(r3, r2); // far
    for (Vec4 &p : f.planes) {
      const float l = std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z);
      if (l > 0) {
        p.x /= l;
        p.y /= l;
        p.z /= l;
        p.w /= l;
      }
    }
    return f;
  }
  bool visible(Vec3 c, float r) const {
    for (const Vec4 &p : planes) {
      if (p.x * c.x + p.y * c.y + p.z * c.z + p.w < -r)
        return false;
    }
    return true;
  }
};

} // namespace

struct Renderer::Impl {
  std::string shader_dir;
  // layouts
  WGPUBindGroupLayout frame_bgl{nullptr}, scene_tex_bgl{nullptr},
      cascade_bgl{nullptr}, leaf_bgl{nullptr}, ssao_bgl{nullptr},
      sky_bgl{nullptr}, post_bgl{nullptr};
  WGPUPipelineLayout main_layout{nullptr}, shadow_layout{nullptr},
      prepass_layout{nullptr}, ssao_layout{nullptr}, sky_layout{nullptr},
      post_layout{nullptr};
  // pipelines
  WGPURenderPipeline p_shadow{nullptr}, p_shadow_foliage{nullptr},
      p_prepass{nullptr}, p_prepass_foliage{nullptr}, p_prepass_glass{nullptr},
      p_ssao{nullptr}, p_blur{nullptr}, p_sky{nullptr}, p_main{nullptr},
      p_foliage{nullptr}, p_down{nullptr}, p_reflection_mip{nullptr},
      p_sky1{nullptr}, p_main1{nullptr},
      p_foliage1{nullptr}, // single-sample variants
      p_shadow_ms{nullptr}, p_glass{nullptr}, p_glass1{nullptr}, p_up{nullptr},
      p_tonemap{nullptr}, p_fxaa{nullptr}, p_blit{nullptr}, p_copy{nullptr};
  // buffers
  WGPUBuffer frame_buf{nullptr}, cascade_buf{nullptr}, material_buf{nullptr},
      light_buf{nullptr}, post_buf{nullptr};
  std::uint32_t material_count{0}, light_count{0};
  MeshBuffers opaque, foliage;
  struct Resource {
    MeshBuffers mesh;
    std::uint32_t first{}, count{};
    Vec3 centre{};
    float radius{};
    bool has_opaque{false}, has_glass{false};
    struct Range {
      std::uint32_t first{}, count{};
    };
    Range visible[memory::visibility_views]{};
  };
  std::vector<Resource> resources;
  std::vector<GpuInstance> all_instances;
  std::uint32_t instance_capacity{};
  WGPUBuffer instance_buf{nullptr}, reflection_frame_buf{nullptr},
      glass_frame_buf{nullptr}, puddle_frame_buf{nullptr},
      pond_frame_buf{nullptr};
  WGPUBindGroup reflection_frame_bg{nullptr}, reflection_tex_bg{nullptr},
      glass_frame_bg{nullptr}, puddle_frame_bg{nullptr}, pond_frame_bg{nullptr};
  WGPUBuffer reflected_glass_frame_buf[3]{};
  WGPUBindGroup reflected_glass_frame_bg[3]{};
  std::vector<PointLight> lights;
  VoxelTransport transport;
  VoxelTransport fine_transport[4];
  Texture point_geometry, fine_point_geometry;
  int point_geometry_fine_index{-1};
  Texture fine_radiance[4][2];
  const Environment *fine_environment[4][2]{};
  int fine_index{-1}, fine_count{4};
  bool isolated_assembly{false};
  bool cached_point_shadows{true};
  Texture radiance[2];
  const Environment *radiance_environment[2]{nullptr, nullptr};
  Texture reflection, puddle_reflection, pond_reflection, reflection_depth,
      scene_color;
  bool has_planar_pond{false};
  std::vector<WGPUTextureView> reflection_mip_views[3];
  std::vector<WGPUBindGroup> reflection_mip_groups[3];
  WGPUBuffer light_tiles[lighting::view_count]{};
  std::uint64_t tile_buffer_bytes[lighting::view_count]{};
  const Environment *light_report_environment{nullptr};
  std::vector<DrawRange> draws; // from the scene
  std::vector<std::pair<std::uint32_t, std::uint32_t>> reflected_glass_draws;
  std::vector<DrawRange> fine; // per-building sub-ranges (sorted by first)
  std::vector<std::pair<std::uint32_t, std::uint32_t>> sel_main,
      sel_shadow; // per-frame (first, count)
  // samplers
  WGPUSampler mat_samp{nullptr}, cube_samp{nullptr}, shadow_samp{nullptr},
      clamp_samp{nullptr};
  // static textures
  Texture shadow_maps;
  std::vector<WGPUTextureView> shadow_layer_views;
  Texture leaf;
  const MaterialArrays *arrays{nullptr};
  const Environment *env{nullptr};
  bool night{false};
  // size-dependent
  std::uint32_t w{0}, h{0};
  Texture depth_pre, normal_pre, ao_a, ao_b, hdr_msaa, depth_msaa, hdr, ldr_a,
      ldr_b;
  Texture taa_hist[2];
  int taa_parity{0};
  bool taa_valid{false};
  Mat4 prev_view_proj{Mat4::identity()};
  Mat4 last_view_proj{Mat4::identity()};
  std::uint32_t frame_index{0};
  Renderer::Stats stats;
  Mat4 cascade_mats[kCascades];
  float cascade_ext[kCascades]{0, 0, 0}, cascade_dep[kCascades]{0, 0, 0};
  bool cascade_valid[kCascades]{false, false, false};
  std::vector<std::pair<std::uint32_t, std::uint32_t>> sel_cascade[kCascades];
  // occlusion culling
  Texture hiz;                            // R32Float pyramid
  std::vector<WGPUTextureView> hiz_views; // per mip
  WGPUBindGroupLayout hiz_bgl{nullptr}, cull_bgl{nullptr};
  WGPUPipelineLayout hiz_layout{nullptr}, cull_layout{nullptr};
  WGPURenderPipeline p_hiz_copy{nullptr}, p_hiz_down{nullptr};
  WGPUComputePipeline p_cull{nullptr};
  std::vector<WGPUBindGroup> hiz_bgs; // per mip pass (index 0 = copy)
  WGPUBuffer cull_uniform{nullptr}, cull_ranges{nullptr}, cull_args{nullptr};
  std::uint32_t cull_capacity{0};
  WGPUBindGroup cull_bg{nullptr};
  std::vector<DrawRange> cand; // unmerged candidates for the main pass
  // CPU-built indirect args per pass (prepass, cascades): one multi-draw call
  // instead of thousands
  WGPUBuffer pass_args[1 + kCascades]{nullptr, nullptr, nullptr, nullptr};
  std::uint32_t pass_args_capacity[1 + kCascades]{0, 0, 0, 0};
  WGPUBuffer cull_readback{nullptr};
  std::uint32_t cull_readback_count{0};
  bool cull_readback_pending{false};
  std::uint32_t occluded_last{0};
  WGPUBufferMapCallbackInfo pending_map{};
  std::uint64_t pending_map_bytes{0};
  bool pending_map_armed{false};
  std::function<void()> pending_map_discard;
  std::shared_ptr<int> callback_lifetime = std::make_shared<int>(0);
  WGPUBindGroupLayout taa_bgl{nullptr};
  WGPUPipelineLayout taa_layout{nullptr};
  WGPURenderPipeline p_taa{nullptr};
  WGPUBuffer taa_buf{nullptr};
  WGPUBindGroup taa_bg[2]{nullptr, nullptr};
  WGPUBindGroup down0_taa_bg[2]{nullptr, nullptr},
      tonemap_taa_bg[2]{nullptr, nullptr};
  std::vector<Texture> bloom;    // downsample chain, level i at w>>(i+1)
  std::vector<Texture> bloom_up; // upsample chain, levels 0..L-2
  // bind groups
  WGPUBindGroup frame_bg{nullptr}, scene_tex_bg{nullptr}, cascade_bg{nullptr},
      leaf_bg{nullptr}, ssao_bg{nullptr}, blur_bg{nullptr}, sky_bg{nullptr},
      tonemap_bg{nullptr}, fxaa_bg{nullptr}, blit_bg{nullptr},
      dbg_ao_bg{nullptr}, dbg_normal_bg{nullptr}, dbg_hdr_bg{nullptr};
  std::vector<WGPUBindGroup> down_bg, up_bg;
  std::uint32_t post_slots{0};
  bool scene_dirty{true};
  bool have_scene{false};

  void bind_light_view(Gpu &gpu, std::uint32_t view) {
    const WGPUBuffer opaque_frames[]{frame_buf, reflection_frame_buf,
                                     puddle_frame_buf, pond_frame_buf};
    const WGPUBuffer optical_frames[]{
        glass_frame_buf, reflected_glass_frame_buf[0],
        reflected_glass_frame_buf[1], reflected_glass_frame_buf[2]};
    WGPUBindGroup *opaque_groups[]{&frame_bg, &reflection_frame_bg,
                                   &puddle_frame_bg, &pond_frame_bg};
    WGPUBindGroup *optical_groups[]{
        &glass_frame_bg, &reflected_glass_frame_bg[0],
        &reflected_glass_frame_bg[1], &reflected_glass_frame_bg[2]};
    auto bind = [&](WGPUBuffer frame, WGPUBindGroup *group) {
      if (*group)
        wgpuBindGroupRelease(*group);
      *group = nullptr;
      *group =
          make_bg(gpu.device, frame_bgl,
                  {bg_buffer(0, frame, sizeof(FrameUniform)),
                   bg_buffer(1, material_buf, WGPU_WHOLE_SIZE),
                   bg_buffer(2, light_buf, WGPU_WHOLE_SIZE),
                   bg_buffer(3, instance_buf, WGPU_WHOLE_SIZE),
                   bg_buffer(4, light_tiles[view], tile_buffer_bytes[view])});
    };
    bind(opaque_frames[view], opaque_groups[view]);
    bind(optical_frames[view], optical_groups[view]);
  }

  void reset_light_lists(Gpu &gpu) {
    const auto count = memory::bytes((std::uint64_t(w) + 15) / 16,
                                     (std::uint64_t(h) + 15) / 16);
    const auto bytes = memory::bytes(count, 8);
    try {
      (void)memory::buffer_size(
          bytes,
          std::min(gpu.max_buffer_size, gpu.max_storage_buffer_binding_size));
    } catch (const std::length_error &error) {
      gpu.fail(std::string("practical light headers: ") + error.what());
    }
    std::vector<std::uint32_t> empty(static_cast<std::size_t>(count * 2));
    for (std::uint32_t view = 0; view < lighting::view_count; ++view) {
      auto replacement =
          gpu.create_buffer(WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst,
                            bytes, empty.data(), "light-lists");
      if (light_tiles[view])
        wgpuBufferRelease(light_tiles[view]);
      light_tiles[view] = replacement;
      tile_buffer_bytes[view] = bytes;
      bind_light_view(gpu, view);
    }
  }

  void upload_light_list(Gpu &gpu, std::uint32_t view,
                         const lighting::List &list) {
    const auto bytes = memory::bytes(list.words.size(), sizeof(std::uint32_t));
    if (bytes > tile_buffer_bytes[view]) {
      auto replacement =
          gpu.create_buffer(WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst,
                            bytes, nullptr, "light-lists");
      if (light_tiles[view])
        wgpuBufferRelease(light_tiles[view]);
      light_tiles[view] = replacement;
      tile_buffer_bytes[view] = bytes;
      bind_light_view(gpu, view);
    }
    gpu.upload_buffer(light_tiles[view], 0, list.words.data(), bytes);
  }

  void release_targets() {
    for (int plane = 0; plane < 3; ++plane) {
      for (auto group : reflection_mip_groups[plane])
        wgpuBindGroupRelease(group);
      for (auto view : reflection_mip_views[plane])
        wgpuTextureViewRelease(view);
      reflection_mip_groups[plane].clear();
      reflection_mip_views[plane].clear();
    }
    for (Texture *t : {&depth_pre, &normal_pre, &ao_a, &ao_b, &hdr_msaa,
                       &depth_msaa, &hdr, &ldr_a, &ldr_b, &taa_hist[0],
                       &taa_hist[1], &hiz, &reflection, &reflection_depth,
                       &scene_color, &puddle_reflection, &pond_reflection})
      t->release();
    for (WGPUTextureView v : hiz_views)
      wgpuTextureViewRelease(v);
    hiz_views.clear();
    for (WGPUBindGroup g : hiz_bgs)
      wgpuBindGroupRelease(g);
    hiz_bgs.clear();
    if (cull_bg != nullptr) {
      wgpuBindGroupRelease(cull_bg);
      cull_bg = nullptr;
    }
    for (WGPUBindGroup *g :
         {&taa_bg[0], &taa_bg[1], &down0_taa_bg[0], &down0_taa_bg[1],
          &tonemap_taa_bg[0], &tonemap_taa_bg[1]}) {
      if (*g != nullptr)
        wgpuBindGroupRelease(*g);
      *g = nullptr;
    }
    taa_valid = false;
    for (Texture &t : bloom)
      t.release();
    for (Texture &t : bloom_up)
      t.release();
    bloom.clear();
    bloom_up.clear();
    for (WGPUBindGroup *g :
         {&ssao_bg, &blur_bg, &tonemap_bg, &fxaa_bg, &blit_bg, &scene_tex_bg,
          &dbg_ao_bg, &dbg_normal_bg, &dbg_hdr_bg, &reflection_tex_bg}) {
      if (*g != nullptr)
        wgpuBindGroupRelease(*g);
      *g = nullptr;
    }
    for (WGPUBindGroup g : down_bg)
      wgpuBindGroupRelease(g);
    for (WGPUBindGroup g : up_bg)
      wgpuBindGroupRelease(g);
    down_bg.clear();
    up_bg.clear();
  }
};

bool Renderer::init(Gpu *gpu, const std::string &shader_dir,
                    RenderSettings settings, std::string *error,
                    std::uint32_t render_width, std::uint32_t render_height) {
  if (!gpu->healthy())
    throw GpuUnavailable(gpu->failure_message());
  WGPULimits visibility_limits{};
  if (wgpuDeviceGetLimits(gpu->device, &visibility_limits) != WGPUStatus_Success ||
      visibility_limits.maxSampledTexturesPerShaderStage < 16 ||
      visibility_limits.maxTextureDimension3D < 256) {
    *error = "point surface visibility requires 16 sampled textures per stage and 256-cell 3D textures";
    return false;
  }
  std::printf("  point visibility limits: sampled textures=%u (required16), "
              "storage textures=%u (additional0), max3D=%u (required256)\n",
              visibility_limits.maxSampledTexturesPerShaderStage,
              visibility_limits.maxStorageTexturesPerShaderStage,
              visibility_limits.maxTextureDimension3D);
  gpu_ = gpu;
  settings_ = settings;
  impl_ = new Impl();
  Impl &I = *impl_;
  I.shader_dir = shader_dir;
  WGPUDevice dev = gpu->device;

  // ---- layouts
  // ---------------------------------------------------------------
  I.frame_bgl = make_bgl(
      dev,
      {buffer_entry(0, WGPUShaderStage_Vertex | WGPUShaderStage_Fragment,
                    WGPUBufferBindingType_Uniform, false, sizeof(FrameUniform)),
       buffer_entry(1, WGPUShaderStage_Fragment,
                    WGPUBufferBindingType_ReadOnlyStorage, false, 0),
       buffer_entry(2, WGPUShaderStage_Fragment,
                    WGPUBufferBindingType_ReadOnlyStorage, false, 0),
       buffer_entry(3, WGPUShaderStage_Vertex,
                    WGPUBufferBindingType_ReadOnlyStorage, false, 0),
       buffer_entry(4, WGPUShaderStage_Fragment,
                    WGPUBufferBindingType_ReadOnlyStorage, false, 0)});
  I.scene_tex_bgl =
      make_bgl(dev, {texture_entry(0, WGPUTextureSampleType_Float,
                                   WGPUTextureViewDimension_2DArray),
                     texture_entry(1, WGPUTextureSampleType_Float,
                                   WGPUTextureViewDimension_2DArray),
                     texture_entry(2, WGPUTextureSampleType_Float,
                                   WGPUTextureViewDimension_2DArray),
                     sampler_entry(3, WGPUSamplerBindingType_Filtering),
                     texture_entry(4, WGPUTextureSampleType_Float,
                                   WGPUTextureViewDimension_Cube),
                     texture_entry(5, WGPUTextureSampleType_Float,
                                   WGPUTextureViewDimension_Cube),
                     sampler_entry(6, WGPUSamplerBindingType_Filtering),
                     texture_entry(7, WGPUTextureSampleType_Depth,
                                   WGPUTextureViewDimension_2DArray),
                     sampler_entry(8, WGPUSamplerBindingType_Comparison),
                     texture_entry(9, WGPUTextureSampleType_Float,
                                   WGPUTextureViewDimension_2D),
                     sampler_entry(10, WGPUSamplerBindingType_Filtering),
                     texture_entry(11, WGPUTextureSampleType_Float,
                                   WGPUTextureViewDimension_2D),
                     texture_entry(12, WGPUTextureSampleType_Float,
                                   WGPUTextureViewDimension_3D),
                     texture_entry(13, WGPUTextureSampleType_Float,
                                   WGPUTextureViewDimension_2D),
                     texture_entry(14, WGPUTextureSampleType_Float,
                                   WGPUTextureViewDimension_2D),
                     texture_entry(15, WGPUTextureSampleType_Float,
                                   WGPUTextureViewDimension_3D),
                     texture_entry(16, WGPUTextureSampleType_Float,
                                   WGPUTextureViewDimension_2D),
                     texture_entry(17, WGPUTextureSampleType_Float,
                                   WGPUTextureViewDimension_2D),
                     texture_entry(18, WGPUTextureSampleType_Uint,
                                   WGPUTextureViewDimension_3D),
                     texture_entry(19, WGPUTextureSampleType_Uint,
                                   WGPUTextureViewDimension_3D)});
  I.cascade_bgl = make_bgl(
      dev, {buffer_entry(0, WGPUShaderStage_Vertex,
                         WGPUBufferBindingType_Uniform, true, 64),
            buffer_entry(1, WGPUShaderStage_Vertex,
                         WGPUBufferBindingType_ReadOnlyStorage, false, 0),
            buffer_entry(2, WGPUShaderStage_Fragment,
                         WGPUBufferBindingType_ReadOnlyStorage, false, 0)});
  I.leaf_bgl =
      make_bgl(dev, {texture_entry(0, WGPUTextureSampleType_Float,
                                   WGPUTextureViewDimension_2D),
                     sampler_entry(1, WGPUSamplerBindingType_Filtering)});
  I.ssao_bgl =
      make_bgl(dev, {texture_entry(0, WGPUTextureSampleType_Depth,
                                   WGPUTextureViewDimension_2D),
                     texture_entry(1, WGPUTextureSampleType_Float,
                                   WGPUTextureViewDimension_2D),
                     sampler_entry(2, WGPUSamplerBindingType_Filtering),
                     texture_entry(3, WGPUTextureSampleType_Float,
                                   WGPUTextureViewDimension_2D)});
  I.sky_bgl =
      make_bgl(dev, {texture_entry(0, WGPUTextureSampleType_Float,
                                   WGPUTextureViewDimension_Cube),
                     sampler_entry(1, WGPUSamplerBindingType_Filtering)});
  I.post_bgl =
      make_bgl(dev, {buffer_entry(0, WGPUShaderStage_Fragment,
                                  WGPUBufferBindingType_Uniform, true,
                                  sizeof(PostParams)),
                     texture_entry(1, WGPUTextureSampleType_Float,
                                   WGPUTextureViewDimension_2D),
                     texture_entry(2, WGPUTextureSampleType_Float,
                                   WGPUTextureViewDimension_2D),
                     sampler_entry(3, WGPUSamplerBindingType_Filtering),
                     texture_entry(4, WGPUTextureSampleType_Depth,
                                   WGPUTextureViewDimension_2D),
                     texture_entry(5, WGPUTextureSampleType_Float,
                                   WGPUTextureViewDimension_2D)});
  I.taa_bgl =
      make_bgl(dev, {buffer_entry(0, WGPUShaderStage_Fragment,
                                  WGPUBufferBindingType_Uniform, false, 160),
                     texture_entry(1, WGPUTextureSampleType_Float,
                                   WGPUTextureViewDimension_2D),
                     texture_entry(2, WGPUTextureSampleType_Float,
                                   WGPUTextureViewDimension_2D),
                     texture_entry(3, WGPUTextureSampleType_Depth,
                                   WGPUTextureViewDimension_2D),
                     sampler_entry(4, WGPUSamplerBindingType_Filtering)});
  I.taa_layout = make_layout(dev, {I.taa_bgl});
  I.hiz_bgl = make_bgl(dev, {texture_entry(0, WGPUTextureSampleType_Depth,
                                           WGPUTextureViewDimension_2D),
                             [&] {
                               WGPUBindGroupLayoutEntry e = texture_entry(
                                   1, WGPUTextureSampleType_UnfilterableFloat,
                                   WGPUTextureViewDimension_2D);
                               return e;
                             }()});
  I.hiz_layout = make_layout(dev, {I.hiz_bgl});
  {
    WGPUBindGroupLayoutEntry e0 = buffer_entry(
        0, WGPUShaderStage_Compute, WGPUBufferBindingType_Uniform, false, 160);
    WGPUBindGroupLayoutEntry e1 =
        buffer_entry(1, WGPUShaderStage_Compute,
                     WGPUBufferBindingType_ReadOnlyStorage, false, 0);
    WGPUBindGroupLayoutEntry e2 = buffer_entry(
        2, WGPUShaderStage_Compute, WGPUBufferBindingType_Storage, false, 0);
    WGPUBindGroupLayoutEntry e3 =
        texture_entry(3, WGPUTextureSampleType_UnfilterableFloat,
                      WGPUTextureViewDimension_2D);
    e3.visibility = WGPUShaderStage_Compute;
    I.cull_bgl = make_bgl(dev, {e0, e1, e2, e3});
  }
  I.cull_layout = make_layout(dev, {I.cull_bgl});
  I.main_layout = make_layout(dev, {I.frame_bgl, I.scene_tex_bgl});
  I.shadow_layout = make_layout(dev, {I.cascade_bgl, I.leaf_bgl});
  I.prepass_layout = make_layout(dev, {I.frame_bgl, I.leaf_bgl});
  I.ssao_layout = make_layout(dev, {I.frame_bgl, I.ssao_bgl});
  I.sky_layout = make_layout(dev, {I.frame_bgl, I.sky_bgl});
  I.post_layout = make_layout(dev, {I.post_bgl});

  // ---- shaders + pipelines
  // -------------------------------------------------------
  auto shader = [&](const char *name) -> WGPUShaderModule {
    WGPUShaderModule m = gpu->load_shader(shader_dir + "/" + name, error);
    return m;
  };
  WGPUShaderModule sm_shadow = shader("shadow.wgsl"),
                   sm_pre = shader("prepass.wgsl"),
                   sm_ssao = shader("ssao.wgsl"), sm_sky = shader("sky.wgsl"),
                   sm_main = shader("main.wgsl"), sm_post = shader("post.wgsl"),
                   sm_taa = shader("taa.wgsl"), sm_hiz = shader("hiz.wgsl"),
                   sm_cull = shader("cull.wgsl");
  if (!error->empty())
    return false;
  for (WGPUShaderModule m : {sm_shadow, sm_pre, sm_ssao, sm_sky, sm_main,
                             sm_post, sm_taa, sm_hiz, sm_cull}) {
    if (m == nullptr) {
      *error = "shader module creation failed (see wgpu errors above)";
      return false;
    }
  }
  const WGPUTextureFormat hdr_fmt = WGPUTextureFormat_RGBA16Float;
  const WGPUTextureFormat ldr_fmt = WGPUTextureFormat_RGBA8Unorm;
  {
    PipelineOpts o;
    o.module = sm_shadow;
    o.fs = "fs_main";
    o.layout = I.shadow_layout;
    o.depth = WGPUTextureFormat_Depth32Float;
    o.cull = WGPUCullMode_None;
    I.p_shadow = make_pipeline(dev, o);
    o.fs = "fs_foliage";
    I.p_shadow_foliage = make_pipeline(dev, o);
  }
  {
    PipelineOpts o;
    o.module = sm_pre;
    o.layout = I.prepass_layout;
    o.fs = "fs_main";
    o.color = hdr_fmt;
    o.depth = WGPUTextureFormat_Depth32Float;
    o.depth_compare = WGPUCompareFunction_Greater; // reversed Z
    I.p_prepass = make_pipeline(dev, o);
    o.cull = WGPUCullMode_None;
    I.p_prepass_glass = make_pipeline(dev, o);
    o.fs = "fs_foliage";
    o.cull = WGPUCullMode_None;
    I.p_prepass_foliage = make_pipeline(dev, o);
  }
  {
    PipelineOpts o;
    o.module = sm_ssao;
    o.layout = I.ssao_layout;
    o.vs = "vs_fullscreen";
    o.fs = "fs_ssao";
    o.color = WGPUTextureFormat_R8Unorm;
    o.vertices = false;
    o.cull = WGPUCullMode_None;
    I.p_ssao = make_pipeline(dev, o);
    o.fs = "fs_blur";
    I.p_blur = make_pipeline(dev, o);
  }
  for (int variant = 0; variant < 2; ++variant) {
    const std::uint32_t samples = variant == 0 ? 4 : 1;
    PipelineOpts o;
    o.module = sm_sky;
    o.layout = I.sky_layout;
    o.vs = "vs_fullscreen";
    o.fs = "fs_sky";
    o.color = hdr_fmt;
    o.depth = WGPUTextureFormat_Depth32Float;
    o.depth_write = false;
    o.depth_compare = WGPUCompareFunction_Always;
    o.samples = samples;
    o.vertices = false;
    o.cull = WGPUCullMode_None;
    (variant == 0 ? I.p_sky : I.p_sky1) = make_pipeline(dev, o);
    PipelineOpts m;
    m.module = sm_main;
    m.layout = I.main_layout;
    m.fs = "fs_main";
    m.color = hdr_fmt;
    m.depth = WGPUTextureFormat_Depth32Float;
    m.samples = samples;
    m.depth_compare = WGPUCompareFunction_Greater; // reversed Z
    (variant == 0 ? I.p_main : I.p_main1) = make_pipeline(dev, m);
    m.cull = WGPUCullMode_None;
    m.alpha_to_coverage = samples > 1;
    (variant == 0 ? I.p_foliage : I.p_foliage1) = make_pipeline(dev, m);
    // Refraction samples one opaque scene copy. Keep the nearest glass layer
    // in depth so inner/rear faces cannot overwrite the outer pane in index
    // order; this also gives temporal history the visible glass depth.
    m.depth_write = true;
    m.alpha_to_coverage = false;
    (variant == 0 ? I.p_glass : I.p_glass1) = make_pipeline(dev, m);
  }
  settings_.msaa = settings_.msaa > 1 ? 4 : 1;
  {
    PipelineOpts o;
    o.module = sm_post;
    o.layout = I.post_layout;
    o.vs = "vs_fullscreen";
    o.vertices = false;
    o.cull = WGPUCullMode_None;
    o.fs = "fs_down";
    o.color = hdr_fmt;
    I.p_down = make_pipeline(dev, o);
    o.fs = "fs_reflection_mip";
    I.p_reflection_mip = make_pipeline(dev, o);
    o.fs = "fs_up";
    I.p_up = make_pipeline(dev, o);
    o.fs = "fs_tonemap";
    o.color = ldr_fmt;
    I.p_tonemap = make_pipeline(dev, o);
    o.fs = "fs_fxaa";
    I.p_fxaa = make_pipeline(dev, o);
    o.fs = "fs_blit";
    I.p_copy = make_pipeline(dev, o); // LDR → LDR copy
    o.color = gpu->surface_format;
    I.p_blit = make_pipeline(dev, o);
  }
  {
    PipelineOpts o;
    o.module = sm_taa;
    o.layout = I.taa_layout;
    o.vs = "vs_fullscreen";
    o.fs = "fs_taa";
    o.color = hdr_fmt;
    o.vertices = false;
    o.cull = WGPUCullMode_None;
    I.p_taa = make_pipeline(dev, o);
  }
  {
    PipelineOpts o;
    o.module = sm_hiz;
    o.layout = I.hiz_layout;
    o.vs = "vs_fullscreen";
    o.fs = "fs_copy";
    o.color = WGPUTextureFormat_R32Float;
    o.vertices = false;
    o.cull = WGPUCullMode_None;
    I.p_hiz_copy = make_pipeline(dev, o);
    o.fs = "fs_down";
    I.p_hiz_down = make_pipeline(dev, o);
    WGPUComputePipelineDescriptor cd{};
    cd.layout = I.cull_layout;
    cd.compute.module = sm_cull;
    cd.compute.entryPoint = sv("cs_cull");
    I.p_cull = wgpuDeviceCreateComputePipeline(dev, &cd);
  }
  I.cull_uniform =
      gpu->create_buffer(WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst, 160,
                         nullptr, "cull-uniform");
  for (WGPUShaderModule m : {sm_shadow, sm_pre, sm_ssao, sm_sky, sm_main,
                             sm_post, sm_taa, sm_hiz, sm_cull})
    wgpuShaderModuleRelease(m);
  I.taa_buf = gpu->create_buffer(
      WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst, 160, nullptr, "taa");

  // ---- buffers, samplers, static textures
  // ----------------------------------------
  I.frame_buf =
      gpu->create_buffer(WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst,
                         sizeof(FrameUniform), nullptr, "frame");
  I.cascade_buf =
      gpu->create_buffer(WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst,
                         kUniformStride * kCascades, nullptr, "cascades");
  I.post_slots = 4 + 2 * kBloomLevels + 4;
  I.post_buf =
      gpu->create_buffer(WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst,
                         kUniformStride * I.post_slots, nullptr, "post");
  I.mat_samp =
      gpu->create_sampler(WGPUAddressMode_Repeat, WGPUFilterMode_Linear, true,
                          8, WGPUCompareFunction_Undefined, "material");
  I.cube_samp =
      gpu->create_sampler(WGPUAddressMode_ClampToEdge, WGPUFilterMode_Linear,
                          true, 1, WGPUCompareFunction_Undefined, "cube");
  I.shadow_samp =
      gpu->create_sampler(WGPUAddressMode_ClampToEdge, WGPUFilterMode_Linear,
                          false, 1, WGPUCompareFunction_LessEqual, "shadow");
  I.clamp_samp =
      gpu->create_sampler(WGPUAddressMode_ClampToEdge, WGPUFilterMode_Linear,
                          false, 1, WGPUCompareFunction_Undefined, "clamp");
  I.shadow_maps = gpu->create_texture(
      settings_.shadow_size, settings_.shadow_size,
      WGPUTextureFormat_Depth32Float,
      WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding, 1,
      kCascades, 1, "shadow-maps");
  for (std::uint32_t i = 0; i < kCascades; ++i) {
    I.shadow_layer_views.push_back(gpu->create_view(
        I.shadow_maps, 0, 1, i, 1, WGPUTextureViewDimension_2D));
  }
  {
    const std::uint32_t ls = 512;
    std::vector<std::uint8_t> leaf = make_leaf_texture(ls, 7);
    std::uint32_t mips = 1;
    while ((ls >> mips) >= 1)
      ++mips;
    I.leaf = gpu->create_texture(ls, ls, WGPUTextureFormat_RGBA8UnormSrgb,
                                 WGPUTextureUsage_TextureBinding |
                                     WGPUTextureUsage_CopyDst,
                                 mips, 1, 1, "leaf");
    gpu->upload_rgba8_mips(I.leaf, 0, leaf.data());
    I.leaf_bg =
        make_bg(dev, I.leaf_bgl,
                {bg_texture(0, I.leaf.view), bg_sampler(1, I.mat_samp)});
  }
  // Cascade instance binding is created together with the scene.
  I.reflection_frame_buf =
      gpu->create_buffer(WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst,
                         sizeof(FrameUniform), nullptr, "reflection-frame");
  I.glass_frame_buf =
      gpu->create_buffer(WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst,
                         sizeof(FrameUniform), nullptr, "glass-frame");
  I.puddle_frame_buf = gpu->create_buffer(
      WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst, sizeof(FrameUniform),
      nullptr, "puddle-reflection-frame");
  I.pond_frame_buf = gpu->create_buffer(
      WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst, sizeof(FrameUniform),
      nullptr, "pond-reflection-frame");
  for (auto &buffer : I.reflected_glass_frame_buf)
    buffer = gpu->create_buffer(
        WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst, sizeof(FrameUniform),
        nullptr, "reflected-glass-frame");
  resize(render_width ? render_width : gpu->width,
         render_height ? render_height : gpu->height);
  return true;
}

void Renderer::shutdown() {
  if (impl_ == nullptr)
    return;
  Impl &I = *impl_;
  I.callback_lifetime.reset();
  if (I.pending_map_armed && I.pending_map_discard)
    I.pending_map_discard();
  I.pending_map_discard = {};
  I.pending_map_armed = false;

  I.release_targets();
  I.opaque.release();
  I.foliage.release();
  I.point_geometry.release();
  I.fine_point_geometry.release();
  for (auto &r : I.resources)
    r.mesh.release();
  for (auto &r : I.radiance)
    r.release();
  for (auto &fields : I.fine_radiance)
    for (auto &r : fields)
      r.release();
  for (WGPUTextureView v : I.shadow_layer_views)
    wgpuTextureViewRelease(v);
  I.shadow_maps.release();
  I.leaf.release();
  for (WGPUBindGroup *g :
       {&I.frame_bg, &I.cascade_bg, &I.leaf_bg, &I.sky_bg,
        &I.reflection_frame_bg, &I.glass_frame_bg, &I.puddle_frame_bg,
        &I.reflected_glass_frame_bg[0], &I.reflected_glass_frame_bg[1],
        &I.reflected_glass_frame_bg[2], &I.pond_frame_bg}) {
    if (*g != nullptr)
      wgpuBindGroupRelease(*g);
  }
  if (I.p_taa != nullptr)
    wgpuRenderPipelineRelease(I.p_taa);
  if (I.p_hiz_copy != nullptr)
    wgpuRenderPipelineRelease(I.p_hiz_copy);
  if (I.p_hiz_down != nullptr)
    wgpuRenderPipelineRelease(I.p_hiz_down);
  if (I.p_cull != nullptr)
    wgpuComputePipelineRelease(I.p_cull);
  for (WGPUPipelineLayout *l : {&I.hiz_layout, &I.cull_layout})
    if (*l != nullptr)
      wgpuPipelineLayoutRelease(*l);
  for (WGPUBindGroupLayout *l : {&I.hiz_bgl, &I.cull_bgl})
    if (*l != nullptr)
      wgpuBindGroupLayoutRelease(*l);
  for (WGPUBuffer *b :
       {&I.cull_uniform, &I.cull_ranges, &I.cull_args, &I.cull_readback,
        &I.pass_args[0], &I.pass_args[1], &I.pass_args[2], &I.pass_args[3]})
    if (*b != nullptr)
      wgpuBufferRelease(*b);
  if (I.taa_layout != nullptr)
    wgpuPipelineLayoutRelease(I.taa_layout);
  if (I.taa_bgl != nullptr)
    wgpuBindGroupLayoutRelease(I.taa_bgl);
  for (WGPUBuffer *b :
       {&I.frame_buf, &I.cascade_buf, &I.material_buf, &I.light_buf,
        &I.post_buf, &I.taa_buf, &I.instance_buf, &I.light_tiles[0],
        &I.light_tiles[1], &I.light_tiles[2], &I.light_tiles[3],
        &I.reflection_frame_buf, &I.glass_frame_buf, &I.puddle_frame_buf,
        &I.reflected_glass_frame_buf[0], &I.reflected_glass_frame_buf[1],
        &I.reflected_glass_frame_buf[2], &I.pond_frame_buf}) {
    if (*b != nullptr)
      wgpuBufferRelease(*b);
  }
  for (WGPUSampler *s :
       {&I.mat_samp, &I.cube_samp, &I.shadow_samp, &I.clamp_samp}) {
    if (*s != nullptr)
      wgpuSamplerRelease(*s);
  }
  for (WGPURenderPipeline *p : {&I.p_glass,          &I.p_glass1,
                                &I.p_shadow,         &I.p_shadow_foliage,
                                &I.p_prepass,        &I.p_prepass_foliage,
                                &I.p_prepass_glass,  &I.p_ssao,
                                &I.p_blur,           &I.p_sky,
                                &I.p_main,           &I.p_foliage,
                                &I.p_sky1,           &I.p_main1,
                                &I.p_foliage1,       &I.p_down,
                                &I.p_reflection_mip, &I.p_up,
                                &I.p_tonemap,        &I.p_fxaa,
                                &I.p_blit,           &I.p_copy}) {
    if (*p != nullptr)
      wgpuRenderPipelineRelease(*p);
  }
  for (WGPUPipelineLayout *l :
       {&I.main_layout, &I.shadow_layout, &I.prepass_layout, &I.ssao_layout,
        &I.sky_layout, &I.post_layout}) {
    if (*l != nullptr)
      wgpuPipelineLayoutRelease(*l);
  }
  for (WGPUBindGroupLayout *l :
       {&I.frame_bgl, &I.scene_tex_bgl, &I.cascade_bgl, &I.leaf_bgl,
        &I.ssao_bgl, &I.sky_bgl, &I.post_bgl}) {
    if (*l != nullptr)
      wgpuBindGroupLayoutRelease(*l);
  }
  delete impl_;
  impl_ = nullptr;
}

void Renderer::set_scene(const Scene &scene, const MaterialArrays &arrays) {
  Impl &I = *impl_;
  I.arrays = &arrays;
  I.opaque.release();
  I.foliage.release();
  auto upload = [&](const Mesh &m, MeshBuffers *out, const char *label) {
    if (m.indices.empty())
      return;
    const auto vertex_bytes =
        memory::bytes(m.vertices.size(), sizeof(PackedVertex));
    const auto index_bytes =
        memory::bytes(m.indices.size(), sizeof(std::uint32_t));
    try {
      if (m.vertices.empty())
        throw std::length_error("indexed mesh has no vertices");
      if (m.indices.size() > std::numeric_limits<std::uint32_t>::max())
        throw std::length_error("mesh index count exceeds uint32");
      (void)memory::buffer_size(vertex_bytes, gpu_->max_buffer_size);
      (void)memory::buffer_size(index_bytes, gpu_->max_buffer_size);
    } catch (const std::length_error &error) {
      gpu_->fail(std::string("mesh '") + label + "': " + error.what() +
                 " (vertex bytes " + std::to_string(vertex_bytes) +
                 ", index bytes " + std::to_string(index_bytes) + ")");
    }
    out->vertices =
        gpu_->create_buffer(WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst,
                            vertex_bytes, nullptr, label);
    std::vector<PackedVertex> packed(static_cast<std::size_t>(
        memory::chunk_records(m.vertices.size(), sizeof(PackedVertex))));
    for (std::size_t first = 0; first < m.vertices.size();) {
      const auto count = std::min(packed.size(), m.vertices.size() - first);
      for (std::size_t i = 0; i < count; ++i)
        packed[i] = pack_vertex(m.vertices[first + i]);
      gpu_->upload_buffer(out->vertices, first * sizeof(PackedVertex),
                          packed.data(), count * sizeof(PackedVertex));
      first += count;
    }
    out->indices = gpu_->create_buffer(WGPUBufferUsage_Index, index_bytes,
                                       m.indices.data(), label);
    out->index_count = static_cast<std::uint32_t>(m.indices.size());
    std::printf(
        "  mesh upload: %s, vertices %llu bytes, indices %llu bytes "
        "(staging <= %llu MiB)\n",
        label, static_cast<unsigned long long>(vertex_bytes),
        static_cast<unsigned long long>(index_bytes),
        static_cast<unsigned long long>(memory::upload_bytes / 1048576));
  };
  upload(scene.opaque, &I.opaque, "opaque");
  upload(scene.foliage, &I.foliage, "foliage");
  for (auto &r : I.resources)
    r.mesh.release();
  I.resources.clear();
  // Validate all transform/range counts before converting them to GPU uint32.
  const auto capacity =
      memory::instance_capacity(scene.asset_instances.size() + 1ull);
  std::vector<GpuInstance> instances;
  instances.reserve(scene.asset_instances.size() + 1);
  instances.push_back({Mat4::identity(),
                       {1, 0, 0, 0},
                       {0, 1, 0, 0},
                       {0, 0, 1, 0},
                       {1, 1, 1, 1}});
  for (std::uint32_t ri = 0; ri < scene.asset_library.resources.size(); ++ri) {
    auto &resource = I.resources.emplace_back();
    resource.first = static_cast<std::uint32_t>(instances.size());
    for (const auto &inst : scene.asset_instances)
      if (inst.resource == ri) {
        const float c = std::cos(inst.yaw), s = std::sin(inst.yaw);
        GpuInstance g{};
        g.model = Mat4::identity();
        g.model.at(0, 0) = c * inst.scale.x;
        g.model.at(2, 0) = -s * inst.scale.x;
        g.model.at(1, 1) = inst.scale.y;
        g.model.at(0, 2) = s * inst.scale.z;
        g.model.at(2, 2) = c * inst.scale.z;
        g.model.at(0, 3) = inst.translation.x;
        g.model.at(1, 3) = inst.translation.y;
        g.model.at(2, 3) = inst.translation.z;
        g.normal0 = {c / inst.scale.x, 0, -s / inst.scale.x, 0};
        g.normal1 = {0, 1 / inst.scale.y, 0, 0};
        g.normal2 = {s / inst.scale.z, 0, c / inst.scale.z, 0};
        g.tint = Vec4{inst.tint, 1};
        instances.push_back(g);
      }
    resource.count =
        static_cast<std::uint32_t>(instances.size()) - resource.first;
    if (resource.count) {
      const auto &mesh = scene.asset_library.resources[ri].mesh;
      upload(mesh, &resource.mesh,
             scene.asset_library.resources[ri].name.c_str());
      Vec3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
      for (const auto &v : mesh.vertices) {
        lo = vmin(lo, v.position);
        hi = vmax(hi, v.position);
        const bool glass = (scene.materials.at(v.material).flags & 128u) != 0;
        resource.has_glass |= glass;
        resource.has_opaque |= !glass;
      }
      resource.centre = (lo + hi) * .5f;
      resource.radius = length(hi - lo) * .5f;
    }
  }
  if (I.instance_buf)
    wgpuBufferRelease(I.instance_buf);
  I.instance_buf = nullptr;
  I.all_instances = std::move(instances);
  I.instance_capacity = capacity;
  I.instance_buf =
      gpu_->create_buffer(WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst,
                          I.instance_capacity * sizeof(GpuInstance), nullptr,
                          "visible-mesh-instances");
  if (I.cascade_bg)
    wgpuBindGroupRelease(I.cascade_bg);
  I.cascade_bg = nullptr;
  for (auto &r : I.radiance)
    r.release();
  I.radiance_environment[0] = I.radiance_environment[1] = nullptr;
  const bool bounded_assembly =
      scene.city_radius > 0 && scene.city_radius <= 50;
  I.isolated_assembly = bounded_assembly;
  I.fine_count = bounded_assembly ? 1 : 4;
  Vec3 assembly_centre{};
  float assembly_cell = .125f;
  if (bounded_assembly) {
    Vec3 low{1e30f, 1e30f, 1e30f}, high{-1e30f, -1e30f, -1e30f};
    auto include = [&](Vec3 p) {
      low = vmin(low, p);
      high = vmax(high, p);
    };
    for (const auto &vertex : scene.opaque.vertices)
      include(vertex.position);
    for (const auto &vertex : scene.foliage.vertices)
      include(vertex.position);
    for (const auto &instance : scene.asset_instances)
      if (instance.resource < scene.asset_library.resources.size())
        for (const auto &vertex :
             scene.asset_library.resources[instance.resource].mesh.vertices)
          include(asset_transform_point(instance, vertex.position));
    if (low.x <= high.x) {
      assembly_centre = (low + high) * .5f;
      const Vec3 span = high - low;
      assembly_cell =
          std::max(.125f, std::max({span.x, span.y, span.z}) / 224.f);
    }
  }
  I.transport.nx = 192;
  I.transport.ny = bounded_assembly ? 128 : 80;
  I.transport.nz = 192;
  I.transport.cell = bounded_assembly ? .5f : 8.f;
  I.transport.origin = bounded_assembly ? assembly_centre - Vec3{48, 32, 48}
                                        : Vec3{-768, -32, -768};
  I.transport.build(scene);
  I.point_geometry.release();
  I.fine_point_geometry.release();
  I.point_geometry_fine_index = -1;
  I.point_geometry = I.transport.upload_visibility(*gpu_);
  const char *local_shots[4] = {"civic", "street", "garden", "landing"};
  for (int f = 0; f < 4; ++f) {
    auto &field = I.fine_transport[f];
    field.nx = bounded_assembly ? 256 : 192;
    field.ny = field.nx;
    field.nz = field.nx;
    field.cell = bounded_assembly ? assembly_cell : .5f;
    Vec3 position{}, target{};
    shot_camera(local_shots[f], position, target);
    // Camera contracts identify the relevant fixed neighbourhood. The field
    // remains world-aligned and does not move as the inspection camera moves.
    field.origin = bounded_assembly
                       ? assembly_centre - Vec3{128, 128, 128} * assembly_cell
                       : position - Vec3{48, 32, 48};
    if (f < I.fine_count)
      field.build(scene);
    else
      field.cells.clear();
    for (int n = 0; n < 2; ++n) {
      I.fine_radiance[f][n].release();
      I.fine_environment[f][n] = nullptr;
    }
  }
  I.fine_index = -1;
  std::printf("  resources: %zu unique meshes, %zu GPU instances (%.1f KiB "
              "transforms)\n",
              I.resources.size(), I.all_instances.size() - 1,
              double(I.all_instances.size() * sizeof(GpuInstance)) / 1024);
  I.draws = scene.draws;
  I.has_planar_pond = std::any_of(
      scene.materials.begin(), scene.materials.end(),
      [](const auto &material) { return (material.flags & 1024u) != 0; });
  I.fine = scene.fine;
  if (I.draws.empty() && !scene.opaque.indices.empty()) {
    DrawRange all;
    all.count = static_cast<std::uint32_t>(scene.opaque.indices.size());
    I.draws.push_back(all);
  }
  // Keep complete contributing triangles in the secondary glass pass. This
  // filters material identity, not geometry detail or projected screen size.
  I.reflected_glass_draws.clear();
  for (const auto &draw : I.draws) {
    if (draw.lod_group >= 0 && draw.lod_level != 0)
      continue;
    for (std::uint32_t at = draw.first; at + 2 < draw.first + draw.count;
         at += 3) {
      bool glass = false;
      for (int corner = 0; corner < 3; ++corner) {
        const auto id =
            scene.opaque.vertices[scene.opaque.indices[at + corner]].material;
        glass |= (scene.materials[id].flags & 128u) != 0;
      }
      if (!glass)
        continue;
      if (!I.reflected_glass_draws.empty() &&
          I.reflected_glass_draws.back().first +
                  I.reflected_glass_draws.back().second ==
              at)
        I.reflected_glass_draws.back().second += 3;
      else
        I.reflected_glass_draws.push_back({at, 3});
    }
  }
  triangles_ = static_cast<std::uint32_t>(
      (scene.opaque.indices.size() + scene.foliage.indices.size()) / 3);
  for (const auto &r : I.resources)
    triangles_ += r.mesh.index_count / 3;
  // materials
  std::vector<GpuMaterial> mats;
  for (const MaterialDesc &d : scene.materials) {
    GpuMaterial g;
    g.base_color = Vec4{d.base_color, 0.5f};
    g.params = Vec4{d.roughness, d.metallic, d.emissive, d.normal_strength};
    const int layer = d.albedo_set.empty() ? -1 : arrays.layer_of(d.albedo_set);
    g.tex = Vec4{static_cast<float>(layer), static_cast<float>(layer),
                 static_cast<float>(layer), d.uv_scale};
    g.misc = Vec4{static_cast<float>(d.flags), d.tint2.x, d.tint2.y, d.tint2.z};
    g.room = Vec4{d.room_w, d.room_h, d.room_d, d.lit_probability};
    mats.push_back(g);
  }
  if (mats.empty())
    mats.push_back(GpuMaterial{});
  if (I.material_buf != nullptr)
    wgpuBufferRelease(I.material_buf);
  I.material_buf = nullptr;
  I.material_buf = gpu_->create_buffer(WGPUBufferUsage_Storage,
                                       mats.size() * sizeof(GpuMaterial),
                                       mats.data(), "materials");
  I.material_count = static_cast<std::uint32_t>(mats.size());
  I.cascade_bg = make_bg(
      gpu_->device, I.cascade_bgl,
      {bg_buffer(0, I.cascade_buf, 64),
       bg_buffer(1, I.instance_buf, I.instance_capacity * sizeof(GpuInstance)),
       bg_buffer(2, I.material_buf, mats.size() * sizeof(GpuMaterial))});
  I.lights = scene.lights;
  std::vector<GpuLight> lights;
  for (const PointLight &l : scene.lights) {
    lights.push_back(
        GpuLight{Vec4{l.position, l.radius}, Vec4{l.color, l.intensity}});
  }
  if (lights.empty())
    lights.push_back(GpuLight{});
  if (I.light_buf != nullptr)
    wgpuBufferRelease(I.light_buf);
  I.light_buf = nullptr;
  I.light_buf = gpu_->create_buffer(WGPUBufferUsage_Storage,
                                    lights.size() * sizeof(GpuLight),
                                    lights.data(), "lights");
  I.light_count = static_cast<std::uint32_t>(scene.lights.size());
  I.reset_light_lists(*gpu_);
  I.light_report_environment = nullptr;
  I.have_scene = true;
  I.scene_dirty = true;
}

void Renderer::set_environment(const Environment *env, bool night) {
  Impl &I = *impl_;
  I.env = env;
  I.night = night;
  I.scene_dirty = true;
  I.taa_valid = false;
}

void Renderer::apply_settings() {
  Impl &I = *impl_;
  settings_.msaa = settings_.msaa > 1 ? 4 : 1;
  if (I.w > 0 && I.h > 0)
    resize(I.w, I.h);
}

const Renderer::Stats &Renderer::stats() const { return impl_->stats; }

void Renderer::resize(std::uint32_t w, std::uint32_t h) {
  Impl &I = *impl_;
  if (w == 0 || h == 0)
    return;
  I.release_targets();
  I.w = w;
  I.h = h;
  const WGPUTextureUsage rt =
      WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding;
  I.depth_pre =
      gpu_->create_texture(w, h, WGPUTextureFormat_Depth32Float,
                           rt | WGPUTextureUsage_CopySrc, 1, 1, 1, "depth-pre");
  I.normal_pre = gpu_->create_texture(w, h, WGPUTextureFormat_RGBA16Float, rt,
                                      1, 1, 1, "normal-pre");
  const std::uint32_t aw = settings_.ssao_half ? std::max(1u, w / 2) : w,
                      ah = settings_.ssao_half ? std::max(1u, h / 2) : h;
  I.ao_a = gpu_->create_texture(aw, ah, WGPUTextureFormat_R8Unorm, rt, 1, 1, 1,
                                "ao-a");
  I.ao_b = gpu_->create_texture(w, h, WGPUTextureFormat_R8Unorm, rt, 1, 1, 1,
                                "ao-b");
  if (settings_.msaa > 1) {
    I.hdr_msaa = gpu_->create_texture(w, h, WGPUTextureFormat_RGBA16Float,
                                      WGPUTextureUsage_RenderAttachment, 1, 1,
                                      settings_.msaa, "hdr-msaa");
    I.depth_msaa = gpu_->create_texture(w, h, WGPUTextureFormat_Depth32Float,
                                        WGPUTextureUsage_RenderAttachment, 1, 1,
                                        settings_.msaa, "depth-msaa");
  } else {
    I.depth_msaa = gpu_->create_texture(w, h, WGPUTextureFormat_Depth32Float,
                                        WGPUTextureUsage_RenderAttachment, 1, 1,
                                        1, "depth-1x");
  }
  I.hdr = gpu_->create_texture(w, h, WGPUTextureFormat_RGBA16Float,
                               rt | WGPUTextureUsage_CopySrc, 1, 1, 1, "hdr");
  I.scene_color = gpu_->create_texture(w, h, WGPUTextureFormat_RGBA16Float,
                                       WGPUTextureUsage_TextureBinding |
                                           WGPUTextureUsage_CopyDst,
                                       1, 1, 1, "transmitted-scene-color");
  std::uint32_t reflection_levels = 1;
  for (auto extent = std::max(w, h); extent > 1; extent >>= 1)
    ++reflection_levels;
  I.reflection = gpu_->create_texture(
      w, h, WGPUTextureFormat_RGBA16Float, rt | WGPUTextureUsage_CopySrc,
      reflection_levels, 1, 1, "planar-scene-reflection");
  I.puddle_reflection = gpu_->create_texture(
      w, h, WGPUTextureFormat_RGBA16Float, rt | WGPUTextureUsage_CopySrc,
      reflection_levels, 1, 1, "puddle-scene-reflection");
  I.pond_reflection = gpu_->create_texture(
      w, h, WGPUTextureFormat_RGBA16Float, rt | WGPUTextureUsage_CopySrc,
      reflection_levels, 1, 1, "pond-scene-reflection");
  for (int plane = 0; plane < 3; ++plane)
    for (std::uint32_t mip = 0; mip < reflection_levels; ++mip)
      I.reflection_mip_views[plane].push_back(
          gpu_->create_view(plane == 0   ? I.reflection
                            : plane == 1 ? I.puddle_reflection
                                         : I.pond_reflection,
                            mip, 1, 0, 1, WGPUTextureViewDimension_2D));
  I.reflection_depth = gpu_->create_texture(
      w, h, WGPUTextureFormat_Depth32Float, WGPUTextureUsage_RenderAttachment,
      1, 1, 1, "planar-reflection-depth");
  if (I.have_scene)
    I.reset_light_lists(*gpu_);
  {
    std::uint32_t mips = 1;
    while ((std::max(w, h) >> mips) >= 1)
      ++mips;
    I.hiz = gpu_->create_texture(w, h, WGPUTextureFormat_R32Float, rt, mips, 1,
                                 1, "hiz");
    for (std::uint32_t m = 0; m < mips; ++m)
      I.hiz_views.push_back(
          gpu_->create_view(I.hiz, m, 1, 0, 1, WGPUTextureViewDimension_2D));
    // pass 0: depth -> mip 0; pass m: mip m-1 -> mip m
    // the copy pass writes mip 0, so its (unused) level input must not alias it
    I.hiz_bgs.push_back(
        make_bg(gpu_->device, I.hiz_bgl,
                {bg_texture(0, I.depth_pre.view),
                 bg_texture(1, I.hiz_views[mips > 1 ? 1 : 0])}));
    for (std::uint32_t m = 1; m < mips; ++m) {
      I.hiz_bgs.push_back(make_bg(gpu_->device, I.hiz_bgl,
                                  {bg_texture(0, I.depth_pre.view),
                                   bg_texture(1, I.hiz_views[m - 1])}));
    }
  }
  I.taa_hist[0] = gpu_->create_texture(w, h, WGPUTextureFormat_RGBA16Float, rt,
                                       1, 1, 1, "taa-0");
  I.taa_hist[1] = gpu_->create_texture(w, h, WGPUTextureFormat_RGBA16Float, rt,
                                       1, 1, 1, "taa-1");
  I.ldr_a =
      gpu_->create_texture(w, h, WGPUTextureFormat_RGBA8Unorm,
                           rt | WGPUTextureUsage_CopySrc, 1, 1, 1, "ldr-a");
  I.ldr_b =
      gpu_->create_texture(w, h, WGPUTextureFormat_RGBA8Unorm,
                           rt | WGPUTextureUsage_CopySrc, 1, 1, 1, "ldr-b");
  std::uint32_t bw = w, bh = h;
  for (std::uint32_t i = 0; i < kBloomLevels; ++i) {
    bw = std::max(1u, bw / 2);
    bh = std::max(1u, bh / 2);
    I.bloom.push_back(gpu_->create_texture(
        bw, bh, WGPUTextureFormat_RGBA16Float, rt, 1, 1, 1, "bloom"));
    if (i + 1 < kBloomLevels)
      I.bloom_up.push_back(gpu_->create_texture(
          bw, bh, WGPUTextureFormat_RGBA16Float, rt, 1, 1, 1, "bloom-up"));
  }
  WGPUDevice dev = gpu_->device;
  I.ssao_bg = make_bg(
      dev, I.ssao_bgl,
      {bg_texture(0, I.depth_pre.view), bg_texture(1, I.normal_pre.view),
       bg_sampler(2, I.clamp_samp), bg_texture(3, I.ao_b.view)});
  I.blur_bg = make_bg(
      dev, I.ssao_bgl,
      {bg_texture(0, I.depth_pre.view), bg_texture(1, I.normal_pre.view),
       bg_sampler(2, I.clamp_samp), bg_texture(3, I.ao_a.view)});
  // post bind groups: slot layout in post_buf: 0 tonemap, 1 fxaa, 2 blit, 3
  // spare, 4.. down[i], 4+L.. up[i]
  auto post_bg = [&](WGPUTextureView a, WGPUTextureView b) {
    return make_bg(
        dev, I.post_bgl,
        {bg_buffer(0, I.post_buf, sizeof(PostParams)), bg_texture(1, a),
         bg_texture(2, b), bg_sampler(3, I.clamp_samp),
         bg_texture(4, I.depth_pre.view), bg_texture(5, I.hdr.view)});
  };
  for (int plane = 0; plane < 3; ++plane)
    for (std::size_t mip = 1; mip < I.reflection_mip_views[plane].size(); ++mip)
      I.reflection_mip_groups[plane].push_back(
          post_bg(I.reflection_mip_views[plane][mip - 1], I.leaf.view));
  for (std::uint32_t i = 0; i < kBloomLevels; ++i) {
    WGPUTextureView src = i == 0 ? I.hdr.view : I.bloom[i - 1].view;
    I.down_bg.push_back(post_bg(src, src));
  }
  for (std::uint32_t i = 0; i < kBloomLevels - 1; ++i) {
    // up[i]: from the level below (down[L-1] first, then up[hi+1]) into up[hi],
    // adding down[hi]
    const std::uint32_t hi = kBloomLevels - 2 - i;
    WGPUTextureView lower =
        i == 0 ? I.bloom[kBloomLevels - 1].view : I.bloom_up[hi + 1].view;
    I.up_bg.push_back(post_bg(lower, I.bloom[hi].view));
  }
  I.tonemap_bg = post_bg(I.hdr.view, I.bloom_up[0].view);
  for (int p = 0; p < 2; ++p) {
    // parity p: TAA writes taa_hist[p] from history taa_hist[1-p]; post reads
    // taa_hist[p]
    I.taa_bg[p] =
        make_bg(dev, I.taa_bgl,
                {bg_buffer(0, I.taa_buf, 160), bg_texture(1, I.hdr.view),
                 bg_texture(2, I.taa_hist[1 - p].view),
                 bg_texture(3, I.depth_pre.view), bg_sampler(4, I.clamp_samp)});
    I.down0_taa_bg[p] = post_bg(I.taa_hist[p].view, I.taa_hist[p].view);
    I.tonemap_taa_bg[p] = post_bg(I.taa_hist[p].view, I.bloom_up[0].view);
  }
  I.fxaa_bg = post_bg(I.ldr_a.view, I.ldr_a.view);
  I.blit_bg = post_bg(I.ldr_b.view, I.ldr_b.view);
  I.dbg_ao_bg = post_bg(I.ao_b.view, I.ao_b.view);
  I.dbg_normal_bg = post_bg(I.normal_pre.view, I.normal_pre.view);
  I.dbg_hdr_bg = post_bg(I.hdr.view, I.hdr.view);
  I.scene_dirty = true;
}

namespace {

// Fit an orthographic light frustum around a camera frustum slice.
void fit_cascade(const Camera &cam, float aspect, float zn, float zf,
                 Vec3 sun_dir, std::uint32_t shadow_size, Mat4 *out_vp,
                 float *out_extent, float *out_depth) {
  const Vec3 f = cam.forward(), r = cam.right(), u = cross(r, f);
  const float th = std::tan(cam.fov_y * 0.5f);
  Vec3 corners[8];
  int k = 0;
  for (float z : {zn, zf}) {
    const float hh = th * z, hw = hh * aspect;
    for (int i = 0; i < 4; ++i) {
      const float sx = (i & 1) ? 1.0f : -1.0f, sy = (i & 2) ? 1.0f : -1.0f;
      corners[k++] = cam.position + f * z + r * (sx * hw) + u * (sy * hh);
    }
  }
  Vec3 centre{0, 0, 0};
  for (const Vec3 &c : corners)
    centre += c;
  centre *= 1.0f / 8.0f;
  float radius = 0.0f;
  for (const Vec3 &c : corners)
    radius = std::max(radius, length(c - centre));
  radius = std::ceil(radius * 4.0f) / 4.0f;
  const Vec3 L = normalize(sun_dir);
  const Vec3 up = std::fabs(L.y) > 0.95f ? Vec3{0, 0, 1} : Vec3{0, 1, 0};
  const float depth_pad = 600.0f;
  Mat4 view = look_at(centre + L * (radius + depth_pad * 0.5f), centre, up);
  // texel snapping
  const float texel = 2.0f * radius / static_cast<float>(shadow_size);
  Vec4 c_ls = mul(view, Vec4{centre, 1.0f});
  const float sx = std::floor(c_ls.x / texel) * texel - c_ls.x;
  const float sy = std::floor(c_ls.y / texel) * texel - c_ls.y;
  Mat4 snap = translate(Vec3{sx, sy, 0.0f});
  view = mul(snap, view);
  const float depth_range = 2.0f * radius + depth_pad;
  Mat4 proj = ortho(-radius, radius, -radius, radius, 0.0f, depth_range);
  *out_vp = mul(proj, view);
  *out_extent = radius;
  *out_depth = depth_range;
}

} // namespace

void Renderer::render(const Camera &camera, float time_s,
                      WGPUTextureView target) {
  if (!gpu_->healthy())
    return;
  try {

    Impl &I = *impl_;
    if (!I.have_scene || I.env == nullptr)
      return;
    WGPUDevice dev = gpu_->device;
    const float night_factor = I.env->night_factor < 0
                                   ? (I.night ? 1.f : 0.f)
                                   : clampf(I.env->night_factor, 0.f, 1.f);
    const int env_index = I.night ? 1 : 0;
    if (I.cached_point_shadows != settings_.point_shadows) {
      for (auto &environment : I.radiance_environment)
        environment = nullptr;
      for (auto &fields : I.fine_environment)
        for (auto &environment : fields)
          environment = nullptr;
      I.cached_point_shadows = settings_.point_shadows;
    }
    if (I.radiance_environment[env_index] != I.env) {
      I.radiance[env_index].release();
      I.radiance[env_index] =
          I.transport.illuminate(*gpu_, *I.env, night_factor, I.lights, nullptr,
                                settings_.point_shadows);
      I.radiance_environment[env_index] = I.env;
      I.scene_dirty = true;
    }
    int fine_index = 0;
    float fine_distance = 1e30f;
    for (int f = 0; f < I.fine_count; ++f) {
      const auto &field = I.fine_transport[f];
      const Vec3 centre = field.origin + Vec3{field.nx * field.cell * .5f,
                                              field.ny * field.cell * .5f,
                                              field.nz * field.cell * .5f};
      const float distance = length(camera.position - centre);
      if (distance < fine_distance) {
        fine_index = f;
        fine_distance = distance;
      }
    }
    if (fine_index != I.fine_index) {
      for (int f = 0; f < 4; ++f)
        if (f != fine_index)
          for (int n = 0; n < 2; ++n) {
            I.fine_radiance[f][n].release();
            I.fine_environment[f][n] = nullptr;
          }
    }
    if (I.point_geometry_fine_index != fine_index) {
      I.fine_point_geometry.release();
      I.fine_point_geometry = I.fine_transport[fine_index].upload_visibility(*gpu_);
      I.point_geometry_fine_index = fine_index;
      I.scene_dirty = true;
    }
    if (I.fine_environment[fine_index][env_index] != I.env) {
      I.fine_radiance[fine_index][env_index].release();
      I.fine_radiance[fine_index][env_index] =
          I.fine_transport[fine_index].illuminate(*gpu_, *I.env, night_factor,
                                                  I.lights, &I.transport,
                                                  settings_.point_shadows);
      I.fine_environment[fine_index][env_index] = I.env;
      I.scene_dirty = true;
    }
    if (fine_index != I.fine_index) {
      I.fine_index = fine_index;
      I.scene_dirty = true;
    }
    if (I.scene_tex_bg == nullptr || I.scene_dirty) {
      if (I.scene_tex_bg != nullptr)
        wgpuBindGroupRelease(I.scene_tex_bg);
      I.scene_tex_bg = make_bg(
          dev, I.scene_tex_bgl,
          {bg_texture(0, I.arrays->albedo.view),
           bg_texture(1, I.arrays->normal.view),
           bg_texture(2, I.arrays->arm.view), bg_sampler(3, I.mat_samp),
           bg_texture(4, I.env->specular.view),
           bg_texture(5, I.env->background.view), bg_sampler(6, I.cube_samp),
           bg_texture(7, I.shadow_maps.view), bg_sampler(8, I.shadow_samp),
           bg_texture(9, I.ao_b.view), bg_sampler(10, I.clamp_samp),
           bg_texture(11, I.leaf.view),
           bg_texture(12, I.radiance[env_index].view),
           bg_texture(13, I.reflection.view),
           bg_texture(14, I.scene_color.view),
           bg_texture(15, I.fine_radiance[fine_index][env_index].view),
           bg_texture(16, I.puddle_reflection.view),
           bg_texture(17, I.pond_reflection.view),
           bg_texture(18, I.point_geometry.view),
           bg_texture(19, I.fine_point_geometry.view)});
      if (I.reflection_tex_bg)
        wgpuBindGroupRelease(I.reflection_tex_bg);
      I.reflection_tex_bg = make_bg(
          dev, I.scene_tex_bgl,
          {bg_texture(0, I.arrays->albedo.view),
           bg_texture(1, I.arrays->normal.view),
           bg_texture(2, I.arrays->arm.view), bg_sampler(3, I.mat_samp),
           bg_texture(4, I.env->specular.view),
           bg_texture(5, I.env->background.view), bg_sampler(6, I.cube_samp),
           bg_texture(7, I.shadow_maps.view), bg_sampler(8, I.shadow_samp),
           bg_texture(9, I.ao_b.view), bg_sampler(10, I.clamp_samp),
           bg_texture(11, I.leaf.view),
           bg_texture(12, I.radiance[env_index].view),
           bg_texture(13, I.leaf.view), bg_texture(14, I.scene_color.view),
           bg_texture(15, I.fine_radiance[fine_index][env_index].view),
           bg_texture(16, I.leaf.view), bg_texture(17, I.leaf.view),
           bg_texture(18, I.point_geometry.view),
           bg_texture(19, I.fine_point_geometry.view)});
      if (I.sky_bg != nullptr)
        wgpuBindGroupRelease(I.sky_bg);
      I.sky_bg = make_bg(
          dev, I.sky_bgl,
          {bg_texture(0, I.env->background.view), bg_sampler(1, I.cube_samp)});
      I.scene_dirty = false;
    }

    // ---- frame uniforms
    // ---------------------------------------------------------
    const float aspect = static_cast<float>(I.w) / static_cast<float>(I.h);
    // The static 300 km sea must reach the optical horizon from elevated
    // viewpoints. Reversed floating-point depth retains near-surface precision.
    const float zn = 0.3f, zf = 220000.0f;
    FrameUniform fu{};
    fu.view = camera.view();
    fu.proj = perspective_reversed(camera.fov_y, aspect, zn, zf);
    const Mat4 view_proj_clean = mul(fu.proj, fu.view);
    float jx = settings_.jitter_x, jy = settings_.jitter_y;
    const bool taa_on = settings_.taa && settings_.debug_view != 12;
    if (taa_on) {
      // Halton(2,3) sequence, 8 samples, centred
      auto halton = [](std::uint32_t i, std::uint32_t b) {
        float f = 1.0f, r = 0.0f;
        while (i > 0) {
          f /= static_cast<float>(b);
          r += f * static_cast<float>(i % b);
          i /= b;
        }
        return r;
      };
      const std::uint32_t k = (I.frame_index % 8) + 1;
      jx += halton(k, 2) - 0.5f;
      jy += halton(k, 3) - 0.5f;
    }
    fu.proj.at(0, 2) += 2.0f * jx / static_cast<float>(I.w);
    fu.proj.at(1, 2) += 2.0f * jy / static_cast<float>(I.h);
    fu.view_proj = mul(fu.proj, fu.view);
    fu.inv_view_proj = inverse(fu.view_proj);
    fu.inv_proj = inverse(fu.proj);
    const float splits[kCascades + 1] = {zn, 48.0f, 220.0f, 1400.0f};
    Vec3 extents{0, 0, 0}, depths{0, 0, 0};
    const Vec3 sun_dir =
        I.env->has_sun ? I.env->sun_dir : Vec3{0.3f, 0.8f, 0.5f};
    bool update_cascade[kCascades];
    for (std::uint32_t i = 0; i < kCascades; ++i) {
      // half rate: cascades 1 and 2 refit on alternating frames, the nearest
      // every frame
      update_cascade[i] = !settings_.shadow_half_rate || i == 0 ||
                          !I.cascade_valid[i] || ((I.frame_index + i) % 2 == 0);
      if (update_cascade[i]) {
        fit_cascade(camera, aspect, splits[i], splits[i + 1], sun_dir,
                    settings_.shadow_size, &I.cascade_mats[i],
                    &I.cascade_ext[i], &I.cascade_dep[i]);
        I.cascade_valid[i] = true;
        gpu_->write_buffer(I.cascade_buf, i * kUniformStride,
                           &I.cascade_mats[i], sizeof(Mat4));
      }
      fu.shadow[i] = I.cascade_mats[i];
      const float ext = I.cascade_ext[i], dep = I.cascade_dep[i];
      if (i == 0) {
        extents.x = ext;
        depths.x = dep;
      } else if (i == 1) {
        extents.y = ext;
        depths.y = dep;
      } else {
        extents.z = ext;
        depths.z = dep;
      }
    }
    fu.camera_pos = Vec4{camera.position, time_s};
    fu.sun_dir = Vec4{sun_dir, (I.env->has_sun && settings_.shadows)
                                   ? 1.0f
                                   : (I.env->has_sun ? 1.0f : 0.0f)};
    const float exposure = I.env->exposure * std::exp2(settings_.exposure_bias);
    fu.sun_color = Vec4{I.env->sun_color, exposure};
    fu.moon_dir = Vec4{I.env->moon_dir, I.env->moon_visible ? 1.f : 0.f};
    fu.moon_color = Vec4{I.env->moon_color, I.env->sun_visible ? 1.f : 0.f};
    fu.moon_shape = {I.env->moon_phase.x, I.env->moon_phase.y,
                     I.env->moon_angular_radius,
                     I.env->display_referred_background ? 1.f : 0.f};
    for (int i = 0; i < 9; ++i)
      fu.sh[i] = Vec4{I.env->sh[i][0] * I.env->lighting_scale,
                      I.env->sh[i][1] * I.env->lighting_scale,
                      I.env->sh[i][2] * I.env->lighting_scale, 0.0f};
    fu.cascade = Vec4{splits[1], splits[2], splits[3],
                      1.0f / static_cast<float>(settings_.shadow_size)};
    fu.cascade_extent = Vec4{extents, jx};
    fu.cascade_depth = Vec4{depths, jy};
    fu.screen =
        Vec4{static_cast<float>(I.w), static_cast<float>(I.h),
             1.0f / static_cast<float>(I.w), 1.0f / static_cast<float>(I.h)};
    const float emissive_scale =
        0.9f /
        I.env->exposure; // display EV also exposes emissive and indirect light
    fu.params = Vec4{emissive_scale, 1.2f, night_factor,
                     static_cast<float>(I.light_count)};
    fu.params2 =
        Vec4{1.0f, 1.0f, settings_.ssao ? 1.0f : 0.0f,
             settings_.shadows ? static_cast<float>(settings_.debug_view)
                               : -static_cast<float>(settings_.debug_view + 1)};
    Mat4 mirror = Mat4::identity();
    mirror.at(1, 1) = -1;
    mirror.at(1, 3) = 2 * settings_.water_height;
    FrameUniform reflected = fu;
    reflected.view = mul(fu.view, mirror);
    reflected.view_proj = mul(fu.proj, reflected.view);
    reflected.inv_view_proj = inverse(reflected.view_proj);
    reflected.camera_pos.y = 2 * settings_.water_height - camera.position.y;
    fu.reflection_vp = reflected.view_proj;
    Mat4 puddle_mirror = mirror;
    puddle_mirror.at(1, 3) = 2 * settings_.puddle_height;
    FrameUniform puddle = fu;
    puddle.view = mul(fu.view, puddle_mirror);
    puddle.view_proj = mul(fu.proj, puddle.view);
    puddle.inv_view_proj = inverse(puddle.view_proj);
    puddle.camera_pos.y = 2 * settings_.puddle_height - camera.position.y;
    fu.puddle_reflection_vp = puddle.view_proj;
    Mat4 pond_mirror = mirror;
    pond_mirror.at(1, 3) = 2 * settings_.pond_height;
    FrameUniform pond = fu;
    pond.view = mul(fu.view, pond_mirror);
    pond.view_proj = mul(fu.proj, pond.view);
    pond.inv_view_proj = inverse(pond.view_proj);
    pond.camera_pos.y = 2 * settings_.pond_height - camera.position.y;
    fu.pond_reflection_vp = pond.view_proj;
    fu.transport = {settings_.indirect ? 1.f : 0.f,
                    settings_.reflections ? 1.f : 0.f, settings_.water_height,
                    0};
    fu.point_visibility = {settings_.point_shadows ? 1.f : 0.f, 0, 0, 0};
    fu.voxel_min = Vec4{I.transport.origin, I.transport.cell};
    fu.voxel_extent = {
        I.transport.nx * I.transport.cell, I.transport.ny * I.transport.cell,
        I.transport.nz * I.transport.cell, I.env->lighting_scale};
    const auto &fine_field = I.fine_transport[fine_index];
    fu.fine_min = Vec4{fine_field.origin, fine_field.cell};
    fu.fine_extent = {
        fine_field.nx * fine_field.cell, fine_field.ny * fine_field.cell,
        fine_field.nz * fine_field.cell,
        (I.isolated_assembly || I.env->background_contains_atmosphere) ? 1.f
                                                                       : 0.f};
    FrameUniform *plane_frames[]{&reflected, &puddle, &pond};
    const WGPUBuffer plane_buffers[]{I.reflection_frame_buf, I.puddle_frame_buf,
                                     I.pond_frame_buf};
    const float plane_heights[]{settings_.water_height, settings_.puddle_height,
                                settings_.pond_height};
    for (int plane = 0; plane < 3; ++plane) {
      auto &view = *plane_frames[plane];
      view.reflection_vp = fu.reflection_vp;
      view.puddle_reflection_vp = fu.puddle_reflection_vp;
      view.pond_reflection_vp = fu.pond_reflection_vp;
      view.transport = fu.transport;
      view.point_visibility = fu.point_visibility;
      view.transport.w = 1;
      view.transport.z = plane_heights[plane];
      view.voxel_min = fu.voxel_min;
      view.voxel_extent = fu.voxel_extent;
      view.fine_min = fu.fine_min;
      view.fine_extent = fu.fine_extent;
      view.params2.z = 0;
      gpu_->write_buffer(plane_buffers[plane], 0, &view, sizeof(view));
      FrameUniform reflected_glass = view;
      reflected_glass.transport.w = 3;
      gpu_->write_buffer(I.reflected_glass_frame_buf[plane], 0,
                         &reflected_glass, sizeof(reflected_glass));
    }
    FrameUniform glass = fu;
    glass.transport.w = 2;
    gpu_->write_buffer(I.glass_frame_buf, 0, &glass, sizeof(glass));
    gpu_->write_buffer(I.frame_buf, 0, &fu, sizeof(fu));
    // Each optical view keeps all projected source spheres. The actual
    // fragment position, not camera distance, decides finite-radius reach.
    const FrameUniform *light_frames[]{&fu, &reflected, &puddle, &pond};
    const bool report_lights = I.light_report_environment != I.env;
    const auto light_limit =
        std::min(gpu_->max_buffer_size, gpu_->max_storage_buffer_binding_size);
    for (std::uint32_t view = 0; view < lighting::view_count; ++view) {
      if (view && (!settings_.reflections || (view == 3 && !I.has_planar_pond)))
        continue;
      lighting::List list;
      try {
        list = lighting::build(I.lights, light_frames[view]->view,
                               light_frames[view]->proj, I.w, I.h, zn,
                               light_limit);
      } catch (const std::length_error &error) {
        gpu_->fail(std::string("practical light lists: ") + error.what());
      }
      I.upload_light_list(*gpu_, view, list);
      if (report_lights)
        std::printf("[light-lists] view=%u sources=%zu max_per_tile=%u "
                    "tiles_over48=%u bytes=%zu no_truncation=1\n",
                    view, I.lights.size(), list.max_count, list.tiles_over_48,
                    list.words.size() * sizeof(std::uint32_t));
    }
    I.light_report_environment = I.env;
    // post params
    {
      PostParams pp{};
      pp.a = Vec4{exposure, settings_.bloom ? 0.28f : 0.0f, 0.0f,
                  0.0f}; // tonemap: exposure, bloom strength
      pp.b.w = I.env->display_referred_background && settings_.debug_view == 0
                   ? 1.f
                   : 0.f;
      gpu_->write_buffer(I.post_buf, 0, &pp, sizeof(pp));
      pp.a = Vec4{1.0f / static_cast<float>(I.w),
                  1.0f / static_cast<float>(I.h), 0.0f, 0.0f}; // fxaa texel
      gpu_->write_buffer(I.post_buf, kUniformStride, &pp, sizeof(pp));
      pp.a = Vec4{gpu_->surface_srgb ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f}; // blit
      gpu_->write_buffer(I.post_buf, 2 * kUniformStride, &pp, sizeof(pp));
      std::uint32_t bw = I.w, bh = I.h;
      for (std::uint32_t i = 0; i < kBloomLevels; ++i) {
        pp.a = Vec4{1.0f / static_cast<float>(bw),
                    1.0f / static_cast<float>(bh), i == 0 ? 1.0f : 0.0f, 0.0f};
        pp.b = Vec4{
            1.0f, 0.5f, 12.0f,
            0.0f}; // threshold (post-exposure units handled below), knee, clamp
        // threshold is in HDR units: express relative to exposure
        pp.b.w = I.env->display_referred_background && settings_.debug_view == 0
                     ? 1.f
                     : 0.f;
        pp.b.x = 1.0f / exposure;
        pp.b.z = 40.0f / exposure;
        gpu_->write_buffer(I.post_buf, (4 + i) * kUniformStride, &pp,
                           sizeof(pp));
        bw = std::max(1u, bw / 2);
        bh = std::max(1u, bh / 2);
      }
      for (std::uint32_t i = 0; i < kBloomLevels - 1; ++i) {
        const Texture &lo = I.bloom[kBloomLevels - 1 - i];
        pp.a = Vec4{1.0f / static_cast<float>(lo.width),
                    1.0f / static_cast<float>(lo.height), 0.0f, 0.65f};
        gpu_->write_buffer(I.post_buf, (4 + kBloomLevels + i) * kUniformStride,
                           &pp, sizeof(pp));
      }
    }

    const bool occlusion_wanted = settings_.occlusion && I.p_cull != nullptr;
    // ---- draw selection: LOD by distance, frustum culling
    // ---------------------------
    {
      const Frustum frustum = Frustum::from(fu.view_proj, true); // reversed Z
      I.sel_main.clear();
      I.sel_shadow.clear();
      I.cand.clear();
      // per LOD group, the finest level whose max distance exceeds the camera
      // distance
      std::vector<int> chosen(static_cast<std::size_t>(std::max(
                                  1, 1 +
                                         [&] {
                                           int mx = -1;
                                           for (const DrawRange &d : I.draws)
                                             mx = std::max(mx, d.lod_group);
                                           return mx;
                                         }())),
                              99);
      for (const DrawRange &d : I.draws) {
        if (d.lod_group < 0)
          continue;
        const float dist = length(d.centre - camera.position);
        if (dist < d.lod_max_distance)
          chosen[static_cast<std::size_t>(d.lod_group)] = std::min(
              chosen[static_cast<std::size_t>(d.lod_group)], d.lod_level);
      }
      // the finest level a group actually has may be coarser than the pick
      for (const DrawRange &d : I.draws) {
        if (d.lod_group < 0)
          continue;
        int &c = chosen[static_cast<std::size_t>(d.lod_group)];
        if (c < d.lod_level) {
          bool has = false;
          for (const DrawRange &e : I.draws)
            if (e.lod_group == d.lod_group && e.lod_level == c) {
              has = true;
              break;
            }
          if (!has)
            c = d.lod_level;
        }
      }
      // coarsest level present per group (shadows use it; also the fallback)
      std::vector<int> coarsest(chosen.size(), -1);
      for (const DrawRange &d : I.draws) {
        if (d.lod_group >= 0)
          coarsest[static_cast<std::size_t>(d.lod_group)] = std::max(
              coarsest[static_cast<std::size_t>(d.lod_group)], d.lod_level);
      }
      // shadow level: the coarsest level that still has real geometry (<= 2);
      // the far shell (3) casts nothing useful
      std::vector<int> shadow_level(chosen.size(), -1);
      for (const DrawRange &d : I.draws) {
        if (d.lod_group >= 0 && d.lod_level <= 2)
          shadow_level[static_cast<std::size_t>(d.lod_group)] = std::max(
              shadow_level[static_cast<std::size_t>(d.lod_group)], d.lod_level);
      }
      for (std::size_t g = 0; g < chosen.size(); ++g)
        if (shadow_level[g] < 0)
          shadow_level[g] = coarsest[g];
      for (std::uint32_t c = 0; c < kCascades; ++c)
        I.sel_cascade[c].clear();
      // light-space culling: sphere against the cascade's ortho box (xy only;
      // depth is padded)
      auto in_cascade = [&](std::uint32_t c, const DrawRange &d) {
        if (d.radius <= 0.0f)
          return true;
        const Vec4 p = mul(I.cascade_mats[c], Vec4{d.centre, 1.0f});
        const float r_ndc = d.radius / std::max(I.cascade_ext[c], 1e-3f);
        return std::fabs(p.x) - r_ndc <= 1.0f && std::fabs(p.y) - r_ndc <= 1.0f;
      };
      I.stats = Stats{};
      I.stats.ranges_total = static_cast<std::uint32_t>(I.draws.size());
      for (const DrawRange &d : I.draws) {
        const bool in_view =
            d.radius <= 0.0f || frustum.visible(d.centre, d.radius);
        const bool draw_main =
            d.lod_group < 0
                ? in_view
                : (d.lod_level ==
                       chosen[static_cast<std::size_t>(d.lod_group)] &&
                   in_view);
        const bool casts =
            d.lod_group < 0 ||
            d.lod_level == shadow_level[static_cast<std::size_t>(d.lod_group)];
        if (draw_main) {
          I.sel_main.emplace_back(d.first, d.count);
          if (d.has_fine && occlusion_wanted) {
            // occlusion candidates: the block's buildings individually
            // (frustum-tested here)
            auto it = std::lower_bound(I.fine.begin(), I.fine.end(), d.first,
                                       [](const DrawRange &f, std::uint32_t v) {
                                         return f.first < v;
                                       });
            std::uint32_t covered = d.first;
            for (; it != I.fine.end() && it->first < d.first + d.count; ++it) {
              if (it->first > covered) {
                DrawRange gap;
                gap.first = covered;
                gap.count = it->first - covered;
                I.cand.push_back(gap);
              }
              if (frustum.visible(it->centre, it->radius))
                I.cand.push_back(*it);
              covered = it->first + it->count;
            }
            if (covered < d.first + d.count) {
              DrawRange gap;
              gap.first = covered;
              gap.count = d.first + d.count - covered;
              I.cand.push_back(gap);
            }
          } else {
            I.cand.push_back(d);
          }
          ++I.stats.ranges_drawn;
          I.stats.indices_drawn += d.count;
        }
        // far-cascade LOD: the last cascade takes tower shells (level 3)
        // instead of real geometry and skips bounded ranges (building blocks,
        // props) more than 350 m from the camera
        const bool shell_level =
            d.lod_group >= 0 &&
            d.lod_level == coarsest[static_cast<std::size_t>(d.lod_group)] &&
            d.lod_level == 3;
        for (std::uint32_t c = 0; c < kCascades; ++c) {
          if (!update_cascade[c] || !in_cascade(c, d))
            continue;
          bool cast_here = casts;
          if (settings_.shadow_far_lod && c == kCascades - 1) {
            if (d.lod_group >= 0)
              cast_here = shell_level ||
                          (casts &&
                           coarsest[static_cast<std::size_t>(d.lod_group)] < 3);
            else if (d.radius > 0.0f &&
                     length(d.centre - camera.position) - d.radius > 350.0f)
              cast_here = false;
          }
          if (cast_here)
            I.sel_cascade[c].emplace_back(d.first, d.count);
        }
      }

      // merge contiguous ranges to cut draw calls
      auto merge = [](std::vector<std::pair<std::uint32_t, std::uint32_t>> &v) {
        std::sort(v.begin(), v.end());
        std::vector<std::pair<std::uint32_t, std::uint32_t>> out;
        for (const auto &r : v) {
          if (!out.empty() && out.back().first + out.back().second == r.first)
            out.back().second += r.second;
          else
            out.push_back(r);
        }
        v.swap(out);
      };
      merge(I.sel_main);
      for (std::uint32_t c = 0; c < kCascades; ++c)
        merge(I.sel_cascade[c]);
    }

    // Compact only transforms per visibility pass; every mesh stays resident
    // once. The source instance list remains camera-independent for transport.
    {
      const Frustum frusta[memory::visibility_views] = {
          Frustum::from(fu.view_proj, true),
          Frustum::from(I.cascade_mats[0]),
          Frustum::from(I.cascade_mats[1]),
          Frustum::from(I.cascade_mats[2]),
          Frustum::from(fu.reflection_vp, true),
          Frustum::from(fu.puddle_reflection_vp, true),
          Frustum::from(fu.pond_reflection_vp, true)};
      std::vector<GpuInstance> visible;
      visible.reserve(I.instance_capacity);
      visible.push_back(I.all_instances[0]);
      const auto views = memory::visibility_views - (I.has_planar_pond ? 0 : 1);
      for (std::uint32_t pass = 0; pass < views; ++pass)
        for (auto &r : I.resources) {
          r.visible[pass].first = static_cast<std::uint32_t>(visible.size());
          for (std::uint32_t i = r.first; i < r.first + r.count; ++i) {
            const auto &instance = I.all_instances[i];
            const Vec3 centre = mul(instance.model, Vec4{r.centre, 1}).xyz();
            const float scale = std::max(
                {length(Vec3{instance.model.at(0, 0), instance.model.at(1, 0),
                             instance.model.at(2, 0)}),
                 length(Vec3{instance.model.at(0, 1), instance.model.at(1, 1),
                             instance.model.at(2, 1)}),
                 length(Vec3{instance.model.at(0, 2), instance.model.at(1, 2),
                             instance.model.at(2, 2)})});
            if (frusta[pass].visible(centre, r.radius * scale)) {
              if (visible.size() >= I.instance_capacity)
                gpu_->fail(
                    "visibility compaction exceeded seven-view capacity");
              visible.push_back(instance);
            }
          }
          r.visible[pass].count = static_cast<std::uint32_t>(visible.size()) -
                                  r.visible[pass].first;
          if (pass == 0)
            I.stats.indices_drawn +=
                std::uint64_t(r.mesh.index_count) * r.visible[pass].count;
        }
      if (visible.size() > I.instance_capacity)
        gpu_->fail("visibility upload exceeds instance buffer capacity");
      // This is before command encoding, so bounded submissions cannot reorder
      // any active render pass. Other per-pass uniform writes stay deferred.
      gpu_->upload_buffer(I.instance_buf, 0, visible.data(),
                          visible.size() * sizeof(GpuInstance));
    }
    WGPUCommandEncoderDescriptor ed{};
    WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(dev, &ed);
    RenderBatch batch(*gpu_, enc, I.frame_index == 0);
    auto draw_mesh = [&](WGPURenderPassEncoder &pass, const MeshBuffers &m) {
      if (m.index_count == 0)
        return;
      batch.vertex(pass, 0, m.vertices, 0, WGPU_WHOLE_SIZE);
      batch.index(pass, m.indices, WGPUIndexFormat_Uint32, 0, WGPU_WHOLE_SIZE);
      batch.draw(pass, m.index_count, 1, 0, 0, 0);
    };
    auto draw_resources = [&](WGPURenderPassEncoder &pass, int selection = 0,
                              bool glass_pass = false) {
      for (const auto &r : I.resources) {
        // Mixed-material resources retain their complete mesh in both passes.
        // Pure foliage/opaque resources never enter the glass-only shader.
        if (glass_pass ? !r.has_glass : !r.has_opaque)
          continue;
        const auto &selected = r.visible[selection];
        if (!selected.count || !r.mesh.index_count)
          continue;
        batch.vertex(pass, 0, r.mesh.vertices, 0, WGPU_WHOLE_SIZE);
        batch.index(pass, r.mesh.indices, WGPUIndexFormat_Uint32, 0,
                    WGPU_WHOLE_SIZE);
        batch.draw(pass, r.mesh.index_count, selected.count, 0, 0,
                   selected.first);
      }
    };
    // Selections are drawn through CPU-built indirect args and one multi-draw
    // call per pass (slot: 0 prepass, 1..3 cascades); the driver overhead of
    // thousands of DrawIndexed calls dominated once ranges were per building.
    auto draw_opaque_indirect =
        [&](WGPURenderPassEncoder &pass,
            const std::vector<std::pair<std::uint32_t, std::uint32_t>> &sel,
            int slot) {
          if (I.opaque.index_count == 0 || sel.empty())
            return;
          const std::uint32_t n = static_cast<std::uint32_t>(sel.size());
          if (n > I.pass_args_capacity[slot]) {
            if (I.pass_args[slot] != nullptr)
              wgpuBufferRelease(I.pass_args[slot]);
            I.pass_args[slot] = nullptr;
            I.pass_args_capacity[slot] =
                std::max(n + 512, I.pass_args_capacity[slot] * 2);
            I.pass_args[slot] = gpu_->create_buffer(
                WGPUBufferUsage_Indirect | WGPUBufferUsage_CopyDst,
                I.pass_args_capacity[slot] * 20ull, nullptr, "pass-args");
          }
          std::vector<std::uint32_t> args(static_cast<std::size_t>(n) * 5);
          for (std::uint32_t i = 0; i < n; ++i) {
            args[i * 5 + 0] = sel[i].second;
            args[i * 5 + 1] = 1;
            args[i * 5 + 2] = sel[i].first;
            args[i * 5 + 3] = 0;
            args[i * 5 + 4] = 0;
          }
          gpu_->write_buffer(I.pass_args[slot], 0, args.data(),
                             args.size() * 4);
          batch.vertex(pass, 0, I.opaque.vertices, 0, WGPU_WHOLE_SIZE);
          batch.index(pass, I.opaque.indices, WGPUIndexFormat_Uint32, 0,
                      WGPU_WHOLE_SIZE);
          for (std::uint32_t i = 0; i < n; ++i) {
            if (sel[i].second / 3 > RenderBatch::triangle_budget)
              batch.draw(pass, sel[i].second, 1, sel[i].first, 0, 0);
            else
              batch.indirect(pass, I.pass_args[slot], i * 20ull,
                             sel[i].second / 3);
          }
        };
    // camera passes use reversed Z (clear to 0, pass if greater); the
    // orthographic shadow cascades keep standard Z (clear to 1, pass if less)
    auto depth_attachment = [](WGPUTextureView v, bool clear,
                               float clear_value) {
      WGPURenderPassDepthStencilAttachment a{};
      a.view = v;
      a.depthLoadOp = clear ? WGPULoadOp_Clear : WGPULoadOp_Load;
      a.depthStoreOp = WGPUStoreOp_Store;
      a.depthClearValue = clear_value;
      return a;
    };
    auto color_attachment = [](WGPUTextureView v, WGPUTextureView resolve,
                               bool clear) {
      WGPURenderPassColorAttachment a{};
      a.view = v;
      a.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
      a.resolveTarget = resolve;
      a.loadOp = clear ? WGPULoadOp_Clear : WGPULoadOp_Load;
      a.storeOp = WGPUStoreOp_Store;
      a.clearValue = WGPUColor{0.0, 0.0, 0.0, 1.0};
      return a;
    };

    // ---- shadow cascades
    // ----------------------------------------------------------
    if (settings_.shadows && I.env->has_sun) {
      for (std::uint32_t c = 0; c < kCascades; ++c) {
        if (!update_cascade[c])
          continue;
        WGPURenderPassDepthStencilAttachment da =
            depth_attachment(I.shadow_layer_views[c], true, 1.0f);
        WGPURenderPassDescriptor rp{};
        rp.depthStencilAttachment = &da;
        WGPURenderPassEncoder pass =
            batch.begin(rp, "shadow-" + std::to_string(c));
        const std::uint32_t off = c * kUniformStride;
        batch.group(pass, 0, I.cascade_bg, 1, &off);
        batch.group(pass, 1, I.leaf_bg, 0, nullptr);
        batch.pipeline(pass, I.p_shadow);
        draw_opaque_indirect(pass, I.sel_cascade[c], 1 + static_cast<int>(c));
        draw_resources(pass, 1 + static_cast<int>(c));
        batch.pipeline(pass, I.p_shadow_foliage);
        draw_mesh(pass, I.foliage);
        batch.end(pass);
      }
    }
    if (settings_.reflections)
      for (int plane = 0; plane < (I.has_planar_pond ? 3 : 2); ++plane) {
        WGPURenderPassColorAttachment ca =
            color_attachment(I.reflection_mip_views[plane][0], nullptr, true);
        WGPURenderPassDepthStencilAttachment da =
            depth_attachment(I.reflection_depth.view, true, 0);
        WGPURenderPassDescriptor rp{};
        rp.colorAttachmentCount = 1;
        rp.colorAttachments = &ca;
        rp.depthStencilAttachment = &da;
        auto pass = batch.begin(rp, plane == 0   ? "ocean-reflection"
                                    : plane == 1 ? "puddle-reflection"
                                                 : "pond-reflection");
        batch.group(pass, 0,
                    plane == 0   ? I.reflection_frame_bg
                    : plane == 1 ? I.puddle_frame_bg
                                 : I.pond_frame_bg,
                    0, nullptr);
        batch.group(pass, 1, I.sky_bg, 0, nullptr);
        batch.pipeline(pass, I.p_sky1);
        wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
        batch.group(pass, 1, I.reflection_tex_bg, 0, nullptr);
        // Mirroring reverses winding. The two-sided pipeline preserves the same
        // geometry and supports alpha foliage rather than discarding its
        // fronts.
        batch.pipeline(pass, I.p_foliage1);
        if (I.opaque.index_count) {
          batch.vertex(pass, 0, I.opaque.vertices, 0, WGPU_WHOLE_SIZE);
          batch.index(pass, I.opaque.indices, WGPUIndexFormat_Uint32, 0,
                      WGPU_WHOLE_SIZE);
          for (const auto &d : I.draws)
            if (d.lod_group < 0 || d.lod_level == 0)
              batch.draw(pass, d.count, 1, d.first, 0, 0);
        }
        draw_resources(pass, 4 + plane);
        draw_mesh(pass, I.foliage);
        batch.end(pass);
        // The same nearest-pane optical model also belongs in reflected
        // scenes. Reuse the primary opaque-copy scratch texture sequentially;
        // no recursive reflection or additional full-size target is needed.
        WGPUTexelCopyTextureInfo source{}, destination{};
        source.texture = plane == 0   ? I.reflection.texture
                         : plane == 1 ? I.puddle_reflection.texture
                                      : I.pond_reflection.texture;
        source.aspect = WGPUTextureAspect_All;
        destination.texture = I.scene_color.texture;
        destination.aspect = WGPUTextureAspect_All;
        const WGPUExtent3D extent{I.w, I.h, 1};
        wgpuCommandEncoderCopyTextureToTexture(enc, &source, &destination,
                                               &extent);
        auto glass_color =
            color_attachment(I.reflection_mip_views[plane][0], nullptr, false);
        auto glass_depth = depth_attachment(I.reflection_depth.view, false, 0);
        WGPURenderPassDescriptor glass_pass{};
        glass_pass.colorAttachmentCount = 1;
        glass_pass.colorAttachments = &glass_color;
        glass_pass.depthStencilAttachment = &glass_depth;
        auto glass =
            batch.begin(glass_pass, plane == 0   ? "ocean-reflected-glass"
                                    : plane == 1 ? "puddle-reflected-glass"
                                                 : "pond-reflected-glass");
        batch.group(glass, 0, I.reflected_glass_frame_bg[plane], 0, nullptr);
        batch.group(glass, 1, I.reflection_tex_bg, 0, nullptr);
        batch.pipeline(glass, I.p_glass1);
        if (!I.reflected_glass_draws.empty()) {
          batch.vertex(glass, 0, I.opaque.vertices, 0, WGPU_WHOLE_SIZE);
          batch.index(glass, I.opaque.indices, WGPUIndexFormat_Uint32, 0,
                      WGPU_WHOLE_SIZE);
          for (const auto &[first, count] : I.reflected_glass_draws)
            batch.draw(glass, count, 1, first, 0, 0);
        }
        draw_resources(glass, 4 + plane, true);
        batch.end(glass);
        // Linear HDR mip averaging preserves reflected radiance. Subresource
        // views keep each sampled source mip disjoint from its destination.
        for (std::size_t mip = 1; mip < I.reflection_mip_views[plane].size();
             ++mip) {
          auto target = color_attachment(I.reflection_mip_views[plane][mip],
                                         nullptr, true);
          WGPURenderPassDescriptor mip_pass{};
          mip_pass.colorAttachmentCount = 1;
          mip_pass.colorAttachments = &target;
          auto filter = wgpuCommandEncoderBeginRenderPass(enc, &mip_pass);
          const std::uint32_t offset = 0;
          wgpuRenderPassEncoderSetBindGroup(
              filter, 0, I.reflection_mip_groups[plane][mip - 1], 1, &offset);
          wgpuRenderPassEncoderSetPipeline(filter, I.p_reflection_mip);
          wgpuRenderPassEncoderDraw(filter, 3, 1, 0, 0);
          wgpuRenderPassEncoderEnd(filter);
          wgpuRenderPassEncoderRelease(filter);
        }
      }
    const bool occlusion = occlusion_wanted;
    const bool need_prepass = settings_.ssao || taa_on || occlusion ||
                              I.env->display_referred_background;
    // ---- prepass (depth + view normal)
    // ------------------------------------------------
    if (need_prepass) {
      WGPURenderPassColorAttachment ca =
          color_attachment(I.normal_pre.view, nullptr, true);
      WGPURenderPassDepthStencilAttachment da =
          depth_attachment(I.depth_pre.view, true, 0.0f);
      WGPURenderPassDescriptor rp{};
      rp.colorAttachmentCount = 1;
      rp.colorAttachments = &ca;
      rp.depthStencilAttachment = &da;
      WGPURenderPassEncoder pass = batch.begin(rp, "depth-prepass");
      batch.group(pass, 0, I.frame_bg, 0, nullptr);
      batch.group(pass, 1, I.leaf_bg, 0, nullptr);
      batch.pipeline(pass, I.p_prepass);
      draw_opaque_indirect(pass, I.sel_main, 0);
      draw_resources(pass);
      batch.pipeline(pass, I.p_prepass_foliage);
      draw_mesh(pass, I.foliage);
      batch.end(pass);
      // SSAO + blur
      auto fullscreen = [&](WGPURenderPipeline p, WGPUBindGroup g1,
                            WGPUTextureView out) {
        WGPURenderPassColorAttachment ca2 =
            color_attachment(out, nullptr, true);
        WGPURenderPassDescriptor rp2{};
        rp2.colorAttachmentCount = 1;
        rp2.colorAttachments = &ca2;
        WGPURenderPassEncoder p2 = wgpuCommandEncoderBeginRenderPass(enc, &rp2);
        batch.group(p2, 0, I.frame_bg, 0, nullptr);
        batch.group(p2, 1, g1, 0, nullptr);
        batch.pipeline(p2, p);
        wgpuRenderPassEncoderDraw(p2, 3, 1, 0, 0);
        wgpuRenderPassEncoderEnd(p2);
        wgpuRenderPassEncoderRelease(p2);
      };
      if (settings_.ssao) {
        fullscreen(I.p_ssao, I.ssao_bg, I.ao_a.view);
        fullscreen(I.p_blur, I.blur_bg, I.ao_b.view);
      }
    }
    // ---- depth pyramid + GPU occlusion culling
    // -------------------------------------------
    if (occlusion) {
      for (std::size_t m = 0; m < I.hiz_bgs.size(); ++m) {
        WGPURenderPassColorAttachment ca =
            color_attachment(I.hiz_views[m], nullptr, true);
        WGPURenderPassDescriptor rp{};
        rp.colorAttachmentCount = 1;
        rp.colorAttachments = &ca;
        WGPURenderPassEncoder pass =
            wgpuCommandEncoderBeginRenderPass(enc, &rp);
        batch.group(pass, 0, I.hiz_bgs[m], 0, nullptr);
        batch.pipeline(pass, m == 0 ? I.p_hiz_copy : I.p_hiz_down);
        wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
        wgpuRenderPassEncoderEnd(pass);
        wgpuRenderPassEncoderRelease(pass);
      }
      // candidate buffer (grow as needed) and args buffer
      const std::uint32_t n = static_cast<std::uint32_t>(I.cand.size());
      // grow the buffers only when the candidate count exceeds them; the bind
      // group alone is rebuilt whenever the depth pyramid was recreated
      if (n > I.cull_capacity || I.cull_ranges == nullptr) {
        I.cull_capacity = std::max(n + 256, I.cull_capacity * 2);
        if (I.cull_ranges != nullptr)
          wgpuBufferRelease(I.cull_ranges);
        if (I.cull_args != nullptr)
          wgpuBufferRelease(I.cull_args);
        I.cull_ranges = nullptr;
        I.cull_args = nullptr;
        I.cull_ranges = gpu_->create_buffer(
            WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst,
            I.cull_capacity * 32ull, nullptr, "cull-ranges");
        I.cull_args = gpu_->create_buffer(
            WGPUBufferUsage_Storage | WGPUBufferUsage_Indirect |
                WGPUBufferUsage_CopySrc,
            I.cull_capacity * 20ull, nullptr, "cull-args");
        if (I.cull_bg != nullptr) {
          wgpuBindGroupRelease(I.cull_bg);
          I.cull_bg = nullptr;
        }
      }
      if (I.cull_bg == nullptr) {
        I.cull_bg =
            make_bg(dev, I.cull_bgl,
                    {bg_buffer(0, I.cull_uniform, 160),
                     bg_buffer(1, I.cull_ranges, I.cull_capacity * 32ull),
                     bg_buffer(2, I.cull_args, I.cull_capacity * 20ull),
                     bg_texture(3, I.hiz.view)});
      }
      if (n > 0) {
        struct GpuRange {
          float cx, cy, cz, r;
          std::uint32_t first, count, pad0, pad1;
        };
        std::vector<GpuRange> gr(n);
        for (std::uint32_t i = 0; i < n; ++i)
          gr[i] = GpuRange{I.cand[i].centre.x,
                           I.cand[i].centre.y,
                           I.cand[i].centre.z,
                           I.cand[i].radius,
                           I.cand[i].first,
                           I.cand[i].count,
                           0,
                           0};
        gpu_->write_buffer(I.cull_ranges, 0, gr.data(), n * sizeof(GpuRange));
        struct CullUniform {
          Mat4 view, proj;
          Vec4 params, params2;
        } cu;
        cu.view = fu.view;
        cu.proj = fu.proj;
        cu.params =
            Vec4{static_cast<float>(n), static_cast<float>(I.hiz_bgs.size()),
                 static_cast<float>(I.w), static_cast<float>(I.h)};
        cu.params2 = Vec4{25.0f, 1.0f, 0.0f, 0.0f};
        gpu_->write_buffer(I.cull_uniform, 0, &cu, sizeof(cu));
        WGPUComputePassDescriptor cpd{};
        WGPUComputePassEncoder cpass =
            wgpuCommandEncoderBeginComputePass(enc, &cpd);
        wgpuComputePassEncoderSetPipeline(cpass, I.p_cull);
        wgpuComputePassEncoderSetBindGroup(cpass, 0, I.cull_bg, 0, nullptr);
        wgpuComputePassEncoderDispatchWorkgroups(cpass, (n + 63) / 64, 1, 1);
        wgpuComputePassEncoderEnd(cpass);
        wgpuComputePassEncoderRelease(cpass);
        // statistics: every 30th frame copy the args to a mappable buffer (read
        // next frame)
        if (I.frame_index % 30 == 0 && !I.cull_readback_pending) {
          const std::uint64_t bytes = n * 20ull;
          if (I.cull_readback == nullptr || I.cull_readback_count < n) {
            if (I.cull_readback != nullptr)
              wgpuBufferRelease(I.cull_readback);
            I.cull_readback = nullptr;
            I.cull_readback = gpu_->create_buffer(
                WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst,
                I.cull_capacity * 20ull, nullptr, "cull-readback");
            I.cull_readback_count = I.cull_capacity;
          }
          wgpuCommandEncoderCopyBufferToBuffer(enc, I.cull_args, 0,
                                               I.cull_readback, 0, bytes);
          I.cull_readback_pending = true;
          struct MapCtx {
            Impl *impl;
            std::uint32_t n;
            std::weak_ptr<int> lifetime;
          };
          auto *ctx = new MapCtx{&I, n, I.callback_lifetime};
          I.pending_map_discard = [ctx] { delete ctx; };
          // map after submit: request now, the callback runs on a later poll
          WGPUBufferMapCallbackInfo mi{};
          mi.mode = WGPUCallbackMode_AllowProcessEvents;
          mi.userdata1 = ctx;
          mi.callback = [](WGPUMapAsyncStatus status, WGPUStringView, void *u1,
                           void *) {
            auto *c = static_cast<MapCtx *>(u1);
            if (c->lifetime.expired()) {
              delete c;
              return;
            }
            if (status == WGPUMapAsyncStatus_Success) {
              const auto *a = static_cast<const std::uint32_t *>(
                  wgpuBufferGetConstMappedRange(c->impl->cull_readback, 0,
                                                c->n * 20ull));
              std::uint32_t occluded = 0;
              for (std::uint32_t i = 0; i < c->n; ++i)
                if (a[i * 5 + 1] == 0)
                  ++occluded;
              c->impl->occluded_last = occluded;
              wgpuBufferUnmap(c->impl->cull_readback);
            }
            c->impl->cull_readback_pending = false;
            delete c;
          };
          I.pending_map = mi;
          I.pending_map_bytes = bytes;
          I.pending_map_armed = true;
        }
      }
    }
    I.stats.ranges_occluded = occlusion ? I.occluded_last : 0;
    // ---- main pass
    // ------------------------------------------------------------------
    {
      const bool msaa = settings_.msaa > 1;
      WGPURenderPassColorAttachment ca =
          color_attachment(msaa ? I.hdr_msaa.view : I.hdr.view,
                           msaa ? I.hdr.view : nullptr, true);
      WGPURenderPassDepthStencilAttachment da =
          depth_attachment(I.depth_msaa.view, true, 0.0f);
      WGPURenderPassDescriptor rp{};
      rp.colorAttachmentCount = 1;
      rp.colorAttachments = &ca;
      rp.depthStencilAttachment = &da;
      WGPURenderPassEncoder pass = batch.begin(rp, "main-geometry");
      batch.group(pass, 0, I.frame_bg, 0, nullptr);
      batch.group(pass, 1, I.sky_bg, 0, nullptr);
      batch.pipeline(pass, msaa ? I.p_sky : I.p_sky1);
      wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
      batch.group(pass, 1, I.scene_tex_bg, 0, nullptr);
      batch.pipeline(pass, msaa ? I.p_main : I.p_main1);
      if (occlusion && !I.cand.empty() && I.opaque.index_count > 0) {
        batch.vertex(pass, 0, I.opaque.vertices, 0, WGPU_WHOLE_SIZE);
        batch.index(pass, I.opaque.indices, WGPUIndexFormat_Uint32, 0,
                    WGPU_WHOLE_SIZE);
        const std::uint32_t n = static_cast<std::uint32_t>(I.cand.size());
        for (std::uint32_t i = 0; i < n; ++i) {
          if (I.cand[i].count / 3 > RenderBatch::triangle_budget)
            batch.draw(pass, I.cand[i].count, 1, I.cand[i].first, 0, 0);
          else
            batch.indirect(pass, I.cull_args, i * 20ull, I.cand[i].count / 3);
        }
      } else {
        draw_opaque_indirect(pass, I.sel_main, 0); // same args as the prepass
      }
      draw_resources(pass);
      batch.pipeline(pass, msaa ? I.p_foliage : I.p_foliage1);
      draw_mesh(pass, I.foliage);
      batch.end(pass);
    }
    // True foreground glazing: sample the opaque scene behind the physical
    // pane. Nearest-pane depth prevents back layers overwriting the front.
    {
      WGPUTexelCopyTextureInfo source{}, destination{};
      source.texture = I.hdr.texture;
      source.aspect = WGPUTextureAspect_All;
      destination.texture = I.scene_color.texture;
      destination.aspect = WGPUTextureAspect_All;
      const WGPUExtent3D extent{I.w, I.h, 1};
      wgpuCommandEncoderCopyTextureToTexture(enc, &source, &destination,
                                             &extent);
      const bool msaa = settings_.msaa > 1;
      auto ca = color_attachment(msaa ? I.hdr_msaa.view : I.hdr.view,
                                 msaa ? I.hdr.view : nullptr, false);
      auto da = depth_attachment(I.depth_msaa.view, false, 0);
      WGPURenderPassDescriptor rp{};
      rp.colorAttachmentCount = 1;
      rp.colorAttachments = &ca;
      rp.depthStencilAttachment = &da;
      auto pass = batch.begin(rp, "true-glass");
      batch.group(pass, 0, I.glass_frame_bg, 0, nullptr);
      batch.group(pass, 1, I.scene_tex_bg, 0, nullptr);
      batch.pipeline(pass, msaa ? I.p_glass : I.p_glass1);
      draw_opaque_indirect(pass, I.sel_main, 0);
      draw_resources(pass, 0, true);
      batch.end(pass);
    }
    if (need_prepass) {
      // Temporal/post depth is single-sampled even when main shading is MSAA.
      // Add only the nearest glass depth after SSAO and opaque culling have
      // consumed their opaque-only inputs; retain the opaque scene underneath.
      auto ca = color_attachment(I.normal_pre.view, nullptr, false);
      auto da = depth_attachment(I.depth_pre.view, false, 0);
      WGPURenderPassDescriptor rp{};
      rp.colorAttachmentCount = 1;
      rp.colorAttachments = &ca;
      rp.depthStencilAttachment = &da;
      auto pass = batch.begin(rp, "glass-temporal-depth");
      batch.group(pass, 0, I.glass_frame_bg, 0, nullptr);
      batch.group(pass, 1, I.leaf_bg, 0, nullptr);
      batch.pipeline(pass, I.p_prepass_glass);
      draw_opaque_indirect(pass, I.sel_main, 0);
      draw_resources(pass, 0, true);
      batch.end(pass);
    }
    // ---- post: bloom, tonemap, fxaa, blit
    // ------------------------------------------------
    auto post = [&](WGPURenderPipeline p, WGPUBindGroup g, std::uint32_t slot,
                    WGPUTextureView out) {
      WGPURenderPassColorAttachment ca = color_attachment(out, nullptr, true);
      WGPURenderPassDescriptor rp{};
      rp.colorAttachmentCount = 1;
      rp.colorAttachments = &ca;
      WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(enc, &rp);
      const std::uint32_t off = slot * kUniformStride;
      if (out == target && target != nullptr) {
        const float scale = std::min(float(gpu_->width) / I.w,
                                     float(gpu_->height) / I.h);
        const float width = I.w * scale, height = I.h * scale;
        wgpuRenderPassEncoderSetViewport(pass, (gpu_->width - width) * .5f,
                                         (gpu_->height - height) * .5f,
                                         width, height, 0.f, 1.f);
      }
      batch.group(pass, 0, g, 1, &off);
      batch.pipeline(pass, p);
      wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
      wgpuRenderPassEncoderEnd(pass);
      wgpuRenderPassEncoderRelease(pass);
    };
    const bool run_taa =
        taa_on; // the prepass (1x depth) runs whenever TAA is on
    const int par = I.taa_parity;
    if (run_taa) {
      struct TaaUniform {
        Mat4 inv_vp;
        Mat4 prev_vp;
        Vec4 params;
        Vec4 jitter;
      } tu;
      tu.inv_vp = inverse(view_proj_clean);
      tu.prev_vp = I.prev_view_proj;
      tu.params = Vec4{0.92f, I.taa_valid ? 1.0f : 0.0f,
                       static_cast<float>(I.w), static_cast<float>(I.h)};
      tu.jitter =
          Vec4{jx, -jy, 0.0f, 0.0f}; // uv y is flipped relative to clip y
      gpu_->write_buffer(I.taa_buf, 0, &tu, sizeof(tu));
      WGPURenderPassColorAttachment ca =
          color_attachment(I.taa_hist[par].view, nullptr, true);
      WGPURenderPassDescriptor rp{};
      rp.colorAttachmentCount = 1;
      rp.colorAttachments = &ca;
      WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(enc, &rp);
      batch.group(pass, 0, I.taa_bg[par], 0, nullptr);
      batch.pipeline(pass, I.p_taa);
      wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
      wgpuRenderPassEncoderEnd(pass);
      wgpuRenderPassEncoderRelease(pass);
    }
    if (settings_.bloom) {
      for (std::uint32_t i = 0; i < kBloomLevels; ++i)
        post(I.p_down, (i == 0 && run_taa) ? I.down0_taa_bg[par] : I.down_bg[i],
             4 + i, I.bloom[i].view);
      for (std::uint32_t i = 0; i < kBloomLevels - 1; ++i) {
        const std::uint32_t hi = kBloomLevels - 2 - i;
        post(I.p_up, I.up_bg[i], 4 + kBloomLevels + i, I.bloom_up[hi].view);
      }
    }
    if (settings_.debug_view == 12 ||
        (settings_.debug_view == 16 || settings_.debug_view == 17 ||
         settings_.debug_view == 18 || settings_.debug_view == 19 ||
         settings_.debug_view == 20)) {
      post(I.p_copy, I.dbg_hdr_bg, 3,
           I.ldr_b.view); // ids/physical window fields, no tonemap or FXAA
    } else {
      post(I.p_tonemap, run_taa ? I.tonemap_taa_bg[par] : I.tonemap_bg, 0,
           I.ldr_a.view);
      if (settings_.fxaa) {
        post(I.p_fxaa, I.fxaa_bg, 1, I.ldr_b.view);
      } else {
        post(I.p_copy, I.fxaa_bg, 3,
             I.ldr_b.view); // slot 3: plain copy (a.x = 0)
      }
    }
    if (settings_.debug_view == 10)
      post(I.p_copy, I.dbg_ao_bg, 3, I.ldr_b.view);
    if (settings_.debug_view == 11)
      post(I.p_copy, I.dbg_normal_bg, 3, I.ldr_b.view);
    if (target != nullptr)
      post(I.p_blit, I.blit_bg, 2, target);

    batch.flush("post-and-output");
    if (I.pending_map_armed) {
      wgpuBufferMapAsync(I.cull_readback, WGPUMapMode_Read, 0,
                         I.pending_map_bytes, I.pending_map);
      I.pending_map_armed = false;
      I.pending_map_discard = {};
    }
    if (run_taa) {
      I.taa_valid = true;
      I.taa_parity = 1 - I.taa_parity;
    } else {
      I.taa_valid = false;
    }
    I.prev_view_proj = view_proj_clean;
    I.last_view_proj = view_proj_clean;
    ++I.frame_index;
  } catch (const GpuUnavailable &failure) {
    std::fprintf(stderr, "render stopped: %s\n", failure.what());
    return;
  }
}

bool Renderer::read_depth(std::vector<float> *depth, std::uint32_t *w,
                          std::uint32_t *h) {
  Impl &I = *impl_;
  *w = I.depth_pre.width;
  *h = I.depth_pre.height;
  return gpu_->read_depth32(I.depth_pre, depth);
}

const Mat4 &Renderer::last_view_proj() const { return impl_->last_view_proj; }

void Renderer::reset_history() { impl_->taa_valid = false; }

bool Renderer::read_frame(std::vector<std::uint8_t> *rgba, std::uint32_t *w,
                          std::uint32_t *h) {
  Impl &I = *impl_;
  *w = I.ldr_b.width;
  *h = I.ldr_b.height;
  return gpu_->read_rgba8(I.ldr_b, rgba);
}

bool Renderer::capture_png(const std::string &path) {
  Impl &I = *impl_;
  std::vector<std::uint8_t> rgba;
  if (!gpu_->read_rgba8(I.ldr_b, &rgba))
    return false;
  for (std::size_t i = 3; i < rgba.size(); i += 4)
    rgba[i] = 255;
  return stbi_write_png(path.c_str(), static_cast<int>(I.ldr_b.width),
                        static_cast<int>(I.ldr_b.height), 4, rgba.data(),
                        static_cast<int>(I.ldr_b.width * 4)) != 0;
}

} // namespace cb
