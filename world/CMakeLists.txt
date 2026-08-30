add_library(infinity_world STATIC
  src/world.cpp)
add_library(infinity::world ALIAS infinity_world)
target_include_directories(infinity_world PUBLIC include)
target_link_libraries(infinity_world
  PUBLIC infinity::gen
  PRIVATE infinity_warnings)
