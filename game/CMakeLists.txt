# The game "Infinity": content generators, simulation, client and tools.
# Depends on the engine targets; the engine never depends on this.

add_library(game_warnings INTERFACE)
if(MSVC)
  target_compile_options(game_warnings INTERFACE /W4 /WX /permissive-)
else()
  target_compile_options(game_warnings INTERFACE -Wall -Wextra -Werror)
endif()

include(FetchContent)
FetchContent_Declare(stb
  GIT_REPOSITORY https://github.com/nothings/stb.git
  GIT_TAG 2c980bb59875b0d32144a71867fbdebb2f77cd20)
FetchContent_MakeAvailable(stb)
add_library(stb_headers INTERFACE)
target_include_directories(stb_headers INTERFACE "${stb_SOURCE_DIR}")
add_library(stb::headers ALIAS stb_headers)

add_subdirectory(gen)
add_subdirectory(tex)
add_subdirectory(sim)
add_subdirectory(cli)
if(INFINITY_BUILD_APP)
  add_subdirectory(app)
endif()
enable_testing()
add_subdirectory(tests)
