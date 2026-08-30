# All third-party dependencies, pinned. No floating branches.
include(FetchContent)

# --- doctest (tests only) ----------------------------------------------------
FetchContent_Declare(doctest
  GIT_REPOSITORY https://github.com/doctest/doctest.git
  GIT_TAG v2.4.12
  GIT_SHALLOW TRUE)
set(DOCTEST_NO_INSTALL ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(doctest)

if(INFINITY_BUILD_APP)
  # --- GLFW ------------------------------------------------------------------
  FetchContent_Declare(glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG 3.4
    GIT_SHALLOW TRUE)
  set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
  set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(glfw)

  # --- wgpu-native (prebuilt release binaries) -------------------------------
  set(WGPU_NATIVE_VERSION "v29.0.1.1")
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(_wgpu_os "linux")
  elseif(APPLE)
    set(_wgpu_os "macos")
  elseif(WIN32)
    set(_wgpu_os "windows")
  else()
    message(FATAL_ERROR "wgpu-native: unsupported OS ${CMAKE_SYSTEM_NAME}")
  endif()
  string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _wgpu_arch)
  if(_wgpu_arch MATCHES "^(x86_64|amd64)$")
    set(_wgpu_arch "x86_64")
  elseif(_wgpu_arch MATCHES "^(arm64|aarch64)$")
    set(_wgpu_arch "aarch64")
  else()
    message(FATAL_ERROR "wgpu-native: unsupported arch ${CMAKE_SYSTEM_PROCESSOR}")
  endif()
  if(WIN32)
    set(_wgpu_asset "wgpu-${_wgpu_os}-${_wgpu_arch}-msvc-release.zip")
  else()
    set(_wgpu_asset "wgpu-${_wgpu_os}-${_wgpu_arch}-release.zip")
  endif()

  FetchContent_Declare(wgpu_native_bin
    URL "https://github.com/gfx-rs/wgpu-native/releases/download/${WGPU_NATIVE_VERSION}/${_wgpu_asset}")
  FetchContent_MakeAvailable(wgpu_native_bin)

  add_library(wgpu_native SHARED IMPORTED GLOBAL)
  set_target_properties(wgpu_native PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${wgpu_native_bin_SOURCE_DIR}/include")
  if(WIN32)
    set_target_properties(wgpu_native PROPERTIES
      IMPORTED_LOCATION "${wgpu_native_bin_SOURCE_DIR}/lib/wgpu_native.dll"
      IMPORTED_IMPLIB "${wgpu_native_bin_SOURCE_DIR}/lib/wgpu_native.dll.lib")
  elseif(APPLE)
    set_target_properties(wgpu_native PROPERTIES
      IMPORTED_LOCATION "${wgpu_native_bin_SOURCE_DIR}/lib/libwgpu_native.dylib")
  else()
    # The release .so carries no SONAME; without this CMake links the literal
    # build-relative path and the binary can't find the library at runtime.
    # NO_SONAME makes it link as -lwgpu_native, resolved via $ORIGIN rpath.
    set_target_properties(wgpu_native PROPERTIES
      IMPORTED_LOCATION "${wgpu_native_bin_SOURCE_DIR}/lib/libwgpu_native.so"
      IMPORTED_NO_SONAME TRUE)
  endif()
  add_library(wgpu::wgpu ALIAS wgpu_native)
endif()

# Dear ImGui (debug HUD) and stb (image dump) are first used in M2/M3; they
# get pinned here (tag + hash) when the first target links them.
