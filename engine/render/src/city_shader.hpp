#pragma once

// The city surface shader (T0021): the cityblock demo's forward PBR
// path — GGX/Smith/Schlick with geometric specular anti-aliasing and
// Toksvig roughness, cascaded shadows with rotated Poisson PCF, SSAO,
// SH irradiance + prefiltered sky specular, up to 64 point lights,
// interior-mapped glass (rooms ray-cast behind every pane, lit per
// room, blinds, distance LOD, mullion grid lines), emissive and
// night-only materials, alpha-tested foliage — on the game's frame:
// positions are camera-relative (the camera sits at the origin), depth
// is reversed-Z, and textures come from the shared surface library
// (albedo.rgb + height.a; normal.xy + roughness.z + ao.w).
//
// Group 0: the item block + shared frame (as the terrain pipeline).
// Group 1: the city frame block, materials, lights, leaf texture,
//          shadow cascades, ambient occlusion, sky cube.
// Group 2: the shared surface material library.

namespace inf::render {

constexpr const char* kCityShader = R"(
struct Uniforms {
  mvp: mat4x4<f32>,
  color: vec4<f32>,
  aux: vec4<f32>,      // xyz = mesh origin relative to the camera
  extra: vec4<f32>,
  palette: vec4<f32>,
};
struct Frame {
  sun_dir: vec4<f32>,
  sun_color: vec4<f32>,
  cam_right: vec4<f32>,
  cam_up: vec4<f32>,
  cam_fwd: vec4<f32>,
  planet_up: vec4<f32>,
  atmo: vec4<f32>,
  planet_center: vec4<f32>,
  material: vec4<f32>,
};
struct CityFrame {
  view: mat4x4<f32>,
  proj: mat4x4<f32>,
  view_proj: mat4x4<f32>,
  inv_view_proj: mat4x4<f32>,
  inv_proj: mat4x4<f32>,
  shadow0: mat4x4<f32>,
  shadow1: mat4x4<f32>,
  shadow2: mat4x4<f32>,
  sh: array<vec4<f32>, 9>,   // irradiance SH (cosine-convolved)
  cascade: vec4<f32>,        // xyz split distances, w = 1 / shadow size
  cascade_extent: vec4<f32>, // xyz ortho half-extent per cascade (m)
  screen: vec4<f32>,         // w, h, 1/w, 1/h
  params: vec4<f32>,         // emissive_scale, ao_strength, night, light_count
  params2: vec4<f32>,        // ibl_intensity, sun_intensity, ssao_on, shadows_on
  jitter: vec4<f32>,         // xy = projection jitter in NDC units
};
struct Material {
  base_color: vec4<f32>,  // rgb tint, a = alpha cutoff (foliage)
  params: vec4<f32>,      // roughness, metallic, emissive, normal_strength
  tex: vec4<f32>,         // albedo layer, normal layer, arm layer, uv_scale (m per repeat)
  misc: vec4<f32>,        // flags, tint2.rgb
  room: vec4<f32>,        // room w, h, d, lit probability
};
struct Light {
  pos_radius: vec4<f32>,  // xyz camera-relative
  color_int: vec4<f32>,
};
struct MaterialTable {
  a: array<vec4<f32>, 64>,
  b: array<vec4<f32>, 64>,
  c: array<vec4<f32>, 64>,
};
@group(0) @binding(0) var<uniform> u: Uniforms;
@group(0) @binding(1) var<uniform> frame: Frame;
@group(1) @binding(0) var<uniform> cf: CityFrame;
@group(1) @binding(1) var<storage, read> materials: array<Material>;
@group(1) @binding(2) var<storage, read> lights: array<Light>;
@group(1) @binding(3) var leaf_tex: texture_2d<f32>;
@group(1) @binding(4) var leaf_samp: sampler;
@group(1) @binding(5) var shadow_tex: texture_depth_2d_array;
@group(1) @binding(6) var shadow_samp: sampler_comparison;
@group(1) @binding(7) var ao_tex: texture_2d<f32>;
@group(1) @binding(8) var clamp_samp: sampler;
@group(1) @binding(9) var spec_cube: texture_cube<f32>;
@group(1) @binding(10) var cube_samp: sampler;
@group(2) @binding(0) var mat_albedo: texture_2d_array<f32>;
@group(2) @binding(1) var mat_normal: texture_2d_array<f32>;
@group(2) @binding(2) var mat_sampler: sampler;
@group(2) @binding(3) var<uniform> mats: MaterialTable;

const FLAG_GLASS: u32 = 1u;
const FLAG_EMISSIVE: u32 = 2u;
const FLAG_PLANAR_XZ: u32 = 4u;
const FLAG_FOLIAGE: u32 = 8u;
const FLAG_TRIPLANAR: u32 = 16u;
const FLAG_NIGHT_ONLY: u32 = 32u;
const PI: f32 = 3.14159265358979;

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

struct VertexIn {
  @location(0) position: vec3<f32>,
  @location(1) normal: vec3<f32>,
  @location(2) tangent: vec4<f32>,
  @location(3) uv: vec2<f32>,
  @location(4) material: u32,
  @location(5) aux: vec4<f32>,
};
struct VOut {
  @builtin(position) pos: vec4<f32>,
  @location(0) world: vec3<f32>,     // camera-relative
  @location(1) normal: vec3<f32>,
  @location(2) tangent: vec4<f32>,
  @location(3) uv: vec2<f32>,
  @location(4) @interpolate(flat) material: u32,
  @location(5) aux: vec4<f32>,
  @location(6) local: vec3<f32>,     // mesh-local (stable texture space)
};

@vertex fn vs_main(in: VertexIn) -> VOut {
  var o: VOut;
  var clip = u.mvp * vec4<f32>(in.position, 1.0);
  clip.x += cf.jitter.x * clip.w;
  clip.y += cf.jitter.y * clip.w;
  o.pos = clip;
  o.world = in.position + u.aux.xyz;
  o.local = in.position + u.extra.xyz;  // origin modulo the tile period
  o.normal = in.normal;
  o.tangent = in.tangent;
  o.uv = in.uv;
  o.material = in.material;
  o.aux = in.aux;
  return o;
}

// Depth-only stage for the shadow cascades (the cascade matrix sits in
// group 1 with a dynamic offset; only the shadow pipeline binds it).
struct Cascade { light_vp: mat4x4<f32> };
@group(1) @binding(11) var<uniform> cascade: Cascade;
struct ShadowOut { @builtin(position) pos: vec4<f32>, @location(0) uv: vec2<f32>, @location(1) @interpolate(flat) material: u32 };
@vertex fn vs_shadow(in: VertexIn) -> ShadowOut {
  var o: ShadowOut;
  o.pos = cascade.light_vp * vec4<f32>(in.position + u.aux.xyz, 1.0);
  o.uv = in.uv;
  o.material = in.material;
  return o;
}
@fragment fn fs_shadow_foliage(in: ShadowOut) {
  let a = textureSample(leaf_tex, leaf_samp, in.uv).a;
  if (a < 0.45) { discard; }
}
// Depth + view-space normal prepass (SSAO).
struct PreOut { @builtin(position) pos: vec4<f32>, @location(0) vnormal: vec3<f32>, @location(1) uv: vec2<f32>, @location(2) @interpolate(flat) material: u32 };
@vertex fn vs_prepass(in: VertexIn) -> PreOut {
  var o: PreOut;
  var clip = u.mvp * vec4<f32>(in.position, 1.0);
  clip.x += cf.jitter.x * clip.w;
  clip.y += cf.jitter.y * clip.w;
  o.pos = clip;
  o.vnormal = (cf.view * vec4<f32>(in.normal, 0.0)).xyz;
  o.uv = in.uv;
  o.material = in.material;
  return o;
}
@fragment fn fs_prepass(in: PreOut) -> @location(0) vec4<f32> {
  let m = materials[in.material];
  let flags = u32(m.misc.x + 0.5);
  if ((flags & FLAG_FOLIAGE) != 0u) {
    let a = textureSample(leaf_tex, leaf_samp, in.uv).a;
    if (a < 0.45) { discard; }
  }
  return vec4<f32>(normalize(in.vnormal), 1.0);
}

fn sh_irradiance(n: vec3<f32>) -> vec3<f32> {
  let c = cf.sh;
  let r = c[0].rgb * 0.282095 + c[1].rgb * (0.488603 * n.y) + c[2].rgb * (0.488603 * n.z) +
          c[3].rgb * (0.488603 * n.x) + c[4].rgb * (1.092548 * n.x * n.y) + c[5].rgb * (1.092548 * n.y * n.z) +
          c[6].rgb * (0.315392 * (3.0 * n.z * n.z - 1.0)) + c[7].rgb * (1.092548 * n.x * n.z) +
          c[8].rgb * (0.546274 * (n.x * n.x - n.y * n.y));
  return max(r, vec3<f32>(0.0));
}
fn d_ggx(ndh: f32, a: f32) -> f32 {
  let a2 = a * a;
  let d = ndh * ndh * (a2 - 1.0) + 1.0;
  return a2 / (PI * d * d + 1e-7);
}
fn v_smith(ndv: f32, ndl: f32, a: f32) -> f32 {
  let a2 = a * a;
  let gv = ndl * sqrt(ndv * ndv * (1.0 - a2) + a2);
  let gl = ndv * sqrt(ndl * ndl * (1.0 - a2) + a2);
  return 0.5 / max(gv + gl, 1e-5);
}
fn f_schlick(f0: vec3<f32>, vdh: f32) -> vec3<f32> {
  let f = pow(1.0 - vdh, 5.0);
  return f0 + (1.0 - f0) * f;
}
fn env_brdf(f0: vec3<f32>, rough: f32, ndv: f32) -> vec3<f32> {
  let c0 = vec4<f32>(-1.0, -0.0275, -0.572, 0.022);
  let c1 = vec4<f32>(1.0, 0.0425, 1.04, -0.04);
  let r = rough * c0 + c1;
  let a004 = min(r.x * r.x, exp2(-9.28 * ndv)) * r.x + r.y;
  let ab = vec2<f32>(-1.04, 1.04) * a004 + r.zw;
  return f0 * ab.x + ab.y;
}

const POISSON: array<vec2<f32>, 12> = array<vec2<f32>, 12>(
  vec2<f32>(-0.326, -0.406), vec2<f32>(-0.840, -0.074), vec2<f32>(-0.696, 0.457), vec2<f32>(-0.203, 0.621),
  vec2<f32>(0.962, -0.195), vec2<f32>(0.473, -0.480), vec2<f32>(0.519, 0.767), vec2<f32>(0.185, -0.893),
  vec2<f32>(0.507, 0.064), vec2<f32>(0.896, 0.412), vec2<f32>(-0.322, -0.933), vec2<f32>(-0.792, -0.598));

fn cascade_index(view_z: f32) -> i32 {
  if (view_z < cf.cascade.x) { return 0; }
  if (view_z < cf.cascade.y) { return 1; }
  if (view_z < cf.cascade.z) { return 2; }
  return -1;
}
fn shadow_factor(world: vec3<f32>, n: vec3<f32>, view_z: f32, ndl: f32, pixel: vec2<f32>) -> f32 {
  if (cf.params2.w < 0.5) { return 1.0; }
  let idx = cascade_index(view_z);
  if (idx < 0) { return 1.0; }
  var m = cf.shadow0;
  var extent = cf.cascade_extent.x;
  if (idx == 1) { m = cf.shadow1; extent = cf.cascade_extent.y; }
  if (idx == 2) { m = cf.shadow2; extent = cf.cascade_extent.z; }
  let texel_world = 2.0 * extent * cf.cascade.w;
  let offset = n * texel_world * (1.6 - 1.0 * ndl) + frame.sun_dir.xyz * texel_world * 0.5;
  let lp = m * vec4<f32>(world + offset, 1.0);
  let uv = vec2<f32>(lp.x * 0.5 + 0.5, 0.5 - lp.y * 0.5);
  if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) { return 1.0; }
  let noise = fract(52.9829189 * fract(dot(pixel, vec2<f32>(0.06711056, 0.00583715)))) * 6.2831853;
  let cs = cos(noise); let sn = sin(noise);
  let radius = cf.cascade.w * 1.6;
  var lit = 0.0;
  for (var i = 0u; i < 12u; i = i + 1u) {
    let p = POISSON[i];
    let r = vec2<f32>(p.x * cs - p.y * sn, p.x * sn + p.y * cs) * radius;
    lit += textureSampleCompare(shadow_tex, shadow_samp, uv + r, idx, lp.z - 0.0004);
  }
  return lit / 12.0;
}

