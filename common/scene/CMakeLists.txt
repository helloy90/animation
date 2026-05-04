
add_library(scene AssimpSceneManager.cpp)

target_include_directories(scene PUBLIC ..)

target_link_libraries(scene PUBLIC glm::glm assimp tinygltf ozz_animation_offline etna render_utils)
