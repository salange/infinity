cmake_minimum_required(VERSION 3.24)
project(infinity-engine VERSION 0.0.1 LANGUAGES C CXX)

# Self-contained engine project (T0011): builds standalone or inside the
# game superbuild. Three libraries:
#   engine-core   deterministic substrate (det::, keys, time, ephemeris,
#                 InfinityTree) — headless, no external deps
#   engine-world  generic world machinery (noise, chunk grids, meshing,
#                 streaming) — headless, depends on engine-core
#   engine-render RHI on wgpu — client-only
# The deterministic sim pair (core+world) is the single lib set a future
# game server links (see DECISIONS: engine module split).

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(INFINITY_BUILD_APP "Build window/GPU targets" ON)
option(INFINITY_ENGINE_TESTS "Build engine unit tests" ON)

add_library(engine_warnings INTERFACE)
if(MSVC)
  target_compile_options(engine_warnings INTERFACE /W4 /WX /permissive-)
else()
  target_compile_options(engine_warnings INTERFACE -Wall -Wextra -Werror)
endif()

# Deterministic-core floating point rules (DECISIONS 2026-08-30).
add_library(engine_det_flags INTERFACE)
if(MSVC)
  target_compile_options(engine_det_flags INTERFACE /fp:strict)
else()
  target_compile_options(engine_det_flags INTERFACE -ffp-contract=off)
endif()

include(cmake/deps.cmake)

add_subdirectory(core)
add_subdirectory(world)
if(INFINITY_BUILD_APP)
  add_subdirectory(render)
endif()
if(INFINITY_ENGINE_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
