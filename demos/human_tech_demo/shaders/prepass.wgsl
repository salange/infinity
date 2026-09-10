#include "common.wgsl"
@group(0) @binding(0) var<uniform> frame: Frame;
@group(0) @binding(1) var<storage, read> materials: array<Material>;
@group(0) @binding(3) var<storage, read> instances: array<Instance>;
@group(1) @binding(0) var leaf_tex: texture_2d<f32>;
@group(1) @binding(1) var leaf_samp: sampler;

struct VOut {
  @builtin(position) pos: vec4<f32>,
  @location(0) vnormal: vec3<f32>,
  @location(1) uv: vec2<f32>,
  @location(2) @interpolate(flat) flags: u32,
};
@vertex fn vs_main(in: VertexIn, @builtin(instance_index) instance_id: u32) -> VOut {
  var o: VOut;
  let u = unpack_vertex(in);
  let instance=instances[instance_id];
  o.pos = frame.view_proj * instance.model * vec4<f32>(in.position, 1.0);
  o.vnormal = (frame.view * vec4<f32>(instance_normal(instance,u.normal), 0.0)).xyz;
  o.uv = in.uv;
  o.flags = u.material;
  return o;
}
@fragment fn fs_main(in: VOut) -> @location(0) vec4<f32> {
  let glass=(u32(materials[in.flags].misc.x+.5)&FLAG_TRANSMISSION)!=0u;
  if(glass!=(frame.transport.w>1.5)){discard;}
  if(glass&&materials[in.flags].room.z<=0.0){discard;}
  return vec4<f32>(normalize(in.vnormal), 1.0);
}
@fragment fn fs_foliage(in: VOut) -> @location(0) vec4<f32> {
  let a = textureSample(leaf_tex, leaf_samp, in.uv).a;
  if (a < 0.45) { discard; }
  if((u32(materials[in.flags].misc.x+.5)&FLAG_TRANSMISSION)!=0u){discard;}
  return vec4<f32>(normalize(in.vnormal), 1.0);
}
