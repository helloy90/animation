
add_library(antialiasing_module AntialiasingModule.cpp)

target_include_directories(antialiasing_module PUBLIC ..)

target_include_directories(antialiasing_module INTERFACE shaders)
# Allow GLSL code to include helper files and compat
target_shader_include_directories(antialiasing_module INTERFACE shaders)

target_link_libraries(antialiasing_module PUBLIC etna render_utils gui scene)


target_add_shaders(antialiasing_module
    shaders/taa.comp
)