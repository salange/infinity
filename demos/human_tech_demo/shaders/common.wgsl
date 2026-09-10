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
  cascade_extent: vec4<f32>, // xyz ortho half-extent per cascade (m), w jitter x (px)
  cascade_depth: vec4<f32>,  // xyz light-space depth range per cascade (m), w jitter y (px)
  screen: vec4<f32>,       // w, h, 1/w, 1/h
  params: vec4<f32>,       // emissive_scale, ao_strength, night, light_count
  params2: vec4<f32>,      // ibl_intensity, sun_intensity, ssao_on, debug
  reflection_vp: mat4x4<f32>,
  puddle_reflection_vp: mat4x4<f32>,
  pond_reflection_vp: mat4x4<f32>,
  transport: vec4<f32>,
  voxel_min: vec4<f32>,
  voxel_extent: vec4<f32>,
  fine_min: vec4<f32>,
  fine_extent: vec4<f32>,
  moon_dir: vec4<f32>, // xyz direction, w visible disc
  moon_color: vec4<f32>, // xyz weak irradiance, w visible primary sun
  moon_shape: vec4<f32>, // illumination x/z, angular radius, display-referred sky
  point_visibility: vec4<f32>, // x independent practical shadow enablement
};

struct Instance { model: mat4x4<f32>, normal0: vec4<f32>, normal1: vec4<f32>, normal2: vec4<f32>, tint: vec4<f32> };
fn instance_normal(i: Instance,n: vec3<f32>) -> vec3<f32> {return normalize(i.normal0.xyz*n.x+i.normal1.xyz*n.y+i.normal2.xyz*n.z);}

struct Material {
  base_color: vec4<f32>,  // rgb tint (occupied glass: one-pass transmission), a = alpha cutoff
  params: vec4<f32>,      // roughness, metallic (occupied glass: coating coverage), emissive, normal_strength
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
const FLAG_WATER: u32 = 64u;
const FLAG_TRANSMISSION: u32 = 128u;
const FLAG_WETTABLE: u32 = 512u;
const FLAG_PLANAR_POND: u32 = 1024u; // true shallow surface at the explicit pond plane
const FLAG_EXPOSED_STONE: u32 = 2048u; // rain-exposed stone with height-map water retention
const PI: f32 = 3.14159265358979;

// The sign carries shadow enablement; the magnitude independently encodes
// every diagnostic view, including direct-light components without shadows.
fn debug_view(encoded: f32) -> i32 {
  return i32(select(encoded,-encoded-1.0,encoded<0.0)+.5);
}

// Packed 32-byte vertex: position f32x3, octahedral normal snorm16x2,
// octahedral tangent snorm16x2, uv f16x2, packed bytes (material lo, hi,
// element random, occlusion 7 bits + tangent sign bit), facade coords
// unorm16x2 in units of 655.35 m (1 cm steps).
struct VertexIn {
  @location(0) position: vec3<f32>,
  @location(1) normal_oct: vec2<f32>,
  @location(2) tangent_oct: vec2<f32>,
  @location(3) uv: vec2<f32>,
  @location(4) packed: vec4<u32>,
  @location(5) aux_packed: vec2<f32>,
};
fn oct_decode(e: vec2<f32>) -> vec3<f32> {
  var v = vec3<f32>(e.x, e.y, 1.0 - abs(e.x) - abs(e.y));
  if (v.z < 0.0) {
    let sx = select(-1.0, 1.0, v.x >= 0.0);
    let sy = select(-1.0, 1.0, v.y >= 0.0);
    v = vec3<f32>((1.0 - abs(v.y)) * sx, (1.0 - abs(v.x)) * sy, v.z);
  }
  return normalize(v);
}
struct Unpacked { normal: vec3<f32>, tangent: vec4<f32>, material: u32, aux: vec4<f32> };
fn unpack_vertex(in: VertexIn) -> Unpacked {
  var o: Unpacked;
  o.normal = oct_decode(in.normal_oct);
  let sign = select(1.0, -1.0, (in.packed.w & 128u) != 0u);
  o.tangent = vec4<f32>(oct_decode(in.tangent_oct), sign);
  o.material = in.packed.x | (in.packed.y << 8u);
  o.aux = vec4<f32>(in.aux_packed * 655.35, f32(in.packed.z) / 255.0, f32(in.packed.w & 127u) / 127.0);
  return o;
}

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

// Horizon radiance comes from atmospheric scattering, never a stretched row
// of bright stars. All camera, reflected-sky and surface-fog passes share it.
fn atmosphere_horizon(direction: vec3<f32>,sun_direction: vec3<f32>,night: f32) -> vec3<f32> {
  let horizontal=normalize(vec3<f32>(direction.x,.001,direction.z));
  let solar_horizontal=normalize(vec3<f32>(sun_direction.x,.001,sun_direction.z));
  let forward=pow(max(dot(horizontal,solar_horizontal),0.0),5.0);
  let daylight=mix(vec3<f32>(.19,.26,.35),vec3<f32>(.62,.43,.26),forward);
  let moonlight=vec3<f32>(.009,.017,.032)+vec3<f32>(.005,.007,.011)*forward;
  return mix(daylight,moonlight,night);
}
