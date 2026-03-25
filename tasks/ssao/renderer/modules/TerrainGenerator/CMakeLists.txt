
add_library(terrain_generator_module TerrainGeneratorModule.cpp)

target_include_directories(terrain_generator_module PUBLIC ..)

target_include_directories(terrain_generator_module INTERFACE shaders)
# Allow GLSL code to include helper files and compat
target_shader_include_directories(terrain_generator_module INTERFACE shaders)

target_link_libraries(terrain_generator_module PUBLIC etna render_utils gui)


target_add_shaders(terrain_generator_module
    shaders/generator.comp
)