fn interior_color(aux: vec2<f32>, view_ts: vec3<f32>, room: vec4<f32>, seed: f32, night: f32) -> vec3<f32> {
  let rw = room.x; let rh = room.y; let rd = room.z;
  let cell = floor(vec2<f32>(aux.x / rw, aux.y / rh));
  let h = hash33(vec3<f32>(cell.x + 3.1, cell.y + 7.7, seed * 91.7));
  let floor_h = hash13(vec3<f32>(17.0, cell.y + 3.3, seed * 51.3));
  let floor_on = floor_h < room.w * select(0.28, 0.62, night > 0.5);
  let lit = select(select(0.0, 1.0, h.x < 0.10), select(0.0, 1.0, h.x < 0.85), floor_on) * (0.45 + 0.55 * h.z);
  let blinds = select(0.0, 1.0, h.z < 0.18);
  let p = vec2<f32>(aux.x - cell.x * rw, aux.y - cell.y * rh);
  var d = -view_ts;
  d.z = min(d.z, -0.02);
  let tx = select((0.0 - p.x) / d.x, (rw - p.x) / d.x, d.x > 0.0);
  let ty = select((0.0 - p.y) / d.y, (rh - p.y) / d.y, d.y > 0.0);
  let tz = -rd / d.z;
  let t = min(min(tx, ty), tz);
  let hit = vec3<f32>(p, 0.0) + d * t;
  let warm = vec3<f32>(1.0, 0.86, 0.68);
  let cool = vec3<f32>(0.78, 0.88, 1.0);
  let light_col = mix(warm, cool, step(0.5, h.y));
  let wall_tint = mix(vec3<f32>(0.78, 0.75, 0.70), vec3<f32>(0.66, 0.70, 0.76), h.y);
  var col = vec3<f32>(0.0);
  if (t == tz) {
    col = wall_tint * 0.55;
    let sx = hit.x / rw; let sy = hit.y / rh;
    let screen = step(0.36, sx) * step(sx, 0.64) * step(0.42, sy) * step(sy, 0.62);
    col += screen * vec3<f32>(0.35, 0.55, 0.95) * 0.35 * lit * night;
    col *= 1.0 - 0.35 * step(sy, 0.28);
  } else if (t == ty) {
    if (d.y > 0.0) {
      col = vec3<f32>(0.92, 0.92, 0.90);
      let gx = fract(hit.x / rw * 2.0); let gz = fract(-hit.z / rd * 2.0);
      let panel = step(0.3, gx) * step(gx, 0.7) * step(0.3, gz) * step(gz, 0.7);
      col += panel * light_col * 1.2 * lit;
    } else {
      col = vec3<f32>(0.30, 0.28, 0.26);
    }
  } else {
    col = wall_tint * 0.72;
  }
  col *= 1.0 / (1.0 + t * 0.12);
  let ambient = mix(0.1, 0.7, lit) * light_col;
  col = col * ambient;
  let slats = 0.6 + 0.4 * step(0.5, fract(p.y * 8.0));
  col = mix(col, vec3<f32>(0.75, 0.74, 0.70) * slats * mix(0.15, 0.6, lit), blinds);
  return col;
}

