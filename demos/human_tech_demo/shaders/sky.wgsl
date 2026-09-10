#include "common.wgsl"
#include "fullscreen.wgsl"
@group(0) @binding(0) var<uniform> frame: Frame;
@group(1) @binding(0) var sky_cube: texture_cube<f32>;
@group(1) @binding(1) var samp: sampler;


// Deterministic spherical lunar terrain, fixed in the body's world-oriented
// basis. Broad basaltic basins and crater rims are authored analytic detail;
// this is not a claim to reproduce a measured lunar atlas.
fn lunar_noise(p: vec3<f32>) -> f32 {
  let i=floor(p);let f=fract(p);let u=f*f*(3.0-2.0*f);
  let a=mix(hash13(i),hash13(i+vec3<f32>(1,0,0)),u.x);
  let b=mix(hash13(i+vec3<f32>(0,1,0)),hash13(i+vec3<f32>(1,1,0)),u.x);
  let c=mix(hash13(i+vec3<f32>(0,0,1)),hash13(i+vec3<f32>(1,0,1)),u.x);
  let d=mix(hash13(i+vec3<f32>(0,1,1)),hash13(i+vec3<f32>(1,1,1)),u.x);
  return mix(mix(a,b,u.y),mix(c,d,u.y),u.z);
}
fn lunar_surface(q: vec3<f32>,pixel: f32) -> vec4<f32> {
  let basin0=exp(-dot((q-vec3<f32>(-.34,.26,.90))*vec3<f32>(3.9,3.0,2.4),(q-vec3<f32>(-.34,.26,.90))*vec3<f32>(3.9,3.0,2.4)));
  let basin1=exp(-dot((q-vec3<f32>(.25,.34,.90))*vec3<f32>(4.7,3.2,2.7),(q-vec3<f32>(.25,.34,.90))*vec3<f32>(4.7,3.2,2.7)));
  let basin2=exp(-dot((q-vec3<f32>(.24,-.20,.94))*vec3<f32>(5.2,4.8,3.0),(q-vec3<f32>(.24,-.20,.94))*vec3<f32>(5.2,4.8,3.0)));
  let maria=clamp(basin0+basin1+basin2,0.0,1.0);
  var albedo=(.84-.42*maria)*(.86+.20*lunar_noise(q*11.0));
  var gradient=vec3<f32>(0.0);
  for(var i=0;i<36;i=i+1) {
    let seed=vec3<f32>(f32(i)*7.31,17.23,4.17);
    let centre=normalize(hash33(seed)*2.0-1.0);
    let radius=.065+.18*hash13(seed+9.2);
    let offset=q-centre;let distance=max(length(offset),.00001);
    let t=distance/radius;
    let resolved=smoothstep(pixel*.55,pixel*1.4,radius);
    let bowl=exp(-3.0*t*t);let rim=exp(-36.0*(t-.92)*(t-.92));
    let slope=.035*(6.0*t*bowl-15.84*(t-.92)*rim)*resolved;
    gradient+=offset/distance*slope;
    albedo*=1.0+resolved*(.12*rim-.065*bowl);
  }
  let normal=normalize(q-(gradient-q*dot(gradient,q)));
  return vec4<f32>(normal,clamp(albedo,.28,.95));
}
fn lunar_display(radiance: vec3<f32>) -> vec3<f32> {
  return clamp((radiance*(2.51*radiance+.03))/(radiance*(2.43*radiance+.59)+.14),vec3<f32>(0.0),vec3<f32>(1.0));
}

