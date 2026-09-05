#include "renderer.hpp"

#include <stb_image_write.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

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
};

struct GpuMaterial {
  Vec4 base_color, params, tex, misc, room;
};
struct GpuLight {
  Vec4 pos_radius, color_int;
};

WGPUBindGroupLayoutEntry buffer_entry(std::uint32_t binding, WGPUShaderStage vis, WGPUBufferBindingType type,
                                      bool dynamic, std::uint64_t min_size) {
  WGPUBindGroupLayoutEntry e{};
  e.binding = binding;
  e.visibility = vis;
  e.buffer.type = type;
  e.buffer.hasDynamicOffset = dynamic ? 1U : 0U;
  e.buffer.minBindingSize = min_size;
  return e;
}
WGPUBindGroupLayoutEntry texture_entry(std::uint32_t binding, WGPUTextureSampleType type,
                                       WGPUTextureViewDimension dim) {
  WGPUBindGroupLayoutEntry e{};
  e.binding = binding;
  e.visibility = WGPUShaderStage_Fragment;
  e.texture.sampleType = type;
  e.texture.viewDimension = dim;
  return e;
}
WGPUBindGroupLayoutEntry sampler_entry(std::uint32_t binding, WGPUSamplerBindingType type) {
  WGPUBindGroupLayoutEntry e{};
  e.binding = binding;
  e.visibility = WGPUShaderStage_Fragment;
  e.sampler.type = type;
  return e;
}
WGPUBindGroupEntry bg_buffer(std::uint32_t binding, WGPUBuffer b, std::uint64_t size) {
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

WGPUVertexBufferLayout vertex_layout(WGPUVertexAttribute* attrs) {
  const WGPUVertexFormat fmts[6] = {WGPUVertexFormat_Float32x3, WGPUVertexFormat_Float32x3, WGPUVertexFormat_Float32x4,
                                    WGPUVertexFormat_Float32x2, WGPUVertexFormat_Uint32, WGPUVertexFormat_Float32x4};
  const std::uint64_t offs[6] = {0, 12, 24, 40, 48, 52};
  for (std::uint32_t i = 0; i < 6; ++i) {
    attrs[i] = WGPUVertexAttribute{};
    attrs[i].format = fmts[i];
    attrs[i].offset = offs[i];
    attrs[i].shaderLocation = i;
  }
  WGPUVertexBufferLayout l{};
  l.arrayStride = sizeof(Vertex);
  l.stepMode = WGPUVertexStepMode_Vertex;
  l.attributeCount = 6;
  l.attributes = attrs;
  return l;
}

struct PipelineOpts {
  WGPUShaderModule module{nullptr};
  const char* vs{"vs_main"};
  const char* fs{nullptr};  // nullptr = depth only
  WGPUPipelineLayout layout{nullptr};
  WGPUTextureFormat color{WGPUTextureFormat_Undefined};
  WGPUTextureFormat depth{WGPUTextureFormat_Undefined};
  bool depth_write{true};
  WGPUCompareFunction depth_compare{WGPUCompareFunction_Less};
  std::uint32_t samples{1};
  WGPUCullMode cull{WGPUCullMode_Back};
  bool alpha_to_coverage{false};
  bool vertices{true};  // false = fullscreen triangle
  bool blend_add{false};
};

WGPURenderPipeline make_pipeline(WGPUDevice device, const PipelineOpts& o) {
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
    ds.depthWriteEnabled = o.depth_write ? WGPUOptionalBool_True : WGPUOptionalBool_False;
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

WGPUPipelineLayout make_layout(WGPUDevice device, std::initializer_list<WGPUBindGroupLayout> groups) {
  std::vector<WGPUBindGroupLayout> v(groups.begin(), groups.end());
  WGPUPipelineLayoutDescriptor d{};
  d.bindGroupLayoutCount = v.size();
  d.bindGroupLayouts = v.data();
  return wgpuDeviceCreatePipelineLayout(device, &d);
}

WGPUBindGroupLayout make_bgl(WGPUDevice device, std::vector<WGPUBindGroupLayoutEntry> entries) {
  WGPUBindGroupLayoutDescriptor d{};
  d.entryCount = entries.size();
  d.entries = entries.data();
  return wgpuDeviceCreateBindGroupLayout(device, &d);
}

WGPUBindGroup make_bg(WGPUDevice device, WGPUBindGroupLayout layout, std::vector<WGPUBindGroupEntry> entries) {
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
    if (vertices != nullptr) wgpuBufferRelease(vertices);
    if (indices != nullptr) wgpuBufferRelease(indices);
    vertices = indices = nullptr;
    index_count = 0;
  }
};

struct PostParams {
  Vec4 a, b;
};

}  // namespace

