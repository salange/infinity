#pragma once
// City layout: a jittered street grid with arteries, secondary roads and
// alleys, one or two diagonal arteries cut through the blocks, districts by
// distance from the government centre, plazas, towers gated by city size,
// standard buildings on the remaining lots, overpasses and props.
#include <cstdint>
#include <string>

#include "rng.hpp"
#include "scene.hpp"

namespace cb {

enum class CitySize : std::uint8_t { Small, Medium, Large, Metropolis };
const char* to_string(CitySize s);
CitySize city_size_for(Rng& rng);
bool parse_city_size(const std::string& text, CitySize* out);

struct CityStats {
  int blocks{0}, lots{0}, towers{0}, standards{0}, plazas{0}, trees{0}, overpasses{0};
  float radius{0.0f};
};

CityStats generate_city(Scene& sc, Rng root, CitySize size);

}  // namespace cb
