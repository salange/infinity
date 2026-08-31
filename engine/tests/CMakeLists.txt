add_executable(engine_tests
  main.cpp
  test_seed.cpp
  test_philox.cpp
  test_fixed64.cpp
  test_mix.cpp
  test_key.cpp
  test_trig.cpp
  test_time.cpp
  test_ephemeris.cpp
  test_tree.cpp
  test_noise.cpp)
target_link_libraries(engine_tests
  PRIVATE engine::core engine::world doctest::doctest engine_warnings)

include(${doctest_SOURCE_DIR}/scripts/cmake/doctest.cmake)
doctest_discover_tests(engine_tests)
