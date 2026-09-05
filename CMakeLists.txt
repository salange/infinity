cmake_minimum_required(VERSION 3.24)
project(infinity VERSION 0.0.1 LANGUAGES C CXX)

# Superbuild: the self-contained ENGINE (framework/infrastructure — later
# its own repo/submodule) and the GAME "Infinity" (one instance using it).
# The game depends on the engine, never the reverse.

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(INFINITY_BUILD_APP "Build window/GPU targets (engine-render, game app)" ON)
option(INFINITY_BUILD_DEMOS "Build the standalone demos (demos/)" ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

enable_testing()
add_subdirectory(engine)
add_subdirectory(game)
if(INFINITY_BUILD_APP AND INFINITY_BUILD_DEMOS)
  add_subdirectory(demos)
endif()
