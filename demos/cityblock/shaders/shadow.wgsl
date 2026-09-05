#include "common.wgsl"
struct Cascade { light_vp: mat4x4<f32> };
@group(0) @binding(0) var<uniform> cascade: Cascade;
@group(1) @binding(0) var leaf_tex: texture_2d<f32>;
@group(1) @binding(1) var leaf_samp: sampler;

struct VOut { @builtin(position) pos: vec4<f32>, @location(0) uv: vec2<f32>, @location(1) @interpolate(flat) material: u32 };

@vertex fn vs_main(in: VertexIn) -> VOut {
  var o: VOut;
  o.pos = cascade.light_vp * vec4<f32>(in.position, 1.0);
  o.uv = in.uv;
  o.material = in.material;
  return o;
}
// Opaque: depth only (no fragment stage). Foliage: alpha test.
@fragment fn fs_foliage(in: VOut) {
  let a = textureSample(leaf_tex, leaf_samp, in.uv).a;
  if (a < 0.45) { discard; }
}
