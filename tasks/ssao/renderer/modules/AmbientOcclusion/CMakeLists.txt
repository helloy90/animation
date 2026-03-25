
add_library(ambient_occlusion_module SSAOModule.cpp)

target_include_directories(ambient_occlusion_module PUBLIC ..)

target_include_directories(ambient_occlusion_module INTERFACE shaders)
# Allow GLSL code to include helper files and compat
target_shader_include_directories(ambient_occlusion_module INTERFACE shaders)

target_link_libraries(ambient_occlusion_module PUBLIC etna render_utils gui scene)


target_add_shaders(ambient_occlusion_module
    shaders/ssao.comp
    shaders/blur.comp
)