@fragment fn fs_main(in: VOut, @builtin(front_facing) front: bool) -> @location(0) vec4<f32> {
  let m = materials[in.material];
  let flags = u32(m.misc.x + 0.5);
  let night = cf.params.z;
  let view_z = dot(frame.cam_fwd.xyz, in.world);
  let V = normalize(-in.world);
  var N = normalize(in.normal);
  let foliage = (flags & FLAG_FOLIAGE) != 0u;
  if (foliage && !front) { N = -N; }
  var T = normalize(in.tangent.xyz - N * dot(in.tangent.xyz, N));
  let B = cross(N, T) * in.tangent.w;

  // ---- texture coordinates -------------------------------------------------
  let scale = max(m.tex.w, 1e-3);
  var uv = in.uv / scale;
  // Planar/triplanar: project in the mesh's own frame (aux = up), so
  // ground planes tile in metres wherever the site sits on the sphere.
  let up = normalize(frame.planet_up.xyz);
  if ((flags & FLAG_PLANAR_XZ) != 0u) {
    let ex = normalize(cross(up, select(vec3<f32>(1.0, 0.0, 0.0), vec3<f32>(0.0, 0.0, 1.0), abs(up.x) > 0.9)));
    let ez = cross(ex, up);
    uv = vec2<f32>(dot(in.local, ex), dot(in.local, ez)) / scale;
  }
  if ((flags & FLAG_TRIPLANAR) != 0u) {
    let an = abs(N);
    if (an.y >= an.x && an.y >= an.z) { uv = in.local.xz / scale; }
    else if (an.x >= an.z) { uv = in.local.zy / scale; }
    else { uv = in.local.xy / scale; }
  }

  // ---- material inputs -------------------------------------------------------
  var albedo = m.base_color.rgb;
  var alpha = 1.0;
  if (foliage) {
    let leaf = textureSample(leaf_tex, leaf_samp, in.uv);
    alpha = leaf.a;
    if (alpha < 0.3) { discard; }
    albedo = leaf.rgb * m.base_color.rgb;
  } else if (m.tex.x >= 0.0) {
    // Until the layer is resident, the registry's mean colour stands in.
    let layer = i32(m.tex.x);
    if (mats.b[layer].w >= 0.5) {
      albedo *= textureSample(mat_albedo, mat_sampler, uv, layer).rgb;
    } else {
      albedo *= mats.c[layer].rgb;
    }
  }
  if ((flags & (FLAG_PLANAR_XZ | FLAG_TRIPLANAR)) != 0u && !foliage) {
    let q = uv * scale * 0.045;
    let n2 = hash13(vec3<f32>(floor(q * 0.31 + 7.0), 2.0));
    let f1 = fract(q); let w1 = f1 * f1 * (3.0 - 2.0 * f1);
    let a = hash13(vec3<f32>(floor(q), 1.0)); let b = hash13(vec3<f32>(floor(q) + vec2<f32>(1.0, 0.0), 1.0));
    let c = hash13(vec3<f32>(floor(q) + vec2<f32>(0.0, 1.0), 1.0)); let d = hash13(vec3<f32>(floor(q) + vec2<f32>(1.0, 1.0), 1.0));
    let smooth_n = mix(mix(a, b, w1.x), mix(c, d, w1.x), w1.y);
    albedo *= 0.86 + 0.28 * smooth_n + 0.06 * (n2 - 0.5);
  }
  var roughness = m.params.x;
  var ao_tex_v = 1.0;
  if (m.tex.y >= 0.0 && mats.b[i32(m.tex.y)].w >= 0.5) {
    let nt4 = textureSample(mat_normal, mat_sampler, uv, i32(m.tex.y));
    ao_tex_v = 0.35 + 0.65 * nt4.w;  // the library's .w is ao (0 for procedural tiles: floor it, as the terrain does)
    roughness = clamp(mix(m.params.x, nt4.z * (m.params.x / 0.6), 0.55), select(0.12, 0.22, m.params.y > 0.5), 1.0);
    let nxy = nt4.xy * 2.0 - 1.0;
    let nt = vec3<f32>(nxy, sqrt(max(0.0, 1.0 - dot(nxy, nxy))));
    // Toksvig: a mip-averaged normal is shorter than one; widen the lobe
    // by the lost length instead of letting sub-pixel bumps sparkle.
    let len = clamp(length(nt), 0.05, 1.0);
    let var_n = (1.0 - len) / len;
    roughness = clamp(sqrt(roughness * roughness + 0.6 * var_n), roughness, 1.0);
    let ns = m.params.w;
    N = normalize(T * (nt.x * ns) + B * (nt.y * ns) + N * nt.z);
  }
  let metallic = m.params.y;
  let ndv = max(dot(N, V), 1e-4);
  {
    let gn = normalize(in.normal);
    let dx = dpdx(gn); let dy = dpdy(gn);
    let variance = 0.25 * (dot(dx, dx) + dot(dy, dy));
    let kr = min(2.0 * variance, 0.2);
    roughness = clamp(sqrt(roughness * roughness + kr), 0.03, 1.0);
  }

  // ---- occlusion --------------------------------------------------------------
  var ssao = 1.0;
  if (cf.params2.z > 0.5) {
    ssao = textureSample(ao_tex, clamp_samp, in.pos.xy * cf.screen.zw).r;
    ssao = pow(ssao, cf.params.y);
  }
  let ao = ssao * ao_tex_v * in.aux.w;

  // ---- sun ----------------------------------------------------------------------
  let L = normalize(frame.sun_dir.xyz);
  var ndl = max(dot(N, L), 0.0);
  let shadow = shadow_factor(in.world, normalize(in.normal), view_z, ndl, in.pos.xy);
  let sun = frame.sun_color.rgb * cf.params2.y;

  let f0 = mix(vec3<f32>(0.04), albedo, metallic);
  let diffuse_color = albedo * (1.0 - metallic);
  var color = vec3<f32>(0.0);

  if ((flags & FLAG_GLASS) != 0u) {
    let wav = (hash13(floor(vec3<f32>(in.aux.x / m.room.x, in.aux.y / m.room.y, 0.0))) - 0.5) * 0.012;
    let Ng = normalize(N + T * wav + B * wav * 0.6);
    let ndv_g = max(dot(Ng, V), 1e-4);
    let R = reflect(-V, Ng);
    let g_rough = m.params.x;
    let refl = textureSampleLevel(spec_cube, cube_samp, R, g_rough * 5.0).rgb * cf.params2.x;
    let f0g = vec3<f32>(0.05);
    let brdf = env_brdf(f0g, g_rough, ndv_g);
    let H = normalize(V + L);
    let ndh = max(dot(Ng, H), 0.0);
    let a = max(g_rough * g_rough, 0.06);
    let spec_sun = d_ggx(ndh, a) * v_smith(ndv_g, max(dot(Ng, L), 0.0), a) * f_schlick(f0g, max(dot(V, H), 0.0)) * max(dot(Ng, L), 0.0);
    let view_ts = vec3<f32>(dot(V, T), dot(V, B), dot(V, N));
    var inside = interior_color(in.aux.xy, view_ts, m.room, in.aux.z, night);
    let fw = fwidth(in.aux.xy);
    let px_per_room = m.room.x / max(fw.x, 1e-4);
    let detail = clamp((px_per_room - 6.0) / 30.0, 0.0, 1.0);
    let cell = floor(in.aux.xy / m.room.xy);
    let hc = hash33(vec3<f32>(cell.x + 3.1, cell.y + 7.7, in.aux.z * 91.7));
    let floor_far = hash13(vec3<f32>(17.0, cell.y + 3.3, in.aux.z * 51.3)) < m.room.w * select(0.28, 0.62, night > 0.5);
    let lit_far = select(select(0.0, 1.0, hc.x < 0.10), select(0.0, 1.0, hc.x < 0.85), floor_far) * (0.45 + 0.55 * hc.z);
    let light_far = mix(vec3<f32>(1.0, 0.86, 0.68), vec3<f32>(0.78, 0.88, 1.0), step(0.5, hc.y));
    let frac_y = fract(in.aux.y / m.room.y);
    let grad = 0.55 + 0.75 * frac_y * frac_y;
    let room_px_fade = clamp((px_per_room - 1.5) / 4.0, 0.0, 1.0);
    let lit_expect = m.room.w * select(0.28, 0.62, night > 0.5) * 0.5;
    let lit_eff = mix(lit_expect, lit_far, room_px_fade);
    let mean_room = vec3<f32>(0.42, 0.40, 0.37) * light_far * mix(0.12, 0.7, lit_eff) * grad;
    inside = mix(mean_room, inside, detail);
    inside = inside * cf.params.x * mix(0.045, 0.6, night);
    let gx = abs(fract(in.aux.x / m.room.x + 0.5) - 0.5) * m.room.x;
    let gy = abs(fract(in.aux.y / m.room.y + 0.5) - 0.5) * m.room.y;
    let line_w = max(0.04, fw.x * 0.9);
    let line_fade = clamp((px_per_room - 5.0) / 14.0, 0.0, 1.0);
    let avg_cover = clamp(2.0 * line_w / m.room.x + 2.0 * line_w / m.room.y, 0.0, 0.5);
    let frame_line = mix(avg_cover, 1.0 - min(1.0, min(gx, gy) / line_w), line_fade);
    let tint = m.misc.yzw;
    color = refl * brdf + sun * spec_sun * shadow + inside * (1.0 - brdf) * tint;
    color = mix(color, vec3<f32>(0.02, 0.02, 0.022) * (sh_irradiance(N) / PI + sun * ndl * shadow / PI), frame_line * 0.85);
    color += diffuse_color * 0.02 * (sh_irradiance(N) + sun * ndl * shadow) / PI;
  } else {
    let H = normalize(V + L);
    let ndh = max(dot(N, H), 0.0);
    let vdh = max(dot(V, H), 0.0);
    let a = max(roughness * roughness, 0.002);
    let a_sun = max(a, 0.06);
    let F = f_schlick(f0, vdh);
    var kd = (1.0 - F) * (1.0 - metallic);
    var diffuse = diffuse_color / PI;
    var spec_scale = 1.0;
    if (foliage) {
      ndl = clamp((dot(N, L) + 0.4) / 1.4, 0.0, 1.0);
      let back = max(dot(-N, L), 0.0) * 0.18;
      diffuse = diffuse * (1.0 + back);
      spec_scale = 0.35;
    }
    let spec = d_ggx(ndh, a_sun) * min(v_smith(ndv, ndl, a_sun), 8.0) * F * spec_scale;
    color += (kd * diffuse + spec) * sun * ndl * shadow;
    let irr = sh_irradiance(N) * cf.params2.x;
    color += diffuse_color * irr / PI * ao;
    let R = reflect(-V, N);
    let pref = textureSampleLevel(spec_cube, cube_samp, R, roughness * 5.0).rgb * cf.params2.x;
    let so = clamp(pow(ndv + ao, exp2(-16.0 * roughness - 1.0)) - 1.0 + ao, 0.0, 1.0);
    color += pref * env_brdf(f0, roughness, ndv) * so;
    let count = i32(cf.params.w + 0.5);
    for (var i = 0; i < count; i = i + 1) {
      let lt = lights[i];
      let dvec = lt.pos_radius.xyz - in.world;
      let d2 = dot(dvec, dvec);
      let r2 = lt.pos_radius.w * lt.pos_radius.w;
      let win = clamp(1.0 - (d2 * d2) / (r2 * r2), 0.0, 1.0);
      let att = win * win / (d2 + 1.0);
      let Ld = normalize(dvec);
      let ndl2 = max(dot(N, Ld), 0.0);
      let Hd = normalize(V + Ld);
      let spec2 = d_ggx(max(dot(N, Hd), 0.0), a) * v_smith(ndv, ndl2, a) * f_schlick(f0, max(dot(V, Hd), 0.0));
      color += (kd * diffuse + spec2) * lt.color_int.rgb * lt.color_int.w * att * ndl2;
    }
  }
  if ((flags & FLAG_EMISSIVE) != 0u) {
    var e = m.params.z * cf.params.x;
    if ((flags & FLAG_NIGHT_ONLY) != 0u) { e *= night; }
    color += m.misc.yzw * e;
  }
  color = min(color, vec3<f32>(24.0 * cf.params.x));
  // Debug views (cf.jitter.z): 1 albedo, 2 normal, 3 ao, 4 shadow, 5 roughness, 6 sun term, 7 sky irradiance.
  let dbg = i32(cf.jitter.z + 0.5);
  if (dbg == 1) { color = albedo * 0.3; }
  else if (dbg == 2) { color = (N * 0.5 + 0.5) * 0.3; }
  else if (dbg == 3) { color = vec3<f32>(ao) * 0.3; }
  else if (dbg == 4) { color = vec3<f32>(shadow) * 0.3; }
  else if (dbg == 5) { color = vec3<f32>(roughness) * 0.3; }
  else if (dbg == 6) { color = sun * ndl * shadow * 0.3; }
  else if (dbg == 7) { color = sh_irradiance(N); }
  return vec4<f32>(color, alpha);
}
)";

}  // namespace inf::render