@fragment fn fs_sky(in: FSOut) -> @location(0) vec4<f32> {
  if(debug_view(frame.params2.w)==14){return vec4<f32>(16.0/frame.sun_color.w);}
  if(debug_view(frame.params2.w)==13||debug_view(frame.params2.w)==15){return vec4<f32>(vec3<f32>(.42)/frame.sun_color.w,1.0);}
  // An infinitely distant authored sky has no geometry edges to integrate.
  // Cancel geometry jitter here so fine stars retain the source sampling.
  let jitter=vec2<f32>(frame.cascade_extent.w,frame.cascade_depth.w)*frame.screen.zw*2.0;
  let ndc = vec4<f32>(vec2<f32>(in.uv.x * 2.0 - 1.0, 1.0 - in.uv.y * 2.0)-jitter, 0.0, 1.0);
  let far = frame.inv_view_proj * ndc;
  let dir = normalize(far.xyz / far.w - frame.camera_pos.xyz);
  // A directional environment is sampled with the actual view ray. Below
  // the tangent horizon its optical path becomes opaque, so no cubemap row
  // is extended vertically into a false wall around the city.
  let source=textureSampleLevel(sky_cube,samp,dir,0.0).rgb;
  let optical_path=1.0/max(dir.y+.080,.001);
  let sky_transmission=exp(-.006*optical_path)*smoothstep(-.10,-.045,dir.y);
  let scattered=atmosphere_horizon(dir,frame.sun_dir.xyz,frame.params.z);
  var c=mix(scattered,source,select(sky_transmission,1.0,frame.fine_extent.w>.5));
  if(frame.moon_color.w>.5 && frame.transport.w<.5) {
    let solar=normalize(frame.sun_dir.xyz);
    let angular=acos(clamp(dot(dir,solar),-1.0,1.0));
    let radius=.00465; // 0.533 degree angular diameter
    let coverage=1.0-smoothstep(radius-.00045,radius+.00045,angular);
    let solid_angle=2.0*PI*(1.0-cos(radius));
    let radiance=frame.sun_color.rgb/max(solid_angle,1e-6);
    c+=frame.sun_color.rgb*exp(-angular/.008)*.65;
    c=mix(c,radiance,coverage);
  }
  if(frame.moon_dir.w>.5 && frame.transport.w<.5) {
    let moon=normalize(frame.moon_dir.xyz);let radius=frame.moon_shape.z;
    if(dot(dir,moon)>cos(radius+.001)) {
      let axis=normalize(cross(vec3<f32>(0,1,0),moon));let vertical=cross(moon,axis);
      let xx=dot(dir,axis)/sin(radius);let yy=dot(dir,vertical)/sin(radius);
      let zz=sqrt(max(0.0,1.0-xx*xx-yy*yy));let sphere=normalize(vec3<f32>(xx,yy,zz));
      let pixel_angle=2.0/(frame.screen.y*frame.proj[1][1]);
      let surface=lunar_surface(sphere,pixel_angle/radius);
      let light=normalize(vec3<f32>(frame.moon_shape.x,0,frame.moon_shape.y));
      let macro_lit=max(dot(sphere,light),0.0);
      let micro_lit=max(dot(surface.xyz,light),0.0);
      // Relief shapes the illuminated terrain without letting crater normals
      // illuminate the body's night side beyond its geometric terminator.
      let illuminated=micro_lit*smoothstep(0.0,pixel_angle/radius*.5,macro_lit);
      let lunar_radiance=vec3<f32>(2.15,2.07,1.90)*surface.w*illuminated;
      let earthshine=mix(c,vec3<f32>(.008,.014,.024),smoothstep(.65,1.0,frame.params.z));
      var moon_colour=earthshine+lunar_radiance;
      if(frame.moon_shape.w>.5) {
        // Authored backgrounds are display-referred. Map only this new
        // radiance component and retain headroom instead of clipping the
        // entire illuminated half to white.
        moon_colour=earthshine+max(vec3<f32>(.96)-earthshine,vec3<f32>(0.0))*lunar_display(lunar_radiance);
      }
      let antialias=max(pixel_angle*.55,.0001);
      let coverage=smoothstep(cos(radius+antialias),cos(radius-antialias),dot(dir,moon));
      c=mix(c,moon_colour,coverage);
    }
  }
  // Zero alpha identifies unoccluded sky through the later display pass.
  return vec4<f32>(c * frame.params2.x, 0.0);
}
