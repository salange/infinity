#include "mesh.hpp"

#include <algorithm>
#include <cmath>

namespace cb {

namespace {

Vec4 make_tangent(Vec3 e_u, Vec3 n) {
  Vec3 t = e_u - n * dot(e_u, n);
  t = normalize(t);
  return Vec4{t, 1.0f};
}

}  // namespace

void Emit::quad(Vec3 a, Vec3 b, Vec3 c, Vec3 d, const QuadUV& uv) {
  Vec3 n = normalize(cross(b - a, d - a));
  const Vec4 tan = make_tangent(b - a, n);
  auto vert = [&](Vec3 p, Vec2 t) {
    Vertex v;
    v.position = p;
    v.normal = n;
    v.tangent = tan;
    v.uv = t;
    v.material = material;
    if (facade) {
      const Vec3 r = p - facade_origin;
      v.aux = Vec4{dot(r, facade_u), dot(r, facade_v), element_random, occlusion};
    } else {
      v.aux = Vec4{t.x, t.y, element_random, occlusion};
    }
    return mesh->add_vertex(v);
  };
  const std::uint32_t ia = vert(a, uv.a), ib = vert(b, uv.b), ic = vert(c, uv.c), id = vert(d, uv.d);
  mesh->add_triangle(ia, ib, ic);
  mesh->add_triangle(ia, ic, id);
}

void Emit::quad_metric(Vec3 a, Vec3 b, Vec3 c, Vec3 d, Vec2 o) {
  const float w0 = length(b - a), w1 = length(c - d);
  const float h0 = length(d - a), h1 = length(c - b);
  quad(a, b, c, d, QuadUV{o, o + Vec2{w0, 0}, o + Vec2{w1, h1}, o + Vec2{0, h0}});
}

void Emit::triangle(Vec3 a, Vec3 b, Vec3 c) {
  const Vec3 e = b - a;
  const Vec3 n = normalize(cross(e, c - a));
  const Vec3 u = normalize(e);
  const Vec3 v = cross(n, u);
  triangle_uv(a, b, c, Vec2{0, 0}, Vec2{length(e), 0}, Vec2{dot(c - a, u), dot(c - a, v)});
}

void Emit::triangle_uv(Vec3 a, Vec3 b, Vec3 c, Vec2 ua, Vec2 ub, Vec2 uc) {
  const Vec3 n = normalize(cross(b - a, c - a));
  const Vec4 tan = make_tangent(b - a, n);
  auto vert = [&](Vec3 p, Vec2 t) {
    Vertex v;
    v.position = p;
    v.normal = n;
    v.tangent = tan;
    v.uv = t;
    v.material = material;
    if (facade) {
      const Vec3 r = p - facade_origin;
      v.aux = Vec4{dot(r, facade_u), dot(r, facade_v), element_random, occlusion};
    } else {
      v.aux = Vec4{t.x, t.y, element_random, occlusion};
    }
    return mesh->add_vertex(v);
  };
  const std::uint32_t ia = vert(a, ua), ib = vert(b, ub), ic = vert(c, uc);
  mesh->add_triangle(ia, ib, ic);
}

void Emit::box(Vec3 c, Vec3 h, Vec3 ax, Vec3 ay, Vec3 az) {
  const Vec3 X = ax * h.x, Y = ay * h.y, Z = az * h.z;
  // 8 corners
  const Vec3 p000 = c - X - Y - Z, p100 = c + X - Y - Z, p110 = c + X + Y - Z, p010 = c - X + Y - Z;
  const Vec3 p001 = c - X - Y + Z, p101 = c + X - Y + Z, p111 = c + X + Y + Z, p011 = c - X + Y + Z;
  quad_metric(p001, p101, p111, p011);  // +Z front
  quad_metric(p100, p000, p010, p110);  // -Z back
  quad_metric(p101, p100, p110, p111);  // +X
  quad_metric(p000, p001, p011, p010);  // -X
  quad_metric(p011, p111, p110, p010);  // +Y top
  quad_metric(p000, p100, p101, p001);  // -Y bottom
}

void Emit::beam(Vec3 p0, Vec3 p1, float w, float h, Vec3 up_hint) {
  const Vec3 d = p1 - p0;
  const float len = length(d);
  if (len < 1e-6f) return;
  const Vec3 az = d * (1.0f / len);
  Vec3 ax = cross(up_hint, az);
  if (length(ax) < 1e-4f) ax = cross(Vec3{1, 0, 0}, az);
  ax = normalize(ax);
  const Vec3 ay = cross(az, ax);
  box((p0 + p1) * 0.5f, Vec3{w * 0.5f, h * 0.5f, len * 0.5f}, ax, ay, az);
}

void Emit::tube(Vec3 p0, Vec3 p1, float radius, int sides, bool caps) {
  const Vec3 d = p1 - p0;
  const float len = length(d);
  if (len < 1e-6f || sides < 3) return;
  const Vec3 az = d * (1.0f / len);
  Vec3 t, b;
  basis(az, t, b);
  const std::uint32_t base = static_cast<std::uint32_t>(mesh->vertices.size());
  for (int i = 0; i <= sides; ++i) {
    const float a = static_cast<float>(i) / static_cast<float>(sides) * 2.0f * kPi;
    const Vec3 n = t * std::cos(a) + b * std::sin(a);
    for (int k = 0; k < 2; ++k) {
      Vertex v;
      v.position = (k == 0 ? p0 : p1) + n * radius;
      v.normal = n;
      v.tangent = Vec4{az, 1.0f};
      v.uv = Vec2{static_cast<float>(i) / static_cast<float>(sides) * 2.0f * kPi * radius, k == 0 ? 0.0f : len};
      v.material = material;
      v.aux = Vec4{v.uv.x, v.uv.y, element_random, occlusion};
      mesh->add_vertex(v);
    }
  }
  for (int i = 0; i < sides; ++i) {
    const std::uint32_t i0 = base + static_cast<std::uint32_t>(i) * 2;
    mesh->add_triangle(i0, i0 + 2, i0 + 3);
    mesh->add_triangle(i0, i0 + 3, i0 + 1);
  }
  if (caps) {
    for (int k = 0; k < 2; ++k) {
      const Vec3 c = k == 0 ? p0 : p1;
      const Vec3 n = k == 0 ? -az : az;
      const std::uint32_t cbase = static_cast<std::uint32_t>(mesh->vertices.size());
      Vertex vc;
      vc.position = c;
      vc.normal = n;
      vc.tangent = Vec4{t, 1.0f};
      vc.uv = Vec2{0, 0};
      vc.material = material;
      vc.aux = Vec4{0, 0, element_random, occlusion};
      mesh->add_vertex(vc);
      for (int i = 0; i <= sides; ++i) {
        const float a = static_cast<float>(i) / static_cast<float>(sides) * 2.0f * kPi;
        Vertex v = vc;
        v.position = c + (t * std::cos(a) + b * std::sin(a)) * radius;
        v.uv = Vec2{std::cos(a) * radius, std::sin(a) * radius};
        mesh->add_vertex(v);
      }
      for (int i = 0; i < sides; ++i) {
        if (k == 1) mesh->add_triangle(cbase, cbase + 1 + i, cbase + 2 + i);
        else mesh->add_triangle(cbase, cbase + 2 + i, cbase + 1 + i);
      }
    }
  }
}

void Emit::frustum(Vec3 p0, Vec3 p1, float r0, float r1, int sides, bool caps) {
  const Vec3 d = p1 - p0;
  const float len = length(d);
  if (len < 1e-6f) return;
  const Vec3 az = d * (1.0f / len);
  Vec3 t, b;
  basis(az, t, b);
  const float slope = (r0 - r1) / len;
  const std::uint32_t base = static_cast<std::uint32_t>(mesh->vertices.size());
  for (int i = 0; i <= sides; ++i) {
    const float a = static_cast<float>(i) / static_cast<float>(sides) * 2.0f * kPi;
    const Vec3 radial = t * std::cos(a) + b * std::sin(a);
    const Vec3 n = normalize(radial + az * slope);
    for (int k = 0; k < 2; ++k) {
      Vertex v;
      v.position = (k == 0 ? p0 + radial * r0 : p1 + radial * r1);
      v.normal = n;
      v.tangent = Vec4{az, 1.0f};
      v.uv = Vec2{static_cast<float>(i) / static_cast<float>(sides) * 2.0f * kPi * (r0 + r1) * 0.5f, k == 0 ? 0.0f : len};
      v.material = material;
      v.aux = Vec4{v.uv.x, v.uv.y, element_random, occlusion};
      mesh->add_vertex(v);
    }
  }
  for (int i = 0; i < sides; ++i) {
    const std::uint32_t i0 = base + static_cast<std::uint32_t>(i) * 2;
    mesh->add_triangle(i0, i0 + 2, i0 + 3);
    mesh->add_triangle(i0, i0 + 3, i0 + 1);
  }
  if (caps) {
    std::vector<Vec2> ring;
    (void)ring;
    for (int k = 0; k < 2; ++k) {
      const float r = k == 0 ? r0 : r1;
      if (r <= 0.0f) continue;
      const Vec3 c = k == 0 ? p0 : p1;
      const Vec3 n = k == 0 ? -az : az;
      const std::uint32_t cbase = static_cast<std::uint32_t>(mesh->vertices.size());
      Vertex vc;
      vc.position = c;
      vc.normal = n;
      vc.tangent = Vec4{t, 1.0f};
      vc.material = material;
      vc.aux = Vec4{0, 0, element_random, occlusion};
      mesh->add_vertex(vc);
      for (int i = 0; i <= sides; ++i) {
        const float a = static_cast<float>(i) / static_cast<float>(sides) * 2.0f * kPi;
        Vertex v = vc;
        v.position = c + (t * std::cos(a) + b * std::sin(a)) * r;
        v.uv = Vec2{std::cos(a) * r, std::sin(a) * r};
        mesh->add_vertex(v);
      }
      for (int i = 0; i < sides; ++i) {
        if (k == 1) mesh->add_triangle(cbase, cbase + 1 + i, cbase + 2 + i);
        else mesh->add_triangle(cbase, cbase + 2 + i, cbase + 1 + i);
      }
    }
  }
}

void Emit::sphere(Vec3 c, float radius, int rings, int sides) {
  const std::uint32_t base = static_cast<std::uint32_t>(mesh->vertices.size());
  for (int r = 0; r <= rings; ++r) {
    const float phi = static_cast<float>(r) / static_cast<float>(rings) * kPi;
    for (int s = 0; s <= sides; ++s) {
      const float th = static_cast<float>(s) / static_cast<float>(sides) * 2.0f * kPi;
      const Vec3 n{std::sin(phi) * std::cos(th), std::cos(phi), std::sin(phi) * std::sin(th)};
      Vertex v;
      v.position = c + n * radius;
      v.normal = n;
      v.tangent = Vec4{normalize(Vec3{-std::sin(th), 0, std::cos(th)}), 1.0f};
      v.uv = Vec2{th * radius, phi * radius};
      v.material = material;
      v.aux = Vec4{v.uv.x, v.uv.y, element_random, occlusion};
      mesh->add_vertex(v);
    }
  }
  const std::uint32_t stride = static_cast<std::uint32_t>(sides + 1);
  for (int r = 0; r < rings; ++r) {
    for (int s = 0; s < sides; ++s) {
      const std::uint32_t i0 = base + static_cast<std::uint32_t>(r) * stride + static_cast<std::uint32_t>(s);
      mesh->add_triangle(i0, i0 + 1, i0 + stride + 1);
      mesh->add_triangle(i0, i0 + stride + 1, i0 + stride);
    }
  }
}

void Emit::polygon(const std::vector<Vec2>& xz, float y, bool up, Vec2 uv_scale) {
  if (xz.size() < 3) return;
  const std::vector<std::uint32_t> tris = triangulate(xz);
  const Vec3 n = up ? Vec3{0, 1, 0} : Vec3{0, -1, 0};
  const std::uint32_t base = static_cast<std::uint32_t>(mesh->vertices.size());
  for (const Vec2& p : xz) {
    Vertex v;
    v.position = Vec3{p.x, y, p.y};
    v.normal = n;
    v.tangent = Vec4{1, 0, 0, 1};
    v.uv = Vec2{p.x * uv_scale.x, p.y * uv_scale.y};
    v.material = material;
    v.aux = Vec4{v.uv.x, v.uv.y, element_random, occlusion};
    mesh->add_vertex(v);
  }
  for (std::size_t i = 0; i + 2 < tris.size(); i += 3) {
    if (up) mesh->add_triangle(base + tris[i], base + tris[i + 2], base + tris[i + 1]);
    else mesh->add_triangle(base + tris[i], base + tris[i + 1], base + tris[i + 2]);
  }
}

void Emit::wall(const std::vector<Vec2>& xz, float y0, float y1, bool closed, bool outward) {
  const std::size_t n = xz.size();
  if (n < 2) return;
  float u = 0.0f;
  const std::size_t count = closed ? n : n - 1;
  for (std::size_t i = 0; i < count; ++i) {
    const Vec2 p = xz[i], q = xz[(i + 1) % n];
    const Vec3 a{p.x, y0, p.y}, b{q.x, y0, q.y}, c{q.x, y1, q.y}, d{p.x, y1, p.y};
    const float w = length(q - p);
    // ccw polygon seen from above: outward normal is to the right of the
    // travel direction (p->q), which cross(b-a, d-a) gives for (a,b,c,d)
    // ordered a=p b=q ... wait: cross((q-p), up) points right of travel.
    if (outward) quad(a, b, c, d, QuadUV{{u, 0}, {u + w, 0}, {u + w, y1 - y0}, {u, y1 - y0}});
    else quad(b, a, d, c, QuadUV{{u + w, 0}, {u, 0}, {u, y1 - y0}, {u + w, y1 - y0}});
    u += w;
  }
}

// ---- plans -----------------------------------------------------------------

std::vector<Vec2> plan_rect(float hx, float hz, Vec2 c) {
  return {{c.x - hx, c.y - hz}, {c.x + hx, c.y - hz}, {c.x + hx, c.y + hz}, {c.x - hx, c.y + hz}};
}

std::vector<Vec2> plan_rounded_rect(float hx, float hz, float r, int seg, Vec2 c) {
  std::vector<Vec2> out;
  r = std::min(r, std::min(hx, hz));
  const Vec2 corners[4] = {{hx - r, hz - r}, {-(hx - r), hz - r}, {-(hx - r), -(hz - r)}, {hx - r, -(hz - r)}};
  for (int k = 0; k < 4; ++k) {
    for (int i = 0; i <= seg; ++i) {
      const float a = (static_cast<float>(k) + static_cast<float>(i) / static_cast<float>(seg)) * kPi * 0.5f;
      out.push_back(Vec2{c.x + corners[k].x + r * std::cos(a), c.y + corners[k].y + r * std::sin(a)});
    }
  }
  return out;
}

std::vector<Vec2> plan_superellipse(float a, float b, float n, int segments, Vec2 c, float rot) {
  std::vector<Vec2> out;
  out.reserve(static_cast<std::size_t>(segments));
  const float cr = std::cos(rot), sr = std::sin(rot);
  for (int i = 0; i < segments; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(segments) * 2.0f * kPi;
    const float ct = std::cos(t), st = std::sin(t);
    const float x = a * (ct < 0 ? -1.0f : 1.0f) * std::pow(std::fabs(ct), 2.0f / n);
    const float y = b * (st < 0 ? -1.0f : 1.0f) * std::pow(std::fabs(st), 2.0f / n);
    out.push_back(Vec2{c.x + x * cr - y * sr, c.y + x * sr + y * cr});
  }
  return out;
}

std::vector<Vec2> plan_circle(float r, int segments, Vec2 c) {
  return plan_superellipse(r, r, 2.0f, segments, c, 0.0f);
}

std::vector<Vec2> plan_lens(float r, float d, int segments, Vec2 c, float rot) {
  // Circles centred at (0, +d) and (0, -d); the lens is |x| <= sqrt(r^2-d^2).
  std::vector<Vec2> out;
  const float half = std::acos(d / r);  // half-angle of each arc
  const float cr = std::cos(rot), sr = std::sin(rot);
  auto push = [&](float x, float y) { out.push_back(Vec2{c.x + x * cr - y * sr, c.y + x * sr + y * cr}); };
  // lower arc (from circle centred at (0, d)), angles from -pi/2-half to -pi/2+half
  for (int i = 0; i < segments; ++i) {
    const float a = -kPi * 0.5f - half + 2.0f * half * static_cast<float>(i) / static_cast<float>(segments);
    push(r * std::cos(a), d + r * std::sin(a));
  }
  for (int i = 0; i < segments; ++i) {
    const float a = kPi * 0.5f - half + 2.0f * half * static_cast<float>(i) / static_cast<float>(segments);
    push(r * std::cos(a), -d + r * std::sin(a));
  }
  return out;
}

std::vector<Vec2> plan_offset(const std::vector<Vec2>& poly, float dist) {
  const std::size_t n = poly.size();
  std::vector<Vec2> out(n);
  for (std::size_t i = 0; i < n; ++i) {
    const Vec2 prev = poly[(i + n - 1) % n], cur = poly[i], next = poly[(i + 1) % n];
    const Vec2 d0 = normalize(cur - prev), d1 = normalize(next - cur);
    const Vec2 n0{d0.y, -d0.x}, n1{d1.y, -d1.x};  // outward for ccw
    Vec2 bis = n0 + n1;
    const float l = length(bis);
    if (l < 1e-5f) {
      out[i] = cur + n1 * dist;
    } else {
      bis = bis * (1.0f / l);
      const float cos_half = dot(bis, n1);
      out[i] = cur + bis * (dist / std::max(cos_half, 0.3f));
    }
  }
  return out;
}

std::vector<Vec2> plan_scale(const std::vector<Vec2>& poly, float s, Vec2 about) {
  std::vector<Vec2> out(poly.size());
  for (std::size_t i = 0; i < poly.size(); ++i) out[i] = about + (poly[i] - about) * s;
  return out;
}

std::vector<Vec2> plan_transform(const std::vector<Vec2>& poly, Vec2 offset, float rot) {
  std::vector<Vec2> out(poly.size());
  const float c = std::cos(rot), s = std::sin(rot);
  for (std::size_t i = 0; i < poly.size(); ++i) {
    out[i] = Vec2{offset.x + poly[i].x * c - poly[i].y * s, offset.y + poly[i].x * s + poly[i].y * c};
  }
  return out;
}

float plan_area(const std::vector<Vec2>& poly) {
  float a = 0.0f;
  for (std::size_t i = 0; i < poly.size(); ++i) {
    const Vec2 p = poly[i], q = poly[(i + 1) % poly.size()];
    a += p.x * q.y - q.x * p.y;
  }
  return 0.5f * a;
}

float plan_perimeter(const std::vector<Vec2>& poly) {
  float l = 0.0f;
  for (std::size_t i = 0; i < poly.size(); ++i) l += length(poly[(i + 1) % poly.size()] - poly[i]);
  return l;
}

bool point_in_polygon(const std::vector<Vec2>& poly, Vec2 p) {
  bool inside = false;
  const std::size_t n = poly.size();
  for (std::size_t i = 0, j = n - 1; i < n; j = i++) {
    const Vec2 a = poly[i], b = poly[j];
    if ((a.y > p.y) != (b.y > p.y)) {
      const float x = (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x;
      if (p.x < x) inside = !inside;
    }
  }
  return inside;
}

std::vector<std::uint32_t> triangulate(const std::vector<Vec2>& poly_in) {
  std::vector<std::uint32_t> out;
  std::vector<std::uint32_t> idx(poly_in.size());
  for (std::size_t i = 0; i < idx.size(); ++i) idx[i] = static_cast<std::uint32_t>(i);
  const bool ccw = plan_area(poly_in) > 0.0f;
  if (!ccw) std::reverse(idx.begin(), idx.end());
  auto cross2 = [](Vec2 a, Vec2 b, Vec2 c) { return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x); };
  int guard = 0;
  while (idx.size() > 3 && guard < 100000) {
    ++guard;
    bool clipped = false;
    const std::size_t n = idx.size();
    for (std::size_t i = 0; i < n; ++i) {
      const std::uint32_t ia = idx[(i + n - 1) % n], ib = idx[i], ic = idx[(i + 1) % n];
      const Vec2 a = poly_in[ia], b = poly_in[ib], c = poly_in[ic];
      if (cross2(a, b, c) <= 1e-9f) continue;  // reflex or degenerate
      bool contains = false;
      for (std::size_t k = 0; k < n; ++k) {
        const std::uint32_t ik = idx[k];
        if (ik == ia || ik == ib || ik == ic) continue;
        const Vec2 p = poly_in[ik];
        if (cross2(a, b, p) >= 0 && cross2(b, c, p) >= 0 && cross2(c, a, p) >= 0) {
          contains = true;
          break;
        }
      }
      if (contains) continue;
      out.push_back(ia);
      out.push_back(ib);
      out.push_back(ic);
      idx.erase(idx.begin() + static_cast<std::ptrdiff_t>(i));
      clipped = true;
      break;
    }
    if (!clipped) {
      // Degenerate remainder: fan it.
      for (std::size_t i = 1; i + 1 < idx.size(); ++i) {
        out.push_back(idx[0]);
        out.push_back(idx[i]);
        out.push_back(idx[i + 1]);
      }
      idx.clear();
      break;
    }
  }
  if (idx.size() == 3) {
    out.push_back(idx[0]);
    out.push_back(idx[1]);
    out.push_back(idx[2]);
  }
  return out;
}

Sampled plan_sample(const std::vector<Vec2>& poly, float step) {
  Sampled s;
  const std::size_t n = poly.size();
  float acc = 0.0f;
  for (std::size_t i = 0; i < n; ++i) {
    const Vec2 p = poly[i], q = poly[(i + 1) % n];
    const float len = length(q - p);
    const int div = std::max(1, static_cast<int>(std::round(len / step)));
    const Vec2 d = normalize(q - p);
    const Vec2 nrm{d.y, -d.x};
    for (int k = 0; k < div; ++k) {
      const float t = static_cast<float>(k) / static_cast<float>(div);
      s.points.push_back(p + (q - p) * t);
      s.arclen.push_back(acc + len * t);
      s.normals.push_back(nrm);
    }
    acc += len;
  }
  return s;
}

std::vector<Vec2> spline(const std::vector<Vec2>& ctrl, int per_seg) {
  std::vector<Vec2> out;
  if (ctrl.size() < 2) return ctrl;
  const int n = static_cast<int>(ctrl.size());
  auto at = [&](int i) { return ctrl[static_cast<std::size_t>(std::clamp(i, 0, n - 1))]; };
  for (int i = 0; i < n - 1; ++i) {
    const Vec2 p0 = at(i - 1), p1 = at(i), p2 = at(i + 1), p3 = at(i + 2);
    for (int k = 0; k < per_seg; ++k) {
      const float t = static_cast<float>(k) / static_cast<float>(per_seg);
      const float t2 = t * t, t3 = t2 * t;
      const Vec2 v = (p1 * 2.0f + (p2 - p0) * t + (p0 * 2.0f - p1 * 5.0f + p2 * 4.0f - p3) * t2 +
                      (p1 * 3.0f - p0 - p2 * 3.0f + p3) * t3) * 0.5f;
      out.push_back(v);
    }
  }
  out.push_back(ctrl.back());
  return out;
}

}  // namespace cb
