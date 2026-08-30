add_executable(infinity_tests
  test_seed.cpp)
target_link_libraries(infinity_tests
  PRIVATE infinity::core doctest::doctest infinity_warnings)
target_compile_definitions(infinity_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)

include(${doctest_SOURCE_DIR}/scripts/cmake/doctest.cmake)
doctest_discover_tests(infinity_tests)
