# The city system (T0021): parametric architecture, ground kit and flora
# generators shared by the game and the cityblock demo, plus the bridge
# from sites/v1 lots to city scenes. Structured per race and faction:
# include/city/<race>/<faction>/ holds one architecture each; the
# registry in architecture.hpp picks it from a StyleVector.
add_library(game_city STATIC
  src/geometry.cpp
  src/scene.cpp
  src/materials.cpp
  src/flora.cpp
  src/towers.cpp
  src/standards.cpp
  src/props.cpp
  src/showcase.cpp
  src/architecture.cpp
  src/site_build.cpp
  src/human/tech/architecture.cpp)
add_library(game::city ALIAS game_city)
target_include_directories(game_city PUBLIC include)
target_compile_features(game_city PUBLIC cxx_std_20)
target_link_libraries(game_city
  PUBLIC engine::core game::gen
  PRIVATE game_warnings)