struct Renderer::Impl {
  std::string shader_dir;
  // layouts
  WGPUBindGroupLayout frame_bgl{nullptr}, scene_tex_bgl{nullptr}, cascade_bgl{nullptr}, leaf_bgl{nullptr},
      ssao_bgl{nullptr}, sky_bgl{nullptr}, post_bgl{nullptr};
  WGPUPipelineLayout main_layout{nullptr}, shadow_layout{nullptr}, prepass_layout{nullptr}, ssao_layout{nullptr},
      sky_layout{nullptr}, post_layout{nullptr};
  // pipelines
  WGPURenderPipeline p_shadow{nullptr}, p_shadow_foliage{nullptr}, p_prepass{nullptr}, p_prepass_foliage{nullptr},
      p_ssao{nullptr}, p_blur{nullptr}, p_sky{nullptr}, p_main{nullptr}, p_foliage{nullptr}, p_down{nullptr},
      p_up{nullptr}, p_tonemap{nullptr}, p_fxaa{nullptr}, p_blit{nullptr}, p_copy{nullptr};
  // buffers
  WGPUBuffer frame_buf{nullptr}, cascade_buf{nullptr}, material_buf{nullptr}, light_buf{nullptr}, post_buf{nullptr};
  std::uint32_t material_count{0}, light_count{0};
  MeshBuffers opaque, foliage;
  // samplers
  WGPUSampler mat_samp{nullptr}, cube_samp{nullptr}, shadow_samp{nullptr}, clamp_samp{nullptr};
  // static textures
  Texture shadow_maps;
  std::vector<WGPUTextureView> shadow_layer_views;
  Texture leaf;
  const MaterialArrays* arrays{nullptr};
  const Environment* env{nullptr};
  bool night{false};
  // size-dependent
  std::uint32_t w{0}, h{0};
  Texture depth_pre, normal_pre, ao_a, ao_b, hdr_msaa, depth_msaa, hdr, ldr_a, ldr_b;
  std::vector<Texture> bloom;     // downsample chain, level i at w>>(i+1)
  std::vector<Texture> bloom_up;  // upsample chain, levels 0..L-2
  // bind groups
  WGPUBindGroup frame_bg{nullptr}, scene_tex_bg{nullptr}, cascade_bg{nullptr}, leaf_bg{nullptr}, ssao_bg{nullptr},
      blur_bg{nullptr}, sky_bg{nullptr}, tonemap_bg{nullptr}, fxaa_bg{nullptr}, blit_bg{nullptr},
      dbg_ao_bg{nullptr}, dbg_normal_bg{nullptr};
  std::vector<WGPUBindGroup> down_bg, up_bg;
  std::uint32_t post_slots{0};
  bool scene_dirty{true};
  bool have_scene{false};

  void release_targets() {
    for (Texture* t : {&depth_pre, &normal_pre, &ao_a, &ao_b, &hdr_msaa, &depth_msaa, &hdr, &ldr_a, &ldr_b}) t->release();
    for (Texture& t : bloom) t.release();
    for (Texture& t : bloom_up) t.release();
    bloom.clear();
    bloom_up.clear();
    for (WGPUBindGroup* g : {&ssao_bg, &blur_bg, &tonemap_bg, &fxaa_bg, &blit_bg, &scene_tex_bg, &dbg_ao_bg, &dbg_normal_bg}) {
      if (*g != nullptr) wgpuBindGroupRelease(*g);
      *g = nullptr;
    }
    for (WGPUBindGroup g : down_bg) wgpuBindGroupRelease(g);
    for (WGPUBindGroup g : up_bg) wgpuBindGroupRelease(g);
    down_bg.clear();
    up_bg.clear();
  }
};

