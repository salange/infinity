find_package(Threads REQUIRED)

add_library(engine_world STATIC
  src/cubesphere.cpp
  src/noise.cpp
  src/chunk_grid.cpp
  src/mesher.cpp
  src/chunk_manager.cpp
  src/edit_store.cpp)
add_library(engine::world ALIAS engine_world)
target_include_directories(engine_world PUBLIC include)
target_link_libraries(engine_world
  PUBLIC engine::core Threads::Threads
  PRIVATE engine_warnings)
