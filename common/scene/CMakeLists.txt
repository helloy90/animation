
add_library(scene AssimpSceneManager.cpp)

target_include_directories(scene PUBLIC ..)

target_link_libraries(scene PUBLIC glm::glm assimp tinygltf etna render_utils)
