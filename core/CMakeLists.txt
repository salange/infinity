configure_file(version.hpp.in ${CMAKE_CURRENT_BINARY_DIR}/generated/core/version.hpp @ONLY)

add_library(infinity_core STATIC
  src/seed.cpp)
add_library(infinity::core ALIAS infinity_core)
target_include_directories(infinity_core PUBLIC
  include
  ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(infinity_core
  PRIVATE infinity_warnings
  PUBLIC infinity_det_flags)
