#include "common.wgsl"
@group(0) @binding(0) var<uniform> frame: Frame;
@group(0) @binding(1) var<storage, read> materials: array<Material>;
@group(0) @binding(2) var<storage, read> lights: array<Light>;
@group(1) @binding(0) var albedo_arr: texture_2d_array<f32>;
@group(1) @binding(1) var normal_arr: texture_2d_array<f32>;
@group(1) @binding(2) var arm_arr: texture_2d_array<f32>;
@group(1) @binding(3) var mat_samp: sampler;
@group(1) @binding(4) var spec_cube: texture_cube<f32>;
@group(1) @binding(5) var sky_cube: texture_cube<f32>;
@group(1) @binding(6) var cube_samp: sampler;
@group(1) @binding(7) var shadow_tex: texture_depth_2d_array;
@group(1) @binding(8) var shadow_samp: sampler_comparison;
@group(1) @binding(9) var ao_tex: texture_2d<f32>;
@group(1) @binding(10) var clamp_samp: sampler;
@group(1) @binding(11) var leaf_tex: texture_2d<f32>;

struct VOut {
  @builtin(position) pos: vec4<f32>,
  @location(0) world: vec3<f32>,
  @location(1) normal: vec3<f32>,
  @location(2) tangent: vec4<f32>,
  @location(3) uv: vec2<f32>,
  @location(4) @interpolate(flat) material: u32,
  @location(5) aux: vec4<f32>,
  @location(6) view_z: f32,
};

@vertex fn vs_main(in: VertexIn) -> VOut {
  var o: VOut;
  let wp = vec4<f32>(in.position, 1.0);
  o.pos = frame.view_proj * wp;
  o.world = in.position;
  o.normal = in.normal;
  o.tangent = in.tangent;
  o.uv = in.uv;
  o.material = in.material;
  o.aux = in.aux;
  o.view_z = -(frame.view * wp).z;
  return o;
}

fn sh_irradiance(n: vec3<f32>) -> vec3<f32> {
  let c = frame.sh;
  let r = c[0].rgb * 0.282095 + c[1].rgb * (0.488603 * n.y) + c[2].rgb * (0.488603 * n.z) +
          c[3].rgb * (0.488603 * n.x) + c[4].rgb * (1.092548 * n.x * n.y) + c[5].rgb * (1.092548 * n.y * n.z) +
          c[6].rgb * (0.315392 * (3.0 * n.z * n.z - 1.0)) + c[7].rgb * (1.092548 * n.x * n.z) +
          c[8].rgb * (0.546274 * (n.x * n.x - n.y * n.y));
  return max(r, vec3<f32>(0.0));
}
fn d_ggx(ndh: f32, a: f32) -> f32 {
  let a2 = a * a;
  let d = ndh * ndh * (a2 - 1.0) + 1.0;
  return a2 / (PI * d * d + 1e-7);
}
fn v_smith(ndv: f32, ndl: f32, a: f32) -> f32 {
  let a2 = a * a;
  let gv = ndl * sqrt(ndv * ndv * (1.0 - a2) + a2);
  let gl = ndv * sqrt(ndl * ndl * (1.0 - a2) + a2);
  return 0.5 / max(gv + gl, 1e-5);
}
fn f_schlick(f0: vec3<f32>, vdh: f32) -> vec3<f32> {
  let f = pow(1.0 - vdh, 5.0);
  return f0 + (1.0 - f0) * f;
}
fn env_brdf(f0: vec3<f32>, rough: f32, ndv: f32) -> vec3<f32> {
  let c0 = vec4<f32>(-1.0, -0.0275, -0.572, 0.022);
  let c1 = vec4<f32>(1.0, 0.0425, 1.04, -0.04);
  let r = rough * c0 + c1;
  let a004 = min(r.x * r.x, exp2(-9.28 * ndv)) * r.x + r.y;
  let ab = vec2<f32>(-1.04, 1.04) * a004 + r.zw;
  return f0 * ab.x + ab.y;
}