bool Renderer::init(Gpu* gpu, const std::string& shader_dir, RenderSettings settings, std::string* error) {
  gpu_ = gpu;
  settings_ = settings;
  impl_ = new Impl();
  Impl& I = *impl_;
  I.shader_dir = shader_dir;
  WGPUDevice dev = gpu->device;

  // ---- layouts ---------------------------------------------------------------
  I.frame_bgl = make_bgl(dev, {buffer_entry(0, WGPUShaderStage_Vertex | WGPUShaderStage_Fragment, WGPUBufferBindingType_Uniform, false, sizeof(FrameUniform)),
                               buffer_entry(1, WGPUShaderStage_Fragment, WGPUBufferBindingType_ReadOnlyStorage, false, 0),
                               buffer_entry(2, WGPUShaderStage_Fragment, WGPUBufferBindingType_ReadOnlyStorage, false, 0)});
  I.scene_tex_bgl = make_bgl(dev, {texture_entry(0, WGPUTextureSampleType_Float, WGPUTextureViewDimension_2DArray),
                                   texture_entry(1, WGPUTextureSampleType_Float, WGPUTextureViewDimension_2DArray),
                                   texture_entry(2, WGPUTextureSampleType_Float, WGPUTextureViewDimension_2DArray),
                                   sampler_entry(3, WGPUSamplerBindingType_Filtering),
                                   texture_entry(4, WGPUTextureSampleType_Float, WGPUTextureViewDimension_Cube),
                                   texture_entry(5, WGPUTextureSampleType_Float, WGPUTextureViewDimension_Cube),
                                   sampler_entry(6, WGPUSamplerBindingType_Filtering),
                                   texture_entry(7, WGPUTextureSampleType_Depth, WGPUTextureViewDimension_2DArray),
                                   sampler_entry(8, WGPUSamplerBindingType_Comparison),
                                   texture_entry(9, WGPUTextureSampleType_Float, WGPUTextureViewDimension_2D),
                                   sampler_entry(10, WGPUSamplerBindingType_Filtering),
                                   texture_entry(11, WGPUTextureSampleType_Float, WGPUTextureViewDimension_2D)});
  I.cascade_bgl = make_bgl(dev, {buffer_entry(0, WGPUShaderStage_Vertex, WGPUBufferBindingType_Uniform, true, 64)});
  I.leaf_bgl = make_bgl(dev, {texture_entry(0, WGPUTextureSampleType_Float, WGPUTextureViewDimension_2D),
                              sampler_entry(1, WGPUSamplerBindingType_Filtering)});
  I.ssao_bgl = make_bgl(dev, {texture_entry(0, WGPUTextureSampleType_Depth, WGPUTextureViewDimension_2D),
                              texture_entry(1, WGPUTextureSampleType_Float, WGPUTextureViewDimension_2D),
                              sampler_entry(2, WGPUSamplerBindingType_Filtering),
                              texture_entry(3, WGPUTextureSampleType_Float, WGPUTextureViewDimension_2D)});
  I.sky_bgl = make_bgl(dev, {texture_entry(0, WGPUTextureSampleType_Float, WGPUTextureViewDimension_Cube),
                             sampler_entry(1, WGPUSamplerBindingType_Filtering)});
  I.post_bgl = make_bgl(dev, {buffer_entry(0, WGPUShaderStage_Fragment, WGPUBufferBindingType_Uniform, true, sizeof(PostParams)),
                              texture_entry(1, WGPUTextureSampleType_Float, WGPUTextureViewDimension_2D),
                              texture_entry(2, WGPUTextureSampleType_Float, WGPUTextureViewDimension_2D),
                              sampler_entry(3, WGPUSamplerBindingType_Filtering)});
  I.main_layout = make_layout(dev, {I.frame_bgl, I.scene_tex_bgl});
  I.shadow_layout = make_layout(dev, {I.cascade_bgl, I.leaf_bgl});
  I.prepass_layout = make_layout(dev, {I.frame_bgl, I.leaf_bgl});
  I.ssao_layout = make_layout(dev, {I.frame_bgl, I.ssao_bgl});
  I.sky_layout = make_layout(dev, {I.frame_bgl, I.sky_bgl});
  I.post_layout = make_layout(dev, {I.post_bgl});

  // ---- shaders + pipelines -------------------------------------------------------
  auto shader = [&](const char* name) -> WGPUShaderModule {
    WGPUShaderModule m = gpu->load_shader(shader_dir + "/" + name, error);
    return m;
  };
  WGPUShaderModule sm_shadow = shader("shadow.wgsl"), sm_pre = shader("prepass.wgsl"), sm_ssao = shader("ssao.wgsl"),
                   sm_sky = shader("sky.wgsl"), sm_main = shader("main.wgsl"), sm_post = shader("post.wgsl");
  if (!error->empty()) return false;
  for (WGPUShaderModule m : {sm_shadow, sm_pre, sm_ssao, sm_sky, sm_main, sm_post}) {
    if (m == nullptr) {
      *error = "shader module creation failed (see wgpu errors above)";
      return false;
    }
  }
  const WGPUTextureFormat hdr_fmt = WGPUTextureFormat_RGBA16Float;
  const WGPUTextureFormat ldr_fmt = WGPUTextureFormat_RGBA8Unorm;
  {
    PipelineOpts o;
    o.module = sm_shadow; o.layout = I.shadow_layout; o.depth = WGPUTextureFormat_Depth32Float; o.cull = WGPUCullMode_None;
    I.p_shadow = make_pipeline(dev, o);
    o.fs = "fs_foliage";
    I.p_shadow_foliage = make_pipeline(dev, o);
  }
  {
    PipelineOpts o;
    o.module = sm_pre; o.layout = I.prepass_layout; o.fs = "fs_main"; o.color = hdr_fmt; o.depth = WGPUTextureFormat_Depth32Float;
    I.p_prepass = make_pipeline(dev, o);
    o.fs = "fs_foliage"; o.cull = WGPUCullMode_None;
    I.p_prepass_foliage = make_pipeline(dev, o);
  }
  {
    PipelineOpts o;
    o.module = sm_ssao; o.layout = I.ssao_layout; o.vs = "vs_fullscreen"; o.fs = "fs_ssao"; o.color = WGPUTextureFormat_R8Unorm;
    o.vertices = false; o.cull = WGPUCullMode_None;
    I.p_ssao = make_pipeline(dev, o);
    o.fs = "fs_blur";
    I.p_blur = make_pipeline(dev, o);
  }
  {
    PipelineOpts o;
    o.module = sm_sky; o.layout = I.sky_layout; o.vs = "vs_fullscreen"; o.fs = "fs_sky"; o.color = hdr_fmt;
    o.depth = WGPUTextureFormat_Depth32Float; o.depth_write = false; o.depth_compare = WGPUCompareFunction_Always;
    o.samples = settings_.msaa; o.vertices = false; o.cull = WGPUCullMode_None;
    I.p_sky = make_pipeline(dev, o);
  }
  {
    PipelineOpts o;
    o.module = sm_main; o.layout = I.main_layout; o.fs = "fs_main"; o.color = hdr_fmt; o.depth = WGPUTextureFormat_Depth32Float;
    o.samples = settings_.msaa;
    I.p_main = make_pipeline(dev, o);
    o.cull = WGPUCullMode_None; o.alpha_to_coverage = settings_.msaa > 1;
    I.p_foliage = make_pipeline(dev, o);
  }
  {
    PipelineOpts o;
    o.module = sm_post; o.layout = I.post_layout; o.vs = "vs_fullscreen"; o.vertices = false; o.cull = WGPUCullMode_None;
    o.fs = "fs_down"; o.color = hdr_fmt; I.p_down = make_pipeline(dev, o);
    o.fs = "fs_up"; I.p_up = make_pipeline(dev, o);
    o.fs = "fs_tonemap"; o.color = ldr_fmt; I.p_tonemap = make_pipeline(dev, o);
    o.fs = "fs_fxaa"; I.p_fxaa = make_pipeline(dev, o);
    o.fs = "fs_blit"; I.p_copy = make_pipeline(dev, o);  // LDR → LDR copy
    o.color = gpu->surface_format; I.p_blit = make_pipeline(dev, o);
  }
  for (WGPUShaderModule m : {sm_shadow, sm_pre, sm_ssao, sm_sky, sm_main, sm_post}) wgpuShaderModuleRelease(m);

  // ---- buffers, samplers, static textures ----------------------------------------
  I.frame_buf = gpu->create_buffer(WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst, sizeof(FrameUniform), nullptr, "frame");
  I.cascade_buf = gpu->create_buffer(WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst, kUniformStride * kCascades, nullptr, "cascades");
  I.post_slots = 4 + 2 * kBloomLevels + 4;
  I.post_buf = gpu->create_buffer(WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst, kUniformStride * I.post_slots, nullptr, "post");
  I.mat_samp = gpu->create_sampler(WGPUAddressMode_Repeat, WGPUFilterMode_Linear, true, 8, WGPUCompareFunction_Undefined, "material");
  I.cube_samp = gpu->create_sampler(WGPUAddressMode_ClampToEdge, WGPUFilterMode_Linear, true, 1, WGPUCompareFunction_Undefined, "cube");
  I.shadow_samp = gpu->create_sampler(WGPUAddressMode_ClampToEdge, WGPUFilterMode_Linear, false, 1, WGPUCompareFunction_LessEqual, "shadow");
  I.clamp_samp = gpu->create_sampler(WGPUAddressMode_ClampToEdge, WGPUFilterMode_Linear, false, 1, WGPUCompareFunction_Undefined, "clamp");
  I.shadow_maps = gpu->create_texture(settings_.shadow_size, settings_.shadow_size, WGPUTextureFormat_Depth32Float,
                                      WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding, 1, kCascades, 1, "shadow-maps");
  for (std::uint32_t i = 0; i < kCascades; ++i) {
    I.shadow_layer_views.push_back(gpu->create_view(I.shadow_maps, 0, 1, i, 1, WGPUTextureViewDimension_2D));
  }
  {
    const std::uint32_t ls = 512;
    std::vector<std::uint8_t> leaf = make_leaf_texture(ls, 7);
    std::uint32_t mips = 1;
    while ((ls >> mips) >= 1) ++mips;
    I.leaf = gpu->create_texture(ls, ls, WGPUTextureFormat_RGBA8UnormSrgb, WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst, mips, 1, 1, "leaf");
    gpu->upload_rgba8_mips(I.leaf, 0, leaf.data());
    I.leaf_bg = make_bg(dev, I.leaf_bgl, {bg_texture(0, I.leaf.view), bg_sampler(1, I.mat_samp)});
  }
  I.cascade_bg = make_bg(dev, I.cascade_bgl, {bg_buffer(0, I.cascade_buf, 64)});
  resize(gpu->width, gpu->height);
  return true;
}

