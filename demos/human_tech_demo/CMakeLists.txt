# Standalone authored visual set, copied from cityblock with its own renderer.
# Shared cosmetic geometry is read from game::city; no game target depends on this demo.
add_executable(human_tech_demo
  src/main.cpp
  src/gpu.cpp
  src/ibl.cpp
  src/textures.cpp
  src/scene.cpp
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

add_executable(human_tech_scene_test tests/scene_test.cpp src/scene.cpp src/catalog.cpp)
target_include_directories(human_tech_scene_test PRIVATE src)
target_link_libraries(human_tech_scene_test PRIVATE game::city)
add_test(NAME human_tech_scene COMMAND human_tech_scene_test)
