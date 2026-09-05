add_executable(game_app
  src/main.cpp
  src/hud.cpp
  src/deep_sky_render.cpp
  src/material_library.cpp
  src/stb_impl.cpp)
target_compile_definitions(game_app PRIVATE
  INFINITY_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
set_target_properties(game_app PROPERTIES OUTPUT_NAME "infinity")
target_link_libraries(game_app
  PRIVATE engine::render game::sim game::tex glfw stb::headers game_warnings)

add_custom_command(TARGET game_app POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          $<TARGET_PROPERTY:wgpu_native,IMPORTED_LOCATION>
          $<TARGET_FILE_DIR:game_app>
  VERBATIM)
if(NOT WIN32)
  set_target_properties(game_app PROPERTIES BUILD_RPATH "$ORIGIN;@loader_path")
endif()
