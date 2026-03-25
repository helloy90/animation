
add_library(water_render_module WaterRenderModule.cpp)

target_include_directories(water_render_module PUBLIC ..)

target_include_directories(water_render_module INTERFACE shaders)
# Allow GLSL code to include helper files and compat
target_shader_include_directories(water_render_module INTERFACE shaders)

target_link_libraries(water_render_module PUBLIC etna render_utils gui lights_module)


target_add_shaders(water_render_module
    shaders/chunk.vert
    shaders/subdivide_chunk.tesc
    shaders/process_chunk.tese
    shaders/water.frag
)