cmake_minimum_required(VERSION 3.24)
project(infinity VERSION 0.0.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(INFINITY_BUILD_APP "Build the windowed app (window + GPU deps)" ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

# --- version info ------------------------------------------------------------
execute_process(
  COMMAND git rev-parse --short HEAD
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  OUTPUT_VARIABLE INFINITY_GIT_HASH
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_QUIET)
if(NOT INFINITY_GIT_HASH)
  set(INFINITY_GIT_HASH "unknown")
endif()

# --- shared compile-option targets -------------------------------------------
add_library(infinity_warnings INTERFACE)
if(MSVC)
  target_compile_options(infinity_warnings INTERFACE /W4 /WX /permissive-)
else()
  target_compile_options(infinity_warnings INTERFACE -Wall -Wextra -Werror)
endif()

# Deterministic-core floating point rules (DECISIONS 2026-08-30):
# no contraction (FMA), never fast-math. Applied to core/gen/world.
add_library(infinity_det_flags INTERFACE)
if(MSVC)
  target_compile_options(infinity_det_flags INTERFACE /fp:strict)
else()
  target_compile_options(infinity_det_flags INTERFACE -ffp-contract=off)
endif()

# --- dependencies ------------------------------------------------------------
include(cmake/deps.cmake)

# --- modules -----------------------------------------------------------------
# Dependency edges (prototype-v0 spec section 10):
#   core <- gen <- world;  sim -> world;  render -> world (view only);
#   app = render + sim + glfw;  cli = world only.
# core/gen/world/cli must never link a window or GPU dependency.
add_subdirectory(core)
add_subdirectory(gen)
add_subdirectory(world)
add_subdirectory(sim)
add_subdirectory(cli)
if(INFINITY_BUILD_APP)
  add_subdirectory(render)
  add_subdirectory(app)
endif()

enable_testing()
add_subdirectory(tests)

# --- lint target -------------------------------------------------------------
find_program(RUN_CLANG_TIDY_EXE NAMES run-clang-tidy run-clang-tidy.py)
if(RUN_CLANG_TIDY_EXE)
  add_custom_target(tidy
    COMMAND ${RUN_CLANG_TIDY_EXE} -p ${CMAKE_BINARY_DIR} -quiet
            "${CMAKE_SOURCE_DIR}/(core|gen|world|sim|render|app|cli)/.*"
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "clang-tidy over project sources"
    VERBATIM)
endif()
