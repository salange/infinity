#include "city/architecture.hpp"

#include "city/human/tech/architecture.hpp"

namespace inf::city {

const Architecture& architecture_for(const gen::StyleVector& style) {
  // One entry per (race, faction) as they get their own architectures;
  // everything else is the human tech placeholder.
  switch (style.race_type) {
    case gen::RaceType::Humanoid:
    default:
      return human_tech::instance();
  }
}

}  // namespace inf::city
