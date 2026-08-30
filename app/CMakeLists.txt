add_executable(infinity_app
  src/main.cpp)
set_target_properties(infinity_app PROPERTIES OUTPUT_NAME "infinity")
target_link_libraries(infinity_app
  PRIVATE infinity::render infinity::sim glfw infinity_warnings)

# Make the wgpu shared library findable next to the binary at runtime.
add_custom_command(TARGET infinity_app POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          $<TARGET_PROPERTY:wgpu_native,IMPORTED_LOCATION>
          $<TARGET_FILE_DIR:infinity_app>
  VERBATIM)
if(NOT WIN32)
  set_target_properties(infinity_app PROPERTIES BUILD_RPATH "$ORIGIN;@loader_path")
endif()
