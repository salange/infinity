// GPU occlusion culling: every candidate draw range (bounding sphere + index
// range) is tested against the depth pyramid of the current frame's prepass
// and its indirect draw arguments are written with instance_count 0 or 1.
struct Range { centre: vec3<f32>, radius: f32, first: u32, count: u32, pad0: u32, pad1: u32 };
struct CullParams {
  view: mat4x4<f32>,
  proj: mat4x4<f32>,
  params: vec4<f32>,   // count, mip count, screen w, screen h
  params2: vec4<f32>,  // near bypass distance, enabled (0/1), 0, 0
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
  if (cp.params2.y > 0.5 && r.radius > 0.0) {
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
      var farthest = 0.0;
      for (var y = lo.y; y <= min(hi.y, lo.y + 1); y = y + 1) {
        for (var x = lo.x; x <= min(hi.x, lo.x + 1); x = x + 1) {
          let c = clamp(vec2<i32>(x, y), vec2<i32>(0), dims - 1);
          farthest = max(farthest, textureLoad(hiz, c, level).r);
        }
      }
      // the sphere's nearest point in depth-buffer units; hidden if even that
      // lies behind everything drawn in its footprint
      let near_depth = depth_ndc_of_view_z(zn);
      if (near_depth > farthest + 1e-5) { visible = false; }
    }
  }
  let o = i * 5u;
  args[o] = r.count;
  args[o + 1u] = select(0u, 1u, visible);
  args[o + 2u] = r.first;
  args[o + 3u] = 0u;
  args[o + 4u] = 0u;
}
