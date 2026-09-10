#include "common.wgsl"
@group(0) @binding(0) var<uniform> frame: Frame;
@group(0) @binding(3) var<storage, read> instances: array<Instance>;
@group(0) @binding(1) var<storage, read> materials: array<Material>;
@group(0) @binding(2) var<storage, read> lights: array<Light>;
@group(0) @binding(4) var<storage, read> light_tiles: array<u32>;
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
@group(1) @binding(12) var radiance_volume: texture_3d<f32>;
@group(1) @binding(13) var planar_reflection: texture_2d<f32>;
@group(1) @binding(14) var transmitted_scene: texture_2d<f32>;
@group(1) @binding(15) var fine_radiance: texture_3d<f32>;
@group(1) @binding(16) var puddle_reflection: texture_2d<f32>;
@group(1) @binding(17) var pond_reflection: texture_2d<f32>;
@group(1) @binding(18) var point_geometry: texture_3d<u32>;
@group(1) @binding(19) var fine_point_geometry: texture_3d<u32>;
#include "point_visibility.wgsl"

struct VOut {
  @builtin(position) pos: vec4<f32>,
  @location(0) world: vec3<f32>,
  @location(1) normal: vec3<f32>,
  @location(2) tangent: vec4<f32>,
  @location(3) uv: vec2<f32>,
  @location(4) @interpolate(flat) material: u32,
  @location(5) aux: vec4<f32>,
  @location(6) view_z: f32,
  @location(7) tint: vec3<f32>,
};

