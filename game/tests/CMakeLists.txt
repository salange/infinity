add_executable(game_tests
  main.cpp
  test_galaxy.cpp
  test_galaxy_route.cpp
  ../app/src/galaxy_flythrough.cpp
  ../app/src/stellar_stream.cpp
  ../app/src/deep_sky_render.cpp
  ../app/src/distant_catalog.cpp
  test_names.cpp
  test_planet.cpp
  test_provinces.cpp
  test_terrain.cpp
  test_world.cpp
  test_seam.cpp
  test_system.cpp
  test_player.cpp
  test_hyperdrive.cpp
  test_debug_overlay.cpp
  test_edits.cpp
  test_map.cpp
  test_surface.cpp
  test_civ.cpp
  test_city.cpp)
target_include_directories(game_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/engine/render/include)
target_link_libraries(game_tests
  PRIVATE game::gen game::sim game::tex game::city doctest::doctest game_warnings)

include(${doctest_SOURCE_DIR}/scripts/cmake/doctest.cmake)
doctest_discover_tests(game_tests)
