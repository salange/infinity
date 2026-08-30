add_library(infinity_gen STATIC
  src/gen.cpp)
add_library(infinity::gen ALIAS infinity_gen)
target_include_directories(infinity_gen PUBLIC include)
target_link_libraries(infinity_gen
  PUBLIC infinity::core
  PRIVATE infinity_warnings)
