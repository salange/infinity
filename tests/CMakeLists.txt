add_executable(infinity_tests
  main.cpp
  test_seed.cpp
  test_philox.cpp
  test_key.cpp
  test_fixed64.cpp
  test_mix.cpp
  test_names.cpp
  test_planet.cpp
  test_provinces.cpp
  test_terrain.cpp
  test_world.cpp
  test_player.cpp)
target_include_directories(infinity_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(infinity_tests
  PRIVATE infinity::core infinity::gen infinity::world infinity::sim doctest::doctest
          infinity_warnings)
include(${doctest_SOURCE_DIR}/scripts/cmake/doctest.cmake)
doctest_discover_tests(infinity_tests)
