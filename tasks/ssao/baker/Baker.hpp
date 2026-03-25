#pragma once

#include <array>
#include <filesystem>
#include <optional>
#include <vector>

#include <glm/glm.hpp>
#include <tiny_gltf.h>


class Baker
{
public:
  explicit Baker(std::filesystem::path path);

  void run();

private:
  struct Vertex
  {
    // First 3 floats are position, 4th float is a packed normal
    glm::vec4 positionAndNormal;
    // First 2 floats are tex coords, 3rd is a packed tangent, 4th is padding
    glm::vec4 texCoordAndTangentAndPadding;

    // TODO: Maybe implement like this later
    // glm::vec3 coords;
    // uint32_t normal;
    // glm::vec2 texture;
    // uint32_t tangent;
    // uint32_t padding = 0;
  };

  struct RealVertex
  {
    glm::vec3 position;
    glm::vec3 normal;
    glm::vec4 tangent;
    glm::vec2 texCoord;
  };

  static_assert(sizeof(Vertex) == sizeof(float) * 8);

  struct RenderElement
  {
    std::uint32_t vertexOffset;
    std::uint32_t vertexCount;
    std::uint32_t indexOffset;
    std::uint32_t indexCount;
    std::optional<std::array<std::vector<double>, 2>> positionMinMax;
    std::optional<std::array<std::vector<double>, 2>> texcoordMinMax;

    // Not implemented!
    // Material* material;
  };

  struct Mesh
  {
    std::uint32_t firstRelem;
    std::uint32_t relemCount;
  };

  struct BakedMeshes
  {
    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<RenderElement> relems;
    std::vector<Mesh> meshes;
  };

  struct Meshes
  {
    std::vector<RealVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<RenderElement> relems;
    std::vector<Mesh> meshes;
  };

  std::optional<tinygltf::Model> loadFile();
  void bakeFile(const tinygltf::Model& model);

  Meshes processMeshes(const tinygltf::Model& model);
  BakedMeshes bakeMeshes(const Meshes& meshes);

  void calculateTangents(Meshes& mesh);

  void checkModelSuitability(tinygltf::Model& model);

  void makeIntermediateModel(const tinygltf::Model& model, Meshes& meshes);

  void changeBuffer(tinygltf::Model& model, BakedMeshes& baked_meshes);
  void changeBufferViews(tinygltf::Model& model, BakedMeshes& baked_meshes);
  void changeAccessors(tinygltf::Model& model, BakedMeshes& baked_meshes);
  void saveFormatted(tinygltf::Model& model);

  tinygltf::TinyGLTF loader;
  std::filesystem::path filepath;

  bool reconstructTangents = false;
};
