
add_library(static_meshes_module MeshesRenderModule.cpp)

target_include_directories(static_meshes_module PUBLIC ..)

target_include_directories(static_meshes_module INTERFACE shaders)
# Allow GLSL code to include helper files and compat
target_shader_include_directories(static_meshes_module INTERFACE shaders)

target_link_libraries(static_meshes_module PUBLIC etna render_utils gui scene)


target_add_shaders(static_meshes_module
    shaders/static_mesh.frag
    shaders/static_mesh.vert 
    shaders/static_mesh_shadow.vert 
    shaders/culling.comp
    shaders/culling_shadow.comp
)