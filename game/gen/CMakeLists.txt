execute_process(
  COMMAND git rev-parse --short HEAD
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  OUTPUT_VARIABLE INFINITY_GIT_HASH
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_QUIET)
if(NOT INFINITY_GIT_HASH)
  set(INFINITY_GIT_HASH "unknown")
endif()
configure_file(version.hpp.in ${CMAKE_CURRENT_BINARY_DIR}/generated/gen/version.hpp @ONLY)

add_library(game_gen STATIC
  src/caves.cpp
  src/features.cpp
  src/macro.cpp
  src/material.cpp
  src/planet.cpp
  src/provinces.cpp
  src/terrain.cpp
  src/effective_field.cpp
  src/universe.cpp
  src/system.cpp
  src/golden_core.cpp
  src/golden_planet.cpp
  src/golden_edits.cpp)
add_library(game::gen ALIAS game_gen)
target_include_directories(game_gen PUBLIC
  include
  ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(game_gen
  PUBLIC engine::world
  PRIVATE game_warnings)