const POISSON: array<vec2<f32>, 12> = array<vec2<f32>, 12>(
  vec2<f32>(-0.326, -0.406), vec2<f32>(-0.840, -0.074), vec2<f32>(-0.696, 0.457), vec2<f32>(-0.203, 0.621),
  vec2<f32>(0.962, -0.195), vec2<f32>(0.473, -0.480), vec2<f32>(0.519, 0.767), vec2<f32>(0.185, -0.893),
  vec2<f32>(0.507, 0.064), vec2<f32>(0.896, 0.412), vec2<f32>(-0.322, -0.933), vec2<f32>(-0.792, -0.598));

fn cascade_index(view_z: f32) -> i32 {
  if (view_z < frame.cascade.x) { return 0; }
  if (view_z < frame.cascade.y) { return 1; }
  if (view_z < frame.cascade.z) { return 2; }
  return -1;
}
fn shadow_factor(world: vec3<f32>, n: vec3<f32>, view_z: f32, ndl: f32, pixel: vec2<f32>) -> f32 {
  if (frame.sun_dir.w < 0.5 || frame.params2.w < -0.5) { return 1.0; }
  let idx = cascade_index(view_z);
  if (idx < 0) { return 1.0; }
  var m = frame.shadow0;
  var extent = frame.cascade_extent.x;
  if (idx == 1) { m = frame.shadow1; extent = frame.cascade_extent.y; }
  if (idx == 2) { m = frame.shadow2; extent = frame.cascade_extent.z; }
  let texel_world = 2.0 * extent * frame.cascade.w;
  let offset = n * texel_world * (1.6 - 1.0 * ndl) + frame.sun_dir.xyz * texel_world * 0.5;
  let lp = m * vec4<f32>(world + offset, 1.0);
  let uv = vec2<f32>(lp.x * 0.5 + 0.5, 0.5 - lp.y * 0.5);
  if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) { return 1.0; }
  // Rotated Poisson PCF, ~1.5 texel radius.
  let noise = fract(52.9829189 * fract(dot(pixel, vec2<f32>(0.06711056, 0.00583715)))) * 6.2831853;
  let cs = cos(noise); let sn = sin(noise);
  let radius = frame.cascade.w * 1.6;
  var lit = 0.0;
  for (var i = 0u; i < 12u; i = i + 1u) {
    let p = POISSON[i];
    let r = vec2<f32>(p.x * cs - p.y * sn, p.x * sn + p.y * cs) * radius;
    lit += textureSampleCompare(shadow_tex, shadow_samp, uv + r, idx, lp.z - 0.0004);
  }
  return lit / 12.0;
}

