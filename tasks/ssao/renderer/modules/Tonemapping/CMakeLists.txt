
add_library(tonemapping_module TonemappingModule.cpp)

target_include_directories(tonemapping_module PUBLIC ..)

target_include_directories(tonemapping_module INTERFACE shaders)
# Allow GLSL code to include helper files and compat
target_shader_include_directories(tonemapping_module INTERFACE shaders)

target_link_libraries(tonemapping_module PUBLIC etna render_utils gui)


target_add_shaders(tonemapping_module
    shaders/calculate_min_max.comp
    shaders/histogram.comp
    shaders/process_histogram.comp
    shaders/postprocess.comp
)