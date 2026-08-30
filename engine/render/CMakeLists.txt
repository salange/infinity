add_library(engine_render STATIC
  src/rhi.cpp)
if(APPLE)
  target_sources(engine_render PRIVATE src/rhi_metal.mm)
  target_link_libraries(engine_render PRIVATE "-framework Cocoa" "-framework QuartzCore")
endif()
add_library(engine::render ALIAS engine_render)
target_include_directories(engine_render PUBLIC include)
target_link_libraries(engine_render
  PUBLIC engine::core wgpu::wgpu
  PRIVATE glfw engine_warnings)
