# cityblock — the standalone test bench for the city system: its own PBR
# renderer on wgpu (HDRI lighting, cascaded shadows, SSAO, interior-mapped
# glass, TAA, bloom/ACES/FXAA) over the shared generators in game/city
# (towers, standard buildings, ground kit, flora) and the engine's key/RNG
# substrate. The game adopts the renderer features through its RHI.
add_executable(cityblock
  src/main.cpp
  src/gpu.cpp
  src/ibl.cpp
  src/textures.cpp
  src/scene.cpp
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
# engine::render carries the Metal layer helper (rhi_metal.mm) the demo's
# surface creation shares with the game app.
target_link_libraries(cityblock PRIVATE engine::core engine::render game::city wgpu::wgpu glfw stb::headers)
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
