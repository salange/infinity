// Finite-segment base-voxel traversal, mirrored by point_visibility.hpp.
// RGB/radiance mips never participate in point visibility. Fine occupancy wins
// inside its world bounds; uncovered space is reported separately from clear.
struct PointGrid { origin: vec3<f32>, cell: f32, dimensions: vec3<i32>, fine: u32 };
fn point_grid_cell(grid: PointGrid,p: vec3<f32>) -> vec3<i32> {
  return vec3<i32>(floor((p-grid.origin)/grid.cell));
}
fn point_grid_interval(grid: PointGrid,start: vec3<f32>,delta: vec3<f32>) -> vec2<f32> {
  var begin=0.0;var end=1.0;
  for(var axis=0u;axis<3u;axis=axis+1u) {
    let low=grid.origin[axis];let high=low+f32(grid.dimensions[axis])*grid.cell;
    let p=start[axis];let d=delta[axis];
    if(abs(d)<1e-12) {
      if(p<low||p>=high){return vec2<f32>(1.0,0.0);}
    } else {
      let a=(low-p)/d;let b=(high-p)/d;
      begin=max(begin,min(a,b));end=min(end,max(a,b));
    }
  }
  return vec2<f32>(begin,end);
}
fn point_normal_component(packed: u32,shift: u32) -> f32 {
  let bits=i32((packed>>shift)&32767u);
  return f32(select(bits,bits-32768,bits>=16384))/16383.0;
}
fn point_grid_occupied(grid: PointGrid,cell: vec3<i32>,a: vec3<f32>,b: vec3<f32>) -> bool {
  var packed=vec2<u32>(0);
  if(grid.fine!=0u){packed=textureLoad(fine_point_geometry,cell,0).xy;}
  else {packed=textureLoad(point_geometry,cell,0).xy;}
  if(packed.x==0u){return false;}
  if((packed.x&0x80000000u)!=0u){return true;}
  let oct=vec2<f32>(point_normal_component(packed.x,0u),point_normal_component(packed.x,15u));
  var decoded=vec3<f32>(oct,1.0-abs(oct.x)-abs(oct.y));
  let fold=max(-decoded.z,0.0);
  decoded.x+=select(fold,-fold,decoded.x>=0.0);
  decoded.y+=select(fold,-fold,decoded.y>=0.0);
  let n=normalize(decoded);
  let low_bits=i32(packed.y&65535u);let high_bits=i32(packed.y>>16u);
  let low=f32(select(low_bits,low_bits-65536,low_bits>=32768))/32767.0;
  let high=f32(select(high_bits,high_bits-65536,high_bits>=32768))/32767.0;
  let centre=grid.origin+(vec3<f32>(cell)+vec3<f32>(.5))*grid.cell;
  let da=dot(n,a-centre)/grid.cell;let db=dot(n,b-centre)/grid.cell;
  return max(da,db)>=low&&min(da,db)<=high;
}
fn point_grid_blocked(grid: PointGrid,start: vec3<f32>,delta: vec3<f32>,
  receiver: vec3<f32>,source: vec3<f32>,begin: f32,end: f32) -> bool {
  if(end<=begin){return false;}
  let receiver_cell=point_grid_cell(grid,receiver);let source_cell=point_grid_cell(grid,source);
  var cursor=clamp(point_grid_cell(grid,start+delta*(begin+min(1e-6,(end-begin)*.25))),
    vec3<i32>(0),grid.dimensions-vec3<i32>(1));
  var step=vec3<i32>(0);var next=vec3<f32>(1e30);var stride=vec3<f32>(1e30);
  for(var axis=0u;axis<3u;axis=axis+1u) {
    let d=delta[axis];step[axis]=i32(sign(d));
    if(step[axis]!=0) {
      stride[axis]=grid.cell/abs(d);
      let boundary=cursor[axis]+select(0,1,step[axis]>0);
      next[axis]=(grid.origin[axis]+f32(boundary)*grid.cell-start[axis])/d;
    }
  }
  let bound=grid.dimensions.x+grid.dimensions.y+grid.dimensions.z+3;
  var entered=begin;
  for(var count=0;count<bound;count=count+1) {
    // Exclude only the cells containing the two actual endpoints. This avoids
    // emitter/receiver self-hits; separate geometry sharing those cells remains
    // below the representation's resolution, not secretly skipped at a bias.
    let crossing=min(next.x,min(next.y,next.z));
    if(any(cursor!=receiver_cell)&&any(cursor!=source_cell)&&
      point_grid_occupied(grid,cursor,start+delta*entered,start+delta*min(crossing,end))){return true;}
    if(crossing>=end){return false;}
    for(var axis=0u;axis<3u;axis=axis+1u) {
      if(next[axis]<=crossing+1e-7) {
        cursor[axis]+=step[axis];next[axis]+=stride[axis];
        if(cursor[axis]<0||cursor[axis]>=grid.dimensions[axis]){return false;}
      }
    }
    entered=crossing;
  }
  return true; // conservative numerical-invariant failure, never a clear cutoff
}
// x visibility, y fraction of this finite segment covered by known occupancy.
fn point_source_visibility(receiver: vec3<f32>,normal: vec3<f32>,source: vec3<f32>) -> vec2<f32> {
  if(frame.point_visibility.x<.5){return vec2<f32>(1.0,0.0);}
  let start=receiver+normal*.002;let delta=source-start;
  if(dot(delta,delta)<1e-12){return vec2<f32>(1.0,0.0);}
  let coarse=PointGrid(frame.voxel_min.xyz,frame.voxel_min.w,vec3<i32>(textureDimensions(point_geometry)),0u);
  let fine=PointGrid(frame.fine_min.xyz,frame.fine_min.w,vec3<i32>(textureDimensions(fine_point_geometry)),1u);
  let ci=point_grid_interval(coarse,start,delta);let fi=point_grid_interval(fine,start,delta);
  var blocked=false;var covered=0.0;
  if(fi.y>fi.x) {
    covered=fi.y-fi.x;
    blocked=point_grid_blocked(fine,start,delta,receiver,source,fi.x,fi.y);
    let before=min(ci.y,fi.x);let after=max(ci.x,fi.y);
    covered+=max(0.0,before-ci.x)+max(0.0,ci.y-after);
    if(!blocked){blocked=point_grid_blocked(coarse,start,delta,receiver,source,ci.x,before);}
    if(!blocked){blocked=point_grid_blocked(coarse,start,delta,receiver,source,after,ci.y);}
  } else {
    covered=max(0.0,ci.y-ci.x);
    blocked=point_grid_blocked(coarse,start,delta,receiver,source,ci.x,ci.y);
  }
  return vec2<f32>(select(1.0,0.0,blocked),clamp(covered,0.0,1.0));
}
