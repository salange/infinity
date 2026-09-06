#include "city/showcase.hpp"

#include <cmath>

#include "city/flora.hpp"
#include "city/materials.hpp"
#include "city/props.hpp"
#include "city/standards.hpp"
#include "city/towers.hpp"

namespace inf::city {

void generate_showcase_small(Scene& sc, Rng root) {
  Mesh& mesh = sc.opaque;
  // Ground: a plaza with a sidewalk rim.
  {
    Emit plaza(&mesh, M_PLAZA);
    plaza.polygon(plan_rect(130.0f, 110.0f), 0.0f, true);
    Emit walk(&mesh, M_SIDEWALK);
    walk.polygon(plan_rect(140.0f, 120.0f), -0.02f, true);
  }
  // Towers: the three most distinct families along the back.
  {
    Rng r = root.child(1);
    build_tower(sc, spec_diagrid(15.0f, 32), Vec2{-70.0f, -60.0f}, 0.0f, r.child(1), 2);
    build_tower(sc, spec_lens(18.0f, 7.0f, 28, 0.4f), Vec2{0.0f, -70.0f}, 0.0f, r.child(2), 2);
    build_tower(sc, spec_finweave(13.0f, 26), Vec2{70.0f, -60.0f}, 0.0f, r.child(3), 2);
  }
  // Standard buildings along the sides.
  {
    Rng r = root.child(2);
    const StdType types[3] = {StdType::Office, StdType::Residential, StdType::Mixed};
    for (int i = 0; i < 3; ++i) {
      StandardSpec s = random_standard(r, 700.0f, 0.4f);
      s.type = types[i];
      const float x = -95.0f + 95.0f * static_cast<float>(i);
      build_standard(sc, s, plan_rect(14.0f, 11.0f, Vec2{x, 55.0f}), 0.0f, r.child(10 + i), 2);
    }
  }
  // Civic centre: the government building and the unification ring.
  {
    Rng r = root.child(3);
    build_government(sc, Vec2{0.0f, -10.0f}, 0.0f, 22.0f, 0.0f, r, 2);
    build_unification_ring(sc, Vec2{0.0f, 40.0f}, 0.0f, 9.0f, 0.0f, 2);
    build_fountain(sc, Vec2{-60.0f, 20.0f}, 6.0f, 0.0f, r, 2);
    build_monument(sc, MonumentKind::Obelisk, Vec2{60.0f, 20.0f}, 0.0f, 1.0f, r, 2);
    build_hedge_ring(sc, plan_rect(120.0f, 100.0f), 3.0f, 0.8f, 1.1f, 0.0f, 24.0f, r);
  }
  // Trees and lamps around the plaza.
  {
    Rng r = root.child(4);
    for (int i = 0; i < 10; ++i) {
      const float a = static_cast<float>(i) / 10.0f * 2.0f * kPi;
      const Vec2 p{std::cos(a) * 95.0f, std::sin(a) * 78.0f};
      gen_tree(sc, r.child(20 + i), P3(p, 0.0f), r.range(7.0f, 11.0f));
    }
    for (int i = 0; i < 8; ++i) {
      const float a = static_cast<float>(i) / 8.0f * 2.0f * kPi + 0.2f;
      const Vec2 p{std::cos(a) * 110.0f, std::sin(a) * 92.0f};
      gen_lamp(sc, P3(p, 0.0f), a + kPi);
    }
  }
  sc.finalize_draws();
}

}  // namespace inf::city
