
add_library(water_generator_module WaterGeneratorModule.cpp)

target_include_directories(water_generator_module PUBLIC ..)

target_include_directories(water_generator_module INTERFACE shaders)
# Allow GLSL code to include helper files and compat
target_shader_include_directories(water_generator_module INTERFACE shaders)

target_link_libraries(water_generator_module PUBLIC etna render_utils gui)


target_add_shaders(water_generator_module
    shaders/generate_initial_spectrum.comp
    shaders/update_spectrum_for_fft.comp
    shaders/horizontal_inverse_fft.comp
    shaders/vertical_inverse_fft.comp
    shaders/assemble.comp
)