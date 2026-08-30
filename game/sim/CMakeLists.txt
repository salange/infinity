add_library(game_sim STATIC
  src/player.cpp)
add_library(game::sim ALIAS game_sim)
target_include_directories(game_sim PUBLIC include)
target_link_libraries(game_sim
  PUBLIC game::gen
  PRIVATE game_warnings)