// Interior mapping: a room box behind the facade plane, ray-marched
// analytically in tangent space (x along the facade, y up, z out).
fn interior_color(aux: vec2<f32>, view_ts: vec3<f32>, room: vec4<f32>, seed: f32, night: f32) -> vec3<f32> {
  let rw = room.x; let rh = room.y; let rd = room.z;
  let cell = floor(vec2<f32>(aux.x / rw, aux.y / rh));
  let h = hash33(vec3<f32>(cell.x + 3.1, cell.y + 7.7, seed * 91.7));
  // Lit rooms cluster by floor (offices light whole floors), with a
  // per-room brightness so the pattern is not a binary checkerboard.
  let floor_h = hash13(vec3<f32>(17.0, cell.y + 3.3, seed * 51.3));
  let floor_on = floor_h < room.w * select(0.28, 0.62, night > 0.5);
  let lit = select(select(0.0, 1.0, h.x < 0.10), select(0.0, 1.0, h.x < 0.85), floor_on) * (0.45 + 0.55 * h.z);
  let blinds = select(0.0, 1.0, h.z < 0.18);
  let p = vec2<f32>(aux.x - cell.x * rw, aux.y - cell.y * rh);
  var d = -view_ts;
  d.z = min(d.z, -0.02);
  let tx = select((0.0 - p.x) / d.x, (rw - p.x) / d.x, d.x > 0.0);
  let ty = select((0.0 - p.y) / d.y, (rh - p.y) / d.y, d.y > 0.0);
  let tz = -rd / d.z;
  let t = min(min(tx, ty), tz);
  let hit = vec3<f32>(p, 0.0) + d * t;
  let warm = vec3<f32>(1.0, 0.86, 0.68);
  let cool = vec3<f32>(0.78, 0.88, 1.0);
  let light_col = mix(warm, cool, step(0.5, h.y));
  let wall_tint = mix(vec3<f32>(0.78, 0.75, 0.70), vec3<f32>(0.66, 0.70, 0.76), h.y);
  var col = vec3<f32>(0.0);
  if (t == tz) {
    col = wall_tint * 0.55;
    let sx = hit.x / rw; let sy = hit.y / rh;
    // a screen / picture on the back wall, and a desk band
    let screen = step(0.36, sx) * step(sx, 0.64) * step(0.42, sy) * step(sy, 0.62);
    col += screen * vec3<f32>(0.35, 0.55, 0.95) * 0.35 * lit * night;
    col *= 1.0 - 0.35 * step(sy, 0.28);
  } else if (t == ty) {
    if (d.y > 0.0) {
      col = vec3<f32>(0.92, 0.92, 0.90);
      let gx = fract(hit.x / rw * 2.0); let gz = fract(-hit.z / rd * 2.0);
      let panel = step(0.3, gx) * step(gx, 0.7) * step(0.3, gz) * step(gz, 0.7);
      col += panel * light_col * 1.2 * lit;
    } else {
      col = vec3<f32>(0.30, 0.28, 0.26);
    }
  } else {
    col = wall_tint * 0.72;
  }
  col *= 1.0 / (1.0 + t * 0.12);
  let ambient = mix(0.1, 0.7, lit) * light_col;
  col = col * ambient;
  // blinds: replace with a pale slatted surface just behind the glass
  let slats = 0.6 + 0.4 * step(0.5, fract(p.y * 8.0));
  col = mix(col, vec3<f32>(0.75, 0.74, 0.70) * slats * mix(0.15, 0.6, lit), blinds);
  return col;
}

struct FsOut { @location(0) color: vec4<f32> };

