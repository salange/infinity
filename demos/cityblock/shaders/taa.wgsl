#include "fullscreen.wgsl"
// Temporal anti-aliasing for a static world: the previous frame is
// reprojected exactly through depth, clamped to the current 3x3 colour
// neighbourhood, and blended with the jittered current frame.
struct TaaParams {
  inv_view_proj: mat4x4<f32>,   // current, unjittered
  prev_view_proj: mat4x4<f32>,  // previous, unjittered
  params: vec4<f32>,            // blend (history weight), valid (0/1), w, h
  jitter: vec4<f32>,            // current jitter in pixels (x, y), 0, 0
};
@group(0) @binding(0) var<uniform> taa: TaaParams;
@group(0) @binding(1) var current: texture_2d<f32>;
@group(0) @binding(2) var history: texture_2d<f32>;
@group(0) @binding(3) var depth_tex: texture_depth_2d;
@group(0) @binding(4) var samp: sampler;

fn luma(c: vec3<f32>) -> f32 { return dot(c, vec3<f32>(0.2126, 0.7152, 0.0722)); }
fn tonemap_w(c: vec3<f32>) -> vec3<f32> { return c / (1.0 + luma(c)); }
fn tonemap_inv(c: vec3<f32>) -> vec3<f32> { return c / max(1.0 - luma(c), 1e-3); }

@fragment fn fs_taa(in: FSOut) -> @location(0) vec4<f32> {
  let size = taa.params.zw;
  let texel = 1.0 / size;
  // the current frame was rendered with a sub-pixel jitter: sample it back at
  // the unjittered pixel centre so the accumulated image does not drift
  let uv_cur = in.uv - taa.jitter.xy * texel;
  let cur = textureSample(current, samp, uv_cur).rgb;
  // 3x3 neighbourhood bounds in tonemapped space
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
  // reproject through depth
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
    // clip toward the current neighbourhood box (variance clipping)
    let centre = (box_lo + box_hi) * 0.5;
    let extent = (box_hi - box_lo) * 0.5 + vec3<f32>(1e-4);
    let d = h - centre;
    let t = max(abs(d.x) / extent.x, max(abs(d.y) / extent.y, abs(d.z) / extent.z));
    hist_t = select(h, centre + d / t, t > 1.0);
    weight = taa.params.x;
    // sky (far depth) can keep full history
    if (depth >= 0.99999) { weight = min(taa.params.x, 0.9); }
  }
  let out_t = mix(cur_t, hist_t, weight);
  return vec4<f32>(tonemap_inv(out_t), 1.0);
}