void Renderer::shutdown() {
  if (impl_ == nullptr) return;
  Impl& I = *impl_;
  I.release_targets();
  I.opaque.release();
  I.foliage.release();
  for (WGPUTextureView v : I.shadow_layer_views) wgpuTextureViewRelease(v);
  I.shadow_maps.release();
  I.leaf.release();
  for (WGPUBindGroup* g : {&I.frame_bg, &I.cascade_bg, &I.leaf_bg, &I.sky_bg}) {
    if (*g != nullptr) wgpuBindGroupRelease(*g);
  }
  for (WGPUBuffer* b : {&I.frame_buf, &I.cascade_buf, &I.material_buf, &I.light_buf, &I.post_buf}) {
    if (*b != nullptr) wgpuBufferRelease(*b);
  }
  for (WGPUSampler* s : {&I.mat_samp, &I.cube_samp, &I.shadow_samp, &I.clamp_samp}) {
    if (*s != nullptr) wgpuSamplerRelease(*s);
  }
  for (WGPURenderPipeline* p : {&I.p_shadow, &I.p_shadow_foliage, &I.p_prepass, &I.p_prepass_foliage, &I.p_ssao, &I.p_blur,
                                &I.p_sky, &I.p_main, &I.p_foliage, &I.p_down, &I.p_up, &I.p_tonemap, &I.p_fxaa, &I.p_blit, &I.p_copy}) {
    if (*p != nullptr) wgpuRenderPipelineRelease(*p);
  }
  for (WGPUPipelineLayout* l : {&I.main_layout, &I.shadow_layout, &I.prepass_layout, &I.ssao_layout, &I.sky_layout, &I.post_layout}) {
    if (*l != nullptr) wgpuPipelineLayoutRelease(*l);
  }
  for (WGPUBindGroupLayout* l : {&I.frame_bgl, &I.scene_tex_bgl, &I.cascade_bgl, &I.leaf_bgl, &I.ssao_bgl, &I.sky_bgl, &I.post_bgl}) {
    if (*l != nullptr) wgpuBindGroupLayoutRelease(*l);
  }
  delete impl_;
  impl_ = nullptr;
}

