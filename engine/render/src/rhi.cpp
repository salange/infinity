#include "render/rhi.hpp"

#include "city_passes.hpp"
#include "city_shader.hpp"

#include <webgpu/webgpu.h>
#include <webgpu/wgpu.h>  // wgpuDevicePoll (wgpu-native extension)

#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <unordered_map>
#include <utility>
#include <vector>

#include <GLFW/glfw3.h>

#if defined(__linux__)
#define GLFW_EXPOSE_NATIVE_WAYLAND
#define GLFW_EXPOSE_NATIVE_X11
#elif defined(__APPLE__)
#define GLFW_EXPOSE_NATIVE_COCOA
#elif defined(_WIN32)
#define GLFW_EXPOSE_NATIVE_WIN32
#endif
#include <GLFW/glfw3native.h>

#if defined(__APPLE__)
extern "C" void* infinityMetalLayerForCocoaWindow(void* nsWindow);
#endif

namespace inf::render {

namespace {

constexpr std::uint64_t kUniformStride = 256;  // minUniformBufferOffsetAlignment
constexpr std::uint32_t kMaxDrawItems = 4096;
constexpr std::uint64_t kItemUniformSize = 128;  // mvp + color + aux + extra + palette
constexpr std::uint64_t kFrameUniformSize = 160;  // 10 vec4s (see Frame in WGSL)

constexpr const char* kMeshShader = R"(
// Per-item block. aux/extra are mode-specific:
//   mode 0 (extra.w): legacy — color.a == 0 lit terrain, > 0 unlit color.
//   mode 1: star photosphere — aux.xyz = camera->star-center unit dir,
//           aux.w = per-star phase seed, extra.x = spot amount.
//   mode 2: corona/glow billboard (additive pass) — aux.w = phase seed,
//           extra.x = intensity, extra.y = photosphere radius in
//           billboard units, extra.z = diffraction-spike strength [0,1].
//   mode 3: glow sprite (additive pass) — soft radial glow, or a rim halo
//           when extra.z > 0. extra.x = intensity, extra.y = falloff
//           exponent (disc) / sharpness (rim), extra.z = rim radius in
//           quad units (0 = disc). Used for lens flares, the sun veil,
//           and planet limb glow.
//   mode 4: analytic sky dome (opaque, fullscreen quad at far depth):
//           per-pixel view-ray gradient sky from the frame uniforms.
struct Uniforms {
  mvp: mat4x4<f32>,
  color: vec4<f32>,
  aux: vec4<f32>,
  extra: vec4<f32>,
  palette: vec4<f32>,  // lit terrain: four material ids (0 = unused)
};
// Per-frame globals (frame of the meshes = anchor-planet-local):
//   sun_dir.xyz light direction; sun_color.rgb light tint, .a time (s);
//   cam_right/up/fwd.xyz camera basis, right.w/up.w = tan(fov/2)*aspect
//   and tan(fov/2), fwd.w = camera altitude / atmosphere height;
//   planet_up.xyz local up at the camera; atmo.rgb sky palette.
// planet_center.xyz = anchor planet center relative to the camera,
// planet_center.w = normal blend: how far lit-terrain shading normals
// are pulled toward the analytic sphere radial (0 on the surface, 1
// from orbit — hides per-chunk normal seams at distance).
struct Frame {
  sun_dir: vec4<f32>,
  sun_color: vec4<f32>,
  cam_right: vec4<f32>,
  cam_up: vec4<f32>,
  cam_fwd: vec4<f32>,
  planet_up: vec4<f32>,
  atmo: vec4<f32>,
  planet_center: vec4<f32>,
  material: vec4<f32>,  // x = per-planet palette shift (-1..1)
  jitter: vec4<f32>,    // xy = temporal AA projection jitter (NDC units)
};
@group(0) @binding(0) var<uniform> u: Uniforms;
@group(0) @binding(1) var<uniform> frame: Frame;
// T0021 WP1: what lit terrain receives from the city passes — the
// cascade matrices and split distances, the shadow maps, the ambient
// occlusion of the prepass.
struct CityFrame {
  view: mat4x4<f32>,
  proj: mat4x4<f32>,
  view_proj: mat4x4<f32>,
  inv_view_proj: mat4x4<f32>,
  inv_proj: mat4x4<f32>,
  shadow0: mat4x4<f32>,
  shadow1: mat4x4<f32>,
  shadow2: mat4x4<f32>,
  sh: array<vec4<f32>, 9>,
  cascade: vec4<f32>,
  cascade_extent: vec4<f32>,
  screen: vec4<f32>,
  params: vec4<f32>,
  params2: vec4<f32>,
  jitter: vec4<f32>,
};
@group(3) @binding(0) var<uniform> cfr: CityFrame;
@group(3) @binding(1) var rc_shadow_tex: texture_depth_2d_array;
@group(3) @binding(2) var rc_shadow_samp: sampler_comparison;
@group(3) @binding(3) var rc_ao_tex: texture_2d<f32>;
@group(3) @binding(4) var rc_clamp_samp: sampler;
@group(3) @binding(5) var rc_depth_pre: texture_depth_2d;
const RC_POISSON: array<vec2<f32>, 12> = array<vec2<f32>, 12>(
  vec2<f32>(-0.326, -0.406), vec2<f32>(-0.840, -0.074), vec2<f32>(-0.696, 0.457), vec2<f32>(-0.203, 0.621),
  vec2<f32>(0.962, -0.195), vec2<f32>(0.473, -0.480), vec2<f32>(0.519, 0.767), vec2<f32>(0.185, -0.893),
  vec2<f32>(0.507, 0.064), vec2<f32>(0.896, 0.412), vec2<f32>(-0.322, -0.933), vec2<f32>(-0.792, -0.598));
fn rc_shadow_factor(world: vec3<f32>, n: vec3<f32>, view_z: f32, ndl: f32, pixel: vec2<f32>) -> f32 {
  if (cfr.params2.w < 0.5) { return 1.0; }
  var idx = -1;
  if (view_z < cfr.cascade.x) { idx = 0; } else if (view_z < cfr.cascade.y) { idx = 1; } else if (view_z < cfr.cascade.z) { idx = 2; }
  if (idx < 0) { return 1.0; }
  var m = cfr.shadow0;
  var extent = cfr.cascade_extent.x;
  if (idx == 1) { m = cfr.shadow1; extent = cfr.cascade_extent.y; }
  if (idx == 2) { m = cfr.shadow2; extent = cfr.cascade_extent.z; }
  let texel_world = 2.0 * extent * cfr.cascade.w;
  let offset = n * texel_world * (1.6 - 1.0 * ndl) + frame.sun_dir.xyz * texel_world * 0.5;
  let lp = m * vec4<f32>(world + offset, 1.0);
  let uv = vec2<f32>(lp.x * 0.5 + 0.5, 0.5 - lp.y * 0.5);
  if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) { return 1.0; }
  let noise = fract(52.9829189 * fract(dot(pixel, vec2<f32>(0.06711056, 0.00583715)))) * 6.2831853;
  let cs = cos(noise); let sn = sin(noise);
  let radius = cfr.cascade.w * 1.6;
  var lit = 0.0;
  for (var i = 0u; i < 12u; i = i + 1u) {
    let p = RC_POISSON[i];
    let r = vec2<f32>(p.x * cs - p.y * sn, p.x * sn + p.y * cs) * radius;
    lit += textureSampleCompare(rc_shadow_tex, rc_shadow_samp, uv + r, idx, lp.z - 0.0004);
  }
  // Fade the last cascade out at its far edge.
  let fade = select(1.0, clamp((cfr.cascade.z - view_z) / (0.15 * cfr.cascade.z), 0.0, 1.0), idx == 2);
  return mix(1.0, lit / 12.0, fade);
}
// Planet cube-map pair (T0016, mode 6): height normalized to [-1,1] over
// the body's amplitude, material albedo; layer = cube face in the SAME
// cube-sphere frame the generators use, so texel<->surface mapping is
// exact by construction. Items without a texture bind a 1x1 default.
@group(1) @binding(0) var planet_height: texture_2d_array<f32>;
@group(1) @binding(1) var planet_material: texture_2d_array<f32>;
@group(1) @binding(2) var planet_sampler: sampler;

// Surface material library (T0019): albedo.rgb + height.a, normal.xy +
// roughness.z + ao/emissive.w, one layer per material id, repeat sampler.
@group(2) @binding(0) var mat_albedo: texture_2d_array<f32>;
@group(2) @binding(1) var mat_normal: texture_2d_array<f32>;
@group(2) @binding(2) var mat_sampler: sampler;
struct MaterialTable {
  a: array<vec4<f32>, 64>,  // tint.rgb, tile size (m)
  b: array<vec4<f32>, 64>,  // roughness, emissive, normal strength, ready
  c: array<vec4<f32>, 64>,  // untinted mean albedo.rgb
};
@group(2) @binding(3) var<uniform> mats: MaterialTable;

const kTilePeriodM: f32 = 256.0;  // must match the app's origin modulo

fn srgb_to_linear(c: vec3<f32>) -> vec3<f32> {
  return pow(max(c, vec3<f32>(0.0)), vec3<f32>(2.2));
}

fn hash2(p: vec2<f32>) -> vec2<f32> {
  var q = vec3<f32>(fract(p.x * 0.1031), fract(p.y * 0.1030), fract((p.x + p.y) * 0.0973));
  q += dot(q, q.yzx + 33.33);
  return fract(vec2<f32>((q.x + q.y) * q.z, (q.x + q.z) * q.y));
}

struct TileSample {
  albedo: vec3<f32>,
  height: f32,
  tnormal: vec2<f32>,  // -1..1
  rough: f32,
  aux: f32,            // ao, or emissive mask
};

// Stochastic tiling on a square lattice split into triangles (the
// Heitz-Neyret triangle-grid blend with Mikkelsen's sharpened weights and
// a variance-preserving colour blend around the tile mean). Each lattice
// vertex owns a random rotation + offset of the tile. Vertex ids are
// hashed MODULO the tile period so the per-chunk coordinate offsets the
// CPU applies (multiples of the period) never change a tile's look —
// no seams at chunk borders, no f32 swim at planet radii.
fn sample_tiled(layer: i32, uv: vec2<f32>, ddx_uv: vec2<f32>, ddy_uv: vec2<f32>,
                period_cells: f32, mean: vec3<f32>) -> TileSample {
  let cell = floor(uv);
  let f = uv - cell;
  var verts = array<vec2<f32>, 3>(cell, cell + vec2<f32>(1.0, 0.0), cell + vec2<f32>(0.0, 1.0));
  var w = vec3<f32>(1.0 - f.x - f.y, f.x, f.y);
  if (f.x + f.y >= 1.0) {
    verts[0] = cell + vec2<f32>(1.0, 1.0);
    w = vec3<f32>(f.x + f.y - 1.0, 1.0 - f.y, 1.0 - f.x);
  }
  w = w * w * w;
  w = w * w;  // ^6: mostly one tile, narrow blend bands
  w = w / (w.x + w.y + w.z);
  var acc_a = vec3<f32>(0.0);
  var acc_h = 0.0;
  var acc_n = vec2<f32>(0.0);
  var acc_r = 0.0;
  var acc_x = 0.0;
  for (var i = 0; i < 3; i++) {
    let vm = verts[i] - period_cells * floor(verts[i] / period_cells);
    let r = hash2(vm + vec2<f32>(0.37, 0.11));
    let ang = r.y * 6.2831853;
    let c = cos(ang);
    let sn = sin(ang);
    // rotated about the vertex, then a random offset
    let d = uv - verts[i];
    let suv = vec2<f32>(c * d.x - sn * d.y, sn * d.x + c * d.y) + r * 7.31;
    let gx = vec2<f32>(c * ddx_uv.x - sn * ddx_uv.y, sn * ddx_uv.x + c * ddx_uv.y);
    let gy = vec2<f32>(c * ddy_uv.x - sn * ddy_uv.y, sn * ddy_uv.x + c * ddy_uv.y);
    let a = textureSampleGrad(mat_albedo, mat_sampler, suv, layer, gx, gy);
    let nm = textureSampleGrad(mat_normal, mat_sampler, suv, layer, gx, gy);
    let n2 = nm.xy * 2.0 - 1.0;
    // rotate the tangent normal back into the uv frame (inverse rotation)
    let n2r = vec2<f32>(c * n2.x + sn * n2.y, -sn * n2.x + c * n2.y);
    let wi = w[i];
    acc_a += wi * (a.rgb - mean);
    acc_h += wi * a.a;
    acc_n += wi * n2r;
    acc_r += wi * nm.z;
    acc_x += wi * nm.w;
  }
  let norm = inverseSqrt(dot(w, w));
  var out: TileSample;
  out.albedo = max(mean + acc_a * norm, vec3<f32>(0.0));
  out.height = acc_h;
  out.tnormal = acc_n;
  out.rough = acc_r;
  out.aux = acc_x;
  return out;
}

// One material on one projection plane: fine scale hex-tiled + coarse
// scale (8x larger, plain) blended by distance (NMS's two-scale trick),
// so close-ups get photographic detail and the horizon gets no moire.
fn sample_material(layer: i32, p: vec2<f32>, dpx: vec2<f32>, dpy: vec2<f32>,
                   coarse_w: f32) -> TileSample {
  let tile = max(mats.a[layer].w, 0.25);
  let mean = srgb_to_linear(mats.c[layer].rgb);
  let fine = sample_tiled(layer, p / tile, dpx / tile, dpy / tile, kTilePeriodM / tile, mean);
  let ct = tile * 8.0;
  let cuv = p / ct + vec2<f32>(0.5, 0.25);
  let ca = textureSampleGrad(mat_albedo, mat_sampler, cuv, layer, dpx / ct, dpy / ct);
  let cn = textureSampleGrad(mat_normal, mat_sampler, cuv, layer, dpx / ct, dpy / ct);
  var out: TileSample;
  out.albedo = mix(fine.albedo, ca.rgb, coarse_w);
  out.height = mix(fine.height, ca.a, coarse_w);
  out.tnormal = mix(fine.tnormal, cn.xy * 2.0 - 1.0, coarse_w);
  out.rough = mix(fine.rough, cn.z, coarse_w);
  out.aux = mix(fine.aux, cn.w, coarse_w);
  return out;
}

// Height-based blend of the vertex's two materials (contrast-preserving:
// the taller texel wins within a soft band), then the planet tint.
fn blend_pair(s0: TileSample, s1: TileSample, blend: f32) -> TileSample {
  let h0 = s0.height + (1.0 - blend);
  let h1 = s1.height + blend;
  let ma = max(h0, h1) - 0.3;
  var w0 = max(h0 - ma, 0.0);
  var w1 = max(h1 - ma, 0.0);
  let inv = 1.0 / max(w0 + w1, 1.0e-4);
  w0 *= inv;
  w1 *= inv;
  var out: TileSample;
  out.albedo = s0.albedo * w0 + s1.albedo * w1;
  out.height = s0.height * w0 + s1.height * w1;
  out.tnormal = s0.tnormal * w0 + s1.tnormal * w1;
  out.rough = s0.rough * w0 + s1.rough * w1;
  out.aux = s0.aux * w0 + s1.aux * w1;
  return out;
}

// Direction -> (u01, v01, face), mirroring world/cubesphere.cpp exactly
// (dominant axis, ties broken x, y, z).
fn cube_face_uv(d: vec3<f32>) -> vec3<f32> {
  let ax = abs(d.x);
  let ay = abs(d.y);
  let az = abs(d.z);
  var face = 0.0;
  var u = 0.0;
  var v = 0.0;
  if (ax >= ay && ax >= az) {
    if (d.x >= 0.0) { face = 0.0; u = d.y / ax; v = d.z / ax; }
    else { face = 1.0; u = -d.y / ax; v = d.z / ax; }
  } else if (ay >= az) {
    if (d.y >= 0.0) { face = 2.0; u = -d.x / ay; v = d.z / ay; }
    else { face = 3.0; u = d.x / ay; v = d.z / ay; }
  } else {
    if (d.z >= 0.0) { face = 4.0; u = d.y / az; v = d.x / az; }
    else { face = 5.0; u = d.y / az; v = -d.x / az; }
  }
  return vec3<f32>((u + 1.0) * 0.5, (v + 1.0) * 0.5, face);
}

// In-face tangent axes for the shading-normal gradient (approximate:
// the uv axes projected onto the tangent plane are close enough for
// lighting).
fn cube_axis_u(face: u32) -> vec3<f32> {
  switch face {
    case 0u: { return vec3<f32>(0.0, 1.0, 0.0); }
    case 1u: { return vec3<f32>(0.0, -1.0, 0.0); }
    case 2u: { return vec3<f32>(-1.0, 0.0, 0.0); }
    case 3u: { return vec3<f32>(1.0, 0.0, 0.0); }
    default: { return vec3<f32>(0.0, 1.0, 0.0); }
  }
}
fn cube_axis_v(face: u32) -> vec3<f32> {
  switch face {
    case 4u: { return vec3<f32>(1.0, 0.0, 0.0); }
    case 5u: { return vec3<f32>(-1.0, 0.0, 0.0); }
    default: { return vec3<f32>(0.0, 0.0, 1.0); }
  }
}

struct VSOut {
  @builtin(position) pos: vec4<f32>,
  @location(0) normal: vec3<f32>,
  @location(1) opos: vec3<f32>,
  // T0019: per-vertex weights over the item's four-material palette
  // (interpolated, so transitions never follow triangle edges). Star
  // billboards reuse .x as a packed rgb and .y as the twinkle phase.
  @location(2) weights: vec4<f32>,
};

@vertex
fn vs_main(@location(0) position: vec3<f32>, @location(1) normal: vec3<f32>,
           @location(2) weights: vec4<f32>) -> VSOut {
  var out: VSOut;
  let mode = u32(u.extra.w + 0.5);
  if (mode == 6u) {
    // Textured planet impostor: displace the unit sphere radially from
    // the height cube map — real silhouette relief for a texture fetch.
    // extra.x = height amplitude / body radius.
    let dir = normalize(position);
    let fuv = cube_face_uv(dir);
    let h = textureSampleLevel(planet_height, planet_sampler, fuv.xy, i32(fuv.z), 0.0).r;
    out.pos = u.mvp * vec4<f32>(dir * (1.0 + h * u.extra.x), 1.0);
    out.pos.x += frame.jitter.x * out.pos.w;
    out.pos.y += frame.jitter.y * out.pos.w;
    out.opos = dir;  // undisplaced unit direction; fragment re-derives uv
    out.normal = dir;
    out.weights = vec4<f32>(0.0);
    return out;
  }
  if (mode == 7u) {
    // Resolved star billboard (T0018 WP2): one static mesh carries the
    // whole field. position = unit direction (galactic frame), normal.xy
    // = quad corner scaled by the star's size, normal.z = HDR peak flux,
    // mat_pack = packed 8-bit rgb, mat_blend = twinkle phase. w = 0
    // makes the transform rotation-only (the field sits at infinity);
    // depth is forced just above the sky dome so terrain and planets
    // occlude stars but the dome never does. extra.xy = NDC per corner
    // unit (from the viewport size).
    var clip = u.mvp * vec4<f32>(position, 0.0);
    let corner = normal.xy;
    let half_len = max(max(abs(corner.x), abs(corner.y)), 1.0e-4);
    out.pos = vec4<f32>(clip.xy + corner * vec2<f32>(u.extra.x, u.extra.y) * clip.w,
                        clip.w * 3.0e-22, clip.w);
    out.opos = vec3<f32>(corner.x / half_len, corner.y / half_len, 0.0);
    out.normal = vec3<f32>(normal.z, 0.0, 0.0);
    out.weights = weights;
    return out;
  }
  out.pos = u.mvp * vec4<f32>(position, 1.0);
  out.pos.x += frame.jitter.x * out.pos.w;
  out.pos.y += frame.jitter.y * out.pos.w;
  out.normal = normal;
  out.opos = position;
  out.weights = weights;
  return out;
}

// material/v1 palette: albedos by material id (0 = unused sentinel).
fn material_albedo(id: u32) -> vec3<f32> {
  switch id {
    case 1u: { return vec3<f32>(0.42, 0.38, 0.34); }  // Rock
    case 2u: { return vec3<f32>(0.46, 0.44, 0.41); }  // Regolith
    case 3u: { return vec3<f32>(0.78, 0.68, 0.47); }  // Sand
    case 4u: { return vec3<f32>(0.28, 0.43, 0.20); }  // Grass
    case 5u: { return vec3<f32>(0.92, 0.94, 0.97); }  // Snow
    case 6u: { return vec3<f32>(0.70, 0.80, 0.90); }  // IceSheet
    case 7u: { return vec3<f32>(0.34, 0.35, 0.29); }  // Seabed
    case 8u: { return vec3<f32>(0.35, 0.32, 0.29); }  // Scree
    default: { return vec3<f32>(0.55, 0.52, 0.45); }
  }
}

// --- cheap deterministic 3D value noise + fbm (visual only) --------------
fn hash3(p: vec3<f32>) -> f32 {
  var q = fract(p * vec3<f32>(0.1031, 0.1030, 0.0973));
  q += dot(q, q.yxz + 33.33);
  return fract((q.x + q.y) * q.z);
}

fn vnoise(p: vec3<f32>) -> f32 {
  let i = floor(p);
  let fr = fract(p);
  let w = fr * fr * (3.0 - 2.0 * fr);
  let n000 = hash3(i + vec3<f32>(0.0, 0.0, 0.0));
  let n100 = hash3(i + vec3<f32>(1.0, 0.0, 0.0));
  let n010 = hash3(i + vec3<f32>(0.0, 1.0, 0.0));
  let n110 = hash3(i + vec3<f32>(1.0, 1.0, 0.0));
  let n001 = hash3(i + vec3<f32>(0.0, 0.0, 1.0));
  let n101 = hash3(i + vec3<f32>(1.0, 0.0, 1.0));
  let n011 = hash3(i + vec3<f32>(0.0, 1.0, 1.0));
  let n111 = hash3(i + vec3<f32>(1.0, 1.0, 1.0));
  let x00 = mix(n000, n100, w.x);
  let x10 = mix(n010, n110, w.x);
  let x01 = mix(n001, n101, w.x);
  let x11 = mix(n011, n111, w.x);
  return mix(mix(x00, x10, w.y), mix(x01, x11, w.y), w.z);
}

fn fbm(p: vec3<f32>) -> f32 {
  var value = 0.0;
  var amplitude = 0.5;
  var q = p;
  for (var i = 0; i < 5; i++) {
    value += amplitude * vnoise(q);
    q = q * 2.02 + vec3<f32>(17.3, 9.1, 4.7);
    amplitude *= 0.5;
  }
  return value;
}

// Narkowicz ACES filmic approximation: HDR-style highlight rolloff to
// white without a post-process chain — the "blinding but soft" look.
fn aces(x: vec3<f32>) -> vec3<f32> {
  let mapped = (x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14);
  return clamp(mapped, vec3<f32>(0.0), vec3<f32>(1.0));
}

// Star photosphere: a per-pixel TEMPERATURE field rendered through a
// blackbody-ish ramp (deep saturated intergranular lanes -> body tint ->
// white-hot granule cores), double domain warp for plasma churn,
// empirical limb darkening, faculae near the limb, sunspots with
// penumbra, chromosphere flash — then ACES-tonemapped from HDR values.
fn star_surface(n_in: vec3<f32>, tint: vec3<f32>, view_dir: vec3<f32>, phase: f32,
                spot_amount: f32, time: f32) -> vec3<f32> {
  let n = normalize(n_in);
  let drift = vec3<f32>(time * 0.006, time * 0.004, time * 0.009);
  let p = n * 15.0 + vec3<f32>(phase * 37.0) + drift;
  // Double domain warp: convection churn instead of static noise.
  let w1 = vec3<f32>(fbm(p * 0.55), fbm(p * 0.55 + 11.7), fbm(p * 0.55 + 71.3));
  let q = p * 1.1 + w1 * 2.0;
  let w2 = vec3<f32>(fbm(q + vec3<f32>(31.4)), fbm(q + vec3<f32>(53.1)),
                     fbm(q + vec3<f32>(97.7)));
  let cells = fbm(p + w2 * 2.6);                     // large convection cells
  let fine = fbm(p * 3.3 + w1 * 1.8 + drift * 2.0);  // fine granulation
  var temp_f = clamp(cells * 0.85 + fine * 0.55, 0.0, 1.3);
  // Sunspots: cool patches, soft penumbra.
  let s = fbm(n * 3.1 + vec3<f32>(phase * 53.0) + drift * 0.3);
  temp_f *= 1.0 - 0.85 * smoothstep(0.64, 0.80, s) * spot_amount;
  // Blackbody-ish ramp derived from the star's tint.
  let lane = tint * tint * 0.5;  // cooler: darker AND more saturated
  var c = mix(lane, tint * 1.2, smoothstep(0.12, 0.74, temp_f));
  c = mix(c, vec3<f32>(1.45), smoothstep(0.74, 1.12, temp_f));
  // Limb darkening (power-law fit) + faculae brightening near the limb.
  let mu = clamp(dot(n, -view_dir), 0.0, 1.0);
  let limb = 0.22 + 0.78 * pow(mu, 0.6);
  let faculae = smoothstep(0.45, 0.10, mu) * smoothstep(0.5, 0.9, fine);
  c = c * limb + tint * faculae * 0.55;
  // Chromosphere flash at the very limb.
  let rim = pow(1.0 - mu, 5.5);
  c += mix(tint, vec3<f32>(1.0, 0.42, 0.22), 0.6) * rim * 1.5;
  return c * 1.7;
}

// Corona billboard (additive, opos.xy in [-1,1]): blinding rim just
// outside the photosphere silhouette, wide chromatic halo (white near
// the disc, saturated tint far out), flowing radial streamers,
// prominence arcs hugging the limb, and distance-adaptive diffraction
// spikes so far stars sparkle. disc_r = photosphere radius in billboard
// units; spike in [0,1] fades the cross out on close approach.
fn corona(opos: vec2<f32>, tint: vec3<f32>, phase: f32, intensity: f32,
          disc_r: f32, spike: f32, time: f32) -> vec3<f32> {
  let r = length(opos);
  let window = smoothstep(1.0, 0.60, r);
  let theta = atan2(opos.y, opos.x);
  let edge = max(r - disc_r, 0.0);
  var c = vec3<f32>(0.0);
  // Blinding inner rim.
  c += mix(vec3<f32>(1.35), tint, 0.3) * exp(-edge * 24.0) * 2.8;
  // Chromatic halo: hue drifts from white-hot to the star tint outward.
  let halo_tint = mix(vec3<f32>(1.0), tint, clamp(edge * 3.2, 0.0, 1.0));
  c += halo_tint * exp(-edge * 5.0) * 0.9;
  c += tint * exp(-edge * 1.8) * 0.22;  // faint far reach
  // Flowing radial streamers (polar FBM, drifting outward over time).
  let ray_p = vec3<f32>(cos(theta), sin(theta), 0.0) * 3.0 +
              vec3<f32>(phase * 19.0) + vec3<f32>(0.0, 0.0, r * 2.2 - time * 0.05);
  let streamer = pow(max(fbm(ray_p) * 1.55 - 0.35, 0.0), 2.0);
  c += tint * streamer * exp(-edge * 3.4) * 1.2;
  // Prominences: red-orange loop arcs right at the limb.
  let band = exp(-abs(r - disc_r * 1.05) * 30.0);
  let arc_p = vec3<f32>(cos(theta), sin(theta), 0.6) * 5.0 +
              vec3<f32>(phase * 71.0, 0.0, time * 0.03);
  let arcs = pow(max(fbm(arc_p) * 1.7 - 0.78, 0.0), 1.4);
  c += vec3<f32>(1.0, 0.30, 0.12) * band * arcs * 2.4;
  // Diffraction spikes: a 4-point cross, only when the star is small on
  // screen — far suns read as bright stars, near suns as raging discs.
  let cross = pow(abs(cos(theta)), 40.0) + pow(abs(sin(theta)), 40.0);
  c += mix(tint, vec3<f32>(1.0), 0.55) * cross * exp(-r * 2.6) * spike * 1.3;
  // Slow, subtle flicker (kept small — visible pulsing reads as a bug).
  let flicker = 0.97 + 0.03 * vnoise(vec3<f32>(time * 0.25, phase * 91.0, 0.0));
  return c * flicker * intensity * window;
}

// Analytic sky dome (tier-1 gradient atmosphere, sources note
// atmosphere-rendering.md): Rayleigh-ish zenith/horizon ramp, Mie
// forward lobe around the sun, sunset band at low sun elevation,
// day/night from the sun-up dot, altitude fade to space.
fn sky_dome(ndc: vec2<f32>) -> vec3<f32> {
  let view = normalize(frame.cam_right.xyz * (ndc.x * frame.cam_right.w) +
                       frame.cam_up.xyz * (ndc.y * frame.cam_up.w) +
                       frame.cam_fwd.xyz);
  let sun = normalize(frame.sun_dir.xyz);
  let up = normalize(frame.planet_up.xyz);
  // Sub-linear falloff: the sky keeps most of its color well up into the
  // band and only thins near the top (a linear ramp read as space from
  // half the atmosphere up, making entry/exit look like a hard curtain).
  let density = pow(clamp(1.0 - frame.cam_fwd.w, 0.0, 1.0), 0.45);
  let sun_h = dot(sun, up);
  let view_h = dot(view, up);
  let cos_vs = dot(view, sun);
  let day = smoothstep(-0.10, 0.30, sun_h);
  let tint = frame.atmo.rgb;
  // Zenith deepens and cools; the horizon brightens and warms.
  let zenith = tint * vec3<f32>(0.40, 0.52, 0.75);
  let horizon = mix(tint, vec3<f32>(1.0, 0.88, 0.72), 0.45) * 1.06;
  var sky = mix(horizon, zenith, pow(clamp(view_h, 0.0, 1.0), 0.55));
  // Sunset band: a low sun reddens the sky toward its azimuth.
  let low_sun = pow(clamp(1.0 - abs(sun_h) * 2.6, 0.0, 1.0), 1.4);
  let toward = pow(clamp(cos_vs, 0.0, 1.0), 2.6);
  sky = mix(sky, vec3<f32>(1.0, 0.42, 0.18), low_sun * toward * 0.75);
  // Mie forward lobe + tight glare around the sun disc.
  let mie = pow(clamp(cos_vs, 0.0, 1.0), 24.0) * 0.55 +
            pow(clamp(cos_vs, 0.0, 1.0), 220.0) * 1.6;
  var c = sky * day + frame.sun_color.rgb * mie * (0.25 + 0.75 * day);
  // Night floor: faint cold airglow instead of dead black.
  c += tint * 0.004 * (1.0 - day);
  // T0018 WP3: the deep sky behind everything — the baked galaxy band
  // cube map (luminance in the height plane, chromaticity in the
  // material plane; extra.x = gain). The atmosphere ADDS scattered light
  // on top instead of replacing space: at night the band shines through,
  // by day the scattered blue washes it out via exposure, and in space
  // (density -> 0) the bake stands alone.
  let fuv = cube_face_uv(view);
  let layer = i32(fuv.z);
  let deep = textureSampleLevel(planet_material, planet_sampler, fuv.xy, layer, 0.0).rgb *
             textureSampleLevel(planet_height, planet_sampler, fuv.xy, layer, 0.0).r *
             u.extra.x;
  let space = vec3<f32>(0.00004, 0.00005, 0.0001);
  return deep + space + c * density;
}

@fragment
fn fs_main(in: VSOut) -> @location(0) vec4<f32> {
  let mode = u32(u.extra.w + 0.5);
  let time = frame.sun_color.a;
  if (mode == 3u) {
    let r = length(in.opos.xy);
    var base = 0.0;
    if (u.extra.z > 0.001) {
      base = exp(-abs(r - u.extra.z) * u.extra.y);
    } else {
      base = pow(clamp(1.0 - r, 0.0, 1.0), u.extra.y);
    }
    let window = smoothstep(1.0, 0.90, r);
    return vec4<f32>(u.color.rgb * (u.extra.x * base * window), 1.0);
  }
  if (mode == 4u) {
    return vec4<f32>(sky_dome(in.opos.xy), 1.0);
  }
  if (mode == 7u) {
    // Star billboard: a tight gaussian core with a faint skirt. The flux
    // (normal.x) is the calibrated HDR peak; bloom gives bright stars
    // their presence. Twinkle only inside an atmosphere — scintillation
    // is an atmospheric effect and doubles as an arrival cue; in vacuum
    // the field is rock-steady.
    let r2 = dot(in.opos.xy, in.opos.xy);
    // No +0.5 here: packed rgb ints are exactly representable in f32,
    // and adding 0.5 above 2^23 creates a round-to-even tie that bumps
    // the integer — wrapping a 255 blue byte to 0 (blue-white stars
    // rendered yellow until this was found the hard way).
    let pack = u32(in.weights.x);
    let tint = vec3<f32>(f32(pack >> 16u), f32((pack >> 8u) & 255u),
                         f32(pack & 255u)) / 255.0;
    var flux = in.normal.x;
    let atmo_depth = clamp(1.0 - frame.cam_fwd.w, 0.0, 1.0);
    if (atmo_depth > 0.0) {
      let tw = sin(time * (7.0 + in.weights.y * 9.0) + in.weights.y * 251.0) *
               sin(time * 13.7 + in.weights.y * 617.0);
      flux *= 1.0 - 0.45 * atmo_depth * (0.5 + 0.5 * tw);
    }
    let shape = exp(-r2 * 9.0) + exp(-r2 * 2.2) * 0.06;
    return vec4<f32>(tint * flux * shape, 1.0);
  }
  if (mode == 6u) {
    // Textured planet impostor: albedo from the material map, shading
    // normal from the height-map gradient (derive, don't store —
    // extra.y = slope scale), lit like terrain.
    let dir = normalize(in.opos);
    let fuv = cube_face_uv(dir);
    let layer = i32(fuv.z);
    let face = u32(fuv.z + 0.5);
    let alb4 = textureSampleLevel(planet_material, planet_sampler, fuv.xy, layer, 0.0);
    let alb = alb4.rgb;
    let texel = 1.0 / f32(textureDimensions(planet_height).x);
    let du = vec2<f32>(texel, 0.0);
    let dv = vec2<f32>(0.0, texel);
    let hx1 = textureSampleLevel(planet_height, planet_sampler, fuv.xy + du, layer, 0.0).r;
    let hx0 = textureSampleLevel(planet_height, planet_sampler, fuv.xy - du, layer, 0.0).r;
    let hy1 = textureSampleLevel(planet_height, planet_sampler, fuv.xy + dv, layer, 0.0).r;
    let hy0 = textureSampleLevel(planet_height, planet_sampler, fuv.xy - dv, layer, 0.0).r;
    let au = cube_axis_u(face);
    let av = cube_axis_v(face);
    let tu = normalize(au - dir * dot(au, dir));
    let tv = normalize(av - dir * dot(av, dir));
    var n = normalize(dir - tu * ((hx1 - hx0) * u.extra.y) -
                      tv * ((hy1 - hy0) * u.extra.y));
    // Per-item light: aux.xyz = body->star unit direction (a sibling
    // planet is NOT lit from the anchor's sun direction).
    let light = normalize(u.aux.xyz);
    let ndl = max(dot(n, light), 0.0);
    let wrap = max((dot(n, light) + 0.08) / 1.08, 0.0);
    // Two-scale modulation in the planet-local frame, like the terrain
    // path — breaks up flat biome fills at texture resolution.
    let mo = 0.6 * vnoise(dir * 220.0) + 0.4 * vnoise(dir * 47.0);
    let base6 = alb * (0.84 + 0.24 * mo);
    var c6 = base6 * (0.02 + 1.08 * mix(ndl, wrap, 0.35)) * frame.sun_color.rgb;
    c6 += base6 * max(dot(n, -light), 0.0) * vec3<f32>(0.05, 0.07, 0.12);
    // Settlement lights (T0020): the bake's alpha is the night-light
    // mask; warm sodium glow that fades in as the sun sets over it.
    let night6 = 1.0 - smoothstep(-0.05, 0.12, dot(n, light));
    c6 += vec3<f32>(1.0, 0.72, 0.38) * alb4.a * night6 * 0.06;
    return vec4<f32>(c6, 1.0);
  }
  if (mode == 1u) {
    let c = star_surface(in.opos, u.color.rgb, normalize(u.aux.xyz), u.aux.w,
                         u.extra.x, time);
    return vec4<f32>(min(c, vec3<f32>(1.0)), 1.0);
  }
  if (mode == 2u) {
    let c = corona(in.opos.xy, u.color.rgb, u.aux.w, u.extra.x, u.extra.y, u.extra.z,
                   time);
    return vec4<f32>(c, 1.0);
  }
  if (mode == 0u && u.color.a > 0.001) {
    // Unlit solid color; alpha passes through (blended pipeline only).
    return vec4<f32>(u.color.rgb, u.color.a);
  }
  // mode 5 falls through to the lit path below with color.a as alpha
  // (lit translucent surfaces — the sea shell).
  // Lit terrain: directional sun + a cool sky/bounce fill from the
  // opposite hemisphere so the night side stays readable and the
  // terminator picks up a blue-hour cast.
  let light = normalize(frame.sun_dir.xyz);
  var n = normalize(in.normal);
  // From orbit, pull the shading normal toward the analytic sphere
  // radial: per-chunk gradient normals disagree slightly across chunk
  // borders, which reads as a quad grid at distance. aux.xyz carries the
  // mesh's translation (camera-relative), so opos + aux is the fragment
  // in camera-relative world space.
  if (frame.planet_center.w > 0.001) {
    let radial = normalize(in.opos + u.aux.xyz - frame.planet_center.xyz);
    n = normalize(mix(n, radial, frame.planet_center.w));
  }
  // Albedo: material library when the vertex carries materials, else the
  // default terrain material or the item's rgb override (e.g. the
  // ocean-blue sea-level impostor).
  var base = vec3<f32>(0.55, 0.52, 0.45);
  if (u.color.r + u.color.g + u.color.b > 0.001) {
    base = u.color.rgb;
  }
  var rough = 0.85;
  var ao = 1.0;
  var emissive = vec3<f32>(0.0);
  var ndl = max(dot(n, light), 0.0);
  var wrap = max((dot(n, light) + 0.08) / 1.08, 0.0);
  let n_geo = n;
  // T0021: cascaded shadows and screen-space occlusion from the city
  // passes (both fall back to 1 when the passes did not run).
  let rc_world = in.opos + u.aux.xyz;
  let rc_view_z = dot(frame.cam_fwd.xyz, rc_world);
  let shadow = rc_shadow_factor(rc_world, n_geo, rc_view_z, ndl, in.pos.xy);
  var ao_ss = 1.0;
  if (cfr.params2.z > 0.5) {
    // Only where this surface is the one the prepass saw (items outside
    // the prepass, or hidden by ones that were, keep full ambient).
    let d_pre = textureLoad(rc_depth_pre, vec2<i32>(in.pos.xy), 0);
    if (abs(d_pre - in.pos.z) < 0.02 * in.pos.z) {
      ao_ss = pow(textureSample(rc_ao_tex, rc_clamp_samp, in.pos.xy * cfr.screen.zw).r, cfr.params.y);
    }
  }
  // Debug views shared with the city shader (3 ao, 4 shadow).
  let rc_dbg = i32(cfr.jitter.z + 0.5);
  if (rc_dbg == 3) { return vec4<f32>(vec3<f32>(ao_ss), 1.0); }
  if (rc_dbg == 4) { return vec4<f32>(vec3<f32>(shadow), 1.0); }
  let p0 = i32(u.palette.x + 0.5);
  if (p0 > 0) {
    // Normalised palette weights (four materials per chunk, T0019).
    var wv = max(in.weights, vec4<f32>(0.0));
    let wsum = wv.x + wv.y + wv.z + wv.w;
    wv = select(vec4<f32>(1.0, 0.0, 0.0, 0.0), wv / wsum, wsum > 1.0e-5);
    let ids = vec4<i32>(p0, i32(u.palette.y + 0.5), i32(u.palette.z + 0.5), i32(u.palette.w + 0.5));
    var ready = true;
    for (var k = 0; k < 4; k++) {
      if (wv[k] > 0.004 && ids[k] > 0 && mats.b[ids[k]].w < 0.5) { ready = false; }
    }
    // Planet-local position for km-scale variation (f32 is fine there)
    // and the precise chunk-local + period-offset position for tiling.
    let world = in.opos + u.aux.xyz - frame.planet_center.xyz;
    let macro_v = 0.86 + 0.20 * vnoise(world * 0.022) + 0.10 * vnoise(world * 0.0016);
    if (ready) {
      let p = in.opos + u.extra.xyz;
      let dpx = dpdx(p);
      let dpy = dpdy(p);
      let dist = length(in.opos + u.aux.xyz);
      let coarse_w = 0.18 + 0.55 * smoothstep(40.0, 900.0, dist);
      // Biplanar projection (Quilez): the two planes the normal faces most.
      let an = abs(n);
      var ma = vec3<i32>(0, 1, 2);
      if (an.y > an.x && an.y > an.z) { ma = vec3<i32>(1, 2, 0); }
      else if (an.z > an.x && an.z > an.y) { ma = vec3<i32>(2, 0, 1); }
      var mi = vec3<i32>(0, 1, 2);
      if (an.y < an.x && an.y < an.z) { mi = vec3<i32>(1, 2, 0); }
      else if (an.z < an.x && an.z < an.y) { mi = vec3<i32>(2, 0, 1); }
      let me = vec3<i32>(3, 3, 3) - mi - ma;
      let uv1 = vec2<f32>(p[ma.y], p[ma.z]);
      let uv2 = vec2<f32>(p[me.y], p[me.z]);
      let dx1 = vec2<f32>(dpx[ma.y], dpx[ma.z]);
      let dy1 = vec2<f32>(dpy[ma.y], dpy[ma.z]);
      let dx2 = vec2<f32>(dpx[me.y], dpx[me.z]);
      let dy2 = vec2<f32>(dpy[me.y], dpy[me.z]);
      var pw = vec2<f32>(an[ma.x], an[me.x]);
      pw = clamp((pw - 0.5773) / (1.0 - 0.5773), vec2<f32>(0.0), vec2<f32>(1.0));
      pw = pw * pw * pw;
      pw = pw / max(pw.x + pw.y, 1.0e-4);
      // Per plane: sample every palette material with a visible weight,
      // height-blend them (taller texel wins inside a soft band, the
      // band dithered by the vertex weights themselves), tint each with
      // its own planet pigment.
      var acc = array<TileSample, 2>();
      for (var plane = 0; plane < 2; plane++) {
        var uvp = uv1; var dxp = dx1; var dyp = dy1;
        if (plane == 1) { uvp = uv2; dxp = dx2; dyp = dy2; }
        var samples = array<TileSample, 4>();
        var hb = vec4<f32>(-10.0);
        for (var k = 0; k < 4; k++) {
          if (wv[k] > 0.004 && ids[k] > 0) {
            var sm = sample_material(ids[k], uvp, dxp, dyp, coarse_w);
            sm.albedo *= mats.a[ids[k]].rgb;
            samples[k] = sm;
            hb[k] = sm.height * 0.6 + wv[k];
          }
        }
        let hmax = max(max(hb.x, hb.y), max(hb.z, hb.w)) - 0.28;
        var bw = max(hb - vec4<f32>(hmax), vec4<f32>(0.0));
        bw = bw / max(bw.x + bw.y + bw.z + bw.w, 1.0e-5);
        var out: TileSample;
        out.albedo = vec3<f32>(0.0);
        out.height = 0.0;
        out.tnormal = vec2<f32>(0.0);
        out.rough = 0.0;
        out.aux = 0.0;
        for (var k = 0; k < 4; k++) {
          if (bw[k] > 0.0) {
            out.albedo += samples[k].albedo * bw[k];
            out.height += samples[k].height * bw[k];
            out.tnormal += samples[k].tnormal * bw[k];
            out.rough += samples[k].rough * bw[k];
            out.aux += samples[k].aux * bw[k];
          }
        }
        acc[plane] = out;
      }
      let s1 = acc[0];
      let s2 = acc[1];
      base = (s1.albedo * pw.x + s2.albedo * pw.y) * macro_v;
      // Detail normal: whiteout-style per-plane perturbation of the
      // analytic terrain normal, scaled by the material's strength.
      var strength = 0.0;
      var emis_amt = 0.0;
      for (var k = 0; k < 4; k++) {
        if (ids[k] > 0) {
          strength += wv[k] * mats.b[ids[k]].z;
          emis_amt += wv[k] * mats.b[ids[k]].y;
        }
      }
      let tn1 = s1.tnormal * strength * pw.x;
      let tn2 = s2.tnormal * strength * pw.y;
      var pert = vec3<f32>(0.0);
      pert[ma.y] += tn1.x * sign(n[ma.x]);
      pert[ma.z] += tn1.y;
      pert[me.y] += tn2.x * sign(n[me.x]);
      pert[me.z] += tn2.y;
      n = normalize(n + pert);
      rough = clamp(s1.rough * pw.x + s2.rough * pw.y, 0.05, 1.0);
      let aux_v = s1.aux * pw.x + s2.aux * pw.y;
      if (emis_amt > 0.0) {
        // Lit windows and glowing crusts: a night-light level (a lit
        // window is far dimmer than sunlit ground), fading in as the sun
        // sets over the planet surface below the fragment.
        let dusk = 1.0 - smoothstep(-0.05, 0.15, dot(n_geo, light));
        emissive = aux_v * emis_amt * (s1.albedo * pw.x + s2.albedo * pw.y) * (0.003 + 0.03 * dusk);
      } else {
        ao = 0.35 + 0.65 * aux_v;
      }
      ndl = max(dot(n, light), 0.0);
      wrap = max((dot(n, light) + 0.08) / 1.08, 0.0);
    } else {
      // No library yet: the table's mean colours (sRGB, decoded here).
      var mean = vec3<f32>(0.0);
      var rsum = 0.0;
      for (var k = 0; k < 4; k++) {
        if (ids[k] > 0) {
          mean += wv[k] * srgb_to_linear(mats.c[ids[k]].rgb) * mats.a[ids[k]].rgb;
          rsum += wv[k] * mats.b[ids[k]].x;
        }
      }
      base = mean * macro_v * (0.94 + 0.12 * vnoise(world * 0.34));
      rough = rsum;
    }
  }
  // Far field (T0019): beyond a few km the per-vertex palettes of coarse
  // chunks would show as seams, so lit terrain fades into the planet's
  // baked far-view albedo (the same cube map the impostor uses — one
  // continuous surface from the ground to orbit). aux.w flags a bound
  // far texture; the detail normal fades with it.
  if (u.aux.w > 0.5 && p0 > 0) {
    let world_far = in.opos + u.aux.xyz - frame.planet_center.xyz;
    let dist_far = length(in.opos + u.aux.xyz);
    let k_far = smoothstep(2500.0, 14000.0, dist_far);
    if (k_far > 0.0) {
      let fuv_far = cube_face_uv(normalize(world_far));
      let far4 = textureSampleLevel(planet_material, planet_sampler, fuv_far.xy,
                                    i32(fuv_far.z), 0.0);
      let far = far4.rgb;
      base = mix(base, far, k_far);
      n = normalize(mix(n, n_geo, k_far));
      ndl = max(dot(n, light), 0.0);
      wrap = max((dot(n, light) + 0.08) / 1.08, 0.0);
      ao = mix(ao, 1.0, k_far);
      // Settlement lights (T0020): the bake's alpha is the night-light
      // mask; the far field carries the same warm glow the impostor does,
      // fading in as the sun sets over it.
      let night_far = 1.0 - smoothstep(-0.05, 0.12, dot(n_geo, light));
      emissive = emissive * (1.0 - k_far) +
                 vec3<f32>(1.0, 0.72, 0.38) * far4.a * night_far * k_far * 0.06;
      rough = mix(rough, 0.9, k_far);
    }
  }
  var color = base * ao * (0.02 + 1.08 * mix(ndl, wrap, 0.35) * shadow) * frame.sun_color.rgb;
  color *= mix(1.0, ao_ss, 0.7);
  let fill = max(dot(n, -light), 0.0);
  color += base * ao * fill * vec3<f32>(0.012, 0.017, 0.03);
  // Specular: GGX-shaped sun highlight driven by the material roughness
  // (wet sand, ice and glossy mats read as such; rock stays matte).
  {
    let view = normalize(-(in.opos + u.aux.xyz));
    let hv = normalize(light + view);
    let ndh = max(dot(n, hv), 0.0);
    let a2 = max(rough * rough * rough * rough, 1.0e-4);
    let dd = ndh * ndh * (a2 - 1.0) + 1.0;
    let dist_ggx = a2 / (3.14159 * dd * dd);
    let f0 = 0.04;
    color += frame.sun_color.rgb * dist_ggx * f0 * ndl * shadow * (0.25 + 0.75 * (1.0 - rough));
  }
  color += emissive;
  // Submerged terrain shades toward deep water by depth (atmo.a carries
  // the sea radius). This keeps the streamed seabed consistent with the
  // opaque ocean impostor — the ocean no longer flips color when chunks
  // arrive or LOD changes.
  if (mode == 0u && frame.atmo.a > 1.0) {
    let frag_r = length(in.opos + u.aux.xyz - frame.planet_center.xyz);
    let depth = frame.atmo.a - frag_r;
    if (depth > 0.0) {
      let absorb = depth / (depth + 30.0);
      let water = vec3<f32>(0.06, 0.20, 0.35) * frame.sun_color.rgb * (0.02 + 0.98 * ndl);
      color = mix(color, water, absorb * 0.92);
    }
  }
  if (mode == 5u) {
    // Water: Fresnel-driven opacity (grazing water is opaque and dark
    // blue — a constant alpha made shallow seas read as bare ground) and
    // a sun glint. aux carries the camera-relative translation, so
    // opos + aux is the fragment's camera-relative position.
    let view = normalize(in.opos + u.aux.xyz);
    let fresnel = pow(1.0 - abs(dot(n, view)), 3.0);
    // The Fresnel term reflects the SKY: it must go dark with the sun
    // (a sunless constant made the night ocean glow under HDR exposure).
    color = mix(color,
                vec3<f32>(0.05, 0.16, 0.30) * frame.sun_color.rgb * (0.03 + 0.97 * wrap),
                fresnel * 0.7);
    let refl = reflect(light * -1.0, n);
    let spec = pow(max(dot(refl, view * -1.0), 0.0), 90.0);
    color += frame.sun_color.rgb * spec * (0.9 * ndl + 0.05);
    let alpha_w = mix(u.color.a, 0.96, fresnel);
    return vec4<f32>(color, alpha_w);
  }
  return vec4<f32>(color, 1.0);
}
)";


// T0018 WP1: the post chain. The scene renders LINEAR HDR into an
// RGBA16F target; this shader owns the single tonemap point. Passes:
// luminance reduction (auto-exposure input), bright extract, separable
// blur (bloom), and the composite: exposure -> Purkinje rod
// desaturation -> bloom add -> ACES. params.a = (exposure, scotopic,
// bloom_amount, threshold); params.b = (texel_x, texel_y, dir_x, dir_y).
constexpr const char* kPostShader = R"(
struct PostParams {
  a: vec4<f32>,
  b: vec4<f32>,
};
@group(0) @binding(0) var src_a: texture_2d<f32>;
@group(0) @binding(1) var src_b: texture_2d<f32>;
@group(0) @binding(2) var samp: sampler;
@group(0) @binding(3) var<uniform> pp: PostParams;

struct FSIn {
  @builtin(position) pos: vec4<f32>,
  @location(0) uv: vec2<f32>,
};

@vertex
fn vs_fullscreen(@builtin(vertex_index) vi: u32) -> FSIn {
  var out: FSIn;
  let x = f32(i32(vi & 1u) * 4 - 1);
  let y = f32(i32(vi >> 1u) * 4 - 1);
  out.pos = vec4<f32>(x, y, 0.0, 1.0);
  out.uv = vec2<f32>(x, -y) * 0.5 + 0.5;
  return out;
}

fn aces_post(x: vec3<f32>) -> vec3<f32> {
  let mapped = (x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14);
  return clamp(mapped, vec3<f32>(0.0), vec3<f32>(1.0));
}

// Log-average luminance of an 8x8 neighbourhood per output pixel (the
// 32x32 target then holds a well-sampled reduction of the whole frame).
@fragment
fn fs_lum(in: FSIn) -> @location(0) vec4<f32> {
  var log_sum = 0.0;
  for (var iy = 0; iy < 8; iy++) {
    for (var ix = 0; ix < 8; ix++) {
      let offset = (vec2<f32>(f32(ix), f32(iy)) - 3.5) * pp.b.xy;
      let c = textureSampleLevel(src_a, samp, in.uv + offset, 0.0).rgb;
      let lum = dot(c, vec3<f32>(0.2126, 0.7152, 0.0722));
      log_sum += log(lum + 1.0e-6);
    }
  }
  return vec4<f32>(exp(log_sum / 64.0), 0.0, 0.0, 1.0);
}

@fragment
fn fs_bright(in: FSIn) -> @location(0) vec4<f32> {
  let c = textureSampleLevel(src_a, samp, in.uv, 0.0).rgb * pp.a.x;
  let bright = max(c - vec3<f32>(pp.a.w), vec3<f32>(0.0));
  return vec4<f32>(bright, 1.0);
}

@fragment
fn fs_blur(in: FSIn) -> @location(0) vec4<f32> {
  let step_uv = pp.b.zw * pp.b.xy;
  var c = textureSampleLevel(src_a, samp, in.uv, 0.0).rgb * 0.227027;
  let w = array<f32, 4>(0.194594, 0.121621, 0.054054, 0.016216);
  for (var i = 1; i <= 4; i++) {
    let offset = step_uv * f32(i) * 1.5;
    c += textureSampleLevel(src_a, samp, in.uv + offset, 0.0).rgb * w[i - 1];
    c += textureSampleLevel(src_a, samp, in.uv - offset, 0.0).rgb * w[i - 1];
  }
  return vec4<f32>(c, 1.0);
}

@fragment
fn fs_composite(in: FSIn) -> @location(0) vec4<f32> {
  let hdr = textureSampleLevel(src_a, samp, in.uv, 0.0).rgb;
  let bloom = textureSampleLevel(src_b, samp, in.uv, 0.0).rgb;
  var c = hdr * pp.a.x + bloom * pp.a.z;
  // Purkinje shift: under scotopic adaptation the rods see luminance
  // only, slightly blue-weighted; cones (colour) hold on only where the
  // EXPOSED brightness is high — bright stars keep their tint, the
  // Milky Way goes grey.
  let rod_lum = dot(c, vec3<f32>(0.15, 0.55, 0.65));
  let rod = rod_lum * vec3<f32>(0.82, 0.95, 1.16);
  // Deliberately WEAK Purkinje (2026-09-01, Sascha): the sky should read
  // like the astrophoto, not the fully rod-limited eye — the band and
  // nebulae keep most of their colour, only the very faintest features
  // drift toward rod grey.
  let cone_keep = smoothstep(0.015, 0.22, dot(c, vec3<f32>(0.2126, 0.7152, 0.0722)));
  c = mix(c, rod, pp.a.y * 0.4 * (1.0 - cone_keep));
  return vec4<f32>(aces_post(c), 1.0);
}
)";

WGPUStringView sv(const char* text) { return WGPUStringView{text, WGPU_STRLEN}; }

std::string to_string(WGPUStringView view) {
  if (view.data == nullptr) {
    return {};
  }
  if (view.length == WGPU_STRLEN) {
    return std::string(view.data);
  }
  return std::string(view.data, view.length);
}

struct AdapterRequest {
  WGPUAdapter adapter = nullptr;
  bool done = false;
  std::string message;
};

struct DeviceRequest {
  WGPUDevice device = nullptr;
  bool done = false;
  std::string message;
};

WGPUSurface create_surface(WGPUInstance instance, GLFWwindow* window,
                           [[maybe_unused]] std::string* error) {
  WGPUSurfaceDescriptor desc{};
#if defined(__linux__)
  const int platform = glfwGetPlatform();
  if (platform == GLFW_PLATFORM_WAYLAND) {
    WGPUSurfaceSourceWaylandSurface source{};
    source.chain.sType = WGPUSType_SurfaceSourceWaylandSurface;
    source.display = glfwGetWaylandDisplay();
    source.surface = glfwGetWaylandWindow(window);
    desc.nextInChain = &source.chain;
    return wgpuInstanceCreateSurface(instance, &desc);
  }
  if (platform == GLFW_PLATFORM_X11) {
    WGPUSurfaceSourceXlibWindow source{};
    source.chain.sType = WGPUSType_SurfaceSourceXlibWindow;
    source.display = glfwGetX11Display();
    source.window = static_cast<std::uint64_t>(glfwGetX11Window(window));
    desc.nextInChain = &source.chain;
    return wgpuInstanceCreateSurface(instance, &desc);
  }
  *error = "unsupported GLFW platform on Linux (need Wayland or X11)";
  return nullptr;
#elif defined(__APPLE__)
  WGPUSurfaceSourceMetalLayer source{};
  source.chain.sType = WGPUSType_SurfaceSourceMetalLayer;
  source.layer = infinityMetalLayerForCocoaWindow(glfwGetCocoaWindow(window));
  desc.nextInChain = &source.chain;
  return wgpuInstanceCreateSurface(instance, &desc);
#elif defined(_WIN32)
  WGPUSurfaceSourceWindowsHWND source{};
  source.chain.sType = WGPUSType_SurfaceSourceWindowsHWND;
  source.hinstance = GetModuleHandle(nullptr);
  source.hwnd = glfwGetWin32Window(window);
  desc.nextInChain = &source.chain;
  return wgpuInstanceCreateSurface(instance, &desc);
#else
  *error = "unsupported platform for surface creation";
  return nullptr;
#endif
}

struct MeshEntry {
  WGPUBuffer buffer = nullptr;
  std::uint32_t vertex_count = 0;
  // City meshes (T0021): indexed, in the 68-byte city vertex layout.
  WGPUBuffer index_buffer = nullptr;
  std::uint32_t index_count = 0;
};

// Column-major 4x4 helpers for the city frame block.
void mat_mul(const float* a, const float* b, float* out) {
  for (int c = 0; c < 4; ++c) {
    for (int r = 0; r < 4; ++r) {
      float sum = 0.0f;
      for (int k = 0; k < 4; ++k) sum += a[k * 4 + r] * b[c * 4 + k];
      out[c * 4 + r] = sum;
    }
  }
}
bool mat_inverse(const float* m, float* out) {
  float inv[16];
  inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
  inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
  inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
  inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
  inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
  inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
  inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
  inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
  inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
  inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
  inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
  inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
  inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
  inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
  inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
  inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
  float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
  if (std::fabs(det) < 1e-30f) {
    for (int i = 0; i < 16; ++i) out[i] = (i % 5 == 0) ? 1.0f : 0.0f;
    return false;
  }
  det = 1.0f / det;
  for (int i = 0; i < 16; ++i) out[i] = inv[i] * det;
  return true;
}
std::uint16_t float_to_half(float f) {
  std::uint32_t x;
  std::memcpy(&x, &f, 4);
  const std::uint32_t sign = (x >> 16) & 0x8000u;
  std::int32_t exp = static_cast<std::int32_t>((x >> 23) & 0xffu) - 127 + 15;
  std::uint32_t mant = x & 0x7fffffu;
  if (exp <= 0) return static_cast<std::uint16_t>(sign);
  if (exp >= 31) return static_cast<std::uint16_t>(sign | 0x7c00u);
  return static_cast<std::uint16_t>(sign | (static_cast<std::uint32_t>(exp) << 10) | (mant >> 13));
}

}  // namespace

struct Rhi::Impl {
  WGPUInstance instance = nullptr;
  WGPUSurface surface = nullptr;
  WGPUAdapter adapter = nullptr;
  WGPUDevice device = nullptr;
  WGPUQueue queue = nullptr;
  WGPUTextureFormat format = WGPUTextureFormat_Undefined;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::string adapter_info;

  // Mesh pipeline state (created on demand).
  WGPURenderPipeline mesh_pipeline = nullptr;
  WGPURenderPipeline mesh_pipeline_blend = nullptr;
  WGPURenderPipeline mesh_pipeline_add = nullptr;
  WGPUBindGroupLayout bind_layout = nullptr;
  WGPUBindGroup bind_group = nullptr;
  WGPUBuffer uniform_buffer = nullptr;
  WGPUBuffer frame_buffer = nullptr;

  // Planet cube-map textures (T0016): group 1 = height array + material
  // array + shared sampler. Items without a texture bind the 1x1 default
  // so every pipeline can share one layout.
  struct PlanetTexEntry {
    WGPUTexture height = nullptr;
    WGPUTexture material = nullptr;
    WGPUTextureView height_view = nullptr;
    WGPUTextureView material_view = nullptr;
    WGPUBindGroup group = nullptr;
    std::uint32_t size = 0;
  };
  WGPUBindGroupLayout tex_layout = nullptr;
  WGPUSampler planet_sampler = nullptr;

  // Surface material library (T0019): group 2 = albedo array + normal
  // array + repeat sampler + material table uniform.
  struct MaterialLib {
    WGPUTexture albedo = nullptr;
    WGPUTexture normal = nullptr;
    WGPUTextureView albedo_view = nullptr;
    WGPUTextureView normal_view = nullptr;
    WGPUBindGroup group = nullptr;
    std::uint32_t size = 0;
    std::uint32_t layers = 0;
    std::uint32_t mips = 1;
  };
  static constexpr std::uint32_t kMaterialSlots = 64;
  static constexpr std::uint64_t kMaterialTableSize = kMaterialSlots * 3 * 16;
  WGPUBindGroupLayout mat_layout = nullptr;
  WGPUSampler material_sampler = nullptr;
  WGPUBuffer material_table = nullptr;
  MaterialLib material_lib;
  float material_cpu[kMaterialSlots * 12] = {};
  bool material_ready[kMaterialSlots] = {};
  bool material_dirty = true;

  MaterialLib make_material_lib(std::uint32_t size, std::uint32_t layers) {
    MaterialLib lib;
    lib.size = size;
    lib.layers = layers;
    lib.mips = 1;
    while ((size >> lib.mips) >= 1U && lib.mips < 16) {
      ++lib.mips;
    }
    WGPUTextureDescriptor desc{};
    desc.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
    desc.dimension = WGPUTextureDimension_2D;
    desc.size = WGPUExtent3D{size, size, layers};
    desc.mipLevelCount = lib.mips;
    desc.sampleCount = 1;
    // Albedo is sRGB-encoded (photographs and generated colours alike):
    // the sRGB format decodes to linear in the sampler, so the HDR chain
    // lights physically plausible albedos. Normal/roughness/ao stay linear.
    desc.format = WGPUTextureFormat_RGBA8UnormSrgb;
    desc.label = sv("material-albedo");
    lib.albedo = wgpuDeviceCreateTexture(device, &desc);
    desc.format = WGPUTextureFormat_RGBA8Unorm;
    desc.label = sv("material-normal");
    lib.normal = wgpuDeviceCreateTexture(device, &desc);
    WGPUTextureViewDescriptor view_desc{};
    view_desc.dimension = WGPUTextureViewDimension_2DArray;
    view_desc.baseArrayLayer = 0;
    view_desc.arrayLayerCount = layers;
    view_desc.baseMipLevel = 0;
    view_desc.mipLevelCount = lib.mips;
    view_desc.format = WGPUTextureFormat_RGBA8UnormSrgb;
    view_desc.aspect = WGPUTextureAspect_All;
    lib.albedo_view = wgpuTextureCreateView(lib.albedo, &view_desc);
    view_desc.format = WGPUTextureFormat_RGBA8Unorm;
    lib.normal_view = wgpuTextureCreateView(lib.normal, &view_desc);
    WGPUBindGroupEntry entries[4] = {};
    entries[0].binding = 0;
    entries[0].textureView = lib.albedo_view;
    entries[1].binding = 1;
    entries[1].textureView = lib.normal_view;
    entries[2].binding = 2;
    entries[2].sampler = material_sampler;
    entries[3].binding = 3;
    entries[3].buffer = material_table;
    entries[3].offset = 0;
    entries[3].size = kMaterialTableSize;
    WGPUBindGroupDescriptor group_desc{};
    group_desc.layout = mat_layout;
    group_desc.entryCount = 4;
    group_desc.entries = entries;
    lib.group = wgpuDeviceCreateBindGroup(device, &group_desc);
    return lib;
  }

  void release_material_lib(MaterialLib& lib) {
    if (lib.group != nullptr) wgpuBindGroupRelease(lib.group);
    if (lib.albedo_view != nullptr) wgpuTextureViewRelease(lib.albedo_view);
    if (lib.normal_view != nullptr) wgpuTextureViewRelease(lib.normal_view);
    if (lib.albedo != nullptr) wgpuTextureRelease(lib.albedo);
    if (lib.normal != nullptr) wgpuTextureRelease(lib.normal);
    lib = MaterialLib{};
  }

  // Uploads one layer with a CPU box-filtered mip chain.
  void write_material_layer(WGPUTexture texture, std::uint32_t layer, const std::uint8_t* rgba) {
    std::vector<std::uint8_t> level(rgba, rgba + static_cast<std::size_t>(material_lib.size) *
                                                    material_lib.size * 4);
    std::uint32_t size = material_lib.size;
    for (std::uint32_t mip = 0; mip < material_lib.mips; ++mip) {
      WGPUTexelCopyTextureInfo dst{};
      dst.texture = texture;
      dst.mipLevel = mip;
      dst.origin = WGPUOrigin3D{0, 0, layer};
      dst.aspect = WGPUTextureAspect_All;
      WGPUTexelCopyBufferLayout layout{};
      layout.offset = 0;
      layout.bytesPerRow = size * 4;
      layout.rowsPerImage = size;
      const WGPUExtent3D extent{size, size, 1};
      wgpuQueueWriteTexture(queue, &dst, level.data(), level.size(), &layout, &extent);
      if (size == 1) {
        break;
      }
      const std::uint32_t half = size / 2;
      std::vector<std::uint8_t> next(static_cast<std::size_t>(half) * half * 4);
      for (std::uint32_t y = 0; y < half; ++y) {
        for (std::uint32_t x = 0; x < half; ++x) {
          for (std::uint32_t c = 0; c < 4; ++c) {
            const std::uint32_t sum =
                level[((2 * y) * size + 2 * x) * 4 + c] + level[((2 * y) * size + 2 * x + 1) * 4 + c] +
                level[((2 * y + 1) * size + 2 * x) * 4 + c] +
                level[((2 * y + 1) * size + 2 * x + 1) * 4 + c];
            next[(y * half + x) * 4 + c] = static_cast<std::uint8_t>((sum + 2) / 4);
          }
        }
      }
      level.swap(next);
      size = half;
    }
  }

  // --- T0018 WP1: HDR post chain ---------------------------------------
  static constexpr WGPUTextureFormat kHdrFormat = WGPUTextureFormat_RGBA16Float;
  static constexpr std::uint32_t kLumSize = 32;
  WGPURenderPipeline overlay_pipeline = nullptr;        // UI after tonemap
  WGPURenderPipeline overlay_pipeline_blend = nullptr;
  WGPURenderPipeline overlay_pipeline_add = nullptr;
  WGPUBindGroupLayout post_layout = nullptr;
  WGPUSampler post_sampler = nullptr;
  WGPUBuffer post_uniforms = nullptr;                   // 8 x 256-byte slots
  WGPURenderPipeline lum_pipeline = nullptr;
  WGPURenderPipeline bright_pipeline = nullptr;
  WGPURenderPipeline blur_pipeline = nullptr;
  WGPURenderPipeline composite_pipeline = nullptr;
  WGPUTexture lum_texture = nullptr;
  WGPUTextureView lum_view = nullptr;
  WGPUBuffer lum_readback = nullptr;
  bool lum_map_inflight = false;
  bool lum_map_ready = false;
  // Eye state: smoothed exposure with asymmetric time constants (glare
  // fast, dark adaptation slow) and the scotopic (rod) fraction.
  float exposure = 1.0f;
  float scotopic = 0.0f;
  float avg_luminance = 0.25f;
  float exposure_last_time = -1.0f;

  struct PostSet {
    WGPUTexture hdr = nullptr;
    WGPUTextureView hdr_view = nullptr;
    WGPUTexture bloom[2] = {nullptr, nullptr};
    WGPUTextureView bloom_view[2] = {nullptr, nullptr};
    // grp_hdr_only: t0 = t1 = hdr (bright/lum — must not reference the
    // bloom target it writes); grp_hdr: t0 = hdr, t1 = bloom[0]
    // (composite); grp_b0/b1: the blur legs.
    WGPUBindGroup grp_hdr_only = nullptr;
    WGPUBindGroup grp_hdr = nullptr;
    WGPUBindGroup grp_b0 = nullptr;
    WGPUBindGroup grp_b1 = nullptr;
    std::uint32_t w = 0;
    std::uint32_t h = 0;
  };
  PostSet post_main;
  PostSet post_rec;

  void release_post_set(PostSet& set) {
    if (set.grp_hdr_only != nullptr) wgpuBindGroupRelease(set.grp_hdr_only);
    if (set.grp_hdr != nullptr) wgpuBindGroupRelease(set.grp_hdr);
    if (set.grp_b0 != nullptr) wgpuBindGroupRelease(set.grp_b0);
    if (set.grp_b1 != nullptr) wgpuBindGroupRelease(set.grp_b1);
    for (int i = 0; i < 2; ++i) {
      if (set.bloom_view[i] != nullptr) wgpuTextureViewRelease(set.bloom_view[i]);
      if (set.bloom[i] != nullptr) wgpuTextureRelease(set.bloom[i]);
    }
    if (set.hdr_view != nullptr) wgpuTextureViewRelease(set.hdr_view);
    if (set.hdr != nullptr) wgpuTextureRelease(set.hdr);
    set = PostSet{};
  }

  WGPUBindGroup make_post_group(WGPUTextureView a, WGPUTextureView b) {
    WGPUBindGroupEntry entries[4] = {};
    entries[0].binding = 0;
    entries[0].textureView = a;
    entries[1].binding = 1;
    entries[1].textureView = b;
    entries[2].binding = 2;
    entries[2].sampler = post_sampler;
    entries[3].binding = 3;
    entries[3].buffer = post_uniforms;
    entries[3].size = 32;  // two vec4s
    WGPUBindGroupDescriptor desc{};
    desc.layout = post_layout;
    desc.entryCount = 4;
    desc.entries = entries;
    return wgpuDeviceCreateBindGroup(device, &desc);
  }

  void ensure_post_set(PostSet& set, std::uint32_t w, std::uint32_t h) {
    if (set.hdr != nullptr && set.w == w && set.h == h) {
      return;
    }
    release_post_set(set);
    set.w = w;
    set.h = h;
    WGPUTextureDescriptor desc{};
    desc.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding;
    desc.dimension = WGPUTextureDimension_2D;
    desc.mipLevelCount = 1;
    desc.sampleCount = 1;
    desc.format = kHdrFormat;
    desc.label = sv("post-hdr");
    desc.size = WGPUExtent3D{w, h, 1};
    set.hdr = wgpuDeviceCreateTexture(device, &desc);
    set.hdr_view = wgpuTextureCreateView(set.hdr, nullptr);
    desc.label = sv("post-bloom");
    desc.size = WGPUExtent3D{w / 2 > 0 ? w / 2 : 1, h / 2 > 0 ? h / 2 : 1, 1};
    for (int i = 0; i < 2; ++i) {
      set.bloom[i] = wgpuDeviceCreateTexture(device, &desc);
      set.bloom_view[i] = wgpuTextureCreateView(set.bloom[i], nullptr);
    }
    set.grp_hdr_only = make_post_group(set.hdr_view, set.hdr_view);
    set.grp_hdr = make_post_group(set.hdr_view, set.bloom_view[0]);
    set.grp_b0 = make_post_group(set.bloom_view[0], set.bloom_view[0]);
    set.grp_b1 = make_post_group(set.bloom_view[1], set.bloom_view[1]);
  }
  PlanetTexEntry default_tex;
  std::unordered_map<std::uint32_t, PlanetTexEntry> planet_textures;
  std::uint32_t next_planet_tex_id = 1;

  PlanetTexEntry make_planet_tex(std::uint32_t face_size) {
    PlanetTexEntry entry;
    entry.size = face_size;
    WGPUTextureDescriptor desc{};
    desc.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
    desc.dimension = WGPUTextureDimension_2D;
    desc.size = WGPUExtent3D{face_size, face_size, 6};
    desc.mipLevelCount = 1;
    desc.sampleCount = 1;
    desc.label = sv("planet-height");
    desc.format = WGPUTextureFormat_R16Float;
    entry.height = wgpuDeviceCreateTexture(device, &desc);
    desc.label = sv("planet-material");
    desc.format = WGPUTextureFormat_RGBA8Unorm;
    entry.material = wgpuDeviceCreateTexture(device, &desc);
    WGPUTextureViewDescriptor view_desc{};
    view_desc.dimension = WGPUTextureViewDimension_2DArray;
    view_desc.baseArrayLayer = 0;
    view_desc.arrayLayerCount = 6;
    view_desc.baseMipLevel = 0;
    view_desc.mipLevelCount = 1;
    view_desc.format = WGPUTextureFormat_R16Float;
    view_desc.aspect = WGPUTextureAspect_All;
    entry.height_view = wgpuTextureCreateView(entry.height, &view_desc);
    view_desc.format = WGPUTextureFormat_RGBA8Unorm;
    entry.material_view = wgpuTextureCreateView(entry.material, &view_desc);
    WGPUBindGroupEntry entries[3] = {};
    entries[0].binding = 0;
    entries[0].textureView = entry.height_view;
    entries[1].binding = 1;
    entries[1].textureView = entry.material_view;
    entries[2].binding = 2;
    entries[2].sampler = planet_sampler;
    WGPUBindGroupDescriptor group_desc{};
    group_desc.layout = tex_layout;
    group_desc.entryCount = 3;
    group_desc.entries = entries;
    entry.group = wgpuDeviceCreateBindGroup(device, &group_desc);
    return entry;
  }

  void release_planet_tex(PlanetTexEntry& entry) {
    if (entry.group != nullptr) wgpuBindGroupRelease(entry.group);
    if (entry.height_view != nullptr) wgpuTextureViewRelease(entry.height_view);
    if (entry.material_view != nullptr) wgpuTextureViewRelease(entry.material_view);
    if (entry.height != nullptr) wgpuTextureRelease(entry.height);
    if (entry.material != nullptr) wgpuTextureRelease(entry.material);
    entry = PlanetTexEntry{};
  }
  WGPUTexture depth_texture = nullptr;
  WGPUTextureView depth_view = nullptr;
  std::unordered_map<std::uint32_t, MeshEntry> meshes;
  std::uint32_t next_mesh_id = 1;
  std::string capture_path;  // non-empty: capture on the next render_frame

  // --- debug frame recorder ---------------------------------------------
  // Reduced-resolution re-renders of the scene, every kRecInterval-th
  // frame, into an in-memory ring of the last kRingSeconds. A trigger
  // dumps the ring to disk and keeps writing frames until rec_until.
  static constexpr std::uint32_t kRecW = 640;
  static constexpr std::uint32_t kRecH = 360;
  static constexpr double kRingSeconds = 3.0;
  bool ring_enabled = false;
  float last_ring_time = -1.0f;
  WGPUTexture rec_color = nullptr;
  WGPUTextureView rec_color_view = nullptr;
  WGPUTexture rec_depth = nullptr;
  WGPUTextureView rec_depth_view = nullptr;
  WGPUBuffer rec_buffer = nullptr;
  std::uint32_t rec_bpr = 0;
  struct RingFrame {
    float time_s;
    std::vector<std::uint8_t> rgb;  // kRecW * kRecH * 3, tight
  };
  std::deque<RingFrame> ring;
  std::uint64_t frame_counter = 0;
  float last_frame_time = 0.0f;
  std::string rec_dir;      // active triggered-recording directory
  double rec_until = -1e30;  // future-record until this frame time
  int rec_seq_index = 0;

  void ensure_recorder_targets() {
    if (rec_color != nullptr) {
      return;
    }
    WGPUTextureDescriptor color_desc{};
    color_desc.label = sv("rec-color");
    color_desc.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc;
    color_desc.dimension = WGPUTextureDimension_2D;
    color_desc.size = WGPUExtent3D{kRecW, kRecH, 1};
    color_desc.format = format;
    color_desc.mipLevelCount = 1;
    color_desc.sampleCount = 1;
    rec_color = wgpuDeviceCreateTexture(device, &color_desc);
    rec_color_view = wgpuTextureCreateView(rec_color, nullptr);
    WGPUTextureDescriptor depth_desc{};
    depth_desc.label = sv("rec-depth");
    depth_desc.usage = WGPUTextureUsage_RenderAttachment;
    depth_desc.dimension = WGPUTextureDimension_2D;
    depth_desc.size = WGPUExtent3D{kRecW, kRecH, 1};
    depth_desc.format = WGPUTextureFormat_Depth32Float;
    depth_desc.mipLevelCount = 1;
    depth_desc.sampleCount = 1;
    rec_depth = wgpuDeviceCreateTexture(device, &depth_desc);
    rec_depth_view = wgpuTextureCreateView(rec_depth, nullptr);
    rec_bpr = ((kRecW * 4 + 255) / 256) * 256;
    WGPUBufferDescriptor buffer_desc{};
    buffer_desc.label = sv("rec-readback");
    buffer_desc.usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst;
    buffer_desc.size = static_cast<std::uint64_t>(rec_bpr) * kRecH;
    rec_buffer = wgpuDeviceCreateBuffer(device, &buffer_desc);
  }

  void write_ppm(const std::string& path, std::uint32_t w, std::uint32_t h,
                 const std::uint8_t* rgb) {
    const std::unique_ptr<std::FILE, int (*)(std::FILE*)> file(std::fopen(path.c_str(), "wb"), &std::fclose);
    if (file == nullptr) {
      std::fprintf(stderr, "recorder: FAILED to open %s\n", path.c_str());
      return;
    }
    std::fprintf(file.get(), "P6\n%u %u\n255\n", w, h);
    std::fwrite(rgb, 1, static_cast<std::size_t>(w) * h * 3, file.get());
  }

  void append_time_index(float time_s) {
    const std::unique_ptr<std::FILE, int (*)(std::FILE*)> file(
        std::fopen((rec_dir + "/times.txt").c_str(), "a"), &std::fclose);
    if (file != nullptr) {
      std::fprintf(file.get(), "seq-%04d %.4f\n", rec_seq_index, time_s);
    }
  }

  void emit_sequence_frame(float time_s, const std::uint8_t* rgb) {
    char name[32];
    std::snprintf(name, sizeof(name), "/seq-%04d.ppm", rec_seq_index);
    write_ppm(rec_dir + name, kRecW, kRecH, rgb);
    append_time_index(time_s);
    ++rec_seq_index;
  }

  void configure_surface() {
    WGPUSurfaceConfiguration config{};
    config.device = device;
    config.format = format;
    config.usage = WGPUTextureUsage_RenderAttachment;
    config.width = width;
    config.height = height;
    config.alphaMode = WGPUCompositeAlphaMode_Auto;
    config.presentMode = WGPUPresentMode_Fifo;
    wgpuSurfaceConfigure(surface, &config);
    recreate_depth();
  }

  void recreate_depth() {
    if (depth_view != nullptr) {
      wgpuTextureViewRelease(depth_view);
      depth_view = nullptr;
    }
    if (depth_texture != nullptr) {
      wgpuTextureRelease(depth_texture);
      depth_texture = nullptr;
    }
    WGPUTextureDescriptor desc{};
    desc.label = sv("depth");
    desc.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding |
                 WGPUTextureUsage_CopySrc;
    desc.dimension = WGPUTextureDimension_2D;
    desc.size = WGPUExtent3D{width, height, 1};
    desc.format = WGPUTextureFormat_Depth32Float;
    desc.mipLevelCount = 1;
    desc.sampleCount = 1;
    depth_texture = wgpuDeviceCreateTexture(device, &desc);
    depth_view = wgpuTextureCreateView(depth_texture, nullptr);
  }

  void ensure_city_resources_early();

  void ensure_mesh_pipeline() {
    if (mesh_pipeline != nullptr) {
      return;
    }
    ensure_city_resources_early();
    WGPUShaderSourceWGSL wgsl{};
    wgsl.chain.sType = WGPUSType_ShaderSourceWGSL;
    wgsl.code = sv(kMeshShader);
    WGPUShaderModuleDescriptor module_desc{};
    module_desc.nextInChain = &wgsl.chain;
    WGPUShaderModule module = wgpuDeviceCreateShaderModule(device, &module_desc);

    WGPUBindGroupLayoutEntry layout_entries[2] = {};
    layout_entries[0].binding = 0;
    layout_entries[0].visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
    layout_entries[0].buffer.type = WGPUBufferBindingType_Uniform;
    layout_entries[0].buffer.hasDynamicOffset = 1U;
    layout_entries[0].buffer.minBindingSize = kItemUniformSize;
    layout_entries[1].binding = 1;
    layout_entries[1].visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
    layout_entries[1].buffer.type = WGPUBufferBindingType_Uniform;
    layout_entries[1].buffer.hasDynamicOffset = 0U;
    layout_entries[1].buffer.minBindingSize = kFrameUniformSize;
    WGPUBindGroupLayoutDescriptor layout_desc{};
    layout_desc.entryCount = 2;
    layout_desc.entries = layout_entries;
    bind_layout = wgpuDeviceCreateBindGroupLayout(device, &layout_desc);

    // Group 1: planet cube-map pair + sampler (T0016). The height map is
    // sampled in the VERTEX stage (displacement).
    WGPUBindGroupLayoutEntry tex_entries[3] = {};
    tex_entries[0].binding = 0;
    tex_entries[0].visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
    tex_entries[0].texture.sampleType = WGPUTextureSampleType_Float;
    tex_entries[0].texture.viewDimension = WGPUTextureViewDimension_2DArray;
    tex_entries[1].binding = 1;
    tex_entries[1].visibility = WGPUShaderStage_Fragment;
    tex_entries[1].texture.sampleType = WGPUTextureSampleType_Float;
    tex_entries[1].texture.viewDimension = WGPUTextureViewDimension_2DArray;
    tex_entries[2].binding = 2;
    tex_entries[2].visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
    tex_entries[2].sampler.type = WGPUSamplerBindingType_Filtering;
    WGPUBindGroupLayoutDescriptor tex_layout_desc{};
    tex_layout_desc.entryCount = 3;
    tex_layout_desc.entries = tex_entries;
    tex_layout = wgpuDeviceCreateBindGroupLayout(device, &tex_layout_desc);

    WGPUSamplerDescriptor sampler_desc{};
    sampler_desc.label = sv("planet-sampler");
    sampler_desc.addressModeU = WGPUAddressMode_ClampToEdge;
    sampler_desc.addressModeV = WGPUAddressMode_ClampToEdge;
    sampler_desc.addressModeW = WGPUAddressMode_ClampToEdge;
    sampler_desc.magFilter = WGPUFilterMode_Linear;
    sampler_desc.minFilter = WGPUFilterMode_Linear;
    sampler_desc.mipmapFilter = WGPUMipmapFilterMode_Nearest;
    sampler_desc.lodMinClamp = 0.0f;
    sampler_desc.lodMaxClamp = 32.0f;
    sampler_desc.maxAnisotropy = 1;
    planet_sampler = wgpuDeviceCreateSampler(device, &sampler_desc);

    // Group 2: the surface material library (T0019).
    WGPUBindGroupLayoutEntry mat_entries[4] = {};
    mat_entries[0].binding = 0;
    mat_entries[0].visibility = WGPUShaderStage_Fragment;
    mat_entries[0].texture.sampleType = WGPUTextureSampleType_Float;
    mat_entries[0].texture.viewDimension = WGPUTextureViewDimension_2DArray;
    mat_entries[1].binding = 1;
    mat_entries[1].visibility = WGPUShaderStage_Fragment;
    mat_entries[1].texture.sampleType = WGPUTextureSampleType_Float;
    mat_entries[1].texture.viewDimension = WGPUTextureViewDimension_2DArray;
    mat_entries[2].binding = 2;
    mat_entries[2].visibility = WGPUShaderStage_Fragment;
    mat_entries[2].sampler.type = WGPUSamplerBindingType_Filtering;
    mat_entries[3].binding = 3;
    mat_entries[3].visibility = WGPUShaderStage_Fragment;
    mat_entries[3].buffer.type = WGPUBufferBindingType_Uniform;
    mat_entries[3].buffer.hasDynamicOffset = 0U;
    mat_entries[3].buffer.minBindingSize = kMaterialTableSize;
    WGPUBindGroupLayoutDescriptor mat_layout_desc{};
    mat_layout_desc.entryCount = 4;
    mat_layout_desc.entries = mat_entries;
    mat_layout = wgpuDeviceCreateBindGroupLayout(device, &mat_layout_desc);
    {
      WGPUSamplerDescriptor msd{};
      msd.label = sv("material-sampler");
      msd.addressModeU = WGPUAddressMode_Repeat;
      msd.addressModeV = WGPUAddressMode_Repeat;
      msd.addressModeW = WGPUAddressMode_Repeat;
      msd.magFilter = WGPUFilterMode_Linear;
      msd.minFilter = WGPUFilterMode_Linear;
      msd.mipmapFilter = WGPUMipmapFilterMode_Linear;
      msd.lodMinClamp = 0.0f;
      msd.lodMaxClamp = 32.0f;
      msd.maxAnisotropy = 8;
      material_sampler = wgpuDeviceCreateSampler(device, &msd);
      WGPUBufferDescriptor bd{};
      bd.label = sv("material-table");
      bd.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
      bd.size = kMaterialTableSize;
      material_table = wgpuDeviceCreateBuffer(device, &bd);
      for (std::uint32_t i = 0; i < kMaterialSlots; ++i) {
        float* row = material_cpu + i * 12;
        row[0] = row[1] = row[2] = 1.0f;
        row[3] = 4.0f;
        row[4] = 0.85f;
        row[5] = 0.0f;
        row[6] = 1.0f;
        row[7] = 0.0f;
        row[8] = row[9] = row[10] = 0.5f;
        row[11] = 0.0f;
      }
      material_dirty = true;
      // A 1x1 placeholder library so every pipeline can bind group 2.
      material_lib = make_material_lib(1, 1);
      const std::uint8_t grey[4] = {128, 128, 128, 128};
      const std::uint8_t flat[4] = {128, 128, 220, 255};
      write_material_layer(material_lib.albedo, 0, grey);
      write_material_layer(material_lib.normal, 0, flat);
    }

    WGPUBindGroupLayout group_layouts[4] = {bind_layout, tex_layout, mat_layout, receive_layout};
    WGPUPipelineLayoutDescriptor pipeline_layout_desc{};
    pipeline_layout_desc.bindGroupLayoutCount = 4;
    pipeline_layout_desc.bindGroupLayouts = group_layouts;
    WGPUPipelineLayout pipeline_layout =
        wgpuDeviceCreatePipelineLayout(device, &pipeline_layout_desc);

    WGPUVertexAttribute attributes[3] = {};
    attributes[0].format = WGPUVertexFormat_Float32x3;
    attributes[0].offset = 0;
    attributes[0].shaderLocation = 0;
    attributes[1].format = WGPUVertexFormat_Float32x3;
    attributes[1].offset = 12;
    attributes[1].shaderLocation = 1;
    attributes[2].format = WGPUVertexFormat_Float32x4;  // palette weights
    attributes[2].offset = 24;
    attributes[2].shaderLocation = 2;
    WGPUVertexBufferLayout vertex_layout{};
    vertex_layout.arrayStride = 40;
    vertex_layout.stepMode = WGPUVertexStepMode_Vertex;
    vertex_layout.attributeCount = 3;
    vertex_layout.attributes = attributes;

    WGPUDepthStencilState depth_state{};
    depth_state.format = WGPUTextureFormat_Depth32Float;
    depth_state.depthWriteEnabled = WGPUOptionalBool_True;
    // Reversed-Z: the app builds its projection with near/far swapped, so
    // closer = LARGER depth; cleared to 0, tested with Greater. This is
    // what keeps solar-system-scale distances stable in an f32 depth
    // buffer (classic-Z quantized them onto the far plane).
    depth_state.depthCompare = WGPUCompareFunction_Greater;
    depth_state.stencilFront.compare = WGPUCompareFunction_Always;
    depth_state.stencilFront.failOp = WGPUStencilOperation_Keep;
    depth_state.stencilFront.depthFailOp = WGPUStencilOperation_Keep;
    depth_state.stencilFront.passOp = WGPUStencilOperation_Keep;
    depth_state.stencilBack = depth_state.stencilFront;
    depth_state.stencilReadMask = 0xFFFFFFFF;
    depth_state.stencilWriteMask = 0xFFFFFFFF;

    WGPUColorTargetState color_target{};
    color_target.format = kHdrFormat;  // scene renders LINEAR HDR (T0018)
    color_target.writeMask = WGPUColorWriteMask_All;
    WGPUFragmentState fragment{};
    fragment.module = module;
    fragment.entryPoint = sv("fs_main");
    fragment.targetCount = 1;
    fragment.targets = &color_target;

    WGPURenderPipelineDescriptor pipeline_desc{};
    pipeline_desc.label = sv("mesh");
    pipeline_desc.layout = pipeline_layout;
    pipeline_desc.vertex.module = module;
    pipeline_desc.vertex.entryPoint = sv("vs_main");
    pipeline_desc.vertex.bufferCount = 1;
    pipeline_desc.vertex.buffers = &vertex_layout;
    pipeline_desc.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    pipeline_desc.primitive.frontFace = WGPUFrontFace_CCW;
    pipeline_desc.primitive.cullMode = WGPUCullMode_None;
    pipeline_desc.depthStencil = &depth_state;
    pipeline_desc.multisample.count = 1;
    pipeline_desc.multisample.mask = 0xFFFFFFFF;
    pipeline_desc.fragment = &fragment;
    mesh_pipeline = wgpuDeviceCreateRenderPipeline(device, &pipeline_desc);

    // Translucent variant: alpha blending, no depth writes.
    WGPUBlendState blend{};
    blend.color.operation = WGPUBlendOperation_Add;
    blend.color.srcFactor = WGPUBlendFactor_SrcAlpha;
    blend.color.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
    blend.alpha.operation = WGPUBlendOperation_Add;
    blend.alpha.srcFactor = WGPUBlendFactor_One;
    blend.alpha.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
    color_target.blend = &blend;
    depth_state.depthWriteEnabled = WGPUOptionalBool_False;
    pipeline_desc.label = sv("mesh-blend");
    mesh_pipeline_blend = wgpuDeviceCreateRenderPipeline(device, &pipeline_desc);

    // Additive variant (corona/glow): src One + dst One, depth-tested so
    // planets occlude the glow, but no depth writes.
    WGPUBlendState additive{};
    additive.color.operation = WGPUBlendOperation_Add;
    additive.color.srcFactor = WGPUBlendFactor_One;
    additive.color.dstFactor = WGPUBlendFactor_One;
    additive.alpha.operation = WGPUBlendOperation_Add;
    additive.alpha.srcFactor = WGPUBlendFactor_One;
    additive.alpha.dstFactor = WGPUBlendFactor_One;
    color_target.blend = &additive;
    pipeline_desc.label = sv("mesh-add");
    mesh_pipeline_add = wgpuDeviceCreateRenderPipeline(device, &pipeline_desc);

    // Overlay variants (T0018): same shader, SURFACE format, drawn after
    // the tonemap so UI ignores exposure; depth-tested against the scene
    // but never writing depth.
    color_target.format = format;
    color_target.blend = nullptr;
    depth_state.depthWriteEnabled = WGPUOptionalBool_False;
    pipeline_desc.label = sv("overlay");
    overlay_pipeline = wgpuDeviceCreateRenderPipeline(device, &pipeline_desc);
    color_target.blend = &blend;
    pipeline_desc.label = sv("overlay-blend");
    overlay_pipeline_blend = wgpuDeviceCreateRenderPipeline(device, &pipeline_desc);
    color_target.blend = &additive;
    pipeline_desc.label = sv("overlay-add");
    overlay_pipeline_add = wgpuDeviceCreateRenderPipeline(device, &pipeline_desc);

    wgpuPipelineLayoutRelease(pipeline_layout);
    wgpuShaderModuleRelease(module);

    // --- post chain (T0018 WP1) -----------------------------------------
    {
      WGPUShaderSourceWGSL post_wgsl{};
      post_wgsl.chain.sType = WGPUSType_ShaderSourceWGSL;
      post_wgsl.code = sv(kPostShader);
      WGPUShaderModuleDescriptor post_module_desc{};
      post_module_desc.nextInChain = &post_wgsl.chain;
      WGPUShaderModule post_module = wgpuDeviceCreateShaderModule(device, &post_module_desc);

      WGPUBindGroupLayoutEntry entries[4] = {};
      entries[0].binding = 0;
      entries[0].visibility = WGPUShaderStage_Fragment;
      entries[0].texture.sampleType = WGPUTextureSampleType_Float;
      entries[0].texture.viewDimension = WGPUTextureViewDimension_2D;
      entries[1].binding = 1;
      entries[1].visibility = WGPUShaderStage_Fragment;
      entries[1].texture.sampleType = WGPUTextureSampleType_Float;
      entries[1].texture.viewDimension = WGPUTextureViewDimension_2D;
      entries[2].binding = 2;
      entries[2].visibility = WGPUShaderStage_Fragment;
      entries[2].sampler.type = WGPUSamplerBindingType_Filtering;
      entries[3].binding = 3;
      entries[3].visibility = WGPUShaderStage_Fragment;
      entries[3].buffer.type = WGPUBufferBindingType_Uniform;
      entries[3].buffer.hasDynamicOffset = 1U;
      entries[3].buffer.minBindingSize = 32;
      WGPUBindGroupLayoutDescriptor layout_info{};
      layout_info.entryCount = 4;
      layout_info.entries = entries;
      post_layout = wgpuDeviceCreateBindGroupLayout(device, &layout_info);

      WGPUSamplerDescriptor samp_desc{};
      samp_desc.label = sv("post-sampler");
      samp_desc.addressModeU = WGPUAddressMode_ClampToEdge;
      samp_desc.addressModeV = WGPUAddressMode_ClampToEdge;
      samp_desc.addressModeW = WGPUAddressMode_ClampToEdge;
      samp_desc.magFilter = WGPUFilterMode_Linear;
      samp_desc.minFilter = WGPUFilterMode_Linear;
      samp_desc.mipmapFilter = WGPUMipmapFilterMode_Nearest;
      samp_desc.lodMaxClamp = 32.0f;
      samp_desc.maxAnisotropy = 1;
      post_sampler = wgpuDeviceCreateSampler(device, &samp_desc);

      WGPUBufferDescriptor uniform_info{};
      uniform_info.label = sv("post-uniforms");
      uniform_info.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
      uniform_info.size = 256 * 8;
      post_uniforms = wgpuDeviceCreateBuffer(device, &uniform_info);

      WGPUPipelineLayoutDescriptor post_pl_desc{};
      post_pl_desc.bindGroupLayoutCount = 1;
      post_pl_desc.bindGroupLayouts = &post_layout;
      WGPUPipelineLayout post_pl = wgpuDeviceCreatePipelineLayout(device, &post_pl_desc);

      WGPUColorTargetState post_target{};
      post_target.writeMask = WGPUColorWriteMask_All;
      WGPUFragmentState post_frag{};
      post_frag.module = post_module;
      post_frag.targetCount = 1;
      post_frag.targets = &post_target;
      WGPURenderPipelineDescriptor post_desc{};
      post_desc.layout = post_pl;
      post_desc.vertex.module = post_module;
      post_desc.vertex.entryPoint = sv("vs_fullscreen");
      post_desc.primitive.topology = WGPUPrimitiveTopology_TriangleList;
      post_desc.primitive.frontFace = WGPUFrontFace_CCW;
      post_desc.primitive.cullMode = WGPUCullMode_None;
      post_desc.multisample.count = 1;
      post_desc.multisample.mask = 0xFFFFFFFF;
      post_desc.fragment = &post_frag;

      post_target.format = WGPUTextureFormat_RGBA32Float;
      post_frag.entryPoint = sv("fs_lum");
      post_desc.label = sv("post-lum");
      lum_pipeline = wgpuDeviceCreateRenderPipeline(device, &post_desc);
      post_target.format = kHdrFormat;
      post_frag.entryPoint = sv("fs_bright");
      post_desc.label = sv("post-bright");
      bright_pipeline = wgpuDeviceCreateRenderPipeline(device, &post_desc);
      post_frag.entryPoint = sv("fs_blur");
      post_desc.label = sv("post-blur");
      blur_pipeline = wgpuDeviceCreateRenderPipeline(device, &post_desc);
      post_target.format = format;
      post_frag.entryPoint = sv("fs_composite");
      post_desc.label = sv("post-composite");
      composite_pipeline = wgpuDeviceCreateRenderPipeline(device, &post_desc);
      wgpuPipelineLayoutRelease(post_pl);
      wgpuShaderModuleRelease(post_module);

      WGPUTextureDescriptor lum_desc{};
      lum_desc.label = sv("post-luminance");
      lum_desc.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc;
      lum_desc.dimension = WGPUTextureDimension_2D;
      lum_desc.size = WGPUExtent3D{kLumSize, kLumSize, 1};
      lum_desc.format = WGPUTextureFormat_RGBA32Float;
      lum_desc.mipLevelCount = 1;
      lum_desc.sampleCount = 1;
      lum_texture = wgpuDeviceCreateTexture(device, &lum_desc);
      lum_view = wgpuTextureCreateView(lum_texture, nullptr);
      WGPUBufferDescriptor read_desc{};
      read_desc.label = sv("post-lum-readback");
      read_desc.usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst;
      read_desc.size = static_cast<std::uint64_t>(kLumSize) * kLumSize * 16;
      lum_readback = wgpuDeviceCreateBuffer(device, &read_desc);
    }

    WGPUBufferDescriptor uniform_desc{};
    uniform_desc.label = sv("uniforms");
    uniform_desc.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
    uniform_desc.size = kUniformStride * kMaxDrawItems;
    uniform_buffer = wgpuDeviceCreateBuffer(device, &uniform_desc);

    WGPUBufferDescriptor frame_desc{};
    frame_desc.label = sv("frame-uniforms");
    frame_desc.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
    frame_desc.size = kFrameUniformSize;
    frame_buffer = wgpuDeviceCreateBuffer(device, &frame_desc);

    WGPUBindGroupEntry bind_entries[2] = {};
    bind_entries[0].binding = 0;
    bind_entries[0].buffer = uniform_buffer;
    bind_entries[0].offset = 0;
    bind_entries[0].size = kItemUniformSize;
    bind_entries[1].binding = 1;
    bind_entries[1].buffer = frame_buffer;
    bind_entries[1].offset = 0;
    bind_entries[1].size = kFrameUniformSize;
    WGPUBindGroupDescriptor bind_desc{};
    bind_desc.layout = bind_layout;
    bind_desc.entryCount = 2;
    bind_desc.entries = bind_entries;
    bind_group = wgpuDeviceCreateBindGroup(device, &bind_desc);

    // 1x1 default planet texture so group 1 is always bindable.
    default_tex = make_planet_tex(1);
    const std::uint16_t half_zero = 0;  // 0.0h
    const std::uint8_t grey[4] = {140, 140, 140, 255};
    for (std::uint32_t face = 0; face < 6; ++face) {
      write_planet_face(default_tex, face, &half_zero, grey);
    }
  }

  void write_planet_face(const PlanetTexEntry& entry, std::uint32_t face,
                         const std::uint16_t* height_half, const std::uint8_t* rgba) {
    WGPUTexelCopyTextureInfo dst{};
    dst.mipLevel = 0;
    dst.origin = WGPUOrigin3D{0, 0, face};
    dst.aspect = WGPUTextureAspect_All;
    WGPUTexelCopyBufferLayout layout{};
    layout.offset = 0;
    layout.rowsPerImage = entry.size;
    const WGPUExtent3D extent{entry.size, entry.size, 1};
    dst.texture = entry.height;
    layout.bytesPerRow = entry.size * 2;
    wgpuQueueWriteTexture(queue, &dst, height_half,
                          static_cast<std::size_t>(entry.size) * entry.size * 2, &layout,
                          &extent);
    dst.texture = entry.material;
    layout.bytesPerRow = entry.size * 4;
    wgpuQueueWriteTexture(queue, &dst, rgba,
                          static_cast<std::size_t>(entry.size) * entry.size * 4, &layout,
                          &extent);
  }

  // --- T0021: the city pipeline ----------------------------------------
  // Indexed 68-byte vertices, the demo's forward PBR shader (city_shader.
  // hpp) on the game's frame. Group 1 carries the city frame block,
  // materials, lights, the leaf texture, shadow cascades, ambient
  // occlusion, the sky cube and (dynamic) the cascade matrix. WP1 adds
  // the passes: three shadow cascades, a depth/normal prepass with SSAO,
  // and temporal anti-aliasing; lit terrain receives shadows and AO
  // through group 3.
  static constexpr std::uint32_t kCityCascades = 3;
  static constexpr std::uint32_t kShadowSize = 2048;
  static constexpr std::uint64_t kCityFrameSize = 8 * 64 + 9 * 16 + 6 * 16;  // 800
  static constexpr std::uint32_t kCitySkySize = 32;
  static constexpr std::uint32_t kCitySkyMips = 6;
  WGPURenderPipeline city_pipeline = nullptr;
  WGPURenderPipeline city_shadow_pipeline = nullptr;
  WGPURenderPipeline city_prepass_pipeline = nullptr;
  WGPURenderPipeline terrain_shadow_pipeline = nullptr;
  WGPURenderPipeline terrain_prepass_pipeline = nullptr;
  WGPURenderPipeline ssao_pipeline = nullptr;
  WGPURenderPipeline blur_pipeline_ao = nullptr;
  WGPURenderPipeline taa_pipeline = nullptr;
  WGPUBindGroupLayout city_layout = nullptr;
  WGPUBindGroupLayout receive_layout = nullptr;      // terrain group 3
  WGPUBindGroupLayout terrain_pass_layout = nullptr; // shadow/prepass group 1
  WGPUBindGroupLayout ao_layout = nullptr;           // ssao/blur group 0
  WGPUBindGroupLayout taa_layout = nullptr;          // taa group 1
  WGPUBindGroup city_group = nullptr;
  WGPUBindGroup receive_group = nullptr;
  WGPUBindGroup terrain_pass_group = nullptr;
  WGPUBindGroup ssao_group = nullptr;
  WGPUBindGroup blur_group = nullptr;
  WGPUBindGroup taa_group[2] = {nullptr, nullptr};
  WGPUBindGroup post_taa_only[2] = {nullptr, nullptr};
  WGPUBindGroup post_taa[2] = {nullptr, nullptr};
  WGPUBuffer city_frame_buf = nullptr;
  WGPUBuffer city_material_buf = nullptr;
  WGPUBuffer city_light_buf = nullptr;
  WGPUBuffer city_cascade_buf = nullptr;
  WGPUBuffer taa_buf = nullptr;
  std::uint64_t city_material_size = 0;
  std::uint64_t city_light_size = 0;
  std::uint32_t city_light_count = 0;
  bool city_group_dirty = true;
  bool city_resources_ready = false;
  WGPUSampler city_leaf_samp = nullptr;
  WGPUSampler city_shadow_samp = nullptr;
  WGPUSampler city_clamp_samp = nullptr;
  WGPUSampler city_cube_samp = nullptr;
  WGPUTexture city_leaf = nullptr;
  WGPUTextureView city_leaf_view = nullptr;
  WGPUTexture city_shadow = nullptr;        // depth array, kCityCascades layers
  WGPUTextureView city_shadow_view = nullptr;
  WGPUTexture city_shadow_dummy = nullptr;
  WGPUTextureView city_shadow_dummy_view = nullptr;
  WGPUBindGroup city_caster_group = nullptr;
  WGPUTextureView city_shadow_layer[kCityCascades] = {nullptr, nullptr, nullptr};
  WGPUTexture city_sky = nullptr;           // small prefiltered sky cube
  WGPUTextureView city_sky_view = nullptr;
  // Size-dependent targets.
  std::uint32_t city_w = 0;
  std::uint32_t city_h = 0;
  WGPUTexture depth_pre = nullptr;
  WGPUTextureView depth_pre_view = nullptr;
  WGPUTexture normal_pre = nullptr;
  WGPUTextureView normal_pre_view = nullptr;
  WGPUTexture ao_a = nullptr;
  WGPUTextureView ao_a_view = nullptr;
  WGPUTexture ao_b = nullptr;
  WGPUTextureView ao_b_view = nullptr;
  WGPUTexture taa_hist[2] = {nullptr, nullptr};
  WGPUTextureView taa_hist_view[2] = {nullptr, nullptr};
  int taa_parity = 0;
  bool taa_valid = false;
  float frame_jitter[2] = {0.0f, 0.0f};   // NDC units
  float frame_jitter_px[2] = {0.0f, 0.0f};
  float city_view_proj_clean[16] = {};
  CitySettings city_settings;
  float city_prev_view_proj[16] = {};
  bool passes_ran = false;  // shadows/AO produced this frame
  // Readback (--sweep).
  bool readback_requested = false;
  bool readback_ready = false;
  std::vector<std::uint8_t> readback_rgba;
  std::vector<float> readback_depth;
  std::uint32_t readback_w = 0;
  std::uint32_t readback_h = 0;

  WGPUTexture make_target(std::uint32_t w, std::uint32_t h, WGPUTextureFormat fmt, WGPUTextureUsage usage,
                          std::uint32_t layers, std::uint32_t mips, const char* label) {
    WGPUTextureDescriptor td{};
    td.label = sv(label);
    td.usage = usage;
    td.dimension = WGPUTextureDimension_2D;
    td.size = WGPUExtent3D{w, h, layers};
    td.format = fmt;
    td.mipLevelCount = mips;
    td.sampleCount = 1;
    return wgpuDeviceCreateTexture(device, &td);
  }

  // Resources every pipeline binds (the terrain pipeline's group 3
  // included), created before any pipeline.
  void ensure_city_resources() {
    if (city_resources_ready) {
      return;
    }
    city_resources_ready = true;
    // Layouts.
    {
      WGPUBindGroupLayoutEntry e[12] = {};
      e[0].binding = 0;
      e[0].visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
      e[0].buffer.type = WGPUBufferBindingType_Uniform;
      e[0].buffer.minBindingSize = kCityFrameSize;
      e[1].binding = 1;
      e[1].visibility = WGPUShaderStage_Fragment;
      e[1].buffer.type = WGPUBufferBindingType_ReadOnlyStorage;
      e[2].binding = 2;
      e[2].visibility = WGPUShaderStage_Fragment;
      e[2].buffer.type = WGPUBufferBindingType_ReadOnlyStorage;
      e[3].binding = 3;
      e[3].visibility = WGPUShaderStage_Fragment;
      e[3].texture.sampleType = WGPUTextureSampleType_Float;
      e[3].texture.viewDimension = WGPUTextureViewDimension_2D;
      e[4].binding = 4;
      e[4].visibility = WGPUShaderStage_Fragment;
      e[4].sampler.type = WGPUSamplerBindingType_Filtering;
      e[5].binding = 5;
      e[5].visibility = WGPUShaderStage_Fragment;
      e[5].texture.sampleType = WGPUTextureSampleType_Depth;
      e[5].texture.viewDimension = WGPUTextureViewDimension_2DArray;
      e[6].binding = 6;
      e[6].visibility = WGPUShaderStage_Fragment;
      e[6].sampler.type = WGPUSamplerBindingType_Comparison;
      e[7].binding = 7;
      e[7].visibility = WGPUShaderStage_Fragment;
      e[7].texture.sampleType = WGPUTextureSampleType_Float;
      e[7].texture.viewDimension = WGPUTextureViewDimension_2D;
      e[8].binding = 8;
      e[8].visibility = WGPUShaderStage_Fragment;
      e[8].sampler.type = WGPUSamplerBindingType_Filtering;
      e[9].binding = 9;
      e[9].visibility = WGPUShaderStage_Fragment;
      e[9].texture.sampleType = WGPUTextureSampleType_Float;
      e[9].texture.viewDimension = WGPUTextureViewDimension_Cube;
      e[10].binding = 10;
      e[10].visibility = WGPUShaderStage_Fragment;
      e[10].sampler.type = WGPUSamplerBindingType_Filtering;
      e[11].binding = 11;
      e[11].visibility = WGPUShaderStage_Vertex;
      e[11].buffer.type = WGPUBufferBindingType_Uniform;
      e[11].buffer.hasDynamicOffset = 1U;
      e[11].buffer.minBindingSize = 64;
      WGPUBindGroupLayoutDescriptor ld{};
      ld.entryCount = 12;
      ld.entries = e;
      city_layout = wgpuDeviceCreateBindGroupLayout(device, &ld);
      // Terrain receive (group 3): frame block, shadow maps, ao.
      WGPUBindGroupLayoutEntry r[6] = {};
      r[0].binding = 0;
      r[0].visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
      r[0].buffer.type = WGPUBufferBindingType_Uniform;
      r[0].buffer.minBindingSize = kCityFrameSize;
      r[1] = e[5]; r[1].binding = 1;
      r[2] = e[6]; r[2].binding = 2;
      r[3] = e[7]; r[3].binding = 3;
      r[4] = e[8]; r[4].binding = 4;
      r[5].binding = 5;
      r[5].visibility = WGPUShaderStage_Fragment;
      r[5].texture.sampleType = WGPUTextureSampleType_Depth;
      r[5].texture.viewDimension = WGPUTextureViewDimension_2D;
      ld.entryCount = 6;
      ld.entries = r;
      receive_layout = wgpuDeviceCreateBindGroupLayout(device, &ld);
      // Terrain shadow/prepass (group 1): cascade (dynamic) + frame block.
      WGPUBindGroupLayoutEntry t[2] = {};
      t[0].binding = 0;
      t[0].visibility = WGPUShaderStage_Vertex;
      t[0].buffer.type = WGPUBufferBindingType_Uniform;
      t[0].buffer.hasDynamicOffset = 1U;
      t[0].buffer.minBindingSize = 64;
      t[1].binding = 1;
      t[1].visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
      t[1].buffer.type = WGPUBufferBindingType_Uniform;
      t[1].buffer.minBindingSize = kCityFrameSize;
      ld.entryCount = 2;
      ld.entries = t;
      terrain_pass_layout = wgpuDeviceCreateBindGroupLayout(device, &ld);
      // SSAO / blur (group 0): frame block, depth, normal, sampler, ao.
      WGPUBindGroupLayoutEntry a[5] = {};
      a[0].binding = 0;
      a[0].visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
      a[0].buffer.type = WGPUBufferBindingType_Uniform;
      a[0].buffer.minBindingSize = kCityFrameSize;
      a[1].binding = 1;
      a[1].visibility = WGPUShaderStage_Fragment;
      a[1].texture.sampleType = WGPUTextureSampleType_Depth;
      a[1].texture.viewDimension = WGPUTextureViewDimension_2D;
      a[2].binding = 2;
      a[2].visibility = WGPUShaderStage_Fragment;
      a[2].texture.sampleType = WGPUTextureSampleType_Float;
      a[2].texture.viewDimension = WGPUTextureViewDimension_2D;
      a[3].binding = 3;
      a[3].visibility = WGPUShaderStage_Fragment;
      a[3].sampler.type = WGPUSamplerBindingType_Filtering;
      a[4].binding = 4;
      a[4].visibility = WGPUShaderStage_Fragment;
      a[4].texture.sampleType = WGPUTextureSampleType_Float;
      a[4].texture.viewDimension = WGPUTextureViewDimension_2D;
      ld.entryCount = 5;
      ld.entries = a;
      ao_layout = wgpuDeviceCreateBindGroupLayout(device, &ld);
      // TAA (group 1): params, current, history.
      WGPUBindGroupLayoutEntry q[3] = {};
      q[0].binding = 0;
      q[0].visibility = WGPUShaderStage_Fragment;
      q[0].buffer.type = WGPUBufferBindingType_Uniform;
      q[0].buffer.minBindingSize = 160;
      q[1].binding = 1;
      q[1].visibility = WGPUShaderStage_Fragment;
      q[1].texture.sampleType = WGPUTextureSampleType_Float;
      q[1].texture.viewDimension = WGPUTextureViewDimension_2D;
      q[2] = q[1]; q[2].binding = 2;
      ld.entryCount = 3;
      ld.entries = q;
      taa_layout = wgpuDeviceCreateBindGroupLayout(device, &ld);
    }
    // Samplers.
    {
      WGPUSamplerDescriptor sd{};
      sd.label = sv("city-leaf");
      sd.addressModeU = sd.addressModeV = sd.addressModeW = WGPUAddressMode_ClampToEdge;
      sd.magFilter = sd.minFilter = WGPUFilterMode_Linear;
      sd.mipmapFilter = WGPUMipmapFilterMode_Linear;
      sd.lodMaxClamp = 32.0f;
      sd.maxAnisotropy = 4;
      city_leaf_samp = wgpuDeviceCreateSampler(device, &sd);
      sd.label = sv("city-clamp");
      sd.maxAnisotropy = 1;
      sd.mipmapFilter = WGPUMipmapFilterMode_Nearest;
      city_clamp_samp = wgpuDeviceCreateSampler(device, &sd);
      sd.label = sv("city-cube");
      sd.mipmapFilter = WGPUMipmapFilterMode_Linear;
      city_cube_samp = wgpuDeviceCreateSampler(device, &sd);
      sd.label = sv("city-shadow");
      sd.compare = WGPUCompareFunction_LessEqual;
      sd.mipmapFilter = WGPUMipmapFilterMode_Nearest;
      city_shadow_samp = wgpuDeviceCreateSampler(device, &sd);
    }
    // Leaf texture: procedural leaf clusters (alpha = coverage), sRGB.
    {
      const std::uint32_t ls = 256;
      std::vector<std::uint8_t> rgba(static_cast<std::size_t>(ls) * ls * 4, 0);
      struct Leaf { float cx, cy, rx, ry, rot, shade; };
      std::vector<Leaf> leaves;
      const auto h01 = [](std::uint32_t x, std::uint32_t y, std::uint32_t z) {
        std::uint32_t h = x * 0x8da6b343u ^ y * 0xd8163841u ^ z * 0xcb1ab31fu;
        h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
        return static_cast<float>(h & 0xffffffu) / 16777216.0f;
      };
      for (std::uint32_t i = 0; i < 60; ++i) {
        const float a = h01(i, 1, 7) * 6.2831853f;
        const float r = std::sqrt(h01(i, 2, 7)) * 0.42f;
        leaves.push_back(Leaf{0.5f + std::cos(a) * r, 0.5f + std::sin(a) * r, 0.05f + 0.06f * h01(i, 3, 7),
                              0.025f + 0.03f * h01(i, 4, 7), h01(i, 5, 7) * 3.14159f, 0.6f + 0.5f * h01(i, 6, 7)});
      }
      for (std::uint32_t y = 0; y < ls; ++y) {
        for (std::uint32_t x = 0; x < ls; ++x) {
          const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(ls);
          const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(ls);
          float cover = 0.0f, shade = 0.0f;
          for (const Leaf& l : leaves) {
            const float dx = u - l.cx, dy = v - l.cy;
            const float c = std::cos(l.rot), sn = std::sin(l.rot);
            const float lx = (dx * c + dy * sn) / l.rx, ly = (-dx * sn + dy * c) / l.ry;
            const float d = lx * lx + ly * ly;
            if (d < 1.0f) {
              const float a = std::min(1.0f, (1.0f - d) * 6.0f);
              if (a > cover) { cover = a; shade = l.shade * (0.85f + 0.3f * std::fabs(lx)); }
            }
          }
          const std::size_t i = (static_cast<std::size_t>(y) * ls + x) * 4;
          const float g = 0.26f * shade, r = 0.10f * shade + 0.04f * (1.0f - shade), b = 0.05f * shade;
          rgba[i + 0] = static_cast<std::uint8_t>(std::pow(std::min(std::max(r, 0.0f), 1.0f), 1.0f / 2.2f) * 255.0f);
          rgba[i + 1] = static_cast<std::uint8_t>(std::pow(std::min(std::max(g, 0.0f), 1.0f), 1.0f / 2.2f) * 255.0f);
          rgba[i + 2] = static_cast<std::uint8_t>(std::pow(std::min(std::max(b, 0.0f), 1.0f), 1.0f / 2.2f) * 255.0f);
          rgba[i + 3] = static_cast<std::uint8_t>(cover * 255.0f);
        }
      }
      city_leaf = make_target(ls, ls, WGPUTextureFormat_RGBA8UnormSrgb,
                              WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst, 1, 1, "city-leaf");
      city_leaf_view = wgpuTextureCreateView(city_leaf, nullptr);
      WGPUTexelCopyTextureInfo dst{};
      dst.texture = city_leaf;
      WGPUTexelCopyBufferLayout layout{};
      layout.bytesPerRow = ls * 4;
      layout.rowsPerImage = ls;
      const WGPUExtent3D extent{ls, ls, 1};
      wgpuQueueWriteTexture(queue, &dst, rgba.data(), rgba.size(), &layout, &extent);
    }
    // Shadow cascades: a depth array with one view per layer.
    {
      city_shadow = make_target(kShadowSize, kShadowSize, WGPUTextureFormat_Depth32Float,
                                WGPUTextureUsage_TextureBinding | WGPUTextureUsage_RenderAttachment, kCityCascades, 1,
                                "city-shadow");
      WGPUTextureViewDescriptor vd{};
      vd.format = WGPUTextureFormat_Depth32Float;
      vd.dimension = WGPUTextureViewDimension_2DArray;
      vd.baseMipLevel = 0;
      vd.mipLevelCount = 1;
      vd.baseArrayLayer = 0;
      vd.arrayLayerCount = kCityCascades;
      vd.aspect = WGPUTextureAspect_DepthOnly;
      city_shadow_view = wgpuTextureCreateView(city_shadow, &vd);
      // The shadow pass binds the same layout while writing the cascades;
      // its group carries a 1x1 stand-in for the shadow array instead.
      city_shadow_dummy = make_target(1, 1, WGPUTextureFormat_Depth32Float, WGPUTextureUsage_TextureBinding, 1, 1,
                                      "city-shadow-dummy");
      vd.arrayLayerCount = 1;
      city_shadow_dummy_view = wgpuTextureCreateView(city_shadow_dummy, &vd);
      for (std::uint32_t i = 0; i < kCityCascades; ++i) {
        vd.dimension = WGPUTextureViewDimension_2D;
        vd.baseArrayLayer = i;
        vd.arrayLayerCount = 1;
        city_shadow_layer[i] = wgpuTextureCreateView(city_shadow, &vd);
      }
    }
    // The sky cube: a small RGBA16F cube with a mip chain, refilled from
    // the analytic sky each frame (mip = roughness).
    {
      city_sky = make_target(kCitySkySize, kCitySkySize, WGPUTextureFormat_RGBA16Float,
                             WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst, 6, kCitySkyMips, "city-sky");
      WGPUTextureViewDescriptor vd{};
      vd.format = WGPUTextureFormat_RGBA16Float;
      vd.dimension = WGPUTextureViewDimension_Cube;
      vd.baseMipLevel = 0;
      vd.mipLevelCount = kCitySkyMips;
      vd.baseArrayLayer = 0;
      vd.arrayLayerCount = 6;
      vd.aspect = WGPUTextureAspect_All;
      city_sky_view = wgpuTextureCreateView(city_sky, &vd);
    }
    // Buffers.
    {
      WGPUBufferDescriptor bd{};
      bd.label = sv("city-frame");
      bd.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
      bd.size = kCityFrameSize;
      city_frame_buf = wgpuDeviceCreateBuffer(device, &bd);
      bd.label = sv("city-cascades");
      bd.size = kUniformStride * kCityCascades;
      city_cascade_buf = wgpuDeviceCreateBuffer(device, &bd);
      bd.label = sv("taa");
      bd.size = 160;
      taa_buf = wgpuDeviceCreateBuffer(device, &bd);
    }
    if (city_material_buf == nullptr) {
      const CityMaterial one{};
      set_city_materials_impl(&one, 1);
    }
    if (city_light_buf == nullptr) {
      const CityLight none{};
      set_city_lights_impl(&none, 0);
    }
    // A 1x1 white AO placeholder until the size-dependent targets exist.
    ensure_city_targets(1, 1);
  }

  // Orthographic light frustum around a camera-frustum slice (camera at
  // the origin, basis and half-tangents from the frame), texel-snapped.
  void fit_cascade(const FrameParams& f, float zn, float zf, float* out_vp, float* out_extent) const {
    const float* r = f.cam_right;
    const float* up = f.cam_up;
    const float* fw = f.cam_fwd;
    float corners[8][3];
    int k = 0;
    for (const float z : {zn, zf}) {
      const float hh = f.tan_half_y * z;
      const float hw = f.tan_half_x * z;
      for (int i = 0; i < 4; ++i) {
        const float sx = (i & 1) ? 1.0f : -1.0f;
        const float sy = (i & 2) ? 1.0f : -1.0f;
        for (int c = 0; c < 3; ++c) corners[k][c] = fw[c] * z + r[c] * (sx * hw) + up[c] * (sy * hh);
        ++k;
      }
    }
    float centre[3] = {0.0f, 0.0f, 0.0f};
    for (auto& c : corners) for (int i = 0; i < 3; ++i) centre[i] += c[i] * 0.125f;
    float radius = 0.0f;
    for (auto& c : corners) {
      const float d[3] = {c[0] - centre[0], c[1] - centre[1], c[2] - centre[2]};
      radius = std::max(radius, std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]));
    }
    radius = std::ceil(radius * 4.0f) / 4.0f;
    float L[3] = {f.sun_dir[0], f.sun_dir[1], f.sun_dir[2]};
    const float ll = std::sqrt(L[0] * L[0] + L[1] * L[1] + L[2] * L[2]);
    for (float& v : L) v /= ll > 0.0f ? ll : 1.0f;
    const float depth_pad = 600.0f;
    // View: from centre + L * (radius + pad/2) toward the centre.
    const float eye[3] = {centre[0] + L[0] * (radius + depth_pad * 0.5f), centre[1] + L[1] * (radius + depth_pad * 0.5f),
                          centre[2] + L[2] * (radius + depth_pad * 0.5f)};
    const float fwd[3] = {-L[0], -L[1], -L[2]};
    float upv[3] = {0.0f, 1.0f, 0.0f};
    if (std::fabs(L[1]) > 0.95f) { upv[0] = 0.0f; upv[1] = 0.0f; upv[2] = 1.0f; }
    float s[3] = {fwd[1] * upv[2] - fwd[2] * upv[1], fwd[2] * upv[0] - fwd[0] * upv[2], fwd[0] * upv[1] - fwd[1] * upv[0]};
    const float sl = std::sqrt(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
    for (float& v : s) v /= sl > 0.0f ? sl : 1.0f;
    const float u[3] = {s[1] * fwd[2] - s[2] * fwd[1], s[2] * fwd[0] - s[0] * fwd[2], s[0] * fwd[1] - s[1] * fwd[0]};
    float view[16] = {};
    view[0] = s[0]; view[4] = s[1]; view[8] = s[2];
    view[1] = u[0]; view[5] = u[1]; view[9] = u[2];
    view[2] = -fwd[0]; view[6] = -fwd[1]; view[10] = -fwd[2];
    view[12] = -(s[0] * eye[0] + s[1] * eye[1] + s[2] * eye[2]);
    view[13] = -(u[0] * eye[0] + u[1] * eye[1] + u[2] * eye[2]);
    view[14] = fwd[0] * eye[0] + fwd[1] * eye[1] + fwd[2] * eye[2];
    view[15] = 1.0f;
    // Texel snapping of the centre in light space.
    const float texel = 2.0f * radius / static_cast<float>(kShadowSize);
    const float cx = view[0] * centre[0] + view[4] * centre[1] + view[8] * centre[2] + view[12];
    const float cy = view[1] * centre[0] + view[5] * centre[1] + view[9] * centre[2] + view[13];
    view[12] += std::floor(cx / texel) * texel - cx;
    view[13] += std::floor(cy / texel) * texel - cy;
    const float depth_range = 2.0f * radius + depth_pad;
    // Orthographic, z in [0, 1] (near = 0).
    float proj[16] = {};
    proj[0] = 1.0f / radius;
    proj[5] = 1.0f / radius;
    proj[10] = 1.0f / (0.0f - depth_range);
    proj[14] = 0.0f;
    proj[15] = 1.0f;
    mat_mul(proj, view, out_vp);
    *out_extent = radius;
  }

  void release_city_targets() {
    for (WGPUBindGroup* g : {&ssao_group, &blur_group, &taa_group[0], &taa_group[1], &post_taa_only[0], &post_taa_only[1],
                             &post_taa[0], &post_taa[1], &receive_group}) {
      if (*g != nullptr) wgpuBindGroupRelease(*g);
      *g = nullptr;
    }
    for (WGPUTextureView* v : {&depth_pre_view, &normal_pre_view, &ao_a_view, &ao_b_view, &taa_hist_view[0], &taa_hist_view[1]}) {
      if (*v != nullptr) wgpuTextureViewRelease(*v);
      *v = nullptr;
    }
    for (WGPUTexture* t : {&depth_pre, &normal_pre, &ao_a, &ao_b, &taa_hist[0], &taa_hist[1]}) {
      if (*t != nullptr) wgpuTextureRelease(*t);
      *t = nullptr;
    }
    taa_valid = false;
    city_group_dirty = true;
  }

  // Size-dependent targets: the prepass depth/normal, the two AO
  // buffers, the two TAA history buffers, and every bind group that
  // references them.
  void ensure_city_targets(std::uint32_t w, std::uint32_t h) {
    if (depth_pre != nullptr && city_w == w && city_h == h) {
      return;
    }
    release_city_targets();
    city_w = w;
    city_h = h;
    const WGPUTextureUsage rt = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding;
    depth_pre = make_target(w, h, WGPUTextureFormat_Depth32Float, rt, 1, 1, "city-depth-pre");
    depth_pre_view = wgpuTextureCreateView(depth_pre, nullptr);
    normal_pre = make_target(w, h, WGPUTextureFormat_RGBA16Float, rt, 1, 1, "city-normal-pre");
    normal_pre_view = wgpuTextureCreateView(normal_pre, nullptr);
    ao_a = make_target(w, h, WGPUTextureFormat_R8Unorm, rt | WGPUTextureUsage_CopyDst, 1, 1, "city-ao-a");
    ao_a_view = wgpuTextureCreateView(ao_a, nullptr);
    ao_b = make_target(w, h, WGPUTextureFormat_R8Unorm, rt | WGPUTextureUsage_CopyDst, 1, 1, "city-ao-b");
    ao_b_view = wgpuTextureCreateView(ao_b, nullptr);
    for (int i = 0; i < 2; ++i) {
      taa_hist[i] = make_target(w, h, kHdrFormat, rt, 1, 1, "city-taa");
      taa_hist_view[i] = wgpuTextureCreateView(taa_hist[i], nullptr);
    }
    // AO starts white (the passes fill it when they run).
    {
      std::vector<std::uint8_t> white(static_cast<std::size_t>(((w + 255) / 256) * 256) * h, 255);
      for (WGPUTexture t : {ao_a, ao_b}) {
        WGPUTexelCopyTextureInfo dst{};
        dst.texture = t;
        WGPUTexelCopyBufferLayout layout{};
        layout.bytesPerRow = ((w + 255) / 256) * 256;
        layout.rowsPerImage = h;
        const WGPUExtent3D extent{w, h, 1};
        wgpuQueueWriteTexture(queue, &dst, white.data(), white.size(), &layout, &extent);
      }
    }
    // Bind groups over the targets.
    {
      WGPUBindGroupEntry e[5] = {};
      e[0].binding = 0; e[0].buffer = city_frame_buf; e[0].size = kCityFrameSize;
      e[1].binding = 1; e[1].textureView = depth_pre_view;
      e[2].binding = 2; e[2].textureView = normal_pre_view;
      e[3].binding = 3; e[3].sampler = city_clamp_samp;
      e[4].binding = 4; e[4].textureView = ao_b_view;
      WGPUBindGroupDescriptor bd{};
      bd.layout = ao_layout;
      bd.entryCount = 5;
      bd.entries = e;
      ssao_group = wgpuDeviceCreateBindGroup(device, &bd);
      e[4].textureView = ao_a_view;
      blur_group = wgpuDeviceCreateBindGroup(device, &bd);
      WGPUBindGroupEntry r[6] = {};
      r[0].binding = 0; r[0].buffer = city_frame_buf; r[0].size = kCityFrameSize;
      r[1].binding = 1; r[1].textureView = city_shadow_view;
      r[2].binding = 2; r[2].sampler = city_shadow_samp;
      r[3].binding = 3; r[3].textureView = ao_b_view;
      r[4].binding = 4; r[4].sampler = city_clamp_samp;
      r[5].binding = 5; r[5].textureView = depth_pre_view;
      bd.layout = receive_layout;
      bd.entryCount = 6;
      bd.entries = r;
      receive_group = wgpuDeviceCreateBindGroup(device, &bd);
      if (terrain_pass_group == nullptr) {
        WGPUBindGroupEntry t[2] = {};
        t[0].binding = 0; t[0].buffer = city_cascade_buf; t[0].size = 64;
        t[1].binding = 1; t[1].buffer = city_frame_buf; t[1].size = kCityFrameSize;
        bd.layout = terrain_pass_layout;
        bd.entryCount = 2;
        bd.entries = t;
        terrain_pass_group = wgpuDeviceCreateBindGroup(device, &bd);
      }
    }
  }

  // TAA bind groups depend on the main post set (its HDR target and
  // bloom); rebuilt whenever either side changes size.
  void ensure_taa_groups() {
    if (post_taa[0] != nullptr || post_main.hdr_view == nullptr || depth_pre == nullptr) {
      return;
    }
    for (int p = 0; p < 2; ++p) {
      WGPUBindGroupEntry e[3] = {};
      e[0].binding = 0; e[0].buffer = taa_buf; e[0].size = 160;
      e[1].binding = 1; e[1].textureView = post_main.hdr_view;
      e[2].binding = 2; e[2].textureView = taa_hist_view[1 - p];
      WGPUBindGroupDescriptor bd{};
      bd.layout = taa_layout;
      bd.entryCount = 3;
      bd.entries = e;
      taa_group[p] = wgpuDeviceCreateBindGroup(device, &bd);
      post_taa_only[p] = make_post_group(taa_hist_view[p], taa_hist_view[p]);
      post_taa[p] = make_post_group(taa_hist_view[p], post_main.bloom_view[0]);
    }
  }

  void ensure_city_pipeline() {
    if (city_pipeline != nullptr) {
      return;
    }
    ensure_city_resources();
    WGPUShaderSourceWGSL wgsl{};
    wgsl.chain.sType = WGPUSType_ShaderSourceWGSL;
    wgsl.code = sv(kCityShader);
    WGPUShaderModuleDescriptor module_desc{};
    module_desc.nextInChain = &wgsl.chain;
    WGPUShaderModule module = wgpuDeviceCreateShaderModule(device, &module_desc);
    wgsl.code = sv(kTerrainPassShader);
    WGPUShaderModule terrain_module = wgpuDeviceCreateShaderModule(device, &module_desc);
    wgsl.code = sv(kCityPostShader);
    WGPUShaderModule post_module = wgpuDeviceCreateShaderModule(device, &module_desc);

    WGPUVertexAttribute attrs[6] = {};
    const WGPUVertexFormat fmts[6] = {WGPUVertexFormat_Float32x3, WGPUVertexFormat_Float32x3, WGPUVertexFormat_Float32x4,
                                      WGPUVertexFormat_Float32x2, WGPUVertexFormat_Uint32, WGPUVertexFormat_Float32x4};
    const std::uint64_t offs[6] = {0, 12, 24, 40, 48, 52};
    for (std::uint32_t i = 0; i < 6; ++i) {
      attrs[i].format = fmts[i];
      attrs[i].offset = offs[i];
      attrs[i].shaderLocation = i;
    }
    WGPUVertexBufferLayout vl{};
    vl.arrayStride = sizeof(CityVertex);
    vl.stepMode = WGPUVertexStepMode_Vertex;
    vl.attributeCount = 6;
    vl.attributes = attrs;
    WGPUVertexAttribute tattrs[3] = {};
    tattrs[0].format = WGPUVertexFormat_Float32x3; tattrs[0].offset = 0; tattrs[0].shaderLocation = 0;
    tattrs[1].format = WGPUVertexFormat_Float32x3; tattrs[1].offset = 12; tattrs[1].shaderLocation = 1;
    tattrs[2].format = WGPUVertexFormat_Float32x4; tattrs[2].offset = 24; tattrs[2].shaderLocation = 2;
    WGPUVertexBufferLayout tvl{};
    tvl.arrayStride = 40;
    tvl.stepMode = WGPUVertexStepMode_Vertex;
    tvl.attributeCount = 3;
    tvl.attributes = tattrs;

    const auto depth_state = [](WGPUCompareFunction cmp, bool write) {
      WGPUDepthStencilState ds{};
      ds.format = WGPUTextureFormat_Depth32Float;
      ds.depthWriteEnabled = write ? WGPUOptionalBool_True : WGPUOptionalBool_False;
      ds.depthCompare = cmp;
      ds.stencilFront.compare = WGPUCompareFunction_Always;
      ds.stencilFront.failOp = ds.stencilFront.depthFailOp = ds.stencilFront.passOp = WGPUStencilOperation_Keep;
      ds.stencilBack = ds.stencilFront;
      ds.stencilReadMask = 0xFFFFFFFF;
      ds.stencilWriteMask = 0xFFFFFFFF;
      return ds;
    };
    const auto make = [&](const char* label, WGPUPipelineLayout layout, WGPUShaderModule mod, const char* vs,
                          const char* fs, WGPUVertexBufferLayout* vbl, WGPUTextureFormat color,
                          const WGPUDepthStencilState* ds) {
      WGPUColorTargetState ct{};
      ct.format = color;
      ct.writeMask = WGPUColorWriteMask_All;
      WGPUFragmentState fst{};
      fst.module = mod;
      fst.entryPoint = sv(fs);
      fst.targetCount = color == WGPUTextureFormat_Undefined ? 0 : 1;
      fst.targets = color == WGPUTextureFormat_Undefined ? nullptr : &ct;
      WGPURenderPipelineDescriptor pd{};
      pd.label = sv(label);
      pd.layout = layout;
      pd.vertex.module = mod;
      pd.vertex.entryPoint = sv(vs);
      pd.vertex.bufferCount = vbl != nullptr ? 1 : 0;
      pd.vertex.buffers = vbl;
      pd.primitive.topology = WGPUPrimitiveTopology_TriangleList;
      pd.primitive.frontFace = WGPUFrontFace_CCW;
      pd.primitive.cullMode = WGPUCullMode_None;
      pd.depthStencil = ds;
      pd.multisample.count = 1;
      pd.multisample.mask = 0xFFFFFFFF;
      pd.fragment = fs != nullptr ? &fst : nullptr;
      return wgpuDeviceCreateRenderPipeline(device, &pd);
    };
    WGPUPipelineLayoutDescriptor pld{};
    WGPUBindGroupLayout city_groups[3] = {bind_layout, city_layout, mat_layout};
    pld.bindGroupLayoutCount = 3;
    pld.bindGroupLayouts = city_groups;
    WGPUPipelineLayout city_pl = wgpuDeviceCreatePipelineLayout(device, &pld);
    WGPUBindGroupLayout terrain_groups[2] = {bind_layout, terrain_pass_layout};
    pld.bindGroupLayoutCount = 2;
    pld.bindGroupLayouts = terrain_groups;
    WGPUPipelineLayout terrain_pl = wgpuDeviceCreatePipelineLayout(device, &pld);
    WGPUBindGroupLayout ao_groups[1] = {ao_layout};
    pld.bindGroupLayoutCount = 1;
    pld.bindGroupLayouts = ao_groups;
    WGPUPipelineLayout ao_pl = wgpuDeviceCreatePipelineLayout(device, &pld);
    WGPUBindGroupLayout taa_groups[2] = {ao_layout, taa_layout};
    pld.bindGroupLayoutCount = 2;
    pld.bindGroupLayouts = taa_groups;
    WGPUPipelineLayout taa_pl = wgpuDeviceCreatePipelineLayout(device, &pld);

    const WGPUDepthStencilState ds_scene = depth_state(WGPUCompareFunction_Greater, true);   // reversed-Z
    const WGPUDepthStencilState ds_shadow = depth_state(WGPUCompareFunction_Less, true);     // ortho, cleared to 1
    city_pipeline = make("city", city_pl, module, "vs_main", "fs_main", &vl, kHdrFormat, &ds_scene);
    city_shadow_pipeline = make("city-shadow", city_pl, module, "vs_shadow", nullptr, &vl, WGPUTextureFormat_Undefined, &ds_shadow);
    city_prepass_pipeline = make("city-prepass", city_pl, module, "vs_prepass", "fs_prepass", &vl, WGPUTextureFormat_RGBA16Float, &ds_scene);
    terrain_shadow_pipeline = make("terrain-shadow", terrain_pl, terrain_module, "vs_shadow", nullptr, &tvl, WGPUTextureFormat_Undefined, &ds_shadow);
    terrain_prepass_pipeline = make("terrain-prepass", terrain_pl, terrain_module, "vs_prepass", "fs_prepass", &tvl, WGPUTextureFormat_RGBA16Float, &ds_scene);
    ssao_pipeline = make("city-ssao", ao_pl, post_module, "vs_fullscreen", "fs_ssao", nullptr, WGPUTextureFormat_R8Unorm, nullptr);
    blur_pipeline_ao = make("city-ao-blur", ao_pl, post_module, "vs_fullscreen", "fs_blur", nullptr, WGPUTextureFormat_R8Unorm, nullptr);
    taa_pipeline = make("city-taa", taa_pl, post_module, "vs_fullscreen", "fs_taa", nullptr, kHdrFormat, nullptr);
    wgpuPipelineLayoutRelease(city_pl);
    wgpuPipelineLayoutRelease(terrain_pl);
    wgpuPipelineLayoutRelease(ao_pl);
    wgpuPipelineLayoutRelease(taa_pl);
    wgpuShaderModuleRelease(module);
    wgpuShaderModuleRelease(terrain_module);
    wgpuShaderModuleRelease(post_module);
    city_group_dirty = true;
  }

  void set_city_materials_impl(const CityMaterial* materials, std::size_t count) {
    if (count == 0) {
      const CityMaterial one{};
      set_city_materials_impl(&one, 1);
      return;
    }
    const std::uint64_t size = count * sizeof(CityMaterial);
    if (city_material_buf == nullptr || city_material_size < size) {
      if (city_material_buf != nullptr) wgpuBufferRelease(city_material_buf);
      WGPUBufferDescriptor bd{};
      bd.label = sv("city-materials");
      bd.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst;
      bd.size = size;
      city_material_buf = wgpuDeviceCreateBuffer(device, &bd);
      city_material_size = size;
      city_group_dirty = true;
    }
    wgpuQueueWriteBuffer(queue, city_material_buf, 0, materials, size);
  }

  void set_city_lights_impl(const CityLight* lights, std::size_t count) {
    static constexpr std::size_t kMax = 64;
    if (count > kMax) count = kMax;
    CityLight block[kMax];
    for (std::size_t i = 0; i < count; ++i) block[i] = lights[i];
    const std::uint64_t size = kMax * sizeof(CityLight);
    if (city_light_buf == nullptr) {
      WGPUBufferDescriptor bd{};
      bd.label = sv("city-lights");
      bd.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst;
      bd.size = size;
      city_light_buf = wgpuDeviceCreateBuffer(device, &bd);
      city_light_size = size;
      city_group_dirty = true;
    }
    wgpuQueueWriteBuffer(queue, city_light_buf, 0, block, size);
    city_light_count = static_cast<std::uint32_t>(count);
  }

  void ensure_city_group() {
    if (!city_group_dirty && city_group != nullptr) {
      return;
    }
    if (city_group != nullptr) wgpuBindGroupRelease(city_group);
    WGPUBindGroupEntry e[12] = {};
    e[0].binding = 0; e[0].buffer = city_frame_buf; e[0].size = kCityFrameSize;
    e[1].binding = 1; e[1].buffer = city_material_buf; e[1].size = city_material_size;
    e[2].binding = 2; e[2].buffer = city_light_buf; e[2].size = city_light_size;
    e[3].binding = 3; e[3].textureView = city_leaf_view;
    e[4].binding = 4; e[4].sampler = city_leaf_samp;
    e[5].binding = 5; e[5].textureView = city_shadow_view;
    e[6].binding = 6; e[6].sampler = city_shadow_samp;
    e[7].binding = 7; e[7].textureView = ao_b_view;
    e[8].binding = 8; e[8].sampler = city_clamp_samp;
    e[9].binding = 9; e[9].textureView = city_sky_view;
    e[10].binding = 10; e[10].sampler = city_cube_samp;
    e[11].binding = 11; e[11].buffer = city_cascade_buf; e[11].size = 64;
    WGPUBindGroupDescriptor bd{};
    bd.layout = city_layout;
    bd.entryCount = 12;
    bd.entries = e;
    city_group = wgpuDeviceCreateBindGroup(device, &bd);
    if (city_caster_group != nullptr) wgpuBindGroupRelease(city_caster_group);
    e[5].textureView = city_shadow_dummy_view;
    city_caster_group = wgpuDeviceCreateBindGroup(device, &bd);
    city_group_dirty = false;
  }

  // The analytic sky as the city's environment: radiance by direction
  // (camera frame), from the frame's clear colour, palette, planet up
  // and sun. Feeds the SH9 irradiance and the small prefiltered cube.
  static void sky_radiance(const FrameParams& f, const float* d, float* out) {
    // The mode-4 sky dome, evaluated on the CPU: tint from the planet
    // palette, day factor from the sun's height, zenith/horizon blend,
    // sunset band, Mie lobe, night airglow, density by altitude; below
    // the horizon a ground bounce of the horizon colour.
    const float* up = f.planet_up;
    const float* sun = f.sun_dir;
    const float sun_h = sun[0] * up[0] + sun[1] * up[1] + sun[2] * up[2];
    const float view_h = d[0] * up[0] + d[1] * up[1] + d[2] * up[2];
    const float cos_vs = d[0] * sun[0] + d[1] * sun[1] + d[2] * sun[2];
    const auto smoothstep = [](float a, float b, float x) {
      const float t = std::min(1.0f, std::max(0.0f, (x - a) / (b - a)));
      return t * t * (3.0f - 2.0f * t);
    };
    const float day = smoothstep(-0.10f, 0.30f, sun_h);
    const float density = std::pow(std::min(1.0f, std::max(0.0f, 1.0f - f.altitude_frac)), 0.45f);
    const float zenith_k[3] = {0.40f, 0.52f, 0.75f};
    const float warm[3] = {1.0f, 0.88f, 0.72f};
    const float sunset[3] = {1.0f, 0.42f, 0.18f};
    const float low_sun = std::pow(std::min(1.0f, std::max(0.0f, 1.0f - std::fabs(sun_h) * 2.6f)), 1.4f);
    const float toward = std::pow(std::max(0.0f, cos_vs), 2.6f);
    const float mie = std::pow(std::max(0.0f, cos_vs), 24.0f) * 0.55f;
    const float vh = std::pow(std::min(1.0f, std::max(0.0f, view_h)), 0.55f);
    for (int c = 0; c < 3; ++c) {
      const float tint = f.atmo_tint[c];
      const float zenith = tint * zenith_k[c];
      const float horizon = (tint + (warm[c] - tint) * 0.45f) * 1.06f;
      float sky = horizon + (zenith - horizon) * vh;
      sky += (sunset[c] - sky) * low_sun * toward * 0.75f;
      float v = sky * day + f.sun_color[c] * mie * (0.25f + 0.75f * day);
      v += tint * 0.004f * (1.0f - day);
      v *= density;
      if (view_h < 0.0f) {
        // Ground: the horizon colour bounced off a mid-grey surface.
        const float ground = (horizon * day + tint * 0.004f * (1.0f - day)) * density * 0.30f;
        v = ground * (0.7f + 0.3f * (1.0f + view_h));
      }
      out[c] = v;
    }
  }

  void update_city_environment(const FrameParams& f, float* sh_out /* 9 x 4 */) {
    // SH9 projection of the sky over 192 Fibonacci directions, then the
    // cosine convolution (A0 = pi, A1 = 2pi/3, A2 = pi/4).
    float sh[9][3] = {};
    const int n = 192;
    for (int i = 0; i < n; ++i) {
      const float z = 1.0f - 2.0f * (static_cast<float>(i) + 0.5f) / static_cast<float>(n);
      const float r = std::sqrt(std::max(0.0f, 1.0f - z * z));
      const float phi = 2.399963f * static_cast<float>(i);
      const float d[3] = {r * std::cos(phi), r * std::sin(phi), z};
      float L[3];
      sky_radiance(f, d, L);
      const float basis[9] = {0.282095f, 0.488603f * d[1], 0.488603f * d[2], 0.488603f * d[0],
                              1.092548f * d[0] * d[1], 1.092548f * d[1] * d[2],
                              0.315392f * (3.0f * d[2] * d[2] - 1.0f), 1.092548f * d[0] * d[2],
                              0.546274f * (d[0] * d[0] - d[1] * d[1])};
      for (int k = 0; k < 9; ++k) {
        for (int c = 0; c < 3; ++c) sh[k][c] += L[c] * basis[k];
      }
    }
    const float w = 4.0f * 3.14159265f / static_cast<float>(n);
    const float conv[9] = {3.14159265f, 2.0943951f, 2.0943951f, 2.0943951f, 0.78539816f, 0.78539816f, 0.78539816f, 0.78539816f, 0.78539816f};
    for (int k = 0; k < 9; ++k) {
      for (int c = 0; c < 3; ++c) sh_out[k * 4 + c] = sh[k][c] * w * conv[k];
      sh_out[k * 4 + 3] = 0.0f;
    }
    // The sky cube: mip 0 from the model, coarser mips by averaging (a
    // roughness chain good enough for a 16-texel face).
    const std::uint32_t s0 = kCitySkySize;
    std::vector<float> level(static_cast<std::size_t>(s0) * s0 * 6 * 3);
    for (std::uint32_t face = 0; face < 6; ++face) {
      for (std::uint32_t y = 0; y < s0; ++y) {
        for (std::uint32_t x = 0; x < s0; ++x) {
          const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(s0) * 2.0f - 1.0f;
          const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(s0) * 2.0f - 1.0f;
          float d[3];
          switch (face) {
            case 0: d[0] = 1.0f; d[1] = -v; d[2] = -u; break;
            case 1: d[0] = -1.0f; d[1] = -v; d[2] = u; break;
            case 2: d[0] = u; d[1] = 1.0f; d[2] = v; break;
            case 3: d[0] = u; d[1] = -1.0f; d[2] = -v; break;
            case 4: d[0] = u; d[1] = -v; d[2] = 1.0f; break;
            default: d[0] = -u; d[1] = -v; d[2] = -1.0f; break;
          }
          const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
          d[0] /= len; d[1] /= len; d[2] /= len;
          float L[3];
          sky_radiance(f, d, L);
          float* o = level.data() + ((static_cast<std::size_t>(face) * s0 + y) * s0 + x) * 3;
          o[0] = L[0]; o[1] = L[1]; o[2] = L[2];
        }
      }
    }
    std::uint32_t size = s0;
    for (std::uint32_t mip = 0; mip < kCitySkyMips; ++mip) {
      std::vector<std::uint16_t> half(static_cast<std::size_t>(size) * size * 4);
      for (std::uint32_t face = 0; face < 6; ++face) {
        for (std::uint32_t i = 0; i < size * size; ++i) {
          const float* o = level.data() + (static_cast<std::size_t>(face) * size * size + i) * 3;
          half[i * 4 + 0] = float_to_half(o[0]);
          half[i * 4 + 1] = float_to_half(o[1]);
          half[i * 4 + 2] = float_to_half(o[2]);
          half[i * 4 + 3] = float_to_half(1.0f);
        }
        WGPUTexelCopyTextureInfo dst{};
        dst.texture = city_sky;
        dst.mipLevel = mip;
        dst.origin = WGPUOrigin3D{0, 0, face};
        WGPUTexelCopyBufferLayout layout{};
        layout.bytesPerRow = size * 8;
        layout.rowsPerImage = size;
        const WGPUExtent3D extent{size, size, 1};
        wgpuQueueWriteTexture(queue, &dst, half.data(), static_cast<std::size_t>(size) * size * 8, &layout, &extent);
      }
      if (size == 1) break;
      const std::uint32_t half_size = size / 2;
      std::vector<float> next(static_cast<std::size_t>(half_size) * half_size * 6 * 3);
      for (std::uint32_t face = 0; face < 6; ++face) {
        for (std::uint32_t y = 0; y < half_size; ++y) {
          for (std::uint32_t x = 0; x < half_size; ++x) {
            float* o = next.data() + ((static_cast<std::size_t>(face) * half_size + y) * half_size + x) * 3;
            for (int c = 0; c < 3; ++c) {
              const auto at = [&](std::uint32_t sx, std::uint32_t sy) {
                return level[((static_cast<std::size_t>(face) * size + sy) * size + sx) * 3 + c];
              };
              o[c] = 0.25f * (at(2 * x, 2 * y) + at(2 * x + 1, 2 * y) + at(2 * x, 2 * y + 1) + at(2 * x + 1, 2 * y + 1));
            }
          }
        }
      }
      level.swap(next);
      size = half_size;
    }
  }

  void release_city() {
    release_city_targets();
    for (WGPUTextureView* v : {&city_leaf_view, &city_shadow_view, &city_shadow_dummy_view, &city_sky_view, &city_shadow_layer[0],
                               &city_shadow_layer[1], &city_shadow_layer[2]}) {
      if (*v != nullptr) wgpuTextureViewRelease(*v);
      *v = nullptr;
    }
    for (WGPUTexture* t : {&city_leaf, &city_shadow, &city_shadow_dummy, &city_sky}) {
      if (*t != nullptr) wgpuTextureRelease(*t);
      *t = nullptr;
    }
    for (WGPUSampler* sp : {&city_leaf_samp, &city_shadow_samp, &city_clamp_samp, &city_cube_samp}) {
      if (*sp != nullptr) wgpuSamplerRelease(*sp);
      *sp = nullptr;
    }
    for (WGPUBuffer* b : {&city_frame_buf, &city_material_buf, &city_light_buf, &city_cascade_buf, &taa_buf}) {
      if (*b != nullptr) wgpuBufferRelease(*b);
      *b = nullptr;
    }
    for (WGPUBindGroup* g : {&city_group, &city_caster_group, &terrain_pass_group}) {
      if (*g != nullptr) wgpuBindGroupRelease(*g);
      *g = nullptr;
    }
    for (WGPURenderPipeline* pp : {&city_pipeline, &city_shadow_pipeline, &city_prepass_pipeline, &terrain_shadow_pipeline,
                                   &terrain_prepass_pipeline, &ssao_pipeline, &blur_pipeline_ao, &taa_pipeline}) {
      if (*pp != nullptr) wgpuRenderPipelineRelease(*pp);
      *pp = nullptr;
    }
    for (WGPUBindGroupLayout* l : {&city_layout, &receive_layout, &terrain_pass_layout, &ao_layout, &taa_layout}) {
      if (*l != nullptr) wgpuBindGroupLayoutRelease(*l);
      *l = nullptr;
    }
  }

  ~Impl() {
    release_city();
    for (auto& [id, mesh] : meshes) {
      wgpuBufferRelease(mesh.buffer);
      if (mesh.index_buffer != nullptr) wgpuBufferRelease(mesh.index_buffer);
    }
    for (auto& [id, entry] : planet_textures) {
      release_planet_tex(entry);
    }
    release_planet_tex(default_tex);
    release_material_lib(material_lib);
    if (material_table != nullptr) wgpuBufferRelease(material_table);
    if (material_sampler != nullptr) wgpuSamplerRelease(material_sampler);
    if (mat_layout != nullptr) wgpuBindGroupLayoutRelease(mat_layout);
    release_post_set(post_main);
    release_post_set(post_rec);
    if (lum_readback != nullptr) wgpuBufferRelease(lum_readback);
    if (lum_view != nullptr) wgpuTextureViewRelease(lum_view);
    if (lum_texture != nullptr) wgpuTextureRelease(lum_texture);
    if (post_uniforms != nullptr) wgpuBufferRelease(post_uniforms);
    if (post_sampler != nullptr) wgpuSamplerRelease(post_sampler);
    if (post_layout != nullptr) wgpuBindGroupLayoutRelease(post_layout);
    if (lum_pipeline != nullptr) wgpuRenderPipelineRelease(lum_pipeline);
    if (bright_pipeline != nullptr) wgpuRenderPipelineRelease(bright_pipeline);
    if (blur_pipeline != nullptr) wgpuRenderPipelineRelease(blur_pipeline);
    if (composite_pipeline != nullptr) wgpuRenderPipelineRelease(composite_pipeline);
    if (overlay_pipeline != nullptr) wgpuRenderPipelineRelease(overlay_pipeline);
    if (overlay_pipeline_blend != nullptr) wgpuRenderPipelineRelease(overlay_pipeline_blend);
    if (overlay_pipeline_add != nullptr) wgpuRenderPipelineRelease(overlay_pipeline_add);
    if (planet_sampler != nullptr) wgpuSamplerRelease(planet_sampler);
    if (tex_layout != nullptr) wgpuBindGroupLayoutRelease(tex_layout);
    if (rec_buffer != nullptr) wgpuBufferRelease(rec_buffer);
    if (rec_color_view != nullptr) wgpuTextureViewRelease(rec_color_view);
    if (rec_color != nullptr) wgpuTextureRelease(rec_color);
    if (rec_depth_view != nullptr) wgpuTextureViewRelease(rec_depth_view);
    if (rec_depth != nullptr) wgpuTextureRelease(rec_depth);
    if (bind_group != nullptr) wgpuBindGroupRelease(bind_group);
    if (mesh_pipeline_blend != nullptr) wgpuRenderPipelineRelease(mesh_pipeline_blend);
    if (mesh_pipeline_add != nullptr) wgpuRenderPipelineRelease(mesh_pipeline_add);
    if (uniform_buffer != nullptr) wgpuBufferRelease(uniform_buffer);
    if (frame_buffer != nullptr) wgpuBufferRelease(frame_buffer);
    if (bind_layout != nullptr) wgpuBindGroupLayoutRelease(bind_layout);
    if (mesh_pipeline != nullptr) wgpuRenderPipelineRelease(mesh_pipeline);
    if (depth_view != nullptr) wgpuTextureViewRelease(depth_view);
    if (depth_texture != nullptr) wgpuTextureRelease(depth_texture);
    if (surface != nullptr) wgpuSurfaceUnconfigure(surface);
    if (queue != nullptr) wgpuQueueRelease(queue);
    if (device != nullptr) wgpuDeviceRelease(device);
    if (adapter != nullptr) wgpuAdapterRelease(adapter);
    if (surface != nullptr) wgpuSurfaceRelease(surface);
    if (instance != nullptr) wgpuInstanceRelease(instance);
  }
};

void Rhi::Impl::ensure_city_resources_early() { ensure_city_resources(); }

Rhi::Rhi(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Rhi::~Rhi() = default;

std::unique_ptr<Rhi> Rhi::create(GLFWwindow* window, std::string* error) {
  auto impl = std::make_unique<Impl>();

  WGPUInstanceDescriptor instance_desc{};
  impl->instance = wgpuCreateInstance(&instance_desc);
  if (impl->instance == nullptr) {
    *error = "wgpuCreateInstance failed";
    return nullptr;
  }

  impl->surface = create_surface(impl->instance, window, error);
  if (impl->surface == nullptr) {
    if (error->empty()) {
      *error = "surface creation failed";
    }
    return nullptr;
  }

  AdapterRequest adapter_request;
  {
    WGPURequestAdapterOptions options{};
    options.compatibleSurface = impl->surface;
    options.powerPreference = WGPUPowerPreference_HighPerformance;
    WGPURequestAdapterCallbackInfo callback_info{};
    callback_info.mode = WGPUCallbackMode_AllowProcessEvents;
    callback_info.callback = [](WGPURequestAdapterStatus status, WGPUAdapter adapter,
                                WGPUStringView message, void* userdata1, void*) {
      auto* request = static_cast<AdapterRequest*>(userdata1);
      if (status == WGPURequestAdapterStatus_Success) {
        request->adapter = adapter;
      } else {
        request->message = to_string(message);
      }
      request->done = true;
    };
    callback_info.userdata1 = &adapter_request;
    wgpuInstanceRequestAdapter(impl->instance, &options, callback_info);
    while (!adapter_request.done) {
      wgpuInstanceProcessEvents(impl->instance);
    }
  }
  if (adapter_request.adapter == nullptr) {
    *error = "no suitable GPU adapter: " + adapter_request.message;
    return nullptr;
  }
  impl->adapter = adapter_request.adapter;

  {
    WGPUAdapterInfo info{};
    if (wgpuAdapterGetInfo(impl->adapter, &info) == WGPUStatus_Success) {
      impl->adapter_info = to_string(info.device) + " (" + to_string(info.description) + ")";
      wgpuAdapterInfoFreeMembers(info);
    }
  }

  DeviceRequest device_request;
  {
    WGPUDeviceDescriptor device_desc{};
    device_desc.uncapturedErrorCallbackInfo.callback =
        [](WGPUDevice const*, WGPUErrorType type, WGPUStringView message, void*, void*) {
          std::fprintf(stderr, "[wgpu] uncaptured error (%d): %.*s\n", static_cast<int>(type),
                       static_cast<int>(message.length), message.data);
        };
    WGPURequestDeviceCallbackInfo callback_info{};
    callback_info.mode = WGPUCallbackMode_AllowProcessEvents;
    callback_info.callback = [](WGPURequestDeviceStatus status, WGPUDevice device,
                                WGPUStringView message, void* userdata1, void*) {
      auto* request = static_cast<DeviceRequest*>(userdata1);
      if (status == WGPURequestDeviceStatus_Success) {
        request->device = device;
      } else {
        request->message = to_string(message);
      }
      request->done = true;
    };
    callback_info.userdata1 = &device_request;
    wgpuAdapterRequestDevice(impl->adapter, &device_desc, callback_info);
    while (!device_request.done) {
      wgpuInstanceProcessEvents(impl->instance);
    }
  }
  if (device_request.device == nullptr) {
    *error = "device request failed: " + device_request.message;
    return nullptr;
  }
  impl->device = device_request.device;
  impl->queue = wgpuDeviceGetQueue(impl->device);

  {
    WGPUSurfaceCapabilities capabilities{};
    if (wgpuSurfaceGetCapabilities(impl->surface, impl->adapter, &capabilities) !=
            WGPUStatus_Success ||
        capabilities.formatCount == 0) {
      *error = "surface reports no supported formats";
      return nullptr;
    }
    impl->format = capabilities.formats[0];
    wgpuSurfaceCapabilitiesFreeMembers(capabilities);
  }

  int fb_width = 0;
  int fb_height = 0;
  glfwGetFramebufferSize(window, &fb_width, &fb_height);
  impl->width = static_cast<std::uint32_t>(fb_width > 0 ? fb_width : 1);
  impl->height = static_cast<std::uint32_t>(fb_height > 0 ? fb_height : 1);
  impl->configure_surface();

  return std::unique_ptr<Rhi>(new Rhi(std::move(impl)));
}

void Rhi::resize(std::uint32_t width, std::uint32_t height) {
  if (width == 0 || height == 0) {
    return;
  }
  impl_->width = width;
  impl_->height = height;
  impl_->configure_surface();
}

std::uint32_t Rhi::create_mesh(const float* vertices, std::size_t float_count) {
  // Legacy 6-float soup: expand to the 10-float terrain layout with zero
  // weights (the shader's flat base-albedo path).
  const std::size_t count = float_count / 6;
  std::vector<float> expanded(count * 10);
  for (std::size_t v = 0; v < count; ++v) {
    std::memcpy(expanded.data() + v * 10, vertices + v * 6, 6 * sizeof(float));
    for (int w = 0; w < 4; ++w) {
      expanded[v * 10 + 6 + static_cast<std::size_t>(w)] = 0.0f;
    }
  }
  return create_mesh_mat(expanded.data(), expanded.size());
}

std::uint32_t Rhi::create_mesh_mat(const float* vertices, std::size_t float_count) {
  // A buffer past the device limit is a validation error that wgpu
  // escalates to a fatal panic on the next submit; refuse it here (the
  // caller draws nothing for this mesh) and say so once.
  constexpr std::size_t kMaxMeshBytes = 200u * 1024u * 1024u;
  if (float_count * sizeof(float) > kMaxMeshBytes) {
    std::fprintf(stderr, "rhi: refusing a %.0f MB mesh (limit %zu MB)\n",
                 static_cast<double>(float_count * sizeof(float)) / 1048576.0, kMaxMeshBytes / 1048576u);
    return 0;
  }
  WGPUBufferDescriptor desc{};
  desc.label = sv("chunk-mesh");
  desc.usage = WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst;
  desc.size = float_count * sizeof(float);
  WGPUBuffer buffer = wgpuDeviceCreateBuffer(impl_->device, &desc);
  wgpuQueueWriteBuffer(impl_->queue, buffer, 0, vertices, desc.size);
  const std::uint32_t id = impl_->next_mesh_id++;
  impl_->meshes.emplace(id, MeshEntry{buffer, static_cast<std::uint32_t>(float_count / 10)});
  return id;
}

void Rhi::destroy_mesh(std::uint32_t mesh) {
  auto it = impl_->meshes.find(mesh);
  if (it != impl_->meshes.end()) {
    wgpuBufferRelease(it->second.buffer);
    if (it->second.index_buffer != nullptr) wgpuBufferRelease(it->second.index_buffer);
    impl_->meshes.erase(it);
  }
}

std::uint32_t Rhi::create_city_mesh(const CityVertex* vertices, std::size_t vertex_count,
                                    const std::uint32_t* indices, std::size_t index_count) {
  constexpr std::size_t kMaxMeshBytes = 200u * 1024u * 1024u;
  if (vertex_count == 0 || index_count == 0) return 0;
  if (vertex_count * sizeof(CityVertex) > kMaxMeshBytes) {
    std::fprintf(stderr, "rhi: refusing a %.0f MB city mesh (limit %zu MB)\n",
                 static_cast<double>(vertex_count * sizeof(CityVertex)) / 1048576.0, kMaxMeshBytes / 1048576u);
    return 0;
  }
  impl_->ensure_mesh_pipeline();
  WGPUBufferDescriptor vd{};
  vd.label = sv("city-mesh");
  vd.usage = WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst;
  vd.size = vertex_count * sizeof(CityVertex);
  WGPUBuffer vb = wgpuDeviceCreateBuffer(impl_->device, &vd);
  wgpuQueueWriteBuffer(impl_->queue, vb, 0, vertices, vd.size);
  WGPUBufferDescriptor id{};
  id.label = sv("city-indices");
  id.usage = WGPUBufferUsage_Index | WGPUBufferUsage_CopyDst;
  id.size = index_count * sizeof(std::uint32_t);
  WGPUBuffer ib = wgpuDeviceCreateBuffer(impl_->device, &id);
  wgpuQueueWriteBuffer(impl_->queue, ib, 0, indices, id.size);
  const std::uint32_t handle = impl_->next_mesh_id++;
  MeshEntry entry;
  entry.buffer = vb;
  entry.vertex_count = static_cast<std::uint32_t>(vertex_count);
  entry.index_buffer = ib;
  entry.index_count = static_cast<std::uint32_t>(index_count);
  impl_->meshes.emplace(handle, entry);
  return handle;
}

void Rhi::set_city_materials(const CityMaterial* materials, std::size_t count) {
  impl_->ensure_mesh_pipeline();
  impl_->set_city_materials_impl(materials, count);
}

void Rhi::set_city_lights(const CityLight* lights, std::size_t count) {
  impl_->ensure_mesh_pipeline();
  impl_->set_city_lights_impl(lights, count);
}

void Rhi::set_city_settings(const CitySettings& settings) { impl_->city_settings = settings; }

std::uint32_t Rhi::create_planet_texture(std::uint32_t face_size) {
  impl_->ensure_mesh_pipeline();  // layouts + sampler exist from here on
  const std::uint32_t id = impl_->next_planet_tex_id++;
  impl_->planet_textures.emplace(id, impl_->make_planet_tex(face_size));
  return id;
}

void Rhi::update_planet_face(std::uint32_t handle, std::uint32_t face,
                             const std::uint16_t* height_half, const std::uint8_t* rgba) {
  const auto it = impl_->planet_textures.find(handle);
  if (it == impl_->planet_textures.end() || face >= 6) {
    return;
  }
  impl_->write_planet_face(it->second, face, height_half, rgba);
}

void Rhi::destroy_planet_texture(std::uint32_t handle) {
  const auto it = impl_->planet_textures.find(handle);
  if (it != impl_->planet_textures.end()) {
    impl_->release_planet_tex(it->second);
    impl_->planet_textures.erase(it);
  }
}

void Rhi::create_material_library(std::uint32_t size, std::uint32_t layers) {
  impl_->ensure_mesh_pipeline();
  if (size == 0 || layers == 0) {
    return;
  }
  impl_->release_material_lib(impl_->material_lib);
  impl_->material_lib = impl_->make_material_lib(size, layers);
  for (std::uint32_t i = 0; i < Impl::kMaterialSlots; ++i) {
    impl_->material_ready[i] = false;
    impl_->material_cpu[i * 12 + 7] = 0.0f;
  }
  impl_->material_dirty = true;
}

void Rhi::upload_material_layer(std::uint32_t layer, const std::uint8_t* albedo_rgba,
                                const std::uint8_t* normal_rgba) {
  Impl::MaterialLib& lib = impl_->material_lib;
  if (lib.albedo == nullptr || layer >= lib.layers || layer >= Impl::kMaterialSlots) {
    return;
  }
  impl_->write_material_layer(lib.albedo, layer, albedo_rgba);
  impl_->write_material_layer(lib.normal, layer, normal_rgba);
  impl_->material_ready[layer] = true;
  impl_->material_cpu[layer * 12 + 7] = 1.0f;
  impl_->material_dirty = true;
}

void Rhi::set_material_params(std::uint32_t layer, const MaterialParams& params) {
  if (layer >= Impl::kMaterialSlots) {
    return;
  }
  float* row = impl_->material_cpu + layer * 12;
  row[0] = params.tint[0];
  row[1] = params.tint[1];
  row[2] = params.tint[2];
  row[3] = params.tile_m;
  row[4] = params.roughness;
  row[5] = params.emissive;
  row[6] = params.normal_strength;
  row[7] = impl_->material_ready[layer] ? 1.0f : 0.0f;
  row[8] = params.mean[0];
  row[9] = params.mean[1];
  row[10] = params.mean[2];
  row[11] = 0.0f;
  impl_->material_dirty = true;
}

bool Rhi::render_frame(const FrameParams& frame, const DrawItem* items,
                       std::size_t item_count) {
  impl_->ensure_mesh_pipeline();
  if (impl_->material_dirty) {
    // Table layout: three arrays of 64 vec4 (a: tint+tile, b: rough/
    // emissive/strength/ready, c: mean) — repack from the row layout.
    float table[Impl::kMaterialSlots * 12];
    for (std::uint32_t i = 0; i < Impl::kMaterialSlots; ++i) {
      const float* row = impl_->material_cpu + i * 12;
      std::memcpy(table + i * 4, row, 4 * sizeof(float));
      std::memcpy(table + Impl::kMaterialSlots * 4 + i * 4, row + 4, 4 * sizeof(float));
      std::memcpy(table + Impl::kMaterialSlots * 8 + i * 4, row + 8, 4 * sizeof(float));
    }
    wgpuQueueWriteBuffer(impl_->queue, impl_->material_table, 0, table, sizeof(table));
    impl_->material_dirty = false;
  }

  {
    float frame_block[40] = {
        frame.sun_dir[0],    frame.sun_dir[1],    frame.sun_dir[2],    0.0f,
        frame.sun_color[0],  frame.sun_color[1],  frame.sun_color[2],  frame.time_s,
        frame.cam_right[0],  frame.cam_right[1],  frame.cam_right[2],  frame.tan_half_x,
        frame.cam_up[0],     frame.cam_up[1],     frame.cam_up[2],     frame.tan_half_y,
        frame.cam_fwd[0],    frame.cam_fwd[1],    frame.cam_fwd[2],    frame.altitude_frac,
        frame.planet_up[0],  frame.planet_up[1],  frame.planet_up[2],  0.0f,
        frame.atmo_tint[0],  frame.atmo_tint[1],  frame.atmo_tint[2],  frame.sea_radius_m,
        frame.planet_center[0], frame.planet_center[1], frame.planet_center[2],
        frame.normal_blend,
        frame.palette_shift, 0.0f, 0.0f, 0.0f,
        impl_->frame_jitter[0], impl_->frame_jitter[1], 0.0f, 0.0f,
    };
    wgpuQueueWriteBuffer(impl_->queue, impl_->frame_buffer, 0, frame_block,
                         sizeof(frame_block));
  }

  // Upload all uniforms before the command buffer executes.
  const std::size_t count = item_count > kMaxDrawItems ? kMaxDrawItems : item_count;
  for (std::size_t i = 0; i < count; ++i) {
    float block[32];
    std::memcpy(block, items[i].mvp, sizeof(items[i].mvp));
    std::memcpy(block + 16, items[i].color, sizeof(items[i].color));
    std::memcpy(block + 20, items[i].aux, sizeof(items[i].aux));
    std::memcpy(block + 24, items[i].extra, sizeof(items[i].extra));
    block[27] = static_cast<float>(items[i].mode);
    for (int k = 0; k < 4; ++k) {
      block[28 + k] = static_cast<float>(items[i].material_palette[k]);
    }
    wgpuQueueWriteBuffer(impl_->queue, impl_->uniform_buffer, i * kUniformStride, block,
                         sizeof(block));
  }

  // Attachment templates: the main pass fills in the surface view; the
  // capture/recorder paths swap in their own offscreen views. Keeping
  // them independent of surface acquisition means captures and the ring
  // keep working when the window is hidden and macOS stops handing out
  // surface textures (fully headless operation).
  WGPURenderPassColorAttachment attachment{};
  attachment.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
  attachment.loadOp = WGPULoadOp_Clear;
  attachment.storeOp = WGPUStoreOp_Store;
  attachment.clearValue = WGPUColor{frame.sky[0], frame.sky[1], frame.sky[2], 1.0};
  WGPURenderPassDepthStencilAttachment depth_attachment{};
  depth_attachment.view = impl_->depth_view;
  depth_attachment.depthLoadOp = WGPULoadOp_Clear;
  depth_attachment.depthStoreOp = WGPUStoreOp_Store;
  depth_attachment.depthClearValue = 0.0f;  // reversed-Z: far plane

  WGPUSurfaceTexture surface_texture{};
  wgpuSurfaceGetCurrentTexture(impl_->surface, &surface_texture);
  const bool have_surface =
      surface_texture.status == WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal ||
      surface_texture.status == WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal;
  if (!have_surface) {
    if (surface_texture.texture != nullptr) {
      wgpuTextureRelease(surface_texture.texture);
    }
    if (surface_texture.status == WGPUSurfaceGetCurrentTextureStatus_Outdated ||
        surface_texture.status == WGPUSurfaceGetCurrentTextureStatus_Lost) {
      impl_->configure_surface();
    }
  }

  // --- eye adaptation (T0018 WP1) ---------------------------------------
  // Consume last frame's luminance readback (async, 1-2 frame latency),
  // then follow the exposure target with asymmetric time constants:
  // glare adapts in a blink, dark adaptation opens over seconds.
  wgpuDevicePoll(impl_->device, 0U, nullptr);
  if (impl_->lum_map_ready) {
    impl_->lum_map_ready = false;
    impl_->lum_map_inflight = false;
    const std::uint64_t lum_bytes =
        static_cast<std::uint64_t>(Impl::kLumSize) * Impl::kLumSize * 16;
    const auto* data = static_cast<const float*>(
        wgpuBufferGetConstMappedRange(impl_->lum_readback, 0, lum_bytes));
    if (data != nullptr) {
      double sum = 0.0;
      for (std::uint32_t i = 0; i < Impl::kLumSize * Impl::kLumSize; ++i) {
        sum += static_cast<double>(data[i * 4]);
      }
      impl_->avg_luminance =
          static_cast<float>(sum / (Impl::kLumSize * Impl::kLumSize));
    }
    wgpuBufferUnmap(impl_->lum_readback);
  }
  {
    float dt = frame.time_s - impl_->exposure_last_time;
    if (impl_->exposure_last_time < 0.0f || dt < 0.0f || dt > 0.5f) {
      dt = 1.0f / 60.0f;
    }
    impl_->exposure_last_time = frame.time_s;
    const float avg = impl_->avg_luminance > 2.0e-5f ? impl_->avg_luminance : 2.0e-5f;
    float target = 0.22f / avg;
    // The rod gain ceiling: low enough that the deep sky keeps its
    // contrast (the band at ~0.2-0.4 display, off-plane sky near black)
    // instead of auto-exposure normalizing space to mid-grey. Was 80
    // before the WP3 band existed; with a luminous sky, 80 washed the
    // whole sphere to grey.
    target = target < 0.15f ? 0.15f : (target > 35.0f ? 35.0f : target);
    const float tau = target < impl_->exposure ? 0.35f : 5.0f;
    impl_->exposure += (target - impl_->exposure) * (1.0f - std::exp(-dt / tau));
    // Scotopic fraction: rods take over as the adapted scene dims (scene
    // units: day averages ~0.3, night ~5e-3, starlit space < 1e-4).
    const float log_avg = std::log10(avg + 1.0e-9f);
    float scotopic = (std::log10(0.03f) - log_avg) / 1.2f;
    impl_->scotopic = scotopic < 0.0f ? 0.0f : (scotopic > 1.0f ? 1.0f : scotopic);
  }

  impl_->ensure_post_set(impl_->post_main, impl_->width, impl_->height);
  impl_->ensure_post_set(impl_->post_rec, Impl::kRecW, Impl::kRecH);

  // Post uniforms: slots 0-3 main, 4-7 recorder (bright/composite, blurH,
  // blurV, luminance). a = (exposure, scotopic, bloom, threshold),
  // b = (texel_x, texel_y, dir_x, dir_y).
  const auto write_post_slots = [&](std::uint32_t base, const Impl::PostSet& set) {
    const float tx = 1.0f / static_cast<float>(set.w);
    const float ty = 1.0f / static_cast<float>(set.h);
    const float btx = 2.0f / static_cast<float>(set.w);
    const float bty = 2.0f / static_cast<float>(set.h);
    const float a[4] = {impl_->exposure, impl_->scotopic, 0.55f, 1.05f};
    float block[8] = {a[0], a[1], a[2], a[3], tx, ty, 0.0f, 0.0f};
    wgpuQueueWriteBuffer(impl_->queue, impl_->post_uniforms, base * 256, block,
                         sizeof(block));
    float blur_h[8] = {a[0], a[1], a[2], a[3], btx, bty, 1.0f, 0.0f};
    wgpuQueueWriteBuffer(impl_->queue, impl_->post_uniforms, (base + 1) * 256, blur_h,
                         sizeof(blur_h));
    float blur_v[8] = {a[0], a[1], a[2], a[3], btx, bty, 0.0f, 1.0f};
    wgpuQueueWriteBuffer(impl_->queue, impl_->post_uniforms, (base + 2) * 256, blur_v,
                         sizeof(blur_v));
    float lum[8] = {a[0], a[1], a[2], a[3], tx, ty, 0.0f, 0.0f};
    wgpuQueueWriteBuffer(impl_->queue, impl_->post_uniforms, (base + 3) * 256, lum,
                         sizeof(lum));
  };
  write_post_slots(0, impl_->post_main);
  write_post_slots(4, impl_->post_rec);

  // --- T0021: the city frame block, jitter, cascades ---------------------
  // Written every frame: lit terrain reads the cascades and the AO of the
  // passes through group 3 whether or not city meshes are on screen.
  bool has_city = false;
  bool has_casters = false;
  for (std::size_t i = 0; i < count; ++i) {
    if (items[i].overlay) continue;
    if (items[i].mode == 8) has_city = true;
    if ((items[i].shadow_caster || items[i].prepass) && (items[i].mode == 8 || items[i].mode == 0)) has_casters = true;
  }
  impl_->ensure_city_pipeline();
  impl_->ensure_city_group();
  impl_->ensure_city_targets(impl_->width, impl_->height);
  const CitySettings& cs = impl_->city_settings;
  const bool passes_on = frame.have_view_proj && has_casters;
  const bool taa_on = cs.taa && frame.have_view_proj;
  {
    // Halton(2,3) jitter, 8 samples, centred, in pixels; the frame block
    // carries it in NDC units for every vertex stage.
    float jx = 0.0f, jy = 0.0f;
    if (taa_on) {
      const auto halton = [](std::uint32_t i, std::uint32_t b) {
        float f = 1.0f, r = 0.0f;
        while (i > 0) { f /= static_cast<float>(b); r += f * static_cast<float>(i % b); i /= b; }
        return r;
      };
      const std::uint32_t k = static_cast<std::uint32_t>(impl_->frame_counter % 8) + 1;
      jx = halton(k, 2) - 0.5f;
      jy = halton(k, 3) - 0.5f;
    }
    impl_->frame_jitter_px[0] = jx;
    impl_->frame_jitter_px[1] = jy;
    impl_->frame_jitter[0] = 2.0f * jx / static_cast<float>(impl_->width);
    impl_->frame_jitter[1] = 2.0f * jy / static_cast<float>(impl_->height);
    // Re-upload the frame block with the jitter (it was written above).
    float jit[4] = {impl_->frame_jitter[0], impl_->frame_jitter[1], 0.0f, 0.0f};
    wgpuQueueWriteBuffer(impl_->queue, impl_->frame_buffer, 36 * sizeof(float), jit, sizeof(jit));
  }
  {
    float block[Impl::kCityFrameSize / 4] = {};
    float* view = block + 0;
    float* proj = block + 16;
    float* view_proj = block + 32;
    float* inv_view_proj = block + 48;
    float* inv_proj = block + 64;
    float* shadow_m = block + 80;  // shadow0..2 at 80, 96, 112
    float* sh = block + 128;
    float* cascade = block + 164;
    float* cascade_extent = block + 168;
    float* screen = block + 172;
    float* params = block + 176;
    float* params2 = block + 180;
    float* jitter = block + 184;
    // View from the camera basis (camera-relative world -> view): rows
    // right, up, -forward; column-major storage.
    const float* r = frame.cam_right;
    const float* up = frame.cam_up;
    const float* fw = frame.cam_fwd;
    for (int k = 0; k < 16; ++k) view[k] = 0.0f;
    view[0] = r[0]; view[4] = r[1]; view[8] = r[2];
    view[1] = up[0]; view[5] = up[1]; view[9] = up[2];
    view[2] = -fw[0]; view[6] = -fw[1]; view[10] = -fw[2];
    view[15] = 1.0f;
    if (frame.have_view_proj) {
      std::memcpy(view_proj, frame.view_proj, sizeof(float) * 16);
      float view_t[16];  // inverse of a rotation = transpose
      for (int c = 0; c < 4; ++c) for (int rr = 0; rr < 4; ++rr) view_t[c * 4 + rr] = view[rr * 4 + c];
      mat_mul(view_proj, view_t, proj);
      mat_inverse(view_proj, inv_view_proj);
      mat_inverse(proj, inv_proj);
    } else {
      for (int k = 0; k < 16; ++k) {
        view_proj[k] = proj[k] = inv_view_proj[k] = inv_proj[k] = (k % 5 == 0) ? 1.0f : 0.0f;
      }
    }
    impl_->update_city_environment(frame, sh);
    // Cascaded shadow maps: three camera-frustum slices fitted with an
    // orthographic light frustum each, texel-snapped (the demo's fit).
    const float splits[Impl::kCityCascades + 1] = {0.5f, 28.0f, 110.0f, 520.0f};
    const bool shadows = cs.shadows && passes_on;
    for (std::uint32_t c = 0; c < Impl::kCityCascades; ++c) {
      float extent = 1.0f;
      float* out = shadow_m + c * 16;
      if (shadows) {
        impl_->fit_cascade(frame, splits[c], splits[c + 1], out, &extent);
      } else {
        for (int k = 0; k < 16; ++k) out[k] = (k % 5 == 0) ? 1.0f : 0.0f;
      }
      wgpuQueueWriteBuffer(impl_->queue, impl_->city_cascade_buf, c * kUniformStride, out, 16 * sizeof(float));
      cascade_extent[c] = extent;
    }
    cascade[0] = splits[1];
    cascade[1] = splits[2];
    cascade[2] = splits[3];
    cascade[3] = 1.0f / static_cast<float>(Impl::kShadowSize);
    screen[0] = static_cast<float>(impl_->width);
    screen[1] = static_cast<float>(impl_->height);
    screen[2] = 1.0f / static_cast<float>(impl_->width);
    screen[3] = 1.0f / static_cast<float>(impl_->height);
    params[0] = 0.3f;  // emissive scale in scene HDR units (sunlit ground ~0.3)
    params[1] = cs.ao_strength;
    params[2] = cs.night;
    params[3] = cs.night > 0.05f ? static_cast<float>(impl_->city_light_count) : 0.0f;
    // Units: the terrain path shades albedo * ndl * sun_color (no 1/pi),
    // so the city's Lambert/GGX terms take the sun as pi * sun_color;
    // the dome's radiance is display-scaled, so the sky ambient is
    // scaled down to a plausible sky/sun ratio.
    params2[0] = cs.ibl_intensity;
    params2[1] = 3.14159265f;
    params2[2] = (cs.ssao && passes_on) ? 1.0f : 0.0f;
    params2[3] = shadows ? 1.0f : 0.0f;
    jitter[0] = impl_->frame_jitter[0];
    jitter[1] = impl_->frame_jitter[1];
    jitter[2] = static_cast<float>(cs.debug_view);
    jitter[3] = 0.0f;
    wgpuQueueWriteBuffer(impl_->queue, impl_->city_frame_buf, 0, block, sizeof(block));
    std::memcpy(impl_->city_view_proj_clean, view_proj, sizeof(float) * 16);
    impl_->passes_ran = passes_on;
    // TAA parameters: this frame's clean inverse and the previous frame's
    // view-projection in this frame's camera-relative space.
    float taa_block[40] = {};
    std::memcpy(taa_block, inv_view_proj, sizeof(float) * 16);
    std::memcpy(taa_block + 16, frame.prev_view_proj, sizeof(float) * 16);
    taa_block[32] = 0.92f;
    taa_block[33] = impl_->taa_valid ? 1.0f : 0.0f;
    taa_block[34] = static_cast<float>(impl_->width);
    taa_block[35] = static_cast<float>(impl_->height);
    taa_block[36] = impl_->frame_jitter_px[0];
    taa_block[37] = -impl_->frame_jitter_px[1];
    wgpuQueueWriteBuffer(impl_->queue, impl_->taa_buf, 0, taa_block, sizeof(taa_block));
  }

  enum class Pass { Opaque, Blend, Additive };
  const auto draw_city = [&](WGPURenderPassEncoder pass) {
    if (!has_city) return;
    wgpuRenderPassEncoderSetPipeline(pass, impl_->city_pipeline);
    const std::uint32_t zero = 0;
    wgpuRenderPassEncoderSetBindGroup(pass, 1, impl_->city_group, 1, &zero);
    wgpuRenderPassEncoderSetBindGroup(pass, 2, impl_->material_lib.group, 0, nullptr);
    for (std::size_t i = 0; i < count; ++i) {
      if (items[i].mode != 8 || items[i].overlay) continue;
      const auto it = impl_->meshes.find(items[i].mesh);
      if (it == impl_->meshes.end() || it->second.index_count == 0) continue;
      const std::uint32_t offset = static_cast<std::uint32_t>(i * kUniformStride);
      wgpuRenderPassEncoderSetBindGroup(pass, 0, impl_->bind_group, 1, &offset);
      wgpuRenderPassEncoderSetVertexBuffer(pass, 0, it->second.buffer, 0, WGPU_WHOLE_SIZE);
      wgpuRenderPassEncoderSetIndexBuffer(pass, it->second.index_buffer, WGPUIndexFormat_Uint32, 0, WGPU_WHOLE_SIZE);
      const std::uint32_t first = items[i].first_index;
      std::uint32_t n = items[i].index_count == 0 ? it->second.index_count : items[i].index_count;
      if (first >= it->second.index_count) continue;
      if (first + n > it->second.index_count) n = it->second.index_count - first;
      wgpuRenderPassEncoderDrawIndexed(pass, n, 1, first, 0, 0);
    }
  };
  const auto draw_bucket = [&](WGPURenderPassEncoder pass, Pass which, bool overlay) {
    for (std::size_t i = 0; i < count; ++i) {
      if (items[i].overlay != overlay) {
        continue;
      }
      if (items[i].mode == 8) {
        continue;  // city meshes: their own pipeline (draw_city)
      }
      const Pass item_pass = items[i].mode == 2 || items[i].mode == 3 ||
                                     items[i].mode == 7
                                 ? Pass::Additive
                             : items[i].translucent ? Pass::Blend
                                                    : Pass::Opaque;
      if (item_pass != which) {
        continue;
      }
      const auto it = impl_->meshes.find(items[i].mesh);
      if (it == impl_->meshes.end() || it->second.vertex_count == 0) {
        continue;
      }
      const std::uint32_t offset = static_cast<std::uint32_t>(i * kUniformStride);
      wgpuRenderPassEncoderSetBindGroup(pass, 0, impl_->bind_group, 1, &offset);
      WGPUBindGroup tex_group = impl_->default_tex.group;
      if (items[i].planet_texture != 0) {
        const auto tex_it = impl_->planet_textures.find(items[i].planet_texture);
        if (tex_it != impl_->planet_textures.end()) {
          tex_group = tex_it->second.group;
        }
      }
      wgpuRenderPassEncoderSetBindGroup(pass, 1, tex_group, 0, nullptr);
      wgpuRenderPassEncoderSetBindGroup(pass, 2, impl_->material_lib.group, 0, nullptr);
      wgpuRenderPassEncoderSetBindGroup(pass, 3, impl_->receive_group, 0, nullptr);
      wgpuRenderPassEncoderSetVertexBuffer(pass, 0, it->second.buffer, 0, WGPU_WHOLE_SIZE);
      wgpuRenderPassEncoderDraw(pass, it->second.vertex_count, 1, 0, 0);
    }
  };
  const auto record_scene = [&](WGPURenderPassEncoder pass) {
    wgpuRenderPassEncoderSetPipeline(pass, impl_->mesh_pipeline);
    draw_bucket(pass, Pass::Opaque, false);
    draw_city(pass);
    wgpuRenderPassEncoderSetPipeline(pass, impl_->mesh_pipeline_blend);
    draw_bucket(pass, Pass::Blend, false);
    wgpuRenderPassEncoderSetPipeline(pass, impl_->mesh_pipeline_add);
    draw_bucket(pass, Pass::Additive, false);
  };
  const auto record_overlay = [&](WGPURenderPassEncoder pass) {
    wgpuRenderPassEncoderSetPipeline(pass, impl_->overlay_pipeline);
    draw_bucket(pass, Pass::Opaque, true);
    wgpuRenderPassEncoderSetPipeline(pass, impl_->overlay_pipeline_blend);
    draw_bucket(pass, Pass::Blend, true);
    wgpuRenderPassEncoderSetPipeline(pass, impl_->overlay_pipeline_add);
    draw_bucket(pass, Pass::Additive, true);
  };

  // One full frame: scene -> HDR, bloom, composite+tonemap -> out, then
  // the LDR overlay pass sharing the scene depth. slot_base picks the
  // uniform block set; do_lum additionally reduces luminance (main only).
  // Shadow casters (city meshes and terrain chunks) into a depth target
  // with the given pipelines; `cascade` selects the light matrix.
  const auto draw_casters = [&](WGPURenderPassEncoder pass, WGPURenderPipeline city_p, WGPURenderPipeline terrain_p,
                                std::uint32_t cascade_offset, bool prepass) {
    bool city_bound = false;
    bool terrain_bound = false;
    for (std::size_t i = 0; i < count; ++i) {
      const DrawItem& it_ = items[i];
      if (it_.overlay || (it_.mode != 8 && it_.mode != 0)) continue;
      if (!(it_.shadow_caster || (prepass && it_.prepass))) continue;
      const auto it = impl_->meshes.find(it_.mesh);
      if (it == impl_->meshes.end() || it->second.vertex_count == 0) continue;
      const std::uint32_t offset = static_cast<std::uint32_t>(i * kUniformStride);
      wgpuRenderPassEncoderSetBindGroup(pass, 0, impl_->bind_group, 1, &offset);
      if (it_.mode == 8) {
        if (!city_bound) {
          wgpuRenderPassEncoderSetPipeline(pass, city_p);
          wgpuRenderPassEncoderSetBindGroup(pass, 1, city_p == impl_->city_shadow_pipeline ? impl_->city_caster_group : impl_->city_group,
                                            1, &cascade_offset);
          wgpuRenderPassEncoderSetBindGroup(pass, 2, impl_->material_lib.group, 0, nullptr);
          city_bound = true;
          terrain_bound = false;
        }
        wgpuRenderPassEncoderSetVertexBuffer(pass, 0, it->second.buffer, 0, WGPU_WHOLE_SIZE);
        wgpuRenderPassEncoderSetIndexBuffer(pass, it->second.index_buffer, WGPUIndexFormat_Uint32, 0, WGPU_WHOLE_SIZE);
        const std::uint32_t first = it_.first_index;
        std::uint32_t n = it_.index_count == 0 ? it->second.index_count : it_.index_count;
        if (first >= it->second.index_count) continue;
        if (first + n > it->second.index_count) n = it->second.index_count - first;
        wgpuRenderPassEncoderDrawIndexed(pass, n, 1, first, 0, 0);
      } else {
        if (!terrain_bound) {
          wgpuRenderPassEncoderSetPipeline(pass, terrain_p);
          wgpuRenderPassEncoderSetBindGroup(pass, 1, impl_->terrain_pass_group, 1, &cascade_offset);
          terrain_bound = true;
          city_bound = false;
        }
        wgpuRenderPassEncoderSetVertexBuffer(pass, 0, it->second.buffer, 0, WGPU_WHOLE_SIZE);
        wgpuRenderPassEncoderDraw(pass, it->second.vertex_count, 1, 0, 0);
      }
    }
  };
  const auto fullscreen_pass = [&](WGPUCommandEncoder encoder, WGPURenderPipeline pipeline, WGPUBindGroup g0,
                                   WGPUBindGroup g1, WGPUTextureView target) {
    WGPURenderPassColorAttachment color{};
    color.view = target;
    color.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
    color.loadOp = WGPULoadOp_Clear;
    color.storeOp = WGPUStoreOp_Store;
    color.clearValue = WGPUColor{1.0, 1.0, 1.0, 1.0};
    WGPURenderPassDescriptor desc{};
    desc.colorAttachmentCount = 1;
    desc.colorAttachments = &color;
    WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(encoder, &desc);
    wgpuRenderPassEncoderSetPipeline(pass, pipeline);
    wgpuRenderPassEncoderSetBindGroup(pass, 0, g0, 0, nullptr);
    if (g1 != nullptr) wgpuRenderPassEncoderSetBindGroup(pass, 1, g1, 0, nullptr);
    wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
    wgpuRenderPassEncoderEnd(pass);
    wgpuRenderPassEncoderRelease(pass);
  };
  // The city passes: shadow cascades, depth/normal prepass, SSAO + blur.
  // Main-resolution only; the recorder draws its frames with the AO and
  // shadows the main frame produced.
  const auto city_passes = [&](WGPUCommandEncoder encoder) {
    if (!passes_on) return;
    if (cs.shadows) {
      for (std::uint32_t c = 0; c < Impl::kCityCascades; ++c) {
        WGPURenderPassDepthStencilAttachment da{};
        da.view = impl_->city_shadow_layer[c];
        da.depthLoadOp = WGPULoadOp_Clear;
        da.depthStoreOp = WGPUStoreOp_Store;
        da.depthClearValue = 1.0f;
        WGPURenderPassDescriptor desc{};
        desc.depthStencilAttachment = &da;
        WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(encoder, &desc);
        draw_casters(pass, impl_->city_shadow_pipeline, impl_->terrain_shadow_pipeline,
                     static_cast<std::uint32_t>(c * kUniformStride), false);
        wgpuRenderPassEncoderEnd(pass);
        wgpuRenderPassEncoderRelease(pass);
      }
    }
    if (cs.ssao) {
      WGPURenderPassColorAttachment ca{};
      ca.view = impl_->normal_pre_view;
      ca.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
      ca.loadOp = WGPULoadOp_Clear;
      ca.storeOp = WGPUStoreOp_Store;
      ca.clearValue = WGPUColor{0.0, 0.0, 1.0, 1.0};
      WGPURenderPassDepthStencilAttachment da{};
      da.view = impl_->depth_pre_view;
      da.depthLoadOp = WGPULoadOp_Clear;
      da.depthStoreOp = WGPUStoreOp_Store;
      da.depthClearValue = 0.0f;  // reversed-Z
      WGPURenderPassDescriptor desc{};
      desc.colorAttachmentCount = 1;
      desc.colorAttachments = &ca;
      desc.depthStencilAttachment = &da;
      WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(encoder, &desc);
      draw_casters(pass, impl_->city_prepass_pipeline, impl_->terrain_prepass_pipeline, 0, true);
      wgpuRenderPassEncoderEnd(pass);
      wgpuRenderPassEncoderRelease(pass);
      fullscreen_pass(encoder, impl_->ssao_pipeline, impl_->ssao_group, nullptr, impl_->ao_a_view);
      fullscreen_pass(encoder, impl_->blur_pipeline_ao, impl_->blur_group, nullptr, impl_->ao_b_view);
    }
  };

  const auto render_full = [&](Impl::PostSet& set, WGPUTextureView depth,
                               WGPUTextureView out_view, std::uint32_t slot_base,
                               bool do_lum) {
    WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(impl_->device, nullptr);
    const bool main_set = &set == &impl_->post_main;
    if (main_set && do_lum) {
      city_passes(encoder);
    }
    // Scene into HDR.
    {
      WGPURenderPassColorAttachment color = attachment;
      color.view = set.hdr_view;
      WGPURenderPassDepthStencilAttachment depth_att = depth_attachment;
      depth_att.view = depth;
      WGPURenderPassDescriptor desc{};
      desc.colorAttachmentCount = 1;
      desc.colorAttachments = &color;
      desc.depthStencilAttachment = &depth_att;
      WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(encoder, &desc);
      record_scene(pass);
      wgpuRenderPassEncoderEnd(pass);
      wgpuRenderPassEncoderRelease(pass);
    }
    const auto fullscreen = [&](WGPURenderPipeline pipeline, WGPUBindGroup group,
                                std::uint32_t slot, WGPUTextureView target) {
      WGPURenderPassColorAttachment color{};
      color.view = target;
      color.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
      color.loadOp = WGPULoadOp_Clear;
      color.storeOp = WGPUStoreOp_Store;
      color.clearValue = WGPUColor{0.0, 0.0, 0.0, 1.0};
      WGPURenderPassDescriptor desc{};
      desc.colorAttachmentCount = 1;
      desc.colorAttachments = &color;
      WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(encoder, &desc);
      wgpuRenderPassEncoderSetPipeline(pass, pipeline);
      const std::uint32_t dyn = slot * 256;
      wgpuRenderPassEncoderSetBindGroup(pass, 0, group, 1, &dyn);
      wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
      wgpuRenderPassEncoderEnd(pass);
      wgpuRenderPassEncoderRelease(pass);
    };
    // Temporal anti-aliasing on the main set: the jittered HDR frame is
    // blended with the reprojected history; the post chain then reads the
    // resolved image. The capture re-render reuses this frame's result.
    WGPUBindGroup grp_only = set.grp_hdr_only;
    WGPUBindGroup grp_comp = set.grp_hdr;
    if (main_set && taa_on) {
      impl_->ensure_taa_groups();
      const int par = impl_->taa_parity;
      if (do_lum && impl_->taa_group[par] != nullptr) {
        fullscreen_pass(encoder, impl_->taa_pipeline, impl_->ssao_group, impl_->taa_group[par],
                        impl_->taa_hist_view[par]);
      }
      if (impl_->post_taa[par] != nullptr) {
        grp_only = impl_->post_taa_only[par];
        grp_comp = impl_->post_taa[par];
      }
    }
    fullscreen(impl_->bright_pipeline, grp_only, slot_base + 0, set.bloom_view[0]);
    fullscreen(impl_->blur_pipeline, set.grp_b0, slot_base + 1, set.bloom_view[1]);
    fullscreen(impl_->blur_pipeline, set.grp_b1, slot_base + 2, set.bloom_view[0]);
    if (do_lum) {
      fullscreen(impl_->lum_pipeline, grp_only, slot_base + 3, impl_->lum_view);
    }
    fullscreen(impl_->composite_pipeline, grp_comp, slot_base + 0, out_view);
    // Overlay (UI) after the tonemap, depth-tested against the scene.
    {
      WGPURenderPassColorAttachment color{};
      color.view = out_view;
      color.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
      color.loadOp = WGPULoadOp_Load;
      color.storeOp = WGPUStoreOp_Store;
      WGPURenderPassDepthStencilAttachment depth_att{};
      depth_att.view = depth;
      depth_att.depthLoadOp = WGPULoadOp_Load;
      depth_att.depthStoreOp = WGPUStoreOp_Store;
      WGPURenderPassDescriptor desc{};
      desc.colorAttachmentCount = 1;
      desc.colorAttachments = &color;
      desc.depthStencilAttachment = &depth_att;
      WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(encoder, &desc);
      record_overlay(pass);
      wgpuRenderPassEncoderEnd(pass);
      wgpuRenderPassEncoderRelease(pass);
    }
    if (do_lum && !impl_->lum_map_inflight) {
      WGPUTexelCopyTextureInfo src{};
      src.texture = impl_->lum_texture;
      WGPUTexelCopyBufferInfo dst{};
      dst.buffer = impl_->lum_readback;
      dst.layout.bytesPerRow = Impl::kLumSize * 16;
      dst.layout.rowsPerImage = Impl::kLumSize;
      const WGPUExtent3D extent{Impl::kLumSize, Impl::kLumSize, 1};
      wgpuCommandEncoderCopyTextureToBuffer(encoder, &src, &dst, &extent);
    }
    WGPUCommandBuffer commands = wgpuCommandEncoderFinish(encoder, nullptr);
    wgpuCommandEncoderRelease(encoder);
    wgpuQueueSubmit(impl_->queue, 1, &commands);
    wgpuCommandBufferRelease(commands);
    if (do_lum && !impl_->lum_map_inflight) {
      impl_->lum_map_inflight = true;
      WGPUBufferMapCallbackInfo map_info{};
      map_info.mode = WGPUCallbackMode_AllowProcessEvents;
      map_info.callback = [](WGPUMapAsyncStatus status, WGPUStringView, void* u1,
                             void* u2) {
        auto* self = static_cast<Impl*>(u1);
        (void)u2;
        self->lum_map_ready = status == WGPUMapAsyncStatus_Success;
        if (!self->lum_map_ready) {
          self->lum_map_inflight = false;
        }
      };
      map_info.userdata1 = impl_.get();
      map_info.userdata2 = nullptr;
      wgpuBufferMapAsync(impl_->lum_readback, WGPUMapMode_Read, 0,
                         static_cast<std::uint64_t>(Impl::kLumSize) * Impl::kLumSize * 16,
                         map_info);
    }
  };

  if (have_surface) {
    WGPUTextureView view = wgpuTextureCreateView(surface_texture.texture, nullptr);
    render_full(impl_->post_main, impl_->depth_view, view, 0, true);
    wgpuTextureViewRelease(view);
    wgpuSurfacePresent(impl_->surface);
    wgpuTextureRelease(surface_texture.texture);
    if (taa_on) {
      impl_->taa_valid = true;
      impl_->taa_parity = 1 - impl_->taa_parity;
    } else {
      impl_->taa_valid = false;
    }
  } else {
    // Headless (hidden window): the eye still adapts — render the scene
    // into the HDR target and run the luminance reduction, skipping the
    // bloom/composite that would need a presentable target.
    WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(impl_->device, nullptr);
    {
      WGPURenderPassColorAttachment color = attachment;
      color.view = impl_->post_main.hdr_view;
      WGPURenderPassDepthStencilAttachment depth_att = depth_attachment;
      WGPURenderPassDescriptor desc{};
      desc.colorAttachmentCount = 1;
      desc.colorAttachments = &color;
      desc.depthStencilAttachment = &depth_att;
      WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(encoder, &desc);
      record_scene(pass);
      wgpuRenderPassEncoderEnd(pass);
      wgpuRenderPassEncoderRelease(pass);
    }
    {
      WGPURenderPassColorAttachment color{};
      color.view = impl_->lum_view;
      color.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
      color.loadOp = WGPULoadOp_Clear;
      color.storeOp = WGPUStoreOp_Store;
      color.clearValue = WGPUColor{0.0, 0.0, 0.0, 1.0};
      WGPURenderPassDescriptor desc{};
      desc.colorAttachmentCount = 1;
      desc.colorAttachments = &color;
      WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(encoder, &desc);
      wgpuRenderPassEncoderSetPipeline(pass, impl_->lum_pipeline);
      const std::uint32_t dyn = 3 * 256;
      wgpuRenderPassEncoderSetBindGroup(pass, 0, impl_->post_main.grp_hdr_only, 1, &dyn);
      wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
      wgpuRenderPassEncoderEnd(pass);
      wgpuRenderPassEncoderRelease(pass);
    }
    if (!impl_->lum_map_inflight) {
      WGPUTexelCopyTextureInfo src{};
      src.texture = impl_->lum_texture;
      WGPUTexelCopyBufferInfo dst{};
      dst.buffer = impl_->lum_readback;
      dst.layout.bytesPerRow = Impl::kLumSize * 16;
      dst.layout.rowsPerImage = Impl::kLumSize;
      const WGPUExtent3D extent{Impl::kLumSize, Impl::kLumSize, 1};
      wgpuCommandEncoderCopyTextureToBuffer(encoder, &src, &dst, &extent);
    }
    WGPUCommandBuffer commands = wgpuCommandEncoderFinish(encoder, nullptr);
    wgpuCommandEncoderRelease(encoder);
    wgpuQueueSubmit(impl_->queue, 1, &commands);
    wgpuCommandBufferRelease(commands);
    if (!impl_->lum_map_inflight) {
      impl_->lum_map_inflight = true;
      WGPUBufferMapCallbackInfo map_info{};
      map_info.mode = WGPUCallbackMode_AllowProcessEvents;
      map_info.callback = [](WGPUMapAsyncStatus status, WGPUStringView, void* u1,
                             void* u2) {
        auto* self = static_cast<Impl*>(u1);
        (void)u2;
        self->lum_map_ready = status == WGPUMapAsyncStatus_Success;
        if (!self->lum_map_ready) {
          self->lum_map_inflight = false;
        }
      };
      map_info.userdata1 = impl_.get();
      map_info.userdata2 = nullptr;
      wgpuBufferMapAsync(impl_->lum_readback, WGPUMapMode_Read, 0,
                         static_cast<std::uint64_t>(Impl::kLumSize) * Impl::kLumSize * 16,
                         map_info);
    }
  }

  // --- one-shot capture: identical frame into an offscreen target -------
  if (!impl_->capture_path.empty() || impl_->readback_requested) {
    const std::string path = impl_->capture_path;
    impl_->capture_path.clear();
    const bool want_readback = impl_->readback_requested;
    impl_->readback_requested = false;
    const std::uint32_t width = impl_->width;
    const std::uint32_t height = impl_->height;
    const std::uint32_t bytes_per_row = ((width * 4 + 255) / 256) * 256;

    WGPUTextureDescriptor color_desc{};
    color_desc.label = sv("capture-color");
    color_desc.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc;
    color_desc.dimension = WGPUTextureDimension_2D;
    color_desc.size = WGPUExtent3D{width, height, 1};
    color_desc.format = impl_->format;
    color_desc.mipLevelCount = 1;
    color_desc.sampleCount = 1;
    WGPUTexture color_tex = wgpuDeviceCreateTexture(impl_->device, &color_desc);
    WGPUTextureView color_view = wgpuTextureCreateView(color_tex, nullptr);

    WGPUBufferDescriptor read_desc{};
    read_desc.label = sv("capture-readback");
    read_desc.usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst;
    read_desc.size = static_cast<std::uint64_t>(bytes_per_row) * height;
    WGPUBuffer read_buffer = wgpuDeviceCreateBuffer(impl_->device, &read_desc);

    // The readback wants the parity the presented frame wrote, so the
    // re-render composes from the same history (the TAA pass already ran).
    if (taa_on) impl_->taa_parity = 1 - impl_->taa_parity;
    render_full(impl_->post_main, impl_->depth_view, color_view, 0, false);
    if (taa_on) impl_->taa_parity = 1 - impl_->taa_parity;

    const std::uint32_t depth_bpr = ((width * 4 + 255) / 256) * 256;
    WGPUBuffer depth_buffer = nullptr;
    if (want_readback) {
      WGPUBufferDescriptor dd{};
      dd.label = sv("readback-depth");
      dd.usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst;
      dd.size = static_cast<std::uint64_t>(depth_bpr) * height;
      depth_buffer = wgpuDeviceCreateBuffer(impl_->device, &dd);
    }
    WGPUCommandEncoder cap_encoder = wgpuDeviceCreateCommandEncoder(impl_->device, nullptr);
    WGPUTexelCopyTextureInfo src{};
    src.texture = color_tex;
    WGPUTexelCopyBufferInfo dst{};
    dst.buffer = read_buffer;
    dst.layout.bytesPerRow = bytes_per_row;
    dst.layout.rowsPerImage = height;
    const WGPUExtent3D extent{width, height, 1};
    wgpuCommandEncoderCopyTextureToBuffer(cap_encoder, &src, &dst, &extent);
    if (want_readback) {
      WGPUTexelCopyTextureInfo dsrc{};
      dsrc.texture = impl_->depth_texture;
      dsrc.aspect = WGPUTextureAspect_DepthOnly;
      WGPUTexelCopyBufferInfo ddst{};
      ddst.buffer = depth_buffer;
      ddst.layout.bytesPerRow = depth_bpr;
      ddst.layout.rowsPerImage = height;
      wgpuCommandEncoderCopyTextureToBuffer(cap_encoder, &dsrc, &ddst, &extent);
    }
    WGPUCommandBuffer cap_commands = wgpuCommandEncoderFinish(cap_encoder, nullptr);
    wgpuCommandEncoderRelease(cap_encoder);
    wgpuQueueSubmit(impl_->queue, 1, &cap_commands);
    wgpuCommandBufferRelease(cap_commands);

    bool mapped_done = false;
    bool mapped_ok = false;
    WGPUBufferMapCallbackInfo map_info{};
    map_info.mode = WGPUCallbackMode_AllowProcessEvents;
    map_info.callback = [](WGPUMapAsyncStatus status, WGPUStringView, void* u1, void* u2) {
      *static_cast<bool*>(u1) = true;
      *static_cast<bool*>(u2) = status == WGPUMapAsyncStatus_Success;
    };
    map_info.userdata1 = &mapped_done;
    map_info.userdata2 = &mapped_ok;
    wgpuBufferMapAsync(read_buffer, WGPUMapMode_Read, 0, read_desc.size, map_info);
    while (!mapped_done) {
      wgpuDevicePoll(impl_->device, 1U, nullptr);
    }
    if (mapped_ok) {
      const auto* data = static_cast<const std::uint8_t*>(
          wgpuBufferGetConstMappedRange(read_buffer, 0, read_desc.size));
      const bool bgra = impl_->format == WGPUTextureFormat_BGRA8Unorm ||
                        impl_->format == WGPUTextureFormat_BGRA8UnormSrgb;
      if (want_readback && data != nullptr) {
        impl_->readback_rgba.resize(static_cast<std::size_t>(width) * height * 4);
        for (std::uint32_t y = 0; y < height; ++y) {
          const std::uint8_t* src_row = data + static_cast<std::size_t>(y) * bytes_per_row;
          std::uint8_t* out = impl_->readback_rgba.data() + static_cast<std::size_t>(y) * width * 4;
          for (std::uint32_t x = 0; x < width; ++x) {
            out[x * 4 + 0] = src_row[x * 4 + (bgra ? 2 : 0)];
            out[x * 4 + 1] = src_row[x * 4 + 1];
            out[x * 4 + 2] = src_row[x * 4 + (bgra ? 0 : 2)];
            out[x * 4 + 3] = 255;
          }
        }
        impl_->readback_w = width;
        impl_->readback_h = height;
      }
      const std::unique_ptr<std::FILE, int (*)(std::FILE*)> file(
          path.empty() ? nullptr : std::fopen(path.c_str(), "wb"), &std::fclose);
      if (file != nullptr && data != nullptr) {
        std::fprintf(file.get(), "P6\n%u %u\n255\n", width, height);
        std::vector<std::uint8_t> row(static_cast<std::size_t>(width) * 3);
        for (std::uint32_t y = 0; y < height; ++y) {
          const std::uint8_t* src_row = data + static_cast<std::size_t>(y) * bytes_per_row;
          for (std::uint32_t x = 0; x < width; ++x) {
            row[x * 3 + 0] = src_row[x * 4 + (bgra ? 2 : 0)];
            row[x * 3 + 1] = src_row[x * 4 + 1];
            row[x * 3 + 2] = src_row[x * 4 + (bgra ? 0 : 2)];
          }
          std::fwrite(row.data(), 1, row.size(), file.get());
        }
        std::printf("capture: wrote %ux%u to %s (avg_lum %.5f exposure %.2f scotopic %.2f)\n",
                    width, height, path.c_str(), impl_->avg_luminance, impl_->exposure,
                    impl_->scotopic);
      } else if (!path.empty()) {
        std::fprintf(stderr, "capture: FAILED to open %s\n", path.c_str());
      }
      wgpuBufferUnmap(read_buffer);
    } else {
      std::fprintf(stderr, "capture: readback map failed\n");
    }
    if (want_readback && depth_buffer != nullptr) {
      bool d_done = false;
      bool d_ok = false;
      WGPUBufferMapCallbackInfo dinfo{};
      dinfo.mode = WGPUCallbackMode_AllowProcessEvents;
      dinfo.callback = [](WGPUMapAsyncStatus status, WGPUStringView, void* u1, void* u2) {
        *static_cast<bool*>(u1) = true;
        *static_cast<bool*>(u2) = status == WGPUMapAsyncStatus_Success;
      };
      dinfo.userdata1 = &d_done;
      dinfo.userdata2 = &d_ok;
      wgpuBufferMapAsync(depth_buffer, WGPUMapMode_Read, 0, static_cast<std::uint64_t>(depth_bpr) * height, dinfo);
      while (!d_done) {
        wgpuDevicePoll(impl_->device, 1U, nullptr);
      }
      if (d_ok) {
        const auto* dd = static_cast<const std::uint8_t*>(
            wgpuBufferGetConstMappedRange(depth_buffer, 0, static_cast<std::uint64_t>(depth_bpr) * height));
        impl_->readback_depth.resize(static_cast<std::size_t>(width) * height);
        for (std::uint32_t y = 0; y < height; ++y) {
          std::memcpy(impl_->readback_depth.data() + static_cast<std::size_t>(y) * width,
                      dd + static_cast<std::size_t>(y) * depth_bpr, static_cast<std::size_t>(width) * 4);
        }
        wgpuBufferUnmap(depth_buffer);
        impl_->readback_ready = true;
      }
      wgpuBufferRelease(depth_buffer);
    }
    wgpuBufferRelease(read_buffer);
    wgpuTextureViewRelease(color_view);
    wgpuTextureRelease(color_tex);
  }

  // --- debug recorder: ring buffer + triggered sequences ----------------
  impl_->last_frame_time = frame.time_s;
  ++impl_->frame_counter;
  const bool future_active = !impl_->rec_dir.empty() &&
                             static_cast<double>(frame.time_s) < impl_->rec_until;
  const bool ring_due = frame.time_s - impl_->last_ring_time >= 0.0333f ||
                        frame.time_s < impl_->last_ring_time;
  if ((impl_->ring_enabled || future_active) && ring_due) {
    impl_->last_ring_time = frame.time_s;
    impl_->ensure_recorder_targets();
    render_full(impl_->post_rec, impl_->rec_depth_view, impl_->rec_color_view, 4, false);
    WGPUCommandEncoder rec_encoder = wgpuDeviceCreateCommandEncoder(impl_->device, nullptr);
    WGPUTexelCopyTextureInfo rec_src{};
    rec_src.texture = impl_->rec_color;
    WGPUTexelCopyBufferInfo rec_dst{};
    rec_dst.buffer = impl_->rec_buffer;
    rec_dst.layout.bytesPerRow = impl_->rec_bpr;
    rec_dst.layout.rowsPerImage = Impl::kRecH;
    const WGPUExtent3D rec_extent{Impl::kRecW, Impl::kRecH, 1};
    wgpuCommandEncoderCopyTextureToBuffer(rec_encoder, &rec_src, &rec_dst, &rec_extent);
    WGPUCommandBuffer rec_commands = wgpuCommandEncoderFinish(rec_encoder, nullptr);
    wgpuCommandEncoderRelease(rec_encoder);
    wgpuQueueSubmit(impl_->queue, 1, &rec_commands);
    wgpuCommandBufferRelease(rec_commands);

    bool map_done = false;
    bool map_ok = false;
    WGPUBufferMapCallbackInfo map_info{};
    map_info.mode = WGPUCallbackMode_AllowProcessEvents;
    map_info.callback = [](WGPUMapAsyncStatus status, WGPUStringView, void* u1, void* u2) {
      *static_cast<bool*>(u1) = true;
      *static_cast<bool*>(u2) = status == WGPUMapAsyncStatus_Success;
    };
    map_info.userdata1 = &map_done;
    map_info.userdata2 = &map_ok;
    wgpuBufferMapAsync(impl_->rec_buffer, WGPUMapMode_Read, 0,
                       static_cast<std::uint64_t>(impl_->rec_bpr) * Impl::kRecH, map_info);
    while (!map_done) {
      wgpuDevicePoll(impl_->device, 1U, nullptr);
    }
    if (map_ok) {
      const auto* data = static_cast<const std::uint8_t*>(wgpuBufferGetConstMappedRange(
          impl_->rec_buffer, 0, static_cast<std::uint64_t>(impl_->rec_bpr) * Impl::kRecH));
      if (data != nullptr) {
        const bool bgra = impl_->format == WGPUTextureFormat_BGRA8Unorm ||
                          impl_->format == WGPUTextureFormat_BGRA8UnormSrgb;
        std::vector<std::uint8_t> rgb(static_cast<std::size_t>(Impl::kRecW) * Impl::kRecH * 3);
        for (std::uint32_t y = 0; y < Impl::kRecH; ++y) {
          const std::uint8_t* src_row = data + static_cast<std::size_t>(y) * impl_->rec_bpr;
          std::uint8_t* dst_row = rgb.data() + static_cast<std::size_t>(y) * Impl::kRecW * 3;
          for (std::uint32_t x = 0; x < Impl::kRecW; ++x) {
            dst_row[x * 3 + 0] = src_row[x * 4 + (bgra ? 2 : 0)];
            dst_row[x * 3 + 1] = src_row[x * 4 + 1];
            dst_row[x * 3 + 2] = src_row[x * 4 + (bgra ? 0 : 2)];
          }
        }
        if (future_active) {
          impl_->emit_sequence_frame(frame.time_s, rgb.data());
        } else {
          impl_->ring.push_back(Impl::RingFrame{frame.time_s, std::move(rgb)});
          while (!impl_->ring.empty() &&
                 (frame.time_s - impl_->ring.front().time_s > Impl::kRingSeconds ||
                  impl_->ring.size() > 120)) {
            impl_->ring.pop_front();
          }
        }
      }
      wgpuBufferUnmap(impl_->rec_buffer);
    }
  }
  return have_surface;
}

void Rhi::request_capture(const std::string& path) { impl_->capture_path = path; }

void Rhi::request_readback() { impl_->readback_requested = true; }

bool Rhi::take_readback(std::vector<std::uint8_t>* rgba, std::vector<float>* depth,
                        std::uint32_t* width, std::uint32_t* height) {
  if (!impl_->readback_ready) return false;
  impl_->readback_ready = false;
  rgba->swap(impl_->readback_rgba);
  depth->swap(impl_->readback_depth);
  *width = impl_->readback_w;
  *height = impl_->readback_h;
  return true;
}

void Rhi::set_ring_enabled(bool enabled) { impl_->ring_enabled = enabled; }

void Rhi::trigger_recording(const std::string& dir, double future_seconds) {
  impl_->rec_dir = dir;
  impl_->rec_seq_index = 0;
  impl_->rec_until = static_cast<double>(impl_->last_frame_time) + future_seconds;
  // Dump the ring (the immediate past) as the sequence prefix.
  for (const Impl::RingFrame& ring_frame : impl_->ring) {
    impl_->emit_sequence_frame(ring_frame.time_s, ring_frame.rgb.data());
  }
  impl_->ring.clear();
  std::printf("recorder: %d ring frames dumped to %s, recording %.1fs more\n",
              impl_->rec_seq_index, dir.c_str(), future_seconds);
}

bool Rhi::recording_active() const {
  return !impl_->rec_dir.empty() &&
         static_cast<double>(impl_->last_frame_time) < impl_->rec_until;
}

bool Rhi::render_clear(float r, float g, float b) {
  FrameParams frame;
  frame.sky[0] = r;
  frame.sky[1] = g;
  frame.sky[2] = b;
  return render_frame(frame, nullptr, 0);
}

const std::string& Rhi::adapter_info() const { return impl_->adapter_info; }

}  // namespace inf::render
