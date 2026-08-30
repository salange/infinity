# Headless target: must never link a window/GPU dependency (checked in ci).
add_executable(infinity_cli
  src/main.cpp)
set_target_properties(infinity_cli PROPERTIES OUTPUT_NAME "infinity-cli")
target_link_libraries(infinity_cli
  PRIVATE infinity::world infinity_warnings)