void Renderer::set_scene(const Scene& scene, const MaterialArrays& arrays) {
  Impl& I = *impl_;
  I.arrays = &arrays;
  I.opaque.release();
  I.foliage.release();
  auto upload = [&](const Mesh& m, MeshBuffers* out, const char* label) {
    if (m.indices.empty()) return;
    out->vertices = gpu_->create_buffer(WGPUBufferUsage_Vertex, m.vertices.size() * sizeof(Vertex), m.vertices.data(), label);
    out->indices = gpu_->create_buffer(WGPUBufferUsage_Index, m.indices.size() * 4, m.indices.data(), label);
    out->index_count = static_cast<std::uint32_t>(m.indices.size());
  };
  upload(scene.opaque, &I.opaque, "opaque");
  upload(scene.foliage, &I.foliage, "foliage");
  triangles_ = static_cast<std::uint32_t>((scene.opaque.indices.size() + scene.foliage.indices.size()) / 3);
  // materials
  std::vector<GpuMaterial> mats;
  for (const MaterialDesc& d : scene.materials) {
    GpuMaterial g;
    g.base_color = Vec4{d.base_color, 0.5f};
    g.params = Vec4{d.roughness, d.metallic, d.emissive, d.normal_strength};
    const int layer = d.albedo_set.empty() ? -1 : arrays.layer_of(d.albedo_set);
    g.tex = Vec4{static_cast<float>(layer), static_cast<float>(layer), static_cast<float>(layer), d.uv_scale};
    g.misc = Vec4{static_cast<float>(d.flags), d.tint2.x, d.tint2.y, d.tint2.z};
    g.room = Vec4{d.room_w, d.room_h, d.room_d, d.lit_probability};
    mats.push_back(g);
  }
  if (mats.empty()) mats.push_back(GpuMaterial{});
  if (I.material_buf != nullptr) wgpuBufferRelease(I.material_buf);
  I.material_buf = gpu_->create_buffer(WGPUBufferUsage_Storage, mats.size() * sizeof(GpuMaterial), mats.data(), "materials");
  I.material_count = static_cast<std::uint32_t>(mats.size());
  std::vector<GpuLight> lights;
  for (const PointLight& l : scene.lights) {
    lights.push_back(GpuLight{Vec4{l.position, l.radius}, Vec4{l.color, l.intensity}});
  }
  if (lights.empty()) lights.push_back(GpuLight{});
  if (I.light_buf != nullptr) wgpuBufferRelease(I.light_buf);
  I.light_buf = gpu_->create_buffer(WGPUBufferUsage_Storage, lights.size() * sizeof(GpuLight), lights.data(), "lights");
  I.light_count = static_cast<std::uint32_t>(scene.lights.size());
  if (I.frame_bg != nullptr) wgpuBindGroupRelease(I.frame_bg);
  I.frame_bg = make_bg(gpu_->device, I.frame_bgl, {bg_buffer(0, I.frame_buf, sizeof(FrameUniform)),
                                                   bg_buffer(1, I.material_buf, mats.size() * sizeof(GpuMaterial)),
                                                   bg_buffer(2, I.light_buf, lights.size() * sizeof(GpuLight))});
  I.have_scene = true;
  I.scene_dirty = true;
}

void Renderer::set_environment(const Environment* env, bool night) {
  Impl& I = *impl_;
  I.env = env;
  I.night = night;
  I.scene_dirty = true;
}

void Renderer::resize(std::uint32_t w, std::uint32_t h) {
  Impl& I = *impl_;
  if (w == 0 || h == 0) return;
  I.release_targets();
  I.w = w;
  I.h = h;
  const WGPUTextureUsage rt = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding;
  I.depth_pre = gpu_->create_texture(w, h, WGPUTextureFormat_Depth32Float, rt, 1, 1, 1, "depth-pre");
  I.normal_pre = gpu_->create_texture(w, h, WGPUTextureFormat_RGBA16Float, rt, 1, 1, 1, "normal-pre");
  I.ao_a = gpu_->create_texture(w, h, WGPUTextureFormat_R8Unorm, rt, 1, 1, 1, "ao-a");
  I.ao_b = gpu_->create_texture(w, h, WGPUTextureFormat_R8Unorm, rt, 1, 1, 1, "ao-b");
  if (settings_.msaa > 1) {
    I.hdr_msaa = gpu_->create_texture(w, h, WGPUTextureFormat_RGBA16Float, WGPUTextureUsage_RenderAttachment, 1, 1, settings_.msaa, "hdr-msaa");
    I.depth_msaa = gpu_->create_texture(w, h, WGPUTextureFormat_Depth32Float, WGPUTextureUsage_RenderAttachment, 1, 1, settings_.msaa, "depth-msaa");
  } else {
    I.depth_msaa = gpu_->create_texture(w, h, WGPUTextureFormat_Depth32Float, WGPUTextureUsage_RenderAttachment, 1, 1, 1, "depth-1x");
  }
  I.hdr = gpu_->create_texture(w, h, WGPUTextureFormat_RGBA16Float, rt, 1, 1, 1, "hdr");
  I.ldr_a = gpu_->create_texture(w, h, WGPUTextureFormat_RGBA8Unorm, rt | WGPUTextureUsage_CopySrc, 1, 1, 1, "ldr-a");
  I.ldr_b = gpu_->create_texture(w, h, WGPUTextureFormat_RGBA8Unorm, rt | WGPUTextureUsage_CopySrc, 1, 1, 1, "ldr-b");
  std::uint32_t bw = w, bh = h;
  for (std::uint32_t i = 0; i < kBloomLevels; ++i) {
    bw = std::max(1u, bw / 2);
    bh = std::max(1u, bh / 2);
    I.bloom.push_back(gpu_->create_texture(bw, bh, WGPUTextureFormat_RGBA16Float, rt, 1, 1, 1, "bloom"));
    if (i + 1 < kBloomLevels) I.bloom_up.push_back(gpu_->create_texture(bw, bh, WGPUTextureFormat_RGBA16Float, rt, 1, 1, 1, "bloom-up"));
  }
  WGPUDevice dev = gpu_->device;
  I.ssao_bg = make_bg(dev, I.ssao_bgl, {bg_texture(0, I.depth_pre.view), bg_texture(1, I.normal_pre.view), bg_sampler(2, I.clamp_samp), bg_texture(3, I.ao_b.view)});
  I.blur_bg = make_bg(dev, I.ssao_bgl, {bg_texture(0, I.depth_pre.view), bg_texture(1, I.normal_pre.view), bg_sampler(2, I.clamp_samp), bg_texture(3, I.ao_a.view)});
  // post bind groups: slot layout in post_buf: 0 tonemap, 1 fxaa, 2 blit, 3 spare, 4.. down[i], 4+L.. up[i]
  auto post_bg = [&](WGPUTextureView a, WGPUTextureView b) {
    return make_bg(dev, I.post_bgl, {bg_buffer(0, I.post_buf, sizeof(PostParams)), bg_texture(1, a), bg_texture(2, b), bg_sampler(3, I.clamp_samp)});
  };
  for (std::uint32_t i = 0; i < kBloomLevels; ++i) {
    WGPUTextureView src = i == 0 ? I.hdr.view : I.bloom[i - 1].view;
    I.down_bg.push_back(post_bg(src, src));
  }
  for (std::uint32_t i = 0; i < kBloomLevels - 1; ++i) {
    // up[i]: from the level below (down[L-1] first, then up[hi+1]) into up[hi], adding down[hi]
    const std::uint32_t hi = kBloomLevels - 2 - i;
    WGPUTextureView lower = i == 0 ? I.bloom[kBloomLevels - 1].view : I.bloom_up[hi + 1].view;
    I.up_bg.push_back(post_bg(lower, I.bloom[hi].view));
  }
  I.tonemap_bg = post_bg(I.hdr.view, I.bloom_up[0].view);
  I.fxaa_bg = post_bg(I.ldr_a.view, I.ldr_a.view);
  I.blit_bg = post_bg(I.ldr_b.view, I.ldr_b.view);
  I.dbg_ao_bg = post_bg(I.ao_b.view, I.ao_b.view);
  I.dbg_normal_bg = post_bg(I.normal_pre.view, I.normal_pre.view);
  I.scene_dirty = true;
}

