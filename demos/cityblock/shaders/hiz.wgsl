#include "fullscreen.wgsl"
// Depth pyramid (Hi-Z): mip 0 copies the 1x depth buffer, every further mip
// holds the farthest depth of its 2x2 children (the MINIMUM under reversed
// Z), so a conservative "is anything in this footprint farther than X" test
// is one lookup.
@group(0) @binding(0) var src_depth: texture_depth_2d;
@group(0) @binding(1) var src_level: texture_2d<f32>;

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
