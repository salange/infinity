#include "fullscreen.wgsl"
struct PostParams { a: vec4<f32>, b: vec4<f32> };
@group(0) @binding(0) var<uniform> pp: PostParams;
@group(0) @binding(1) var src: texture_2d<f32>;
@group(0) @binding(2) var src2: texture_2d<f32>;
@group(0) @binding(3) var samp: sampler;

// ---- bloom downsample (13 taps, Jimenez) --------------------------------
@fragment fn fs_down(in: FSOut) -> @location(0) vec4<f32> {
  let t = pp.a.xy;  // source texel size
  let uv = in.uv;
  let a = textureSample(src, samp, uv + t * vec2<f32>(-2.0, -2.0)).rgb;
  let b = textureSample(src, samp, uv + t * vec2<f32>( 0.0, -2.0)).rgb;
  let c = textureSample(src, samp, uv + t * vec2<f32>( 2.0, -2.0)).rgb;
  let d = textureSample(src, samp, uv + t * vec2<f32>(-2.0,  0.0)).rgb;
  let e = textureSample(src, samp, uv).rgb;
  let f = textureSample(src, samp, uv + t * vec2<f32>( 2.0,  0.0)).rgb;
  let g = textureSample(src, samp, uv + t * vec2<f32>(-2.0,  2.0)).rgb;
  let h = textureSample(src, samp, uv + t * vec2<f32>( 0.0,  2.0)).rgb;
  let i = textureSample(src, samp, uv + t * vec2<f32>( 2.0,  2.0)).rgb;
  let j = textureSample(src, samp, uv + t * vec2<f32>(-1.0, -1.0)).rgb;
  let k = textureSample(src, samp, uv + t * vec2<f32>( 1.0, -1.0)).rgb;
  let l = textureSample(src, samp, uv + t * vec2<f32>(-1.0,  1.0)).rgb;
  let m = textureSample(src, samp, uv + t * vec2<f32>( 1.0,  1.0)).rgb;
  var col = e * 0.125;
  col += (a + c + g + i) * 0.03125;
  col += (b + d + f + h) * 0.0625;
  col += (j + k + l + m) * 0.125;
  if (pp.a.z > 0.5) {
    // first level: threshold with a soft knee, and clamp fireflies
    let lum = max(max(col.r, col.g), col.b);
    let knee = pp.b.x * pp.b.y;
    let soft = clamp(lum - pp.b.x + knee, 0.0, 2.0 * knee);
    let contribution = max(soft * soft / (4.0 * knee + 1e-4), lum - pp.b.x) / max(lum, 1e-4);
    col = col * clamp(contribution, 0.0, 1.0);
    col = min(col, vec3<f32>(pp.b.z));
  }
  return vec4<f32>(col, 1.0);
}
// ---- bloom upsample (tent) + add the same-level source --------------------
@fragment fn fs_up(in: FSOut) -> @location(0) vec4<f32> {
  let t = pp.a.xy * 1.5;
  let uv = in.uv;
  var s = textureSample(src, samp, uv + t * vec2<f32>(-1.0, -1.0)).rgb;
  s += textureSample(src, samp, uv + t * vec2<f32>( 0.0, -1.0)).rgb * 2.0;
  s += textureSample(src, samp, uv + t * vec2<f32>( 1.0, -1.0)).rgb;
  s += textureSample(src, samp, uv + t * vec2<f32>(-1.0,  0.0)).rgb * 2.0;
  s += textureSample(src, samp, uv).rgb * 4.0;
  s += textureSample(src, samp, uv + t * vec2<f32>( 1.0,  0.0)).rgb * 2.0;
  s += textureSample(src, samp, uv + t * vec2<f32>(-1.0,  1.0)).rgb;
  s += textureSample(src, samp, uv + t * vec2<f32>( 0.0,  1.0)).rgb * 2.0;
  s += textureSample(src, samp, uv + t * vec2<f32>( 1.0,  1.0)).rgb;
  s = s / 16.0;
  let same = textureSample(src2, samp, uv).rgb;
  return vec4<f32>(same + s * pp.a.w, 1.0);
}
// ---- tonemap: exposure, bloom, ACES, sRGB encode -------------------------
fn aces(x: vec3<f32>) -> vec3<f32> {
  let a = 2.51; let b = 0.03; let c = 2.43; let d = 0.59; let e = 0.14;
  return clamp((x * (a * x + b)) / (x * (c * x + d) + e), vec3<f32>(0.0), vec3<f32>(1.0));
}
fn srgb_encode(c: vec3<f32>) -> vec3<f32> {
  let lo = c * 12.92;
  let hi = 1.055 * pow(c, vec3<f32>(1.0 / 2.4)) - 0.055;
  return select(hi, lo, c <= vec3<f32>(0.0031308));
}
@fragment fn fs_tonemap(in: FSOut) -> @location(0) vec4<f32> {
  let hdr = textureSample(src, samp, in.uv).rgb;
  let bloom = textureSample(src2, samp, in.uv).rgb;
  var c = (hdr + bloom * pp.a.y) * pp.a.x;
  // gentle vignette
  let d = in.uv - vec2<f32>(0.5);
  c = c * (1.0 - 0.18 * dot(d, d) * 2.0);
  var m = aces(c);
  return vec4<f32>(srgb_encode(m), 1.0);
}
// ---- FXAA (3.11 quality preset, compact) -----------------------------------
fn luma(c: vec3<f32>) -> f32 { return dot(c, vec3<f32>(0.299, 0.587, 0.114)); }
@fragment fn fs_fxaa(in: FSOut) -> @location(0) vec4<f32> {
  let t = pp.a.xy;
  let uv = in.uv;
  let rgbM = textureSample(src, samp, uv).rgb;
  let lM = luma(rgbM);
  let lN = luma(textureSample(src, samp, uv + vec2<f32>(0.0, -t.y)).rgb);
  let lS = luma(textureSample(src, samp, uv + vec2<f32>(0.0,  t.y)).rgb);
  let lW = luma(textureSample(src, samp, uv + vec2<f32>(-t.x, 0.0)).rgb);
  let lE = luma(textureSample(src, samp, uv + vec2<f32>( t.x, 0.0)).rgb);
  let lMin = min(lM, min(min(lN, lS), min(lW, lE)));
  let lMax = max(lM, max(max(lN, lS), max(lW, lE)));
  let range = lMax - lMin;
  if (range < max(0.0312, lMax * 0.125)) { return vec4<f32>(rgbM, 1.0); }
  let lNW = luma(textureSample(src, samp, uv + vec2<f32>(-t.x, -t.y)).rgb);
  let lNE = luma(textureSample(src, samp, uv + vec2<f32>( t.x, -t.y)).rgb);
  let lSW = luma(textureSample(src, samp, uv + vec2<f32>(-t.x,  t.y)).rgb);
  let lSE = luma(textureSample(src, samp, uv + vec2<f32>( t.x,  t.y)).rgb);
  let edgeH = abs(-2.0 * lW + lNW + lSW) + abs(-2.0 * lM + lN + lS) * 2.0 + abs(-2.0 * lE + lNE + lSE);
  let edgeV = abs(-2.0 * lN + lNW + lNE) + abs(-2.0 * lM + lW + lE) * 2.0 + abs(-2.0 * lS + lSW + lSE);
  let horizontal = edgeH >= edgeV;
  let l1 = select(lW, lN, horizontal);
  let l2 = select(lE, lS, horizontal);
  let g1 = l1 - lM;
  let g2 = l2 - lM;
  let steep1 = abs(g1) >= abs(g2);
  let grad = 0.25 * max(abs(g1), abs(g2));
  var step = select(t.x, t.y, horizontal);
  var lAvg = 0.0;
  if (steep1) { step = -step; lAvg = 0.5 * (l1 + lM); } else { lAvg = 0.5 * (l2 + lM); }
  var cur = uv;
  if (horizontal) { cur.y += step * 0.5; } else { cur.x += step * 0.5; }
  let off = select(vec2<f32>(0.0, t.y), vec2<f32>(t.x, 0.0), horizontal);
  var uv1 = cur - off;
  var uv2 = cur + off;
  var e1 = luma(textureSample(src, samp, uv1).rgb) - lAvg;
  var e2 = luma(textureSample(src, samp, uv2).rgb) - lAvg;
  var r1 = abs(e1) >= grad;
  var r2 = abs(e2) >= grad;
  if (!r1) { uv1 -= off; }
  if (!r2) { uv2 += off; }
  for (var i = 0; i < 8 && !(r1 && r2); i = i + 1) {
    if (!r1) { e1 = luma(textureSample(src, samp, uv1).rgb) - lAvg; }
    if (!r2) { e2 = luma(textureSample(src, samp, uv2).rgb) - lAvg; }
    r1 = abs(e1) >= grad;
    r2 = abs(e2) >= grad;
    let q = 1.5;
    if (!r1) { uv1 -= off * q; }
    if (!r2) { uv2 += off * q; }
  }
  let d1 = select(uv.x - uv1.x, uv.y - uv1.y, horizontal);
  let d2 = select(uv2.x - uv.x, uv2.y - uv.y, horizontal);
  let dir1 = d1 < d2;
  let dist = min(d1, d2);
  let edgeLen = d1 + d2;
  let pixelOffset = -dist / edgeLen + 0.5;
  let isLumaCenterSmaller = lM < lAvg;
  let correct = (select(e2, e1, dir1) < 0.0) != isLumaCenterSmaller;
  var finalOffset = select(0.0, pixelOffset, correct);
  // subpixel
  let lumaAvgAll = (1.0 / 12.0) * (2.0 * (lN + lS + lW + lE) + lNW + lNE + lSW + lSE);
  let sub = clamp(abs(lumaAvgAll - lM) / range, 0.0, 1.0);
  let sub2 = (-2.0 * sub + 3.0) * sub * sub;
  finalOffset = max(finalOffset, sub2 * sub2 * 0.75);
  var fuv = uv;
  if (horizontal) { fuv.y += finalOffset * step; } else { fuv.x += finalOffset * step; }
  return vec4<f32>(textureSample(src, samp, fuv).rgb, 1.0);
}
// ---- blit to the surface -----------------------------------------------------
fn srgb_decode(c: vec3<f32>) -> vec3<f32> {
  let lo = c / 12.92;
  let hi = pow((c + 0.055) / 1.055, vec3<f32>(2.4));
  return select(hi, lo, c <= vec3<f32>(0.04045));
}
@fragment fn fs_blit(in: FSOut) -> @location(0) vec4<f32> {
  let c = textureSample(src, samp, in.uv).rgb;
  if (pp.a.x > 0.5) { return vec4<f32>(srgb_decode(c), 1.0); }
  return vec4<f32>(c, 1.0);
}