namespace {

// Fit an orthographic light frustum around a camera frustum slice.
void fit_cascade(const Camera& cam, float aspect, float zn, float zf, Vec3 sun_dir, std::uint32_t shadow_size,
                 Mat4* out_vp, float* out_extent, float* out_depth) {
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
  for (const Vec3& c : corners) centre += c;
  centre *= 1.0f / 8.0f;
  float radius = 0.0f;
  for (const Vec3& c : corners) radius = std::max(radius, length(c - centre));
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

}  // namespace

void Renderer::render(const Camera& camera, float time_s, WGPUTextureView target) {
  Impl& I = *impl_;
  if (!I.have_scene || I.env == nullptr) return;
  WGPUDevice dev = gpu_->device;
  if (I.scene_tex_bg == nullptr || I.scene_dirty) {
    if (I.scene_tex_bg != nullptr) wgpuBindGroupRelease(I.scene_tex_bg);
    I.scene_tex_bg = make_bg(dev, I.scene_tex_bgl, {bg_texture(0, I.arrays->albedo.view), bg_texture(1, I.arrays->normal.view),
                                                  bg_texture(2, I.arrays->arm.view), bg_sampler(3, I.mat_samp),
                                                  bg_texture(4, I.env->specular.view), bg_texture(5, I.env->background.view),
                                                  bg_sampler(6, I.cube_samp), bg_texture(7, I.shadow_maps.view),
                                                  bg_sampler(8, I.shadow_samp), bg_texture(9, I.ao_b.view),
                                                  bg_sampler(10, I.clamp_samp), bg_texture(11, I.leaf.view)});
    if (I.sky_bg != nullptr) wgpuBindGroupRelease(I.sky_bg);
    I.sky_bg = make_bg(dev, I.sky_bgl, {bg_texture(0, I.env->background.view), bg_sampler(1, I.cube_samp)});
    I.scene_dirty = false;
  }

  // ---- frame uniforms ---------------------------------------------------------
  const float aspect = static_cast<float>(I.w) / static_cast<float>(I.h);
  const float zn = 0.3f, zf = 4000.0f;
  FrameUniform fu{};
  fu.view = camera.view();
  fu.proj = perspective(camera.fov_y, aspect, zn, zf);
  fu.view_proj = mul(fu.proj, fu.view);
  fu.inv_view_proj = inverse(fu.view_proj);
  fu.inv_proj = inverse(fu.proj);
  const float splits[kCascades + 1] = {zn, 28.0f, 110.0f, 520.0f};
  Vec3 extents{0, 0, 0}, depths{0, 0, 0};
  const Vec3 sun_dir = I.env->has_sun ? I.env->sun_dir : Vec3{0.3f, 0.8f, 0.5f};
  for (std::uint32_t i = 0; i < kCascades; ++i) {
    float ext = 0.0f, dep = 0.0f;
    fit_cascade(camera, aspect, splits[i], splits[i + 1], sun_dir, settings_.shadow_size, &fu.shadow[i], &ext, &dep);
    if (i == 0) { extents.x = ext; depths.x = dep; } else if (i == 1) { extents.y = ext; depths.y = dep; } else { extents.z = ext; depths.z = dep; }
    gpu_->write_buffer(I.cascade_buf, i * kUniformStride, &fu.shadow[i], sizeof(Mat4));
  }
  fu.camera_pos = Vec4{camera.position, time_s};
  fu.sun_dir = Vec4{sun_dir, (I.env->has_sun && settings_.shadows) ? 1.0f : (I.env->has_sun ? 1.0f : 0.0f)};
  const float exposure = I.env->exposure * std::exp2(settings_.exposure_bias);
  fu.sun_color = Vec4{I.env->sun_color, exposure};
  for (int i = 0; i < 9; ++i) fu.sh[i] = Vec4{I.env->sh[i][0], I.env->sh[i][1], I.env->sh[i][2], 0.0f};
  fu.cascade = Vec4{splits[1], splits[2], splits[3], 1.0f / static_cast<float>(settings_.shadow_size)};
  fu.cascade_extent = Vec4{extents, 0.0f};
  fu.cascade_depth = Vec4{depths, 0.0f};
  fu.screen = Vec4{static_cast<float>(I.w), static_cast<float>(I.h), 1.0f / static_cast<float>(I.w), 1.0f / static_cast<float>(I.h)};
  const float emissive_scale = 0.9f / exposure;
  fu.params = Vec4{emissive_scale, 2.0f, I.night ? 1.0f : 0.0f, I.night ? static_cast<float>(I.light_count) : 0.0f};
  fu.params2 = Vec4{1.0f, 1.0f, settings_.ssao ? 1.0f : 0.0f, settings_.shadows ? static_cast<float>(settings_.debug_view) : (settings_.debug_view > 0 ? static_cast<float>(settings_.debug_view) : -1.0f)};
  gpu_->write_buffer(I.frame_buf, 0, &fu, sizeof(fu));
  // post params
  {
    PostParams pp{};
    pp.a = Vec4{exposure, settings_.bloom ? 1.0f : 0.0f, 0.0f, 0.0f};  // tonemap: exposure, bloom strength
    gpu_->write_buffer(I.post_buf, 0, &pp, sizeof(pp));
    pp.a = Vec4{1.0f / static_cast<float>(I.w), 1.0f / static_cast<float>(I.h), 0.0f, 0.0f};  // fxaa texel
    gpu_->write_buffer(I.post_buf, kUniformStride, &pp, sizeof(pp));
    pp.a = Vec4{gpu_->surface_srgb ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f};  // blit
    gpu_->write_buffer(I.post_buf, 2 * kUniformStride, &pp, sizeof(pp));
    std::uint32_t bw = I.w, bh = I.h;
    for (std::uint32_t i = 0; i < kBloomLevels; ++i) {
      pp.a = Vec4{1.0f / static_cast<float>(bw), 1.0f / static_cast<float>(bh), i == 0 ? 1.0f : 0.0f, 0.0f};
      pp.b = Vec4{1.0f, 0.5f, 12.0f, 0.0f};  // threshold (post-exposure units handled below), knee, clamp
      // threshold is in HDR units: express relative to exposure
      pp.b.x = 1.0f / exposure;
      pp.b.z = 40.0f / exposure;
      gpu_->write_buffer(I.post_buf, (4 + i) * kUniformStride, &pp, sizeof(pp));
      bw = std::max(1u, bw / 2);
      bh = std::max(1u, bh / 2);
    }
    for (std::uint32_t i = 0; i < kBloomLevels - 1; ++i) {
      const Texture& lo = I.bloom[kBloomLevels - 1 - i];
      pp.a = Vec4{1.0f / static_cast<float>(lo.width), 1.0f / static_cast<float>(lo.height), 0.0f, 0.65f};
      gpu_->write_buffer(I.post_buf, (4 + kBloomLevels + i) * kUniformStride, &pp, sizeof(pp));
    }
  }

  WGPUCommandEncoderDescriptor ed{};
  WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(dev, &ed);
  auto draw_mesh = [&](WGPURenderPassEncoder pass, const MeshBuffers& m) {
    if (m.index_count == 0) return;
    wgpuRenderPassEncoderSetVertexBuffer(pass, 0, m.vertices, 0, WGPU_WHOLE_SIZE);
    wgpuRenderPassEncoderSetIndexBuffer(pass, m.indices, WGPUIndexFormat_Uint32, 0, WGPU_WHOLE_SIZE);
    wgpuRenderPassEncoderDrawIndexed(pass, m.index_count, 1, 0, 0, 0);
  };
  auto depth_attachment = [](WGPUTextureView v, bool clear) {
    WGPURenderPassDepthStencilAttachment a{};
    a.view = v;
    a.depthLoadOp = clear ? WGPULoadOp_Clear : WGPULoadOp_Load;
    a.depthStoreOp = WGPUStoreOp_Store;
    a.depthClearValue = 1.0f;
    return a;
  };
  auto color_attachment = [](WGPUTextureView v, WGPUTextureView resolve, bool clear) {
    WGPURenderPassColorAttachment a{};
    a.view = v;
    a.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
    a.resolveTarget = resolve;
    a.loadOp = clear ? WGPULoadOp_Clear : WGPULoadOp_Load;
    a.storeOp = WGPUStoreOp_Store;
    a.clearValue = WGPUColor{0.0, 0.0, 0.0, 1.0};
    return a;
  };

  // ---- shadow cascades ----------------------------------------------------------
  if (settings_.shadows && I.env->has_sun) {
    for (std::uint32_t c = 0; c < kCascades; ++c) {
      WGPURenderPassDepthStencilAttachment da = depth_attachment(I.shadow_layer_views[c], true);
      WGPURenderPassDescriptor rp{};
      rp.depthStencilAttachment = &da;
      WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(enc, &rp);
      const std::uint32_t off = c * kUniformStride;
      wgpuRenderPassEncoderSetBindGroup(pass, 0, I.cascade_bg, 1, &off);
      wgpuRenderPassEncoderSetBindGroup(pass, 1, I.leaf_bg, 0, nullptr);
      wgpuRenderPassEncoderSetPipeline(pass, I.p_shadow);
      draw_mesh(pass, I.opaque);
      wgpuRenderPassEncoderSetPipeline(pass, I.p_shadow_foliage);
      draw_mesh(pass, I.foliage);
      wgpuRenderPassEncoderEnd(pass);
      wgpuRenderPassEncoderRelease(pass);
    }
  }
  // ---- prepass (depth + view normal) ------------------------------------------------
  if (settings_.ssao) {
    WGPURenderPassColorAttachment ca = color_attachment(I.normal_pre.view, nullptr, true);
    WGPURenderPassDepthStencilAttachment da = depth_attachment(I.depth_pre.view, true);
    WGPURenderPassDescriptor rp{};
    rp.colorAttachmentCount = 1;
    rp.colorAttachments = &ca;
    rp.depthStencilAttachment = &da;
    WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(enc, &rp);
    wgpuRenderPassEncoderSetBindGroup(pass, 0, I.frame_bg, 0, nullptr);
    wgpuRenderPassEncoderSetBindGroup(pass, 1, I.leaf_bg, 0, nullptr);
    wgpuRenderPassEncoderSetPipeline(pass, I.p_prepass);
    draw_mesh(pass, I.opaque);
    wgpuRenderPassEncoderSetPipeline(pass, I.p_prepass_foliage);
    draw_mesh(pass, I.foliage);
    wgpuRenderPassEncoderEnd(pass);
    wgpuRenderPassEncoderRelease(pass);
    // SSAO + blur
    auto fullscreen = [&](WGPURenderPipeline p, WGPUBindGroup g1, WGPUTextureView out) {
      WGPURenderPassColorAttachment ca2 = color_attachment(out, nullptr, true);
      WGPURenderPassDescriptor rp2{};
      rp2.colorAttachmentCount = 1;
      rp2.colorAttachments = &ca2;
      WGPURenderPassEncoder p2 = wgpuCommandEncoderBeginRenderPass(enc, &rp2);
      wgpuRenderPassEncoderSetBindGroup(p2, 0, I.frame_bg, 0, nullptr);
      wgpuRenderPassEncoderSetBindGroup(p2, 1, g1, 0, nullptr);
      wgpuRenderPassEncoderSetPipeline(p2, p);
      wgpuRenderPassEncoderDraw(p2, 3, 1, 0, 0);
      wgpuRenderPassEncoderEnd(p2);
      wgpuRenderPassEncoderRelease(p2);
    };
    fullscreen(I.p_ssao, I.ssao_bg, I.ao_a.view);
    fullscreen(I.p_blur, I.blur_bg, I.ao_b.view);
  }
  // ---- main pass ------------------------------------------------------------------
  {
    const bool msaa = settings_.msaa > 1;
    WGPURenderPassColorAttachment ca = color_attachment(msaa ? I.hdr_msaa.view : I.hdr.view, msaa ? I.hdr.view : nullptr, true);
    WGPURenderPassDepthStencilAttachment da = depth_attachment(I.depth_msaa.view, true);
    WGPURenderPassDescriptor rp{};
    rp.colorAttachmentCount = 1;
    rp.colorAttachments = &ca;
    rp.depthStencilAttachment = &da;
    WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(enc, &rp);
    wgpuRenderPassEncoderSetBindGroup(pass, 0, I.frame_bg, 0, nullptr);
    wgpuRenderPassEncoderSetBindGroup(pass, 1, I.sky_bg, 0, nullptr);
    wgpuRenderPassEncoderSetPipeline(pass, I.p_sky);
    wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
    wgpuRenderPassEncoderSetBindGroup(pass, 1, I.scene_tex_bg, 0, nullptr);
    wgpuRenderPassEncoderSetPipeline(pass, I.p_main);
    draw_mesh(pass, I.opaque);
    wgpuRenderPassEncoderSetPipeline(pass, I.p_foliage);
    draw_mesh(pass, I.foliage);
    wgpuRenderPassEncoderEnd(pass);
    wgpuRenderPassEncoderRelease(pass);
  }
  // ---- post: bloom, tonemap, fxaa, blit ------------------------------------------------
  auto post = [&](WGPURenderPipeline p, WGPUBindGroup g, std::uint32_t slot, WGPUTextureView out) {
    WGPURenderPassColorAttachment ca = color_attachment(out, nullptr, true);
    WGPURenderPassDescriptor rp{};
    rp.colorAttachmentCount = 1;
    rp.colorAttachments = &ca;
    WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(enc, &rp);
    const std::uint32_t off = slot * kUniformStride;
    wgpuRenderPassEncoderSetBindGroup(pass, 0, g, 1, &off);
    wgpuRenderPassEncoderSetPipeline(pass, p);
    wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
    wgpuRenderPassEncoderEnd(pass);
    wgpuRenderPassEncoderRelease(pass);
  };
  if (settings_.bloom) {
    for (std::uint32_t i = 0; i < kBloomLevels; ++i) post(I.p_down, I.down_bg[i], 4 + i, I.bloom[i].view);
    for (std::uint32_t i = 0; i < kBloomLevels - 1; ++i) {
      const std::uint32_t hi = kBloomLevels - 2 - i;
      post(I.p_up, I.up_bg[i], 4 + kBloomLevels + i, I.bloom_up[hi].view);
    }
  }
  post(I.p_tonemap, I.tonemap_bg, 0, I.ldr_a.view);
  if (settings_.fxaa) {
    post(I.p_fxaa, I.fxaa_bg, 1, I.ldr_b.view);
  } else {
    post(I.p_copy, I.fxaa_bg, 3, I.ldr_b.view);  // slot 3: plain copy (a.x = 0)
  }
  if (settings_.debug_view == 10) post(I.p_copy, I.dbg_ao_bg, 3, I.ldr_b.view);
  if (settings_.debug_view == 11) post(I.p_copy, I.dbg_normal_bg, 3, I.ldr_b.view);
  if (target != nullptr) post(I.p_blit, I.blit_bg, 2, target);

  WGPUCommandBufferDescriptor cd{};
  WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, &cd);
  wgpuQueueSubmit(gpu_->queue, 1, &cmd);
  wgpuCommandBufferRelease(cmd);
  wgpuCommandEncoderRelease(enc);
}

bool Renderer::capture_png(const std::string& path) {
  Impl& I = *impl_;
  std::vector<std::uint8_t> rgba;
  if (!gpu_->read_rgba8(I.ldr_b, &rgba)) return false;
  for (std::size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;
  return stbi_write_png(path.c_str(), static_cast<int>(I.ldr_b.width), static_cast<int>(I.ldr_b.height), 4, rgba.data(),
                        static_cast<int>(I.ldr_b.width * 4)) != 0;
}

}  // namespace cb
