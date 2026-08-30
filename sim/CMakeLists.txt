add_library(infinity_sim STATIC
  src/sim.cpp)
add_library(infinity::sim ALIAS infinity_sim)
target_include_directories(infinity_sim PUBLIC include)
target_link_libraries(infinity_sim
  PUBLIC infinity::world
  PRIVATE infinity_warnings)
