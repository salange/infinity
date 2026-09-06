#include "common.wgsl"
#include "fullscreen.wgsl"
@group(0) @binding(0) var<uniform> frame: Frame;
@group(1) @binding(0) var sky_cube: texture_cube<f32>;
@group(1) @binding(1) var samp: sampler;

@fragment fn fs_sky(in: FSOut) -> @location(0) vec4<f32> {
  let ndc = vec4<f32>(in.uv.x * 2.0 - 1.0, 1.0 - in.uv.y * 2.0, 1.0, 1.0);
  let far = frame.inv_view_proj * ndc;
  let dir = normalize(far.xyz / far.w - frame.camera_pos.xyz);
  var c = textureSampleLevel(sky_cube, samp, dir, 0.0).rgb;
  // Below the horizon the map shows the capture site's ground: fade to a
  // neutral haze so the block sits on its own ground.
  let below = smoothstep(0.0, -0.08, dir.y);
  let haze = textureSampleLevel(sky_cube, samp, vec3<f32>(dir.x, 0.02, dir.z), 3.0).rgb;
  c = mix(c, haze, below);
  return vec4<f32>(c * frame.params2.x, 1.0);
}