@fragment fn fs_main(in: VOut, @builtin(front_facing) front: bool) -> FsOut {
  let m = materials[in.material];
  let flags = u32(m.misc.x + 0.5);
  let night = frame.params.z;
  let V = normalize(frame.camera_pos.xyz - in.world);
  var N = normalize(in.normal);
  let foliage = (flags & FLAG_FOLIAGE) != 0u;
  if (foliage && !front) { N = -N; }
  var T = normalize(in.tangent.xyz - N * dot(in.tangent.xyz, N));
  let B = cross(N, T) * in.tangent.w;

  // ---- texture coordinates -------------------------------------------------
  let scale = max(m.tex.w, 1e-3);
  var uv = in.uv / scale;
  if ((flags & FLAG_PLANAR_XZ) != 0u) { uv = in.world.xz / scale; }
  if ((flags & FLAG_TRIPLANAR) != 0u) {
    let an = abs(N);
    if (an.y >= an.x && an.y >= an.z) { uv = in.world.xz / scale; }
    else if (an.x >= an.z) { uv = in.world.zy / scale; }
    else { uv = in.world.xy / scale; }
  }

  // ---- material inputs -------------------------------------------------------
  var albedo = m.base_color.rgb;
  var alpha = 1.0;
  if (foliage) {
    let leaf = textureSample(leaf_tex, mat_samp, in.uv);
    alpha = leaf.a;
    if (alpha < 0.3) { discard; }
    albedo = leaf.rgb * m.base_color.rgb;
  } else if (m.tex.x >= 0.0) {
    albedo *= textureSample(albedo_arr, mat_samp, uv, i32(m.tex.x)).rgb;
  }
  if ((flags & (FLAG_PLANAR_XZ | FLAG_TRIPLANAR)) != 0u && !foliage) {
    // low-frequency tonal variation so large surfaces do not read as one tile
    let q = in.world.xz * 0.045;
    let n1 = hash13(vec3<f32>(floor(q), 1.0));
    let n2 = hash13(vec3<f32>(floor(q * 0.31 + 7.0), 2.0));
    let f1 = fract(q); let w1 = f1 * f1 * (3.0 - 2.0 * f1);
    let a = hash13(vec3<f32>(floor(q), 1.0)); let b = hash13(vec3<f32>(floor(q) + vec2<f32>(1.0, 0.0), 1.0));
    let c = hash13(vec3<f32>(floor(q) + vec2<f32>(0.0, 1.0), 1.0)); let d = hash13(vec3<f32>(floor(q) + vec2<f32>(1.0, 1.0), 1.0));
    let smooth_n = mix(mix(a, b, w1.x), mix(c, d, w1.x), w1.y);
    albedo *= 0.86 + 0.28 * smooth_n + 0.06 * (n2 - 0.5) + 0.0 * n1;
  }
  var roughness = m.params.x;
  var ao_tex_v = 1.0;
  if (m.tex.z >= 0.0) {
    let arm = textureSample(arm_arr, mat_samp, uv, i32(m.tex.z));
    ao_tex_v = arm.r;
    roughness = clamp(mix(m.params.x, arm.g * (m.params.x / 0.6), 0.55), select(0.12, 0.22, m.params.y > 0.5), 1.0);
  }
  if (m.tex.y >= 0.0) {
    let nt = textureSample(normal_arr, mat_samp, uv, i32(m.tex.y)).xyz * 2.0 - 1.0;
    let ns = m.params.w;
    N = normalize(T * (nt.x * ns) + B * (nt.y * ns) + N * nt.z);
  }
  let metallic = m.params.y;
  let ndv = max(dot(N, V), 1e-4);
  // Geometric specular anti-aliasing: widen the lobe by the normal's
  // screen-space variance (thin tubes, dense fins).
  {
    let gn = normalize(in.normal);
    let dx = dpdx(gn); let dy = dpdy(gn);
    let variance = 0.25 * (dot(dx, dx) + dot(dy, dy));
    let kr = min(2.0 * variance, 0.2);
    roughness = clamp(sqrt(roughness * roughness + kr), 0.03, 1.0);
  }

  // ---- occlusion --------------------------------------------------------------
  var ssao = 1.0;
  if (frame.params2.z > 0.5) {
    ssao = textureSample(ao_tex, clamp_samp, in.pos.xy * frame.screen.zw).r;
    ssao = pow(ssao, frame.params.y);
  }
  let ao = ssao * ao_tex_v * in.aux.w;

  // ---- sun ----------------------------------------------------------------------
  let L = normalize(frame.sun_dir.xyz);
  var ndl = max(dot(N, L), 0.0);
  let shadow = shadow_factor(in.world, normalize(in.normal), in.view_z, ndl, in.pos.xy);
  let sun = frame.sun_color.rgb * frame.params2.y * frame.sun_dir.w;

  let f0 = mix(vec3<f32>(0.04), albedo, metallic);
  let diffuse_color = albedo * (1.0 - metallic);
  var color = vec3<f32>(0.0);
  var dbg_sun = vec3<f32>(0.0);
  var dbg_ibl_d = vec3<f32>(0.0);
  var dbg_ibl_s = vec3<f32>(0.0);
  var dbg_spec = vec3<f32>(0.0);

  if ((flags & FLAG_GLASS) != 0u) {
    // Slight waviness in the pane normal for realism.
    let wav = (hash13(floor(vec3<f32>(in.aux.x / m.room.x, in.aux.y / m.room.y, 0.0))) - 0.5) * 0.012;
    let Ng = normalize(N + T * wav + B * wav * 0.6);
    let ndv_g = max(dot(Ng, V), 1e-4);
    let R = reflect(-V, Ng);
    let g_rough = m.params.x;
    let refl = textureSampleLevel(spec_cube, cube_samp, R, g_rough * 5.0).rgb * frame.params2.x;
    let f0g = vec3<f32>(0.05);
    let brdf = env_brdf(f0g, g_rough, ndv_g);
    // sun highlight
    let H = normalize(V + L);
    let ndh = max(dot(Ng, H), 0.0);
    let a = max(g_rough * g_rough, 0.06);
    let spec_sun = d_ggx(ndh, a) * v_smith(ndv_g, max(dot(Ng, L), 0.0), a) * f_schlick(f0g, max(dot(V, H), 0.0)) * max(dot(Ng, L), 0.0);
    let view_ts = vec3<f32>(dot(V, T), dot(V, B), dot(V, N));
    var inside = interior_color(in.aux.xy, view_ts, m.room, in.aux.z, night);
    // Distance LOD: when a room spans few pixels, fade the parallax detail
    // toward the room's mean so the facade does not alias into moire.
    let fw = fwidth(in.aux.xy);
    let px_per_room = m.room.x / max(fw.x, 1e-4);
    let detail = clamp((px_per_room - 6.0) / 30.0, 0.0, 1.0);
    let cell = floor(in.aux.xy / m.room.xy);
    let hc = hash33(vec3<f32>(cell.x + 3.1, cell.y + 7.7, in.aux.z * 91.7));
    let floor_far = hash13(vec3<f32>(17.0, cell.y + 3.3, in.aux.z * 51.3)) < m.room.w * select(0.28, 0.62, night > 0.5);
    let lit_far = select(select(0.0, 1.0, hc.x < 0.10), select(0.0, 1.0, hc.x < 0.85), floor_far) * (0.45 + 0.55 * hc.z);
    let light_far = mix(vec3<f32>(1.0, 0.86, 0.68), vec3<f32>(0.78, 0.88, 1.0), step(0.5, hc.y));
    let frac_y = fract(in.aux.y / m.room.y);
    let grad = 0.55 + 0.75 * frac_y * frac_y;  // ceilings brighter than floors
    let mean_room = vec3<f32>(0.42, 0.40, 0.37) * light_far * mix(0.12, 0.7, lit_far) * grad;
    inside = mix(mean_room, inside, detail);
    // By day a room is dark next to the sky; at night it is the light source.
    inside = inside * frame.params.x * mix(0.045, 0.6, night);
    // Mullion grid at the room boundaries (thin dark frame lines).
    let gx = abs(fract(in.aux.x / m.room.x + 0.5) - 0.5) * m.room.x;
    let gy = abs(fract(in.aux.y / m.room.y + 0.5) - 0.5) * m.room.y;
    let line_w = max(0.04, fw.x * 0.9);
    let frame_line = 1.0 - min(1.0, min(gx, gy) / line_w);
    let tint = m.misc.yzw;
    color = refl * brdf + sun * spec_sun * shadow + inside * (1.0 - brdf) * tint;
    color = mix(color, vec3<f32>(0.02, 0.02, 0.022) * (sh_irradiance(N) / PI + sun * ndl * shadow / PI), frame_line * 0.85);
    // dust/dirt on glass: a faint diffuse term
    color += diffuse_color * 0.02 * (sh_irradiance(N) + sun * ndl * shadow) / PI;
  } else {
    let H = normalize(V + L);
    let ndh = max(dot(N, H), 0.0);
    let vdh = max(dot(V, H), 0.0);
    let a = max(roughness * roughness, 0.002);
    let a_sun = max(a, 0.06);  // the sun is a disc: widen the lobe, no point-light glints
    let F = f_schlick(f0, vdh);
    var kd = (1.0 - F) * (1.0 - metallic);
    var diffuse = diffuse_color / PI;
    var spec_scale = 1.0;
    if (foliage) {
      // wrap lighting + translucency for leaves; the specular lobe uses the
      // same (wrapped) cosine as the visibility term or grazing leaves explode
      ndl = clamp((dot(N, L) + 0.4) / 1.4, 0.0, 1.0);
      let back = max(dot(-N, L), 0.0) * 0.18;
      diffuse = diffuse * (1.0 + back);
      spec_scale = 0.35;
    }
    let spec = d_ggx(ndh, a_sun) * min(v_smith(ndv, ndl, a_sun), 8.0) * F * spec_scale;
    dbg_sun = kd * diffuse * sun * ndl * shadow;
    dbg_spec = spec * sun * ndl * shadow;
    color += (kd * diffuse + spec) * sun * ndl * shadow;
    // IBL
    let irr = sh_irradiance(N) * frame.params2.x;
    dbg_ibl_d = diffuse_color * irr / PI * ao;
    color += dbg_ibl_d;
    let R = reflect(-V, N);
    let pref = textureSampleLevel(spec_cube, cube_samp, R, roughness * 5.0).rgb * frame.params2.x;
    let so = clamp(pow(ndv + ao, exp2(-16.0 * roughness - 1.0)) - 1.0 + ao, 0.0, 1.0);  // Lagarde spec occlusion
    dbg_ibl_s = pref * env_brdf(f0, roughness, ndv) * so;
    color += dbg_ibl_s;
    // point lights (night)
    let count = i32(frame.params.w + 0.5);
    for (var i = 0; i < count; i = i + 1) {
      let lt = lights[i];
      let dvec = lt.pos_radius.xyz - in.world;
      let d2 = dot(dvec, dvec);
      let r2 = lt.pos_radius.w * lt.pos_radius.w;
      let win = clamp(1.0 - (d2 * d2) / (r2 * r2), 0.0, 1.0);
      let att = win * win / (d2 + 1.0);
      let Ld = normalize(dvec);
      let ndl2 = max(dot(N, Ld), 0.0);
      let Hd = normalize(V + Ld);
      let spec2 = d_ggx(max(dot(N, Hd), 0.0), a) * v_smith(ndv, ndl2, a) * f_schlick(f0, max(dot(V, Hd), 0.0));
      color += (kd * diffuse + spec2) * lt.color_int.rgb * lt.color_int.w * att * ndl2;
    }
  }
  // emissive
  if ((flags & FLAG_EMISSIVE) != 0u) {
    var e = m.params.z * frame.params.x;
    if ((flags & FLAG_NIGHT_ONLY) != 0u) { e *= night; }
    color += m.misc.yzw * e;
  }

  // Clamp the HDR range so glints cannot flood the bloom chain.
  color = min(color, vec3<f32>(24.0 / frame.sun_color.w));

  // ---- debug views --------------------------------------------------------------
  let dbg = i32(frame.params2.w + 0.5);
  if (dbg == 1) { color = albedo / frame.sun_color.w; }
  else if (dbg == 2) { color = (N * 0.5 + 0.5) / frame.sun_color.w; }
  else if (dbg == 3) { color = vec3<f32>(ao) / frame.sun_color.w; }
  else if (dbg == 4) {
    let ci = cascade_index(in.view_z);
    var cc = vec3<f32>(0.2);
    if (ci == 0) { cc = vec3<f32>(1.0, 0.3, 0.3); } else if (ci == 1) { cc = vec3<f32>(0.3, 1.0, 0.3); } else if (ci == 2) { cc = vec3<f32>(0.3, 0.3, 1.0); }
    color = cc * (0.4 + 0.6 * shadow) / frame.sun_color.w;
  } else if (dbg == 5) { color = vec3<f32>(roughness) / frame.sun_color.w; }
  else if (dbg == 6) { color = dbg_sun; }
  else if (dbg == 7) { color = dbg_ibl_d; }
  else if (dbg == 8) { color = dbg_ibl_s; }
  else if (dbg == 9) { color = dbg_spec; }
  var o: FsOut;
  o.color = vec4<f32>(color, alpha);
  return o;
}
