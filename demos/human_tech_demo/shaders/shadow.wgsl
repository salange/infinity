#include "common.wgsl"
struct Cascade { light_vp: mat4x4<f32> };
@group(0) @binding(0) var<uniform> cascade: Cascade;
@group(0) @binding(2) var<storage, read> materials: array<Material>;
@group(0) @binding(1) var<storage, read> instances: array<Instance>;
@group(1) @binding(0) var leaf_tex: texture_2d<f32>;
@group(1) @binding(1) var leaf_samp: sampler;

struct VOut { @builtin(position) pos: vec4<f32>, @location(0) uv: vec2<f32>, @location(1) @interpolate(flat) material: u32 };

@vertex fn vs_main(in: VertexIn, @builtin(instance_index) instance_id: u32) -> VOut {
  var o: VOut;
  o.pos = cascade.light_vp * instances[instance_id].model * vec4<f32>(in.position, 1.0);
  o.uv = in.uv;
  o.material = in.packed.x | (in.packed.y << 8u);
  return o;
}
// Clear panes transmit sunlight rather than casting an opaque rectangle.
@fragment fn fs_main(in: VOut) {if((u32(materials[in.material].misc.x+.5)&FLAG_TRANSMISSION)!=0u){discard;}}
// Foliage: alpha test.
@fragment fn fs_foliage(in: VOut) {
  let a = textureSample(leaf_tex, leaf_samp, in.uv).a;
  if (a < 0.45) { discard; }
}
