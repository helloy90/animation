
add_library(terrain_render_module TerrainRenderModule.cpp)

target_include_directories(terrain_render_module PUBLIC ..)

target_include_directories(terrain_render_module INTERFACE shaders)
# Allow GLSL code to include helper files and compat
target_shader_include_directories(terrain_render_module INTERFACE shaders)

target_link_libraries(terrain_render_module PUBLIC etna render_utils gui)


target_add_shaders(terrain_render_module
    shaders/chunk.vert
    shaders/subdivide_chunk.tesc
    shaders/process_chunk.tese
    shaders/chunk_shadow.vert
    shaders/subdivide_chunk_shadow.tesc
    shaders/process_chunk_shadow.tese
    shaders/terrain.frag
)