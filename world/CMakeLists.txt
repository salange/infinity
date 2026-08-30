find_package(Threads REQUIRED)

add_library(infinity_world STATIC
  src/world.cpp
  src/chunk_manager.cpp)
add_library(infinity::world ALIAS infinity_world)
target_include_directories(infinity_world PUBLIC include)
target_link_libraries(infinity_world
  PUBLIC infinity::gen Threads::Threads
  PRIVATE infinity_warnings)
