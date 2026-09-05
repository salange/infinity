# Headless target: must never link a window/GPU dependency (checked in ci).
add_executable(game_cli
  src/main.cpp
  src/commands_planet.cpp
  src/commands_system.cpp
  src/commands_civ.cpp
  src/stb_impl.cpp)
set_target_properties(game_cli PROPERTIES OUTPUT_NAME "infinity-cli")
target_link_libraries(game_cli
  PRIVATE game::gen game::tex stb::headers game_warnings)
