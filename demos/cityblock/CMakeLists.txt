# cityblock — a standalone generator + renderer for one city block of the
# human tech faction. Own PBR renderer on wgpu (HDRI lighting, cascaded
# shadows, SSAO, interior-mapped glass, bloom/ACES/FXAA); own grammar.
# Shares only the engine's deterministic key/RNG substrate.
add_executable(cityblock
  src/main.cpp
  src/gpu.cpp
  src/ibl.cpp
  src/textures.cpp
  src/geometry.cpp
  src/scene.cpp
  src/towers.cpp
  src/props.cpp
  src/standards.cpp
  src/city.cpp
  src/renderer.cpp
  src/stb_impl.cpp)
target_compile_features(cityblock PRIVATE cxx_std_20)
target_compile_definitions(cityblock PRIVATE
  CITYBLOCK_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
if(MSVC)
  target_compile_options(cityblock PRIVATE /W4 /permissive-)
else()
  target_compile_options(cityblock PRIVATE -Wall -Wextra)
endif()
target_link_libraries(cityblock PRIVATE engine::core wgpu::wgpu glfw stb::headers)
find_package(Threads REQUIRED)
target_link_libraries(cityblock PRIVATE Threads::Threads)
add_custom_command(TARGET cityblock POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          $<TARGET_PROPERTY:wgpu_native,IMPORTED_LOCATION>
          $<TARGET_FILE_DIR:cityblock>
  VERBATIM)
if(NOT WIN32)
  set_target_properties(cityblock PROPERTIES BUILD_RPATH "$ORIGIN;@loader_path")
endif()
