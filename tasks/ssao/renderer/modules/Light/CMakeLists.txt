
add_library(lights_module LightModule.cpp Light.h DirectionalLight.h ShadowCastingDirectionalLight.cpp)

target_include_directories(lights_module PUBLIC ..)

target_include_directories(lights_module INTERFACE shaders)
# Allow GLSL code to include helper files and compat
target_shader_include_directories(lights_module INTERFACE .)

target_link_libraries(lights_module PUBLIC etna render_utils gui)


target_add_shaders(lights_module
    shaders/displace_lights.comp
)