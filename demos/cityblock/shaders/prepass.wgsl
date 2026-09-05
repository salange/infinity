#include "common.wgsl"
@group(0) @binding(0) var<uniform> frame: Frame;
@group(1) @binding(0) var leaf_tex: texture_2d<f32>;
@group(1) @binding(1) var leaf_samp: sampler;

struct VOut {
  @builtin(position) pos: vec4<f32>,
  @location(0) vnormal: vec3<f32>,
  @location(1) uv: vec2<f32>,
  @location(2) @interpolate(flat) flags: u32,
};
@vertex fn vs_main(in: VertexIn) -> VOut {
  var o: VOut;
  o.pos = frame.view_proj * vec4<f32>(in.position, 1.0);
  o.vnormal = (frame.view * vec4<f32>(in.normal, 0.0)).xyz;
  o.uv = in.uv;
  o.flags = in.material;
  return o;
}
@fragment fn fs_main(in: VOut) -> @location(0) vec4<f32> {
  return vec4<f32>(normalize(in.vnormal), 1.0);
}
@fragment fn fs_foliage(in: VOut) -> @location(0) vec4<f32> {
  let a = textureSample(leaf_tex, leaf_samp, in.uv).a;
  if (a < 0.45) { discard; }
  return vec4<f32>(normalize(in.vnormal), 1.0);
}
