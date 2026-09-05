#pragma once
// Geometry container and builders. One vertex format for everything; all
// positions in world metres (y up). The builders emit indexed triangles
// with metric UVs (u along, v up) so texture tile sizes are set per
// material, and an `aux` channel carrying facade-local coordinates for
// interior mapping plus a per-element random and a baked occlusion.
#include <cstdint>
#include <vector>

#include "math.hpp"

namespace cb {

struct Vertex {
  Vec3 position;
  Vec3 normal;
  Vec4 tangent;  // xyz + handedness
  Vec2 uv;
  std::uint32_t material{0};
  Vec4 aux;  // x,y facade-local metres; z element random; w occlusion (1 = open)
};
static_assert(sizeof(Vertex) == 68);

struct Mesh {
  std::vector<Vertex> vertices;
  std::vector<std::uint32_t> indices;
  Vec3 bounds_min{1e30f, 1e30f, 1e30f};
  Vec3 bounds_max{-1e30f, -1e30f, -1e30f};

  std::uint32_t add_vertex(const Vertex& v) {
    bounds_min = vmin(bounds_min, v.position);
    bounds_max = vmax(bounds_max, v.position);
    vertices.push_back(v);
    return static_cast<std::uint32_t>(vertices.size() - 1);
  }
  void add_triangle(std::uint32_t a, std::uint32_t b, std::uint32_t c) {
    indices.push_back(a);
    indices.push_back(b);
    indices.push_back(c);
  }
  void append(const Mesh& o) {
    const std::uint32_t base = static_cast<std::uint32_t>(vertices.size());
    for (const Vertex& v : o.vertices) add_vertex(v);
    for (std::uint32_t i : o.indices) indices.push_back(base + i);
  }
  std::size_t triangle_count() const { return indices.size() / 3; }
};

// Quad corners counter-clockwise seen from the front (normal = (b-a)x(d-a)).
struct QuadUV {
  Vec2 a, b, c, d;
};

struct Emit {
  Mesh* mesh;
  std::uint32_t material{0};
  float element_random{0.0f};
  float occlusion{1.0f};
  // Facade frame for aux.xy: origin and unit axes; when `facade` is false
  // aux.xy = uv.
  bool facade{false};
  Vec3 facade_origin;
  Vec3 facade_u{1, 0, 0};
  Vec3 facade_v{0, 1, 0};

  Emit(Mesh* m, std::uint32_t mat) : mesh(m), material(mat) {}

  // Planar quad a,b,c,d (ccw from front) with explicit uvs.
  void quad(Vec3 a, Vec3 b, Vec3 c, Vec3 d, const QuadUV& uv);
  // Quad with metric uvs derived from its own edges (u along a->b, v along a->d).
  void quad_metric(Vec3 a, Vec3 b, Vec3 c, Vec3 d, Vec2 uv_origin = {0, 0});
  // Axis-aligned or oriented box: centre, half extents, basis (x, y, z axes).
  void box(Vec3 centre, Vec3 half, Vec3 ax = {1, 0, 0}, Vec3 ay = {0, 1, 0}, Vec3 az = {0, 0, 1});
  // Box between two points with a rectangular cross-section (w across, h up).
  void beam(Vec3 p0, Vec3 p1, float w, float h, Vec3 up_hint = {0, 1, 0});
  // Cylinder/tube along p0->p1.
  void tube(Vec3 p0, Vec3 p1, float radius, int sides, bool caps = false);
  void sphere(Vec3 centre, float radius, int rings, int sides);
  // Cone/frustum along p0->p1.
  void frustum(Vec3 p0, Vec3 p1, float r0, float r1, int sides, bool caps = true);
  // Horizontal polygon cap at height y (ccw seen from above => normal up
  // when `up` is true). Concave polygons are ear-clipped.
  void polygon(const std::vector<Vec2>& xz, float y, bool up, Vec2 uv_scale = {1, 1});
  // Vertical wall strip along a polyline (closed when `closed`), from y0 to y1.
  void wall(const std::vector<Vec2>& xz, float y0, float y1, bool closed, bool outward = true);
  // Triangle with metric uvs.
  void triangle(Vec3 a, Vec3 b, Vec3 c);
  void triangle_uv(Vec3 a, Vec3 b, Vec3 c, Vec2 ua, Vec2 ub, Vec2 uc);
};

// 2D plan helpers (x, z) — counter-clockwise seen from above (+y).
std::vector<Vec2> plan_rect(float hx, float hz, Vec2 centre = {0, 0});
std::vector<Vec2> plan_rounded_rect(float hx, float hz, float r, int corner_segments, Vec2 centre = {0, 0});
std::vector<Vec2> plan_superellipse(float a, float b, float n, int segments, Vec2 centre = {0, 0}, float rot = 0.0f);
std::vector<Vec2> plan_circle(float r, int segments, Vec2 centre = {0, 0});
// Lens: intersection of two circles of radius r whose centres are 2*d apart.
std::vector<Vec2> plan_lens(float r, float d, int segments, Vec2 centre = {0, 0}, float rot = 0.0f);
std::vector<Vec2> plan_offset(const std::vector<Vec2>& poly, float distance);  // miter offset (outward > 0)
std::vector<Vec2> plan_scale(const std::vector<Vec2>& poly, float s, Vec2 about);
std::vector<Vec2> plan_transform(const std::vector<Vec2>& poly, Vec2 offset, float rot);
float plan_area(const std::vector<Vec2>& poly);  // signed, ccw positive
float plan_perimeter(const std::vector<Vec2>& poly);
bool point_in_polygon(const std::vector<Vec2>& poly, Vec2 p);
// Ear clipping: indices into poly (triples), ccw polygon.
std::vector<std::uint32_t> triangulate(const std::vector<Vec2>& poly);
// Resample a closed polygon to points spaced ~step apart (returns points and
// their arclength parameter).
struct Sampled {
  std::vector<Vec2> points;
  std::vector<float> arclen;
  std::vector<Vec2> normals;  // outward
};
Sampled plan_sample(const std::vector<Vec2>& poly, float step);

// Catmull-Rom spline through control points (open), sampled at n points.
std::vector<Vec2> spline(const std::vector<Vec2>& ctrl, int samples_per_segment);

}  // namespace cb
