add_library(engine_core STATIC
  src/seed.cpp
  src/philox.cpp
  src/key.cpp
  src/fixed64.cpp
  src/trig.cpp
  src/world_clock.cpp
  src/ephemeris.cpp
  src/tree.cpp)
add_library(engine::core ALIAS engine_core)
target_include_directories(engine_core PUBLIC include)
target_link_libraries(engine_core
  PRIVATE engine_warnings
  PUBLIC engine_det_flags)
