// Shared declarations (textually included by the loader: `#include "common.wgsl"`).
struct Frame {
  view: mat4x4<f32>,
  proj: mat4x4<f32>,
  view_proj: mat4x4<f32>,
  inv_view_proj: mat4x4<f32>,
  inv_proj: mat4x4<f32>,
  shadow0: mat4x4<f32>,
  shadow1: mat4x4<f32>,
  shadow2: mat4x4<f32>,
  camera_pos: vec4<f32>,   // xyz, w = time (s)
  sun_dir: vec4<f32>,      // xyz toward the sun, w = has_sun
  sun_color: vec4<f32>,    // rgb irradiance, w = exposure
  sh: array<vec4<f32>, 9>, // irradiance SH (cosine-convolved)
  cascade: vec4<f32>,      // xyz split distances, w = 1 / shadow size
  cascade_extent: vec4<f32>, // xyz ortho half-extent per cascade (m)
  cascade_depth: vec4<f32>,  // xyz light-space depth range per cascade (m)
  screen: vec4<f32>,       // w, h, 1/w, 1/h
  params: vec4<f32>,       // emissive_scale, ao_strength, night, light_count
  params2: vec4<f32>,      // ibl_intensity, sun_intensity, ssao_on, debug
};

struct Material {
  base_color: vec4<f32>,  // rgb tint, a = alpha cutoff (foliage)
  params: vec4<f32>,      // roughness, metallic, emissive, normal_strength
  tex: vec4<f32>,         // albedo layer, normal layer, arm layer, uv_scale (m per repeat)
  misc: vec4<f32>,        // flags, tint2.rgb
  room: vec4<f32>,        // room w, h, d, lit probability
};

struct Light {
  pos_radius: vec4<f32>,
  color_int: vec4<f32>,
};

const FLAG_GLASS: u32 = 1u;
const FLAG_EMISSIVE: u32 = 2u;
const FLAG_PLANAR_XZ: u32 = 4u;
const FLAG_FOLIAGE: u32 = 8u;
const FLAG_TRIPLANAR: u32 = 16u;
const FLAG_NIGHT_ONLY: u32 = 32u;
const PI: f32 = 3.14159265358979;

struct VertexIn {
  @location(0) position: vec3<f32>,
  @location(1) normal: vec3<f32>,
  @location(2) tangent: vec4<f32>,
  @location(3) uv: vec2<f32>,
  @location(4) material: u32,
  @location(5) aux: vec4<f32>,
};

fn hash13(p: vec3<f32>) -> f32 {
  var q = fract(p * vec3<f32>(0.1031, 0.1030, 0.0973));
  q += dot(q, q.yzx + 33.33);
  return fract((q.x + q.y) * q.z);
}
fn hash33(p: vec3<f32>) -> vec3<f32> {
  var q = fract(p * vec3<f32>(0.1031, 0.1030, 0.0973));
  q += dot(q, q.yxz + 33.33);
  return fract((q.xxy + q.yxx) * q.zyx);
}
fn luminance(c: vec3<f32>) -> f32 { return dot(c, vec3<f32>(0.2126, 0.7152, 0.0722)); }