@vertex fn vs_main(in: VertexIn, @builtin(instance_index) instance_id: u32) -> VOut {
  var o: VOut;
  let u = unpack_vertex(in);
  let instance=instances[instance_id];
  let wp = instance.model * vec4<f32>(in.position, 1.0);
  o.pos = frame.view_proj * wp;
  o.world = wp.xyz;
  o.tint = instance.tint.rgb;
  o.normal = instance_normal(instance,u.normal);
  o.tangent = vec4<f32>(instance_normal(instance,u.tangent.xyz),u.tangent.w);
  o.uv = in.uv;
  o.material = u.material;
  o.aux = u.aux;
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
fn secondary_view() -> bool {
  // Passes 0/2 are primary opaque/glass; 1/3 are mirrored opaque/glass.
  return (u32(frame.transport.w+.5)&1u)!=0u;
}
// Each ray samples the camera-independent scene volume. Occluded sky is
// replaced by the surface's reflected radiance, including colour bleeding.
// The volume is deliberately coarse; geometric PCF/SSAO supplies local contact.
fn transport_cell(p: vec3<f32>) -> f32 {
  let uv=(p-frame.fine_min.xyz)/frame.fine_extent.xyz;
  let margin=min(min(uv.x,uv.y),min(uv.z,min(1.0-uv.x,min(1.0-uv.y,1.0-uv.z))));
  return mix(frame.voxel_min.w,frame.fine_min.w,smoothstep(.04,.12,margin));
}
fn transport_ray(origin: vec3<f32>, direction: vec3<f32>, sky: vec3<f32>, spread: f32) -> vec3<f32> {
  var col=vec3<f32>(0.0);var visibility=1.0;
  var distance=transport_cell(origin)*1.6;
  for(var step=0;step<32;step=step+1) {
    let p=origin+direction*distance;
    let uv=(p-frame.voxel_min.xyz)/frame.voxel_extent.xyz;
    if(any(uv<vec3<f32>(0.0))||any(uv>vec3<f32>(1.0))) {break;}
    let fine_uv=(p-frame.fine_min.xyz)/frame.fine_extent.xyz;
    let margin=min(min(fine_uv.x,fine_uv.y),min(fine_uv.z,min(1.0-fine_uv.x,min(1.0-fine_uv.y,1.0-fine_uv.z))));
    let fine_weight=smoothstep(.025,.065,margin);
    let coarse_lod=log2(max(2.0*spread*distance/frame.voxel_min.w,1.0));
    let fine_lod=log2(max(2.0*spread*distance/frame.fine_min.w,1.0));
    var cell=textureSampleLevel(radiance_volume,cube_samp,uv,coarse_lod);
    if(fine_weight>0.0){cell=mix(cell,textureSampleLevel(fine_radiance,cube_samp,fine_uv,fine_lod),fine_weight);}
    let coverage=clamp(cell.a*1.8,0.0,1.0);
    col+=visibility*(cell.rgb/max(cell.a,.0001))*coverage;
    visibility*=1.0-coverage;
    if(visibility<0.04){break;}
    distance+=max(transport_cell(p)*(1.0+f32(step)*0.12),distance*spread*.65);
  }
  return col+sky*visibility;
}
fn scene_irradiance(world: vec3<f32>,normal: vec3<f32>) -> vec3<f32> {
  let sky=sh_irradiance(normal);
  if(frame.transport.x<0.5||secondary_view()){return sky;}
  let origin=world+normal*transport_cell(world)*1.25;
  let axis=select(vec3<f32>(0.0,1.0,0.0),vec3<f32>(1.0,0.0,0.0),abs(normal.y)>0.9);
  let t=normalize(cross(axis,normal));let b=cross(normal,t);
  var irradiance=vec3<f32>(0.0);
  for(var ray=0;ray<12;ray=ray+1) {
    let angle=f32(ray)*2.39996323;
    let radial=sqrt((f32(ray)+.5)/12.0);
    let d=normal*sqrt(1.0-radial*radial)+(t*cos(angle)+b*sin(angle))*radial;
    irradiance+=transport_ray(origin,d,sky/PI,.25);
  }
  return irradiance*(PI/12.0);
}
fn scene_reflection(world: vec3<f32>,normal: vec3<f32>,direction: vec3<f32>,rough: f32) -> vec3<f32> {
  let sky=textureSampleLevel(spec_cube,cube_samp,direction,rough*5.0).rgb*frame.voxel_extent.w;
  if(frame.transport.x<.5||secondary_view()){return sky;}
  // The GGX half-vector median is atan(alpha) at normal incidence. Reflection
  // doubles that angle; the old 0.35*alpha cone was much too sharp on ceramics.
  let alpha=rough*rough;
  let spread=clamp(2.0*alpha/max(1.0-alpha*alpha,.05),.015,1.4);
  return transport_ray(world+normal*transport_cell(world)*1.4,direction,sky,spread);
}
fn sample_planar(uv: vec2<f32>,lod: f32,plane: u32) -> vec3<f32> {
  if(plane==2u){return textureSampleLevel(pond_reflection,cube_samp,uv,lod).rgb;}
  if(plane==1u){return textureSampleLevel(puddle_reflection,cube_samp,uv,lod).rgb;}
  return textureSampleLevel(planar_reflection,cube_samp,uv,lod).rgb;
}
fn water_reflection(world: vec3<f32>,normal: vec3<f32>,rough: f32,fallback: vec3<f32>,pond: bool) -> vec3<f32> {
  if(frame.transport.y<.5||secondary_view()){return fallback;}
  let plane=select(select(0u,1u,world.y>frame.transport.z+.6),2u,pond);
  var matrix=frame.reflection_vp;
  if(plane==1u){matrix=frame.puddle_reflection_vp;}
  if(plane==2u){matrix=frame.pond_reflection_vp;}
  let v=normalize(frame.camera_pos.xyz-world);
  let axis=select(vec3<f32>(0,1,0),vec3<f32>(1,0,0),abs(normal.y)>.9);
  let tangent=normalize(cross(axis,normal));let bitangent=cross(normal,tangent);
  let alpha=max(rough*rough,.0004);
  let nv=max(dot(normal,v),.0001);
  let stretched=normalize(vec3<f32>(alpha*dot(v,tangent),alpha*dot(v,bitangent),nv));
  let lateral=dot(stretched.xy,stretched.xy);
  var disk_x=vec3<f32>(1,0,0);
  if(lateral>1e-8){disk_x=vec3<f32>(-stretched.y,stretched.x,0)/sqrt(lateral);}
  let disk_y=cross(stretched,disk_x);
  let visible_weight=2.0*nv/(nv+sqrt(alpha*alpha+(1.0-alpha*alpha)*nv*nv));
  let texel_angle=2.0/(frame.screen.y*frame.proj[1][1]);
  let max_lod=floor(log2(max(frame.screen.x,frame.screen.y)));
  var radiance=vec3<f32>(0.0);var weight=0.0;
  // Fixed GGX visible-normal samples are stable across frames and camera motion
  // (Heitz 2018, https://jcgt.org/published/0007/04/01/).
  // Their projected angular footprint, rather than an arbitrary UV radius,
  // determines the image blur. PDF-sized mips suppress sparse star replicas.
  for(var tap=0;tap<32;tap=tap+1) {
    let u=(f32(tap)+.5)/32.0;
    let phi=f32(tap)*2.39996323;
    let disk_a=sqrt(u)*cos(phi);
    let horizon_split=.5*(1.0+stretched.z);
    let disk_b=mix(sqrt(max(1.0-disk_a*disk_a,0.0)),sqrt(u)*sin(phi),horizon_split);
    let cap=disk_x*disk_a+disk_y*disk_b+stretched*sqrt(max(1.0-disk_a*disk_a-disk_b*disk_b,0.0));
    let h=normalize(tangent*(alpha*cap.x)+bitangent*(alpha*cap.y)+normal*max(cap.z,0.0));
    let cos_h=max(dot(normal,h),0.0);
    let vh=dot(v,h);
    if(vh<=0.0){continue;}
    let direction=reflect(-v,h);let nl=max(dot(normal,direction),0.0);
    if(nl<=0.0){continue;}
    let denominator=cos_h*cos_h*(alpha*alpha-1.0)+1.0;
    let distribution=alpha*alpha/(PI*max(denominator*denominator,1e-15));
    let pdf=max(distribution*visible_weight/(4.0*nv),.00001);
    let sample_angle=1.0/(32.0*pdf);
    let lod=clamp(.5*log2(max(sample_angle/(texel_angle*texel_angle),1.0)),0.0,max_lod);
    let clip=matrix*vec4<f32>(direction,0.0);
    var value=fallback;
    if(clip.w>0.0) {
      let uv=vec2<f32>(clip.x/clip.w*.5+.5,.5-clip.y/clip.w*.5);
      let edge=smoothstep(0.0,.02,min(min(uv.x,uv.y),min(1.0-uv.x,1.0-uv.y)));
      if(edge>0.0){value=mix(fallback,sample_planar(uv,lod,plane),edge);}
    }
    let sample_weight=2.0*nl/(nl+sqrt(alpha*alpha+(1.0-alpha*alpha)*nl*nl));
    radiance+=value*sample_weight;weight+=sample_weight;
  }
  return select(fallback,radiance/max(weight,.0001),weight>.0001);
}

fn d_ggx(ndh: f32, a: f32) -> f32 {
  let a2 = a * a;
  let d = ndh * ndh * (a2 - 1.0) + 1.0;
  return a2 / (PI * d * d + 1e-7);
}
fn d_brushed(n: vec3<f32>,h: vec3<f32>,t: vec3<f32>,b: vec3<f32>,rough: f32) -> f32 {
  let ax=max(rough*rough*.62,.018);let ay=max(rough*rough*1.55,.018);
  let q=pow(dot(h,t)/ax,2.0)+pow(dot(h,b)/ay,2.0)+pow(max(dot(h,n),0.0),2.0);
  return 1.0/(PI*ax*ay*q*q+1e-6);
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

// Occupied glazing contract: base RGB is normal-incidence one-pass pane
// transmission, metallic is colored coating coverage, tint2 is the room tint.
// This bounded coating approximation preserves neutral grazing reflection;
// it is not a spectral thin-film simulation or an opaque albedo multiplier.
fn occupied_glass_f0(transmittance: vec3<f32>, coating: f32) -> vec3<f32> {
  let hue=transmittance/max(max(transmittance.x,transmittance.y),max(transmittance.z,.001));
  return mix(vec3<f32>(.04),hue*.32,clamp(coating,0.0,1.0));
}

fn display_to_scene_radiance(display_linear: vec3<f32>) -> vec3<f32> {
  // Invert the final ACES fit solely to express the authored atmosphere in
  // scene radiance. The sky itself remains untouched and display-referred.
  let y=clamp(display_linear,vec3<f32>(0.0),vec3<f32>(.98));
  let a=vec3<f32>(2.51)-2.43*y;
  let b=vec3<f32>(.03)-.59*y;
  return (-b+sqrt(b*b+4.0*a*y*.14))/(2.0*a*frame.sun_color.w);
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

// The resolved and subpixel room paths use the same occupancy distribution.
// A floor is active with probability q; individual rooms have 10%/85% occupancy
// on inactive/active floors, multiplied by mean brightness (.45+.55*.5).
fn room_floor_probability(probability: f32, night: f32) -> f32 {
  return clamp(probability * mix(.58,.78,night),0.0,1.0);
}
fn room_lit_mean(probability: f32, night: f32) -> f32 {
  return .725 * (.10 + .75 * room_floor_probability(probability,night));
}
fn room_dark_ambient(night: f32) -> f32 {return mix(.10,.008,night);}
// Integrate the actual floor state and ceiling gradient jointly when horizontal
// rooms are unresolved but vertical floor bands still span pixels. Only called
// for a footprint narrower than2/3 of a floor, hence at most two intersected rows.
// Returns E[lit], E[gradient], E[lit*gradient]; the last is not their product.
fn room_floor_integrals(y: f32,footprint: f32,height: f32,probability: f32,
  seed: f32,night: f32) -> vec3<f32> {
  let width=max(footprint/height,1e-6);
  let position=y/height;let local=fract(position);var first=floor(position);
  var first_length=width;var second_length=0.0;
  var u=local-width*.5;var v=local+width*.5;
  if(local<width*.5) {
    first-=1.0;first_length=width*.5-local;second_length=width-first_length;
    u=1.0-first_length;v=1.0;
  } else if(1.0-local<width*.5) {
    second_length=width*.5-(1.0-local);first_length=width-second_length;
    u=1.0-first_length;v=1.0;
  }
  var integrated=vec3<f32>(0.0);
  for(var i=0u;i<2u;i=i+1u) {
    let row=first+f32(i);let segment_length=select(first_length,second_length,i==1u);
    if(segment_length<=0.0){continue;}
    let a=select(u,0.0,i==1u);let b=select(v,second_length,i==1u);
    // Factor the cubic difference to avoid cancellation on tiny footprints.
    let gradient=.55+.25*(a*a+a*b+b*b);
    let floor_active=hash13(vec3<f32>(17.0,row+3.3,seed*51.3))<room_floor_probability(probability,night);
    let lit=.725*select(.10,.85,floor_active);
    integrated+=vec3<f32>(lit,gradient,lit*gradient)*(segment_length/width);
  }
  return integrated;
}

// Integral of a periodic hard band centered on every integer period.
fn grid_band_primitive(x: f32, period: f32, half_width: f32) -> f32 {
  let shifted=x+half_width;
  let cycles=floor(shifted/period);
  return cycles*(2.0*half_width)+clamp(shifted-cycles*period,0.0,2.0*half_width);
}
fn grid_band_coverage(x: f32, footprint: f32, period: f32, half_width: f32) -> f32 {
  // Reduce the origin before subtraction to retain precision on large facades.
  let phase=fract(x/period)*period;
  let width=max(footprint,.0001);
  return clamp((grid_band_primitive(phase+width*.5,period,half_width)-
                grid_band_primitive(phase-width*.5,period,half_width))/width,0.0,1.0);
}
fn grid_distance(x: f32, period: f32) -> f32 {
  return abs(fract(x/period+.5)-.5)*period;
}
fn grid_coverage_at_width(p: vec2<f32>, footprint: vec2<f32>, period: vec2<f32>, half_width: f32) -> f32 {
  let x=grid_band_coverage(p.x,footprint.x,period.x,half_width);
  let y=grid_band_coverage(p.y,footprint.y,period.y,half_width);
  return x+y-x*y;
}
fn filtered_mullion_grid(p: vec2<f32>, footprint_in: vec2<f32>, period: vec2<f32>) -> f32 {
  // Preserve the existing triangular 4cm-half-width profile. Its 2D maximum
  // equals the integral of unions of hard bands over widths from zero to4cm.
  // Band coverage is piecewise linear in width; on each interval its union is
  // quadratic, so two-point Gauss integration is exact for this box footprint.
  let support=.04;
  let footprint=max(footprint_in,vec2<f32>(.0001));
  var knots=array<f32,6>(0.0,support,
      clamp(grid_distance(p.x-footprint.x*.5,period.x),0.0,support),
      clamp(grid_distance(p.x+footprint.x*.5,period.x),0.0,support),
      clamp(grid_distance(p.y-footprint.y*.5,period.y),0.0,support),
      clamp(grid_distance(p.y+footprint.y*.5,period.y),0.0,support));
  for(var i=1u;i<6u;i+=1u) {
    let value=knots[i];var j=i;
    loop {
      if(j==0u){break;}
      if(knots[j-1u]<=value){break;}
      knots[j]=knots[j-1u];j-=1u;
    }
    knots[j]=value;
  }
  var integral=0.0;
  for(var i=1u;i<6u;i+=1u) {
    let half_interval=(knots[i]-knots[i-1u])*.5;
    let centre=(knots[i]+knots[i-1u])*.5;
    let offset=half_interval*.577350269189626;
    integral+=half_interval*(grid_coverage_at_width(p,footprint,period,centre-offset)+
                             grid_coverage_at_width(p,footprint,period,centre+offset));
  }
  return clamp(integral/support,0.0,1.0);
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
  let floor_on = floor_h < room_floor_probability(room.w,night);
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
  let light_col = mix(warm, cool, step(0.86, h.y));
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
  let ambient = mix(room_dark_ambient(night),.75,lit) * light_col;
  col = col * ambient;
  // blinds: replace with a pale slatted surface just behind the glass
  let slats = 0.6 + 0.4 * step(0.5, fract(p.y * 8.0));
  col = mix(col, vec3<f32>(0.75, 0.74, 0.70) * slats * mix(0.15, 0.6, lit), blinds);
  return col;
}

// ---- analytic facade patterns -----------------------------------------------------
// Far detail levels carry lattice members, fins and louvre blades as a pattern
// on the glass instead of geometry: code in aux.w (1 diagrid, 2 x-frame,
// 3 hex lattice, 4 ribbon fins, 5 fin weave, 6 louvres; +8 dark members),
// module and cell height (m) in uv. Drawn band-limited: exact coverage while
// a cell spans several pixels, the pattern's mean coverage once it does not.
fn sd_seg(p: vec2<f32>, a: vec2<f32>, b: vec2<f32>) -> f32 {
  let ab = b - a;
  let t = clamp(dot(p - a, ab) / max(dot(ab, ab), 1e-6), 0.0, 1.0);
  return length(p - a - ab * t);
}
// Anti-aliased coverage of the periodic line family f = integer: `wpx` is
// the pixel footprint of f (periods per pixel, from its screen derivative),
// `rr` the member half width in periods. Box-filtered while a period spans
// pixels; the family's mean coverage once it spans under ~4 px, where even
// the filtered pattern would alias.
fn line_cov(f: f32, wpx_in: f32, rr: f32) -> f32 {
  let wpx = max(wpx_in, 1e-5);
  let d = abs(fract(f + 0.5) - 0.5);
  let exact = clamp((rr + 0.5 * wpx - d) / wpx, 0.0, 1.0);
  let mean = min(2.0 * rr, 1.0);
  return mix(mean, exact, clamp((1.0 / wpx - 2.0) / 2.0, 0.0, 1.0));
}
fn cov_union(a: f32, b: f32) -> f32 { return 1.0 - (1.0 - a) * (1.0 - b); }
// pixel footprint (fwidth) of the linear function a*u + b*y of the facade
// coordinates, from their screen derivatives dax = dpdx(uv), day = dpdy(uv)
fn fw_lin(a: f32, b: f32, dax: vec2<f32>, day: vec2<f32>) -> f32 {
  return abs(a * dax.x + b * dax.y) + abs(a * day.x + b * day.y);
}
// Coverage of the facade pattern `kind` at facade point p (m), module M, cell H.
fn pattern_cov(kind: u32, p: vec2<f32>, M: f32, H: f32, dax: vec2<f32>, day: vec2<f32>) -> f32 {
  if (kind == 1u || kind == 2u) {
    // diagonals through the cell corners (both directions), x-frame adds the chords
    let gd = sqrt(1.0 / (M * M) + 1.0 / (H * H));  // |grad f| in 1/m
    let r = select(0.42, 0.5, kind == 2u) * gd;    // half width in periods
    let f1 = p.x / M + p.y / H;
    let f2 = p.x / M - p.y / H;
    var c = cov_union(line_cov(f1, fw_lin(1.0 / M, 1.0 / H, dax, day), r), line_cov(f2, fw_lin(1.0 / M, -1.0 / H, dax, day), r));
    if (kind == 2u) { c = cov_union(c, line_cov(p.y / H, fw_lin(0.0, 1.0 / H, dax, day), 0.5 / H)); }
    return c;
  } else if (kind == 3u) {
    // hex lattice: rows 0.75 cells apart, odd rows shifted half a module (see lattice())
    let rowh = 0.75 * H;
    let j = floor(p.y / rowh);
    let yy = p.y - j * rowh;
    let shift = select(0.0, 0.5, (i32(j) & 1) == 1);
    let xx = fract(p.x / M - shift) * M;
    let dv = length(vec2<f32>(min(xx, M - xx), max(0.0, 0.25 * H - yy)));  // vertical sides
    let q = vec2<f32>(xx, yy);
    let dz = min(sd_seg(q, vec2<f32>(0.0, 0.25 * H), vec2<f32>(0.5 * M, 0.0)),
                 sd_seg(q, vec2<f32>(0.5 * M, 0.0), vec2<f32>(M, 0.25 * H)));  // zigzag
    let d = min(dv, dz);
    let fd = max(0.5 * (fw_lin(1.0, 0.0, dax, day) + fw_lin(0.0, 1.0, dax, day)), 1e-4);  // metres per pixel
    let r = 0.3;
    let exact = clamp((r + 0.5 * fd - d) / fd, 0.0, 1.0);
    let mean = clamp(2.0 * r * (0.5 * H + 2.0 * sqrt(0.25 * M * M + H * H / 16.0)) / (M * rowh), 0.0, 1.0);
    return mix(mean, exact, clamp((min(M, rowh) / fd - 2.0) / 2.0, 0.0, 1.0));
  } else if (kind == 4u) {
    return line_cov(p.y / H, fw_lin(0.0, 1.0 / H, dax, day), 0.26 / H);  // a fin ledge at every floor line
  } else if (kind == 5u) {
    // vertical fins, one per module, shifted half a module on odd floors
    let shift = select(0.0, 0.5, (i32(floor(p.y / H)) & 1) == 1);
    return line_cov(p.x / M - 0.5 - shift, fw_lin(1.0 / M, 0.0, dax, day), 0.16 / M);
  } else if (kind == 6u) {
    return line_cov(2.0 * p.y / H, fw_lin(0.0, 2.0 / H, dax, day), 0.18 * 2.0 / H);  // two blades per floor
  }
  return 0.0;
}

// The crescent is a separate weak source. Its direct response is independent
// of the warm, shadowed twilight sun; diffuse moon bounce is negligible here.
// Packed CSR lists belong to this exact camera, including reflected views.
fn practical_header(pixel: vec2<f32>) -> vec2<u32> {
  let dimensions=vec2<u32>(ceil(frame.screen.xy/16.0));
  let tile=min(vec2<u32>(pixel)/16u,dimensions-vec2<u32>(1u));
  let base=(tile.y*dimensions.x+tile.x)*2u;
  return vec2<u32>(light_tiles[base],light_tiles[base+1u]);
}
fn practical_radiance(world: vec3<f32>,pixel: vec2<f32>,n: vec3<f32>,v: vec3<f32>,
  t: vec3<f32>,b: vec3<f32>,f0: vec3<f32>,body: vec3<f32>,rough: f32,
  specular_weight: f32,botanical: bool,brushed: bool) -> vec3<f32> {
  let header=practical_header(pixel);
  let ndv=max(dot(n,v),1e-4);let alpha=max(rough*rough,.002);
  var result=vec3<f32>(0.0);
  for(var i=0u;i<header.y;i=i+1u) {
    let source=lights[light_tiles[header.x+i]];
    let delta=source.pos_radius.xyz-world;
    let d2=dot(delta,delta);let r2=source.pos_radius.w*source.pos_radius.w;
    if(r2<=0.0||d2>=r2||source.color_int.w==0.0){continue;}
    let direction=delta*inverseSqrt(max(d2,1e-8));
    let sum=v+direction;let half_vector=sum*inverseSqrt(max(dot(sum,sum),1e-8));
    let cosine=max(dot(n,direction),0.0);
    let fresnel=f_schlick(f0,max(dot(v,half_vector),0.0));
    let distribution=select(d_ggx(max(dot(n,half_vector),0.0),alpha),
      d_brushed(n,half_vector,t,b,max(rough,.18)),brushed);
    let reflection=distribution*v_smith(ndv,cosine,alpha)*fresnel*cosine*specular_weight;
    // body already contains (1-metallic), exactly once. Each source owns its
    // Fresnel and botanical incident direction, rather than reusing the sun.
    var diffuse_cosine=cosine;var diffuse_body=body;
    if(botanical) {
      diffuse_cosine=clamp((dot(n,direction)+.4)/1.4,0.0,1.0);
      diffuse_body*=1.0+max(dot(-n,direction),0.0)*.65;
    }
    let diffuse=(1.0-fresnel)*diffuse_body/PI*diffuse_cosine;
    let window=clamp(1.0-d2*d2/(r2*r2),0.0,1.0);
    let attenuation=window*window/(d2+1.0);
    let visibility=point_source_visibility(world,n,source.pos_radius.xyz).x;
    result+=(diffuse+reflection)*source.color_int.rgb*source.color_int.w*attenuation*mix(.12,1.0,frame.params.z)*visibility;
  }
  return result;
}
fn practical_diagnostic(world: vec3<f32>,pixel: vec2<f32>) -> vec3<f32> {
  let header=practical_header(pixel);var reaching=0u;
  for(var i=0u;i<header.y;i=i+1u) {
    let source=lights[light_tiles[header.x+i]];
    let delta=source.pos_radius.xyz-world;
    if(dot(delta,delta)<source.pos_radius.w*source.pos_radius.w&&source.color_int.w>0.0){reaching+=1u;}
  }
  // Raw8-bit diagnostic is bounded to255; CPU logs retain unrestricted counts.
  return vec3<f32>(f32(min(header.y,255u))/255.0,f32(min(reaching,255u))/255.0,select(0.0,1.0,secondary_view()));
}
fn practical_visibility_diagnostic(world: vec3<f32>,normal: vec3<f32>,pixel: vec2<f32>) -> vec3<f32> {
  let header=practical_header(pixel);var reaching=0.0;var blocked=0.0;var known=0.0;
  for(var i=0u;i<header.y;i=i+1u) {
    let source=lights[light_tiles[header.x+i]];let delta=source.pos_radius.xyz-world;
    if(dot(delta,delta)<source.pos_radius.w*source.pos_radius.w&&source.color_int.w>0.0) {
      let visibility=point_source_visibility(world,normal,source.pos_radius.xyz);
      reaching+=1.0;blocked+=1.0-visibility.x;known+=visibility.y;
    }
  }
  return vec3<f32>(blocked/max(reaching,1.0),known/max(reaching,1.0),min(reaching,255.0)/255.0);
}
fn moon_light(n: vec3<f32>,v: vec3<f32>,f0: vec3<f32>,diffuse: vec3<f32>,rough: f32,specular_weight: f32) -> vec3<f32> {
  if(frame.moon_dir.w<.5){return vec3<f32>(0.0);}
  let l=normalize(frame.moon_dir.xyz);let h=normalize(v+l);
  let nl=max(dot(n,l),0.0);let nv=max(dot(n,v),.0001);
  let alpha=max(rough*rough,.008);let f=f_schlick(f0,max(dot(v,h),0.0));
  let spec=d_ggx(max(dot(n,h),0.0),alpha)*min(v_smith(nv,nl,alpha),8.0)*f;
  return (diffuse*(1.0-f)/PI+spec*specular_weight)*frame.moon_color.rgb*nl;
}

struct FsOut { @location(0) color: vec4<f32> };

@fragment fn fs_main(in: VOut, @builtin(front_facing) front: bool) -> FsOut {
  let m = materials[in.material];
  let flags = u32(m.misc.x + 0.5);
  if(secondary_view()&&(in.world.y<frame.transport.z+.02||(flags&FLAG_WATER)!=0u)){discard;}
  let transmission=(flags&FLAG_TRANSMISSION)!=0u;
  if(frame.transport.w>1.5&&!transmission){discard;}
  if(frame.transport.w<1.5&&transmission){discard;}
  // Zero surface coverage is an absent pane, including depth and the sky's
  // display classification. It must not bend or fog the background lookup.
  if(transmission&&m.room.z<=0.0){discard;}
  let night = frame.params.z;
  let V = normalize(frame.camera_pos.xyz - in.world);
  var N = normalize(in.normal);
  let alpha_foliage = (flags & FLAG_FOLIAGE) != 0u;
  let foliage = (flags & (FLAG_FOLIAGE|256u)) != 0u;
  // The mirrored view reverses raster winding while world-space normals stay
  // unchanged. Recover the physical face before orienting two-sided optics.
  let physical_front=front!=secondary_view();
  if ((foliage || transmission) && !physical_front) { N = -N; }
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
  var albedo = m.base_color.rgb*in.tint;
  var alpha = 1.0;
  if (alpha_foliage) {
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
  var surface_height = .5;
  if (m.tex.z >= 0.0) {
    let arm = textureSample(arm_arr, mat_samp, uv, i32(m.tex.z));
    ao_tex_v = arm.r;
    surface_height = arm.b;
    roughness = clamp(mix(m.params.x, arm.g * (m.params.x / 0.6), 0.55), select(0.12, 0.22, m.params.y > 0.5), 1.0);
    if ((flags & FLAG_EXPOSED_STONE) != 0u) {
      // An authored dry honed finish retains the reviewed tile's spatial
      // roughness variation without inheriting its polished mean finish.
      roughness=clamp(m.params.x+(arm.g-.0658778)*.25,.45,.90);
    }
  }
  if (m.tex.y >= 0.0) {
    let nt = textureSample(normal_arr, mat_samp, uv, i32(m.tex.y)).xyz * 2.0 - 1.0;
    // Toksvig: a mip-averaged normal is shorter than one; the lost length
    // is the normal variance under this pixel — widen the specular lobe by
    // it instead of letting a sub-pixel bump field sparkle.
    let len = clamp(length(nt), 0.05, 1.0);
    let var_n = (1.0 - len) / len;
    roughness = clamp(sqrt(roughness * roughness + 0.6 * var_n), roughness, 1.0);
    let ns = m.params.w;
    N = normalize(T * (nt.x * ns) + B * (nt.y * ns) + N * nt.z);
  }
  // Broad damp patches on the civic paving; low spatial frequency keeps
  // the polished/dry transition stable during a walking-camera sweep.
  if ((flags & FLAG_WETTABLE) != 0u && N.y > 0.8) {
    let exposed_stone=(flags&FLAG_EXPOSED_STONE)!=0u;
    let q = in.world.xz * select(.10,.42,exposed_stone);
    let f = fract(q); let w = f*f*(3.0-2.0*f);
    let p = floor(q);
    let a=hash13(vec3<f32>(p,7.0));
    let b=hash13(vec3<f32>(p+vec2<f32>(1.0,0.0),7.0));
    let c=hash13(vec3<f32>(p+vec2<f32>(0.0,1.0),7.0));
    let d=hash13(vec3<f32>(p+vec2<f32>(1.0,1.0),7.0));
    let retention=mix(mix(a,b,w.x),mix(c,d,w.x),w.y);
    var wet=smoothstep(0.48,0.72,retention);
    if(exposed_stone) {
      // Water remains in the reviewed stone's shallow recesses. The existing
      // drain and sheltered storefront dry sooner than the exposed slabs.
      let film_level=.47+.22*retention;
      let threshold_width=max(.04,fwidth(surface_height));
      let recess=smoothstep(-threshold_width,threshold_width,film_level-surface_height);
      let shelter=smoothstep(-121.3,-118.0,in.world.x);
      let nearest_drain_z=83.0+24.0*clamp(round((in.world.z-83.0)/24.0),0.0,2.0);
      let drain_delta=in.world.xz-vec2<f32>(-106.5,nearest_drain_z);
      let drain=1.0-.75*exp(-dot(drain_delta,drain_delta)/3.5);
      wet=recess*shelter*drain;
    }
    roughness=mix(roughness,0.13,wet);
    albedo*=mix(1.0,0.65,wet);
  }
  let water=(flags&FLAG_WATER)!=0u;
  if(water) {
    // Stationary wind ripples are world anchored and band limited through
    // geometric specular AA; a camera move never changes their phase.
    var ripple=vec2<f32>(0.0);
    for(var wave=0;wave<12;wave=wave+1) {
      let w=f32(wave);let angle=.35+sin(w*2.17)*1.2;
      let direction=vec2<f32>(cos(angle),sin(angle));
      let frequency=.055*pow(1.34,w);
      let phase=dot(in.world.xz,direction)*frequency+w*2.718;
      let footprint=max(abs(dpdx(phase)),abs(dpdy(phase)));
      let resolved=1.0-smoothstep(.7,2.6,footprint);
      ripple+=direction*sin(phase)*(.014*pow(.88,w))*resolved;
    }
    N=normalize(N+vec3<f32>(ripple.x,0,ripple.y));roughness=.14;
  }
  let metallic = select(m.params.y,0.0,water);
  let ndv = max(dot(N, V), 1e-4);
  // Geometric specular anti-aliasing: widen the lobe by the normal's
  // screen-space variance (thin tubes, dense fins).
  {
    let gn = select(normalize(in.normal),N,water);
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

  let f0 = mix(select(vec3<f32>(0.04),vec3<f32>(0.0204),water), albedo, metallic);
  // The existing botanical cuticle coverage applies to every incident light.
  // It attenuates reflected energy, never the supplied green body pigment.
  let specular_weight=select(1.0,.35,foliage);
  let diffuse_color = albedo * (1.0 - metallic);
  var color = vec3<f32>(0.0);
  var dbg_sun = vec3<f32>(0.0);
  var dbg_ibl_d = vec3<f32>(0.0);
  var dbg_ibl_s = vec3<f32>(0.0);
  var dbg_spec = vec3<f32>(0.0);
  var dbg_window_fields = vec3<f32>(0.0);
  var dbg_floor_fields = vec3<f32>(0.0);
  var dbg_room_footprints = vec3<f32>(0.0);
  var transmitted_background = vec3<f32>(0.0);
  var transmitted_weight = vec3<f32>(0.0);

  if ((flags & FLAG_GLASS) != 0u) {
    // Slight waviness in the pane normal for realism.
    let wav = (hash13(floor(vec3<f32>(in.aux.x / m.room.x, in.aux.y / m.room.y, 0.0))) - 0.5) * 0.012;
    let Ng = normalize(N + T * wav + B * wav * 0.6);
    let ndv_g = max(dot(Ng, V), 1e-4);
    let R = reflect(-V, Ng);
    let g_rough = roughness;
    let refl = scene_reflection(in.world,Ng,R,g_rough) * frame.params2.x;
    let transmittance=clamp(m.base_color.rgb,vec3<f32>(.001),vec3<f32>(1.0));
    let f0g = occupied_glass_f0(transmittance,m.params.y);
    let brdf = env_brdf(f0g, g_rough, ndv_g);
    // sun highlight
    let H = normalize(V + L);
    let ndh = max(dot(Ng, H), 0.0);
    let a = max(g_rough * g_rough, 0.008);
    let spec_sun = d_ggx(ndh, a) * v_smith(ndv_g, max(dot(Ng, L), 0.0), a) * f_schlick(f0g, max(dot(V, H), 0.0)) * max(dot(Ng, L), 0.0);
    let view_ts = vec3<f32>(dot(V, T), dot(V, B), dot(V, N));
    var inside = interior_color(in.aux.xy, view_ts, m.room, in.aux.z, night);
    // Distance LOD: when a room spans few pixels, fade the parallax detail
    // toward the room's mean so the facade does not alias into moire.
    let fw = fwidth(in.aux.xy);
    let dax = dpdx(in.aux.xy);
    let day = dpdy(in.aux.xy);
    let px_per_room = 1.0/max(max(fw.x/m.room.x,fw.y/m.room.y),1e-4);
    let detail = clamp((px_per_room - 6.0) / 30.0, 0.0, 1.0);
    let cell = floor(in.aux.xy / m.room.xy);
    let hc = hash33(vec3<f32>(cell.x + 3.1, cell.y + 7.7, in.aux.z * 91.7));
    let floor_far = hash13(vec3<f32>(17.0, cell.y + 3.3, in.aux.z * 51.3)) < room_floor_probability(m.room.w,night);
    let lit_far = select(select(0.0, 1.0, hc.x < 0.10), select(0.0, 1.0, hc.x < 0.85), floor_far) * (0.45 + 0.55 * hc.z);
    let light_far = mix(vec3<f32>(1.0, 0.86, 0.68), vec3<f32>(0.78, 0.88, 1.0), step(0.86, hc.y));
    let frac_y = fract(in.aux.y / m.room.y);
    let grad = 0.55 + 0.75 * frac_y * frac_y;  // ceilings brighter than floors
    let room_px_fade = clamp((px_per_room - 1.5) / 4.0, 0.0, 1.0);
    // Do not invent a separate occupancy pattern when rooms become subpixel.
    // Its expectation includes the resolved model's lit rooms on inactive floors.
    let lit_expect = room_lit_mean(m.room.w,night);
    let pixels_per_axis=1.0/max(fw/m.room.xy,vec2<f32>(1e-4));
    let vertical_fade=clamp((pixels_per_axis.y-1.5)/.5,0.0,1.0);
    var floor_fields=vec3<f32>(lit_expect,.8,lit_expect*.8);
    if(vertical_fade>0.0) {
      floor_fields=mix(floor_fields,room_floor_integrals(in.aux.y,fw.y,m.room.y,m.room.w,in.aux.z,night),vertical_fade);
    }
    let lit_eff = mix(floor_fields.x, lit_far, room_px_fade);
    let mean_light=mix(vec3<f32>(1.0,.86,.68),vec3<f32>(.78,.88,1.0),.14);
    let filtered_light=mix(mean_light,light_far,room_px_fade);
    let filtered_grad=mix(.8,grad,room_px_fade); // integral of .55+.75*y*y
    let ambient=room_dark_ambient(night);
    let conditional_energy=ambient*floor_fields.y+(.75-ambient)*floor_fields.z;
    let resolved_energy=mix(ambient,.75,lit_far)*grad;
    var mean_room=vec3<f32>(.42,.40,.37)*mix(mean_light*conditional_energy,light_far*resolved_energy,room_px_fade);
    // Preserve the existing fully resolved operation order and parallax path.
    if(room_px_fade>=1.0||vertical_fade<=0.0) {
      mean_room=vec3<f32>(.42,.40,.37)*filtered_light*mix(ambient,.75,lit_eff)*filtered_grad;
    }
    let gradient_eff=mix(floor_fields.y,grad,room_px_fade);
    let joint_eff=mix(floor_fields.z,lit_far*grad,room_px_fade);
    dbg_floor_fields=vec3<f32>(lit_eff,gradient_eff/1.3,joint_eff/1.3);
    dbg_room_footprints=vec3<f32>(clamp(pixels_per_axis/32.0,vec2<f32>(0),vec2<f32>(1)),vertical_fade);
    inside = mix(mean_room, inside, detail);
    // By day a room is dark next to the sky; at night it is the light source.
    inside = inside * frame.params.x * mix(0.65, 2.7, night);
    // Mullion grid at the room boundaries (thin dark frame lines). When a
    // room spans only a few pixels the lines beat against the pixel grid
    // (moire), so their contrast fades into the uniform darkening their
    // average coverage would produce.
    let frame_line = filtered_mullion_grid(in.aux.xy,fw,m.room.xy);
    dbg_window_fields = vec3<f32>(lit_eff,frame_line,room_px_fade);
    let tint = m.misc.yzw;
    // Snell's law bounds the optical path through a 1.5-IOR pane even at
    // grazing view angles; do not use 1/ndv, which spuriously turns it opaque.
    let cos_inside=sqrt(1.0-(1.0-ndv_g*ndv_g)/(1.5*1.5));
    let through=pow(transmittance,vec3<f32>(1.0/cos_inside));
    color = refl * brdf + sun * spec_sun * shadow + inside * (1.0 - brdf) * tint * through;
    dbg_ibl_s=refl*brdf;
    dbg_ibl_d=inside*(1.0-brdf)*tint*through;
    dbg_spec=sun*spec_sun*shadow;
    color += practical_radiance(in.world,in.pos.xy,Ng,V,T,B,f0g,vec3<f32>(0.0),g_rough,1.0,false,false);
    let frame_body=vec3<f32>(.02,.02,.022);
    let frame_practical=practical_radiance(in.world,in.pos.xy,N,V,T,B,vec3<f32>(.04),frame_body,.55,1.0,false,false);
    color = mix(color, frame_body * (sh_irradiance(N) / PI + sun * ndl * shadow / PI)+frame_practical, frame_line * 0.85);
    dbg_ibl_s*=1.0-frame_line*.85;dbg_ibl_d*=1.0-frame_line*.85;dbg_spec*=1.0-frame_line*.85;
    color += moon_light(Ng,V,f0g,vec3<f32>(0.0),g_rough,1.0);
    // far-level facade pattern (lattice members, fins, blades) on the glass
    let pcode = u32(in.aux.w * 127.0 + 0.5);
    if (pcode < 16u && (pcode & 7u) != 0u && in.uv.x > 0.0 && in.uv.y > 0.0) {
      let cov = pattern_cov(pcode & 7u, in.aux.xy, in.uv.x, in.uv.y, dax, day);
      // members are tubes and ledges lit mostly from above; dark metal members
      // read as a dim reflection of the sky rather than a diffuse brown
      let dark = (pcode & 8u) != 0u;
      let mem_alb = select(vec3<f32>(0.80, 0.80, 0.78), vec3<f32>(0.20, 0.15, 0.11), dark);
      let Nm = normalize(N + vec3<f32>(0.0, 0.9, 0.0));
      let ndl_m = max(dot(Nm, L), 0.0);
      // the facade's own shadow lookup is biased for a grazing sun and speckles;
      // look the members up with their own normal
      let shadow_m = shadow_factor(in.world, Nm, in.view_z, ndl_m, in.pos.xy);
      let mem_lit = mem_alb * (sh_irradiance(Nm) + sun * ndl_m * shadow_m) / PI+
        practical_radiance(in.world,in.pos.xy,Nm,V,T,B,vec3<f32>(.04),mem_alb,.6,1.0,false,false);
      color = mix(color, mem_lit, cov);
      dbg_ibl_s*=1.0-cov;dbg_ibl_d*=1.0-cov;dbg_spec*=1.0-cov;
    }
    // dust/dirt on glass: a faint diffuse term
    color += diffuse_color * 0.02 * (sh_irradiance(N) + sun * ndl * shadow) / PI;
  } else {
    let H = normalize(V + L);
    let ndh = max(dot(N, H), 0.0);
    let vdh = max(dot(V, H), 0.0);
    let a = max(roughness * roughness, 0.002);
    let a_sun = max(a, 0.008);  // the sun is a disc: widen the lobe, no point-light glints
    let F = f_schlick(f0, vdh);
    var kd = (1.0 - F) * (1.0 - metallic);
    var diffuse = diffuse_color / PI;
    let surface_ndl=ndl;
    if (foliage) {
      // Wrap models diffuse transmission through a leaf. Surface reflection
      // keeps the actual incident cosine and its matching Smith visibility.
      ndl = clamp((dot(N, L) + 0.4) / 1.4, 0.0, 1.0);
      let back = max(dot(-N, L), 0.0) * 0.65;
      diffuse = diffuse * (1.0 + back);
    }
    let distribution=select(d_ggx(ndh,a_sun),d_brushed(N,H,T,B,max(roughness,.18)),metallic>.55);
    let spec = distribution * min(v_smith(ndv, surface_ndl, a_sun), 8.0) * F * specular_weight;
    dbg_sun = kd * diffuse * sun * ndl * shadow;
    dbg_spec = spec * sun * surface_ndl * shadow;
    color += dbg_sun + dbg_spec;
    if(foliage){let through=pow(max(dot(-V,L),0.0),3.0)*.36; color+=albedo*sun*through*mix(.35,1.0,shadow)/PI;}
    // IBL
    let irr = scene_irradiance(in.world,N) * frame.params2.x;
    dbg_ibl_d = diffuse_color * irr / PI * ao;
    color += dbg_ibl_d;
    let R = reflect(-V, N);
    var pref = scene_reflection(in.world,N,R,roughness) * frame.params2.x;
    // The physical raised reflection plane also serves its adjacent wet stone.
    // Selecting it by roughness created a discontinuity inside a single slab.
    let planar_wet_stone=(flags&FLAG_WETTABLE)!=0u && normalize(in.normal).y>.9 &&
      (abs(in.world.y-1.222)<.04 || abs(in.world.y)<.04);
    if(water || planar_wet_stone) { pref=water_reflection(in.world,N,roughness,pref,false); }
    if(water) {
      // Beer-Lambert attenuation through a coastal basin; absorption channels
      // differ instead of treating water as a tinted metal.
      let depth=5.0+2.5*sin(in.world.x*.007)*cos(in.world.z*.009);
      let absorption=exp(-vec3<f32>(.24,.055,.026)*depth/max(ndv,.12));
      let bottom=vec3<f32>(.10,.12,.09)*irr/PI;
      let scatter=vec3<f32>(.014,.058,.065)*irr/PI;
      color=mix(scatter,bottom,absorption)*(1.0-f_schlick(vec3<f32>(.0204),ndv))+dbg_spec;
    }
    let so = clamp(pow(ndv + ao, exp2(-16.0 * roughness - 1.0)) - 1.0 + ao, 0.0, 1.0);  // Lagarde spec occlusion
    dbg_ibl_s = pref * env_brdf(f0, roughness, ndv) * so * specular_weight;
    color += dbg_ibl_s;
    color += moon_light(N,V,f0,select(diffuse_color,vec3<f32>(0.0),water),roughness,specular_weight);
    color += practical_radiance(in.world,in.pos.xy,N,V,T,B,f0,
      select(diffuse_color,vec3<f32>(0.0),water),roughness,specular_weight,foliage,metallic>.55);

  }
  if(transmission) {
    let ior=max(m.room.x,1.001);let fzero=pow((ior-1.0)/(ior+1.0),2.0);
    let fresnel=f_schlick(vec3<f32>(fzero),max(abs(dot(N,V)),.001));
    let refracted=refract(-V,N,1.0/ior);
    let inside_cos=max(-dot(N,refracted),.1);
    // A parallel sheet returns the outgoing ray to its original direction,
    // with this lateral displacement accumulated over the physical thickness.
    let displacement=max(m.room.w,0.0)*(refracted/inside_cos+V/max(ndv,.1));
    let shifted=frame.view_proj*vec4<f32>(in.world+displacement,1.0);
    let unshifted=frame.view_proj*vec4<f32>(in.world,1.0);
    let delta_ndc=shifted.xy/shifted.w-unshifted.xy/unshifted.w;
    let refraction_uv=in.pos.xy*frame.screen.zw+delta_ndc*vec2<f32>(.5,-.5);
    let behind=textureSampleLevel(transmitted_scene,clamp_samp,refraction_uv,0.0).rgb;
    let uncovered=textureSampleLevel(transmitted_scene,clamp_samp,in.pos.xy*frame.screen.zw,0.0).rgb;
    // glTF baseColor is the pane's one-pass tint. No arbitrary absorption
    // spectrum is substituted for it; oblique incidence lengthens the path.
    let absorption=pow(clamp(m.base_color.rgb,vec3<f32>(.001),vec3<f32>(1.0)),vec3<f32>(1.0/inside_cos));
    var reflected=scene_reflection(in.world,N,reflect(-V,N),roughness);
    if((flags&FLAG_PLANAR_POND)!=0u) {
      reflected=water_reflection(in.world,N,roughness,reflected,true);
    }
    let H=normalize(V+L);let nl=max(dot(N,L),0.0);
    let alpha_g=max(roughness*roughness,.008);
    let solar_spec=d_ggx(max(dot(N,H),0.0),alpha_g)*v_smith(ndv,nl,alpha_g)*f_schlick(vec3<f32>(fzero),max(dot(V,H),0.0))*nl*sun*shadow;
    let lunar_spec=moon_light(N,V,vec3<f32>(fzero),vec3<f32>(0.0),roughness,1.0);
    let coverage=clamp(m.room.z,0.0,1.0);
    let optical_weight=absorption*clamp(m.room.y,0.0,1.0)*(1.0-fresnel);
    transmitted_background=mix(uncovered,behind*optical_weight,coverage);
    transmitted_weight=mix(vec3<f32>(1.0),optical_weight,coverage);
    let point_spec=practical_radiance(in.world,in.pos.xy,N,V,T,B,vec3<f32>(fzero),vec3<f32>(0.0),roughness,1.0,false,false);
    color=transmitted_background+coverage*(reflected*env_brdf(vec3<f32>(fzero),roughness,ndv)+solar_spec+lunar_spec+point_spec);
    // True glass replaces the provisional opaque response. Its diagnostics
    // must report these actual optical terms, not the discarded diffuse body.
    // Components are pre-atmosphere; transmission includes the sampled opaque
    // background, while reflection/specular report this pane's own response.
    dbg_ibl_d=transmitted_background;
    dbg_ibl_s=coverage*reflected*env_brdf(vec3<f32>(fzero),roughness,ndv);
    dbg_spec=coverage*solar_spec;
    dbg_sun=vec3<f32>(0.0);
  }
  // emissive
  if ((flags & FLAG_EMISSIVE) != 0u) {
    var e = m.params.z * frame.params.x;
    if ((flags & FLAG_NIGHT_ONLY) != 0u) { e *= night; }
    color += m.misc.yzw * e;
  }

  // Height-weighted aerial perspective: keep the nearby ceramic crisp,
  // separate successive districts, and meet the environment at the horizon.
  let distance_m = length(in.world - frame.camera_pos.xyz);
  let density = mix(0.00010,0.000065,night) * exp(-max(in.world.y, 0.0) / 380.0);
  let fog = 1.0 - exp(-distance_m * density);
  let sight = normalize(in.world - frame.camera_pos.xyz);
  let horizon = atmosphere_horizon(sight,frame.sun_dir.xyz,night);
  let forward_scatter=pow(max(dot(sight,frame.sun_dir.xyz),0.0),8.0);
  var mist_colour=horizon*frame.params2.x+frame.sun_color.rgb*forward_scatter*.006;
  if(frame.moon_shape.w>.5) {
    // Distant extinction converges to the actual atmosphere at this world
    // azimuth. A broad angular footprint rejects individual stars/cloud-edge
    // stripes; only the far field approaches it, so nearby contrast survives.
    let horizon_dir=normalize(vec3<f32>(sight.x,.001,sight.z));
    let broad_lod=max(log2(f32(textureDimensions(sky_cube).x))-4.0,0.0);
    // Only the last few angular pixels converge to the local boundary value.
    // Using this detail across the city would stretch stars into vertical fog
    // stripes; using the broad average at infinity would leave a color seam.
    let lod=mix(broad_lod,.5,smoothstep(30000.0,90000.0,distance_m));
    let boundary=textureSampleLevel(sky_cube,cube_samp,horizon_dir,lod).rgb;
    let far_mix=1.0-exp(-max(distance_m-6000.0,0.0)/12000.0);
    mist_colour=mix(mist_colour,display_to_scene_radiance(boundary),far_mix);
  }
  // The opaque background copy already includes its complete camera-path
  // atmosphere. Only the new pane response needs camera-to-pane extinction;
  // in-scattered light before the pane must not be attenuated by its tint.
  let pane_response=color-transmitted_background;
  var atmosphere_transmission=1.0-fog;
  color = mix(color, mist_colour, fog);
  if(water) {
    let ocean_extinction=1.0-exp(-max(distance_m-7000.0,0.0)*.00018);
    color=mix(color,mist_colour,ocean_extinction);
    atmosphere_transmission*=1.0-ocean_extinction;
  }
  // Thin coastal mist layers lie in world space and survive camera motion.
  let mist_height=exp(-pow((in.world.y-32.0)/28.0,2.0));
  let coastal_mist=(1.0-exp(-distance_m*.000022))*mist_height;
  color=mix(color,mist_colour,coastal_mist);
  atmosphere_transmission*=1.0-coastal_mist;
  if(transmission) {
    color=transmitted_background+pane_response*atmosphere_transmission+
          mist_colour*(1.0-atmosphere_transmission)*(1.0-transmitted_weight);
  }

  // Clamp the HDR range so glints cannot flood the bloom chain.
  color = min(color, vec3<f32>(10.0 / frame.sun_color.w));

  // ---- debug views --------------------------------------------------------------
  let dbg = debug_view(frame.params2.w);
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
  else if (dbg == 16) {color=dbg_window_fields;}
  else if (dbg == 17) {color=practical_diagnostic(in.world,in.pos.xy);}
  else if (dbg == 18) {color=practical_visibility_diagnostic(in.world,N,in.pos.xy);}
  else if (dbg == 19) {color=dbg_floor_fields;}
  else if (dbg == 20) {color=dbg_room_footprints;}
  else if (dbg == 12) { color = vec3<f32>(f32(in.material) / 255.0, 0.0, 0.0); }
  else if (dbg == 13) {color=vec3<f32>(.45)*(scene_irradiance(in.world,N)+sun*ndl*shadow)/PI*ssao;}
  else if (dbg == 14) {color=vec3<f32>(0.0);}
  else if (dbg == 15) {color=albedo*(vec3<f32>(.4)+vec3<f32>(1.4)*max(dot(N,normalize(vec3<f32>(.4,.8,.3))),0.0))/frame.sun_color.w;}


  var o: FsOut;
  o.color = vec4<f32>(color, alpha);
  return o;
}
