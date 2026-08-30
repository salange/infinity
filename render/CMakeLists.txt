add_library(infinity_render STATIC
  src/rhi.cpp)
if(APPLE)
  target_sources(infinity_render PRIVATE src/rhi_metal.mm)
  target_link_libraries(infinity_render PRIVATE "-framework Cocoa" "-framework QuartzCore")
endif()
add_library(infinity::render ALIAS infinity_render)
target_include_directories(infinity_render PUBLIC include)
target_link_libraries(infinity_render
  PUBLIC infinity::world wgpu::wgpu
  PRIVATE glfw infinity_warnings)
