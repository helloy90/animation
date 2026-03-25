
add_library(particle_module ParticleModule.cpp Emitter.cpp)

target_include_directories(particle_module PUBLIC ..)

target_include_directories(particle_module INTERFACE shaders)
# Allow GLSL code to include helper files and compat
target_shader_include_directories(particle_module INTERFACE shaders)

target_link_libraries(particle_module PUBLIC etna render_utils gui)
