#pragma once

// T0021 WP1: the passes around the city pipeline — shadow cascades and
// the depth/normal prepass for TERRAIN meshes (the 40-byte layout; the
// city meshes use their own stages in city_shader.hpp), screen-space
// ambient occlusion with a depth-aware blur, and temporal anti-aliasing
// (jittered projection, exact depth reprojection, variance clipping).
// Depth is reversed-Z (near = 1, far = 0) in the scene targets; the
// shadow maps are plain orthographic depth (Less, cleared to 1).

namespace inf::render {

constexpr const char* kTerrainPassShader = R"(
struct Uniforms {
  mvp: mat4x4<f32>,
  color: vec4<f32>,
  aux: vec4<f32>,
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
  jitter: vec4<f32>,
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
  sh: array<vec4<f32>, 9>,
  cascade: vec4<f32>,
  cascade_extent: vec4<f32>,
  screen: vec4<f32>,
  params: vec4<f32>,
  params2: vec4<f32>,
  jitter: vec4<f32>,
};
struct Cascade { light_vp: mat4x4<f32> };
@group(0) @binding(0) var<uniform> u: Uniforms;
@group(0) @binding(1) var<uniform> frame: Frame;
@group(1) @binding(0) var<uniform> cascade: Cascade;
@group(1) @binding(1) var<uniform> cf: CityFrame;

struct ShadowOut { @builtin(position) pos: vec4<f32> };
@vertex fn vs_shadow(@location(0) position: vec3<f32>, @location(1) normal: vec3<f32>,
                     @location(2) weights: vec4<f32>) -> ShadowOut {
  var o: ShadowOut;
  o.pos = cascade.light_vp * vec4<f32>(position + u.aux.xyz, 1.0);
  return o;
}
struct PreOut { @builtin(position) pos: vec4<f32>, @location(0) vnormal: vec3<f32> };
@vertex fn vs_prepass(@location(0) position: vec3<f32>, @location(1) normal: vec3<f32>,
                      @location(2) weights: vec4<f32>) -> PreOut {
  var o: PreOut;
  var clip = u.mvp * vec4<f32>(position, 1.0);
  clip.x += cf.jitter.x * clip.w;
  clip.y += cf.jitter.y * clip.w;
  o.pos = clip;
  o.vnormal = (cf.view * vec4<f32>(normal, 0.0)).xyz;
  return o;
}
@fragment fn fs_prepass(in: PreOut) -> @location(0) vec4<f32> {
  return vec4<f32>(normalize(in.vnormal), 1.0);
}
)";

constexpr const char* kCityPostShader = R"(
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
struct TaaParams {
  inv_view_proj: mat4x4<f32>,   // current, unjittered
  prev_view_proj: mat4x4<f32>,  // previous, in this frame's camera-relative space
  params: vec4<f32>,            // history weight, valid (0/1), w, h
  jitter: vec4<f32>,            // current jitter in pixels (x, y), 0, 0
};
@group(0) @binding(0) var<uniform> cf: CityFrame;
@group(0) @binding(1) var depth_tex: texture_depth_2d;
@group(0) @binding(2) var normal_tex: texture_2d<f32>;
@group(0) @binding(3) var samp: sampler;
@group(0) @binding(4) var ao_tex: texture_2d<f32>;
@group(1) @binding(0) var<uniform> taa: TaaParams;
@group(1) @binding(1) var current: texture_2d<f32>;
@group(1) @binding(2) var history: texture_2d<f32>;

struct FSOut { @builtin(position) pos: vec4<f32>, @location(0) uv: vec2<f32> };
@vertex fn vs_fullscreen(@builtin(vertex_index) vi: u32) -> FSOut {
  var o: FSOut;
  let x = f32(i32(vi & 1u) * 4 - 1);
  let y = f32(i32(vi >> 1u) * 4 - 1);
  o.pos = vec4<f32>(x, y, 0.0, 1.0);
  o.uv = vec2<f32>(x * 0.5 + 0.5, 0.5 - y * 0.5);
  return o;
}

fn view_pos(uv: vec2<f32>, depth: f32) -> vec3<f32> {
  let ndc = vec4<f32>(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0);
  let p = cf.inv_proj * ndc;
  return p.xyz / p.w;
}

const KERNEL: array<vec3<f32>, 16> = array<vec3<f32>, 16>(
  vec3<f32>( 0.5381, 0.1856, 0.4319), vec3<f32>( 0.1379, 0.2486, 0.4430),
  vec3<f32>( 0.3371, 0.5679, 0.0057), vec3<f32>(-0.6999,-0.0451, 0.0019),
  vec3<f32>( 0.0689,-0.1598, 0.8547), vec3<f32>( 0.0560, 0.0069, 0.1843),
  vec3<f32>(-0.0146, 0.1402, 0.0762), vec3<f32>( 0.0100,-0.1924, 0.0344),
  vec3<f32>(-0.3577,-0.5301, 0.4358), vec3<f32>(-0.3169, 0.1063, 0.0158),
  vec3<f32>( 0.0103,-0.5869, 0.0046), vec3<f32>(-0.0897,-0.4940, 0.3287),
  vec3<f32>( 0.7119,-0.0154, 0.0918), vec3<f32>(-0.0533, 0.0596, 0.5411),
  vec3<f32>( 0.0352,-0.0631, 0.5460), vec3<f32>(-0.4776, 0.2847, 0.0271));

// Reversed-Z: the far plane is depth 0.
@fragment fn fs_ssao(in: FSOut) -> @location(0) vec4<f32> {
  let depth = textureSample(depth_tex, samp, in.uv);
  if (depth <= 1.0e-7) { return vec4<f32>(1.0); }
  let p = view_pos(in.uv, depth);
  let n = normalize(textureSample(normal_tex, samp, in.uv).xyz);
  let px = in.pos.xy;
  let noise = fract(52.9829189 * fract(dot(px, vec2<f32>(0.06711056, 0.00583715))));
  let ang = noise * 6.2831853;
  let rnd = vec3<f32>(cos(ang), sin(ang), 0.0);
  var t = normalize(rnd - n * dot(rnd, n));
  if (abs(dot(n, rnd)) > 0.99) { t = normalize(cross(n, vec3<f32>(0.0, 1.0, 0.0))); }
  let b = cross(n, t);
  let dist = -p.z;
  let radius = clamp(0.8 + dist * 0.03, 1.0, 4.0);
  var occlusion = 0.0;
  for (var i = 0u; i < 16u; i = i + 1u) {
    let fi = (f32(i) + 0.5) / 16.0;
    let k = normalize(KERNEL[i]) * mix(0.12, 1.0, fi * fi);
    let s = p + (t * k.x + b * k.y + n * k.z) * radius;
    var clip = cf.proj * vec4<f32>(s, 1.0);
    let suv = vec2<f32>(clip.x / clip.w * 0.5 + 0.5, 0.5 - clip.y / clip.w * 0.5);
    if (suv.x < 0.0 || suv.x > 1.0 || suv.y < 0.0 || suv.y > 1.0) { continue; }
    let sd = textureSample(depth_tex, samp, suv);
    if (sd <= 1.0e-7) { continue; }
    let sp = view_pos(suv, sd);
    let range = smoothstep(0.0, 1.0, radius / max(abs(p.z - sp.z), 1e-4));
    if (sp.z >= s.z + 0.05) { occlusion += range; }
  }
  var ao = clamp(1.0 - 1.25 * occlusion / 16.0, 0.0, 1.0);
  // Beyond a few hundred metres the kernel is sub-pixel and the term is
  // noise on terrain: fade to open.
  ao = mix(ao, 1.0, smoothstep(400.0, 1200.0, dist));
  return vec4<f32>(ao, ao, ao, 1.0);
}

@fragment fn fs_blur(in: FSOut) -> @location(0) vec4<f32> {
  let d0 = textureSample(depth_tex, samp, in.uv);
  let z0 = view_pos(in.uv, d0).z;
  var sum = 0.0;
  var wsum = 0.0;
  for (var y = -2; y <= 1; y = y + 1) {
    for (var x = -2; x <= 1; x = x + 1) {
      let uv = in.uv + vec2<f32>(f32(x) + 0.5, f32(y) + 0.5) * cf.screen.zw;
      let d = textureSample(depth_tex, samp, uv);
      let z = view_pos(uv, d).z;
      let w = exp(-abs(z - z0) * 2.0) + 1.0e-3;  // floor: no 0/0 on steep far-field depth
      sum += textureSample(ao_tex, samp, uv).r * w;
      wsum += w;
    }
  }
  let ao = sum / max(wsum, 1e-4);
  return vec4<f32>(ao, ao, ao, 1.0);
}

fn luma(c: vec3<f32>) -> f32 { return dot(c, vec3<f32>(0.2126, 0.7152, 0.0722)); }
fn tonemap_w(c: vec3<f32>) -> vec3<f32> { return c / (1.0 + luma(c)); }
fn tonemap_inv(c: vec3<f32>) -> vec3<f32> { return c / max(1.0 - luma(c), 1e-3); }

@fragment fn fs_taa(in: FSOut) -> @location(0) vec4<f32> {
  let size = taa.params.zw;
  let texel = 1.0 / size;
  let uv_cur = in.uv - taa.jitter.xy * texel;
  let cur = textureSample(current, samp, uv_cur).rgb;
  var mn = vec3<f32>(1e9);
  var mx = vec3<f32>(-1e9);
  var m1 = vec3<f32>(0.0);
  var m2 = vec3<f32>(0.0);
  for (var y = -1; y <= 1; y = y + 1) {
    for (var x = -1; x <= 1; x = x + 1) {
      let c = tonemap_w(textureSample(current, samp, uv_cur + vec2<f32>(f32(x), f32(y)) * texel).rgb);
      mn = min(mn, c);
      mx = max(mx, c);
      m1 += c;
      m2 += c * c;
    }
  }
  let mean = m1 / 9.0;
  let sigma = sqrt(max(m2 / 9.0 - mean * mean, vec3<f32>(0.0)));
  let box_lo = max(mn, mean - sigma * 1.25);
  let box_hi = min(mx, mean + sigma * 1.25);
  let depth = textureSample(depth_tex, samp, in.uv);
  let ndc = vec4<f32>(in.uv.x * 2.0 - 1.0, 1.0 - in.uv.y * 2.0, depth, 1.0);
  let wp = taa.inv_view_proj * ndc;
  let world = wp.xyz / wp.w;
  let pc = taa.prev_view_proj * vec4<f32>(world, 1.0);
  let prev_uv = vec2<f32>(pc.x / pc.w * 0.5 + 0.5, 0.5 - pc.y / pc.w * 0.5);
  let cur_t = tonemap_w(cur);
  var hist_t = cur_t;
  var weight = 0.0;
  if (taa.params.y > 0.5 && pc.w > 0.0 && prev_uv.x > 0.0 && prev_uv.x < 1.0 && prev_uv.y > 0.0 && prev_uv.y < 1.0) {
    let h = tonemap_w(textureSample(history, samp, prev_uv).rgb);
    let centre = (box_lo + box_hi) * 0.5;
    let extent = (box_hi - box_lo) * 0.5 + vec3<f32>(1e-4);
    let d = h - centre;
    let t = max(abs(d.x) / extent.x, max(abs(d.y) / extent.y, abs(d.z) / extent.z));
    hist_t = select(h, centre + d / t, t > 1.0);
    weight = taa.params.x;
    if (depth <= 1.0e-7) { weight = min(taa.params.x, 0.9); }
  }
  let out_t = mix(cur_t, hist_t, weight);
  return vec4<f32>(tonemap_inv(out_t), 1.0);
}
)";

// T0022 B.2: the depth pyramid (Hi-Z) for GPU occlusion culling. Mip 0
// copies the prepass depth, every further mip holds the farthest depth
// of its 2x2 children — the MINIMUM under reversed Z — so a conservative
// "is anything in this footprint farther than X" test is one lookup.
constexpr const char* kCityHizShader = R"(
@group(0) @binding(0) var src_depth: texture_depth_2d;
@group(0) @binding(1) var src_level: texture_2d<f32>;
struct FSOut { @builtin(position) pos: vec4<f32>, @location(0) uv: vec2<f32> };
@vertex fn vs_fullscreen(@builtin(vertex_index) vi: u32) -> FSOut {
  var o: FSOut;
  let x = f32(i32(vi & 1u) * 4 - 1);
  let y = f32(i32(vi >> 1u) * 4 - 1);
  o.pos = vec4<f32>(x, y, 0.0, 1.0);
  o.uv = vec2<f32>(x * 0.5 + 0.5, 0.5 - y * 0.5);
  return o;
}
@fragment fn fs_copy(in: FSOut) -> @location(0) vec4<f32> {
  let dims = textureDimensions(src_depth);
  let p = vec2<i32>(in.pos.xy);
  let d = textureLoad(src_depth, clamp(p, vec2<i32>(0), vec2<i32>(dims) - vec2<i32>(1)), 0);
  return vec4<f32>(d, 0.0, 0.0, 1.0);
}
@fragment fn fs_down(in: FSOut) -> @location(0) vec4<f32> {
  let dims = vec2<i32>(textureDimensions(src_level));
  let p = vec2<i32>(in.pos.xy) * 2;
  let a = textureLoad(src_level, clamp(p, vec2<i32>(0), dims - 1), 0).r;
  let b = textureLoad(src_level, clamp(p + vec2<i32>(1, 0), vec2<i32>(0), dims - 1), 0).r;
  let c = textureLoad(src_level, clamp(p + vec2<i32>(0, 1), vec2<i32>(0), dims - 1), 0).r;
  let d = textureLoad(src_level, clamp(p + vec2<i32>(1, 1), vec2<i32>(0), dims - 1), 0).r;
  var m = min(min(a, b), min(c, d));
  // odd source sizes: include the extra column/row so nothing is missed
  if ((dims.x & 1) == 1) {
    m = min(m, textureLoad(src_level, clamp(p + vec2<i32>(2, 0), vec2<i32>(0), dims - 1), 0).r);
    m = min(m, textureLoad(src_level, clamp(p + vec2<i32>(2, 1), vec2<i32>(0), dims - 1), 0).r);
  }
  if ((dims.y & 1) == 1) {
    m = min(m, textureLoad(src_level, clamp(p + vec2<i32>(0, 2), vec2<i32>(0), dims - 1), 0).r);
    m = min(m, textureLoad(src_level, clamp(p + vec2<i32>(1, 2), vec2<i32>(0), dims - 1), 0).r);
  }
  return vec4<f32>(m, 0.0, 0.0, 1.0);
}
)";

// GPU occlusion culling: every main-pass range (camera-relative bounding
// sphere + its DrawIndexedIndirect arguments) is tested against the
// depth pyramid of this frame's prepass and its instance count set to 0
// or 1. Ranges without the occlude flag (unbounded gaps, foliage) pass.
constexpr const char* kCityCullShader = R"(
struct Range { centre: vec3<f32>, radius: f32, flags: u32, pad0: u32, pad1: u32, pad2: u32 };
struct CullParams {
  view: mat4x4<f32>,
  proj: mat4x4<f32>,
  params: vec4<f32>,   // count, mip count, screen w, screen h
  params2: vec4<f32>,  // near bypass distance, 0, 0, 0
};
@group(0) @binding(0) var<uniform> cp: CullParams;
@group(0) @binding(1) var<storage, read> ranges: array<Range>;
@group(0) @binding(2) var<storage, read_write> args: array<u32>;   // 5 u32 per draw
@group(0) @binding(3) var hiz: texture_2d<f32>;

fn depth_ndc_of_view_z(zv: f32) -> f32 {
  // clip z / clip w for a view-space point at depth zv (negative forward)
  let cz = cp.proj[2][2] * zv + cp.proj[3][2];
  let cw = cp.proj[2][3] * zv + cp.proj[3][3];
  return cz / cw;
}

@compute @workgroup_size(64)
fn cs_cull(@builtin(global_invocation_id) gid: vec3<u32>) {
  let i = gid.x;
  let n = u32(cp.params.x);
  if (i >= n) { return; }
  let r = ranges[i];
  var visible = true;
  if ((r.flags & 8u) != 0u && r.radius > 0.0) {
    let vc = (cp.view * vec4<f32>(r.centre, 1.0)).xyz;
    let dist = length(vc);
    if (dist > r.radius + cp.params2.x) {
      // conservative screen rect of the sphere from its view-space bounds
      let zn = min(vc.z + r.radius, -0.05);            // nearest view depth (least negative)
      let sx = cp.proj[0][0]; let sy = cp.proj[1][1];
      let x0 = (vc.x - r.radius) * sx / -zn; let x1 = (vc.x + r.radius) * sx / -zn;
      let y0 = (vc.y - r.radius) * sy / -zn; let y1 = (vc.y + r.radius) * sy / -zn;
      let ndc_min = clamp(vec2<f32>(min(x0, x1), min(y0, y1)), vec2<f32>(-1.0), vec2<f32>(1.0));
      let ndc_max = clamp(vec2<f32>(max(x0, x1), max(y0, y1)), vec2<f32>(-1.0), vec2<f32>(1.0));
      let size = cp.params.zw;
      let uv_min = vec2<f32>(ndc_min.x * 0.5 + 0.5, 0.5 - ndc_max.y * 0.5) * size;
      let uv_max = vec2<f32>(ndc_max.x * 0.5 + 0.5, 0.5 - ndc_min.y * 0.5) * size;
      let extent = max(uv_max.x - uv_min.x, uv_max.y - uv_min.y);
      let level = clamp(i32(ceil(log2(max(extent, 1.0)))), 0, i32(cp.params.y) - 1);
      let scale = f32(1u << u32(level));
      let lo = vec2<i32>(floor(uv_min / scale));
      let hi = vec2<i32>(floor((uv_max - vec2<f32>(0.5)) / scale));
      let dims = vec2<i32>(textureDimensions(hiz, level));
      var farthest = 1.0;  // reversed Z: farther is smaller
      for (var y = lo.y; y <= min(hi.y, lo.y + 1); y = y + 1) {
        for (var x = lo.x; x <= min(hi.x, lo.x + 1); x = x + 1) {
          let c = clamp(vec2<i32>(x, y), vec2<i32>(0), dims - 1);
          farthest = min(farthest, textureLoad(hiz, c, level).r);
        }
      }
      // the sphere's nearest point in depth-buffer units; hidden if even
      // that lies behind everything drawn in its footprint
      let near_depth = depth_ndc_of_view_z(zn);
      if (near_depth < farthest * (1.0 - 1e-4) - 1e-7) { visible = false; }
    }
  }
  args[i * 5u + 1u] = select(0u, 1u, visible);
}
)";

}  // namespace inf::render
