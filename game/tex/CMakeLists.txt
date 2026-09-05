add_library(game_tex STATIC
  src/tiles.cpp)
add_library(game::tex ALIAS game_tex)
target_include_directories(game_tex PUBLIC include)
target_link_libraries(game_tex PRIVATE game_warnings)
