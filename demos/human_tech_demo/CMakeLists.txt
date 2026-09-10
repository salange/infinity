# Standalone authored visual set, copied from cityblock with its own renderer.
# Shared cosmetic geometry is read from game::city; no game target depends on this demo.
add_executable(human_tech_demo
  src/main.cpp
  src/gpu.cpp
  src/ibl.cpp
  src/textures.cpp
  src/scene.cpp
  src/tower_floor_identity.cpp
  src/garden_staging.cpp
  src/garden_canopy.cpp
  src/garden_frame.cpp
  src/garden_guard.cpp
  src/market_staging.cpp
  src/market_structure.cpp
  src/market_canopy.cpp
  src/market_climbers.cpp
  src/landing_lounge.cpp
  src/landing_canopies.cpp
  src/roof_staging.cpp
  src/civic_forecourt.cpp
  src/street_forecourt.cpp
  src/transit_concourse.cpp
  src/civic_landmarks.cpp
  src/canal_construction.cpp
  src/asset_pipeline.cpp
  src/lookdev.cpp
  src/catalog.cpp
  src/renderer.cpp
  src/stb_impl.cpp)
target_compile_features(human_tech_demo PRIVATE cxx_std_20)
target_compile_definitions(human_tech_demo PRIVATE
  HUMAN_TECH_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
if(MSVC)
  target_compile_options(human_tech_demo PRIVATE /W4 /permissive-)
else()
  target_compile_options(human_tech_demo PRIVATE -Wall -Wextra)
endif()
# engine::render carries the Metal layer helper (rhi_metal.mm) the demo's
# surface creation shares with the game app.
target_link_libraries(human_tech_demo PRIVATE engine::core engine::render game::city wgpu::wgpu glfw stb::headers)
find_package(Threads REQUIRED)
target_link_libraries(human_tech_demo PRIVATE Threads::Threads)
add_custom_command(TARGET human_tech_demo POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          $<TARGET_PROPERTY:wgpu_native,IMPORTED_LOCATION>
          $<TARGET_FILE_DIR:human_tech_demo>
  VERBATIM)
if(NOT WIN32)
  set_target_properties(human_tech_demo PROPERTIES BUILD_RPATH "$ORIGIN;@loader_path")
endif()

add_executable(human_tech_scene_test tests/scene_test.cpp src/scene.cpp src/catalog.cpp src/asset_pipeline.cpp src/garden_staging.cpp src/garden_canopy.cpp src/garden_frame.cpp src/garden_guard.cpp src/market_staging.cpp src/market_structure.cpp src/market_canopy.cpp src/market_climbers.cpp src/landing_lounge.cpp src/landing_canopies.cpp src/roof_staging.cpp src/civic_forecourt.cpp src/street_forecourt.cpp src/transit_concourse.cpp src/civic_landmarks.cpp src/canal_construction.cpp)
target_include_directories(human_tech_scene_test PRIVATE src)
target_link_libraries(human_tech_scene_test PRIVATE game::city)
set(_human_tech_kit "${CMAKE_CURRENT_SOURCE_DIR}/assets/generated/human_tech_kit.htkit")
if(EXISTS "${_human_tech_kit}")
  add_test(NAME human_tech_scene COMMAND human_tech_scene_test "${_human_tech_kit}")
else()
  add_test(NAME human_tech_scene COMMAND human_tech_scene_test)
endif()

add_executable(human_tech_route_test tests/route_test.cpp src/scene.cpp src/catalog.cpp src/asset_pipeline.cpp src/garden_staging.cpp src/garden_canopy.cpp src/garden_frame.cpp src/garden_guard.cpp src/market_staging.cpp src/market_structure.cpp src/market_canopy.cpp src/market_climbers.cpp src/landing_lounge.cpp src/landing_canopies.cpp src/roof_staging.cpp src/civic_forecourt.cpp src/street_forecourt.cpp src/transit_concourse.cpp src/civic_landmarks.cpp src/canal_construction.cpp)
target_include_directories(human_tech_route_test PRIVATE src)
target_link_libraries(human_tech_route_test PRIVATE game::city)
if(EXISTS "${_human_tech_kit}")
  add_test(NAME human_tech_routes COMMAND human_tech_route_test "${_human_tech_kit}")
else()
  add_test(NAME human_tech_routes COMMAND human_tech_route_test)
endif()

# Roof placement checks use the same complete geometry modules as the scene.
get_target_property(_human_tech_roof_sources human_tech_scene_test SOURCES)
list(REMOVE_ITEM _human_tech_roof_sources tests/scene_test.cpp)
add_executable(human_tech_roof_test tests/roof_allocation_test.cpp ${_human_tech_roof_sources})
target_include_directories(human_tech_roof_test PRIVATE src)
target_link_libraries(human_tech_roof_test PRIVATE game::city)
if(EXISTS "${_human_tech_kit}")
  add_test(NAME human_tech_roof_allocation COMMAND human_tech_roof_test "${_human_tech_kit}")
  set_tests_properties(human_tech_roof_allocation PROPERTIES LABELS "authored-assets")
endif()

add_executable(human_tech_asset_test tests/asset_pipeline_test.cpp src/asset_pipeline.cpp)
target_include_directories(human_tech_asset_test PRIVATE src)
target_link_libraries(human_tech_asset_test PRIVATE game::city)
if(EXISTS "${_human_tech_kit}")
  add_test(NAME human_tech_asset_contract COMMAND human_tech_asset_test "${_human_tech_kit}")
  set_tests_properties(human_tech_asset_contract PROPERTIES LABELS "authored-assets")
endif()

add_executable(human_tech_renderer_memory_test tests/renderer_memory_test.cpp)
target_include_directories(human_tech_renderer_memory_test PRIVATE src)
target_link_libraries(human_tech_renderer_memory_test PRIVATE game::city)
add_test(NAME human_tech_renderer_memory COMMAND human_tech_renderer_memory_test)

add_executable(human_tech_renderer_lights_test tests/renderer_lights_test.cpp)
target_include_directories(human_tech_renderer_lights_test PRIVATE src)
target_link_libraries(human_tech_renderer_lights_test PRIVATE game::city)
add_test(NAME human_tech_renderer_lights COMMAND human_tech_renderer_lights_test)

add_executable(human_tech_point_visibility_test tests/point_visibility_test.cpp src/asset_pipeline.cpp)
target_include_directories(human_tech_point_visibility_test PRIVATE src)
target_link_libraries(human_tech_point_visibility_test PRIVATE game::city wgpu::wgpu)
add_test(NAME human_tech_point_visibility COMMAND human_tech_point_visibility_test)

add_executable(human_tech_voxel_coverage_test tests/voxel_coverage_test.cpp)
target_include_directories(human_tech_voxel_coverage_test PRIVATE src)
target_link_libraries(human_tech_voxel_coverage_test PRIVATE game::city)
add_test(NAME human_tech_voxel_coverage COMMAND human_tech_voxel_coverage_test)

add_executable(human_tech_window_floor_filter_test tests/window_floor_filter_test.cpp)
target_include_directories(human_tech_window_floor_filter_test PRIVATE src)
target_link_libraries(human_tech_window_floor_filter_test PRIVATE game::city)
add_test(NAME human_tech_window_floor_filter COMMAND human_tech_window_floor_filter_test)

foreach(_scene_target human_tech_scene_test human_tech_route_test human_tech_roof_test)
  target_sources(${_scene_target} PRIVATE src/tower_floor_identity.cpp)
endforeach()
add_executable(human_tech_tower_floor_identity_test tests/tower_floor_identity_test.cpp src/tower_floor_identity.cpp)
target_include_directories(human_tech_tower_floor_identity_test PRIVATE src)
target_link_libraries(human_tech_tower_floor_identity_test PRIVATE game::city)
add_test(NAME human_tech_tower_floor_identity COMMAND human_tech_tower_floor_identity_test)
