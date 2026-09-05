#include "common.wgsl"
#include "fullscreen.wgsl"
@group(0) @binding(0) var<uniform> frame: Frame;
@group(1) @binding(0) var depth_tex: texture_depth_2d;
@group(1) @binding(1) var normal_tex: texture_2d<f32>;
@group(1) @binding(2) var samp: sampler;

fn view_pos(uv: vec2<f32>, depth: f32) -> vec3<f32> {
  let ndc = vec4<f32>(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0);
  let p = frame.inv_proj * ndc;
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

@fragment fn fs_ssao(in: FSOut) -> @location(0) vec4<f32> {
  let depth = textureSample(depth_tex, samp, in.uv);
  if (depth >= 0.99999) { return vec4<f32>(1.0); }
  let p = view_pos(in.uv, depth);
  let n = normalize(textureSample(normal_tex, samp, in.uv).xyz);
  // Interleaved gradient noise for the rotation.
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
    var clip = frame.proj * vec4<f32>(s, 1.0);
    let suv = vec2<f32>(clip.x / clip.w * 0.5 + 0.5, 0.5 - clip.y / clip.w * 0.5);
    if (suv.x < 0.0 || suv.x > 1.0 || suv.y < 0.0 || suv.y > 1.0) { continue; }
    let sd = textureSample(depth_tex, samp, suv);
    let sp = view_pos(suv, sd);
    let range = smoothstep(0.0, 1.0, radius / max(abs(p.z - sp.z), 1e-4));
    if (sp.z >= s.z + 0.05) { occlusion += range; }
  }
  let ao = clamp(1.0 - 1.25 * occlusion / 16.0, 0.0, 1.0);
  return vec4<f32>(ao, ao, ao, 1.0);
}

// Depth-aware 4x4 blur.
@group(1) @binding(3) var ao_tex: texture_2d<f32>;
@fragment fn fs_blur(in: FSOut) -> @location(0) vec4<f32> {
  let d0 = textureSample(depth_tex, samp, in.uv);
  let z0 = view_pos(in.uv, d0).z;
  var sum = 0.0;
  var wsum = 0.0;
  for (var y = -2; y <= 1; y = y + 1) {
    for (var x = -2; x <= 1; x = x + 1) {
      let uv = in.uv + vec2<f32>(f32(x) + 0.5, f32(y) + 0.5) * frame.screen.zw;
      let d = textureSample(depth_tex, samp, uv);
      let z = view_pos(uv, d).z;
      let w = exp(-abs(z - z0) * 2.0);
      sum += textureSample(ao_tex, samp, uv).r * w;
      wsum += w;
    }
  }
  let ao = sum / max(wsum, 1e-4);
  return vec4<f32>(ao, ao, ao, 1.0);
}
