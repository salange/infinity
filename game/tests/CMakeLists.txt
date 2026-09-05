add_executable(game_tests
  main.cpp
  test_galaxy.cpp
  test_names.cpp
  test_planet.cpp
  test_provinces.cpp
  test_terrain.cpp
  test_world.cpp
  test_seam.cpp
  test_system.cpp
  test_player.cpp
  test_edits.cpp
  test_map.cpp
  test_surface.cpp)
target_include_directories(game_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(game_tests
  PRIVATE game::gen game::sim game::tex doctest::doctest game_warnings)

include(${doctest_SOURCE_DIR}/scripts/cmake/doctest.cmake)
doctest_discover_tests(game_tests)
