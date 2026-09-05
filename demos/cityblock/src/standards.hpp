#pragma once
// Standard buildings: the 3–6 storey fabric of the city, designed to be
// cheap (one wall band and one glass strip per storey per edge, no mullion
// boxes) while sharing the towers' architecture and the entrance/roof kit.
#include <cstdint>
#include <vector>

#include "materials.hpp"
#include "props.hpp"
#include "rng.hpp"
#include "scene.hpp"

namespace cb {

enum class StdType : std::uint8_t { Office, Residential, Mixed, Civic, Lab };

struct StandardSpec {
  StdType type{StdType::Office};
  int storeys{4};
  float floor_h{3.6f};
  EntranceKind entrance{EntranceKind::Canopy};
  RoofKind roof{RoofKind::Parapet};
  Mat wall{M_WALL_LIGHT};
  Mat glass{M_GLASS_STD};
  float glass_lo{0.9f};   // sill height of the glass strip
  float glass_hi{0.4f};   // lintel height above the strip
  bool balconies{false};
  bool retail_ground{false};
  bool pilasters{true};
  float random{0.0f};
};

StandardSpec random_standard(Rng& rng, float footprint_area, float city_t /* 0 centre … 1 edge */);
void build_standard(Scene& sc, const StandardSpec& spec, const std::vector<Vec2>& footprint, float y, Rng rng, int detail);

}  // namespace cb
