#pragma once

#include <filesystem>

#include <glm/glm.hpp>
#include <tiny_gltf.h>
#include <etna/Buffer.hpp>
#include <etna/BlockingTransferHelper.hpp>
#include <etna/VertexInput.hpp>
#include <vector>
#include <vulkan/vulkan_handles.hpp>

#include "etna/Sampler.hpp"
#include "resource/ResourceManager.hpp"
#include "etna/DescriptorSet.hpp"
#include "resource/Material.hpp"
#include "resource/Texture2D.hpp"


// Bounds for each render element
struct Bounds
{
  // w coordinate is padding
  glm::vec4 minPos;
  glm::vec4 maxPos;
};

// A single render element (relem) corresponds to a single draw call
// of a certain pipeline with specific bindings (including material data)
struct RenderElement
{
  std::uint32_t vertexOffset;
  std::uint32_t indexOffset;
  std::uint32_t indexCount;

  Material::Id material = Material::Id::Invalid;

  auto operator<=>(const RenderElement& other) const = default;
};

struct HashRenderElement
{
  std::size_t operator()(const RenderElement& render_element) const
  {
    return std::hash<std::uint32_t>()(render_element.indexCount) ^
      std::hash<std::uint32_t>()(render_element.indexOffset) ^
      std::hash<std::uint32_t>()(render_element.vertexOffset);
  }
};

// A mesh is a collection of relems. A scene may have the same mesh
// located in several different places, so a scene consists of **instances**,
// not meshes.
struct Mesh
{
  std::uint32_t firstRelem;
  std::uint32_t relemCount;
};

class SceneManager
{
public:
  SceneManager();

  void selectScene(std::filesystem::path path);
  void selectBakedScene(std::filesystem::path path);

  // Every instance is a mesh drawn with a certain transform
  // NOTE: maybe you can pass some additional data through unused matrix entries?
  std::span<const glm::mat4x4> getInstanceMatrices() { return instanceMatrices; }
  std::span<const std::uint32_t> getInstanceMeshes() { return instanceMeshes; }

  // Every mesh is a collection of relems
  std::span<const Mesh> getMeshes() { return meshes; }

  // Every relem is a single draw call
  std::span<const RenderElement> getRenderElements() { return renderElements; }

  const Texture2D& getTexture(Texture2D::Id id) const { return texture2dManager.getResource(id); }
  const Material& getMaterial(Material::Id id) const { return materialManager.getResource(id); }

  std::span<const Bounds> getRenderElementsBounds() { return renderElementsBounds; }

  vk::Buffer getVertexBuffer() { return unifiedVbuf.get(); }
  vk::Buffer getIndexBuffer() { return unifiedIbuf.get(); }

  etna::Buffer& getMaterialBuffer() { return unifiedMaterialsbuf; }

  etna::Buffer& getRelemsBuffer() { return unifiedRelemsbuf; }
  etna::Buffer& getBoundsBuffer() { return unifiedBoundsbuf; }
  etna::Buffer& getMeshesBuffer() { return unifiedMeshesbuf; }
  etna::Buffer& getInstanceMeshesBuffer() { return unifiedInstanceMeshesbuf; }
  etna::Buffer& getInstanceMatricesBuffer() { return unifiedInstanceMatricesbuf; }
  etna::Buffer& getRelemInstanceOffsetsBuffer() { return unifiedRelemInstanceOffsetsbuf; }
  etna::Buffer& getDrawInstanceIndicesBuffer() { return unifiedDrawInstanceIndicesbuf; }
  etna::Buffer& getDrawCommandsBuffer() { return unifiedDrawCommandsbuf; }

  std::vector<etna::Binding> getBindlessBindings() const;

  etna::VertexByteStreamFormatDescription getVertexFormatDescription();

  void updateMatrices(const glm::mat4& transform);

  // for now one placeholder for all materials
  Texture2D::Id baseColorPlaceholder;
  Texture2D::Id metallicRoughnessPlaceholder;
  Texture2D::Id normalPlaceholder;

  Material::Id materialPlaceholder;

private:
  struct RenderElementGLSLCompat
  {
    std::uint32_t vertexOffset;
    std::uint32_t indexOffset;
    std::uint32_t indexCount;
    std::uint32_t material;
  };
  static_assert(sizeof(RenderElementGLSLCompat) % (sizeof(float) * 4) == 0);


  struct MaterialGLSLCompat
  {
    glm::vec4 baseColorFactor;
    float roughnessFactor;
    float metallicFactor;
    std::uint32_t baseColorTexture;
    std::uint32_t metallicRoughnessTexture;
    std::uint32_t normalTexture;
    std::uint32_t _padding0 = 0;
    std::uint32_t _padding1 = 0;
    std::uint32_t _padding2 = 0;
  };
  static_assert(sizeof(MaterialGLSLCompat) % (sizeof(float) * 4) == 0);

  struct ProcessedInstances
  {
    std::vector<glm::mat4x4> matrices;
    std::vector<std::uint32_t> meshes;
  };

  struct Vertex
  {
    // First 3 floats are position, 4th float is a packed normal
    glm::vec4 positionAndNormal;
    // First 2 floats are tex coords, 3rd is a packed tangent, 4th is padding
    glm::vec4 texCoordAndTangentAndPadding;
  };
  static_assert(sizeof(Vertex) == sizeof(float) * 8);

  struct ProcessedMeshes
  {
    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<RenderElement> relems;
    std::vector<Mesh> meshes;
    std::vector<Bounds> bounds;
  };

  struct BakedMeshes
  {
    std::span<const Vertex> vertices;
    std::span<const std::uint32_t> indices;
    std::vector<RenderElement> relems;
    std::vector<Mesh> meshes;
    std::vector<Bounds> bounds;
  };

  std::optional<tinygltf::Model> loadModel(std::filesystem::path path);

  std::vector<vk::Format> parseTextures(const tinygltf::Model& model);

  void processTextures(
    const tinygltf::Model& model,
    std::vector<vk::Format> textures_info,
    std::filesystem::path path);
  void processMaterials(const tinygltf::Model& model);

  Texture2D::Id generatePlaceholderTexture(
    std::string name, vk::Format format, vk::ClearColorValue clear_color);

  void generatePlaceholderMaterial();

  ProcessedInstances processInstances(const tinygltf::Model& model) const;
  ProcessedMeshes processMeshes(const tinygltf::Model& model) const;
  BakedMeshes processBakedMeshes(const tinygltf::Model& model) const;
  void uploadData(std::span<const Vertex> vertices, std::span<const std::uint32_t> indices);

private:
  tinygltf::TinyGLTF loader;
  std::unique_ptr<etna::OneShotCmdMgr> oneShotCommands;
  etna::BlockingTransferHelper transferHelper;

  std::vector<RenderElement> renderElements;
  std::vector<Mesh> meshes;
  std::vector<glm::mat4x4> instanceMatrices;
  std::vector<std::uint32_t> instanceMeshes;
  std::vector<Bounds> renderElementsBounds;

  MaterialManager materialManager;
  Texture2DManager texture2dManager;

  etna::Sampler defaultSampler;

  etna::Buffer unifiedVbuf;
  etna::Buffer unifiedIbuf;

  etna::Buffer unifiedMaterialsbuf;

  etna::Buffer unifiedRelemsbuf;
  etna::Buffer unifiedBoundsbuf;
  etna::Buffer unifiedMeshesbuf;
  etna::Buffer unifiedInstanceMatricesbuf;
  etna::Buffer unifiedInstanceMeshesbuf;
  etna::Buffer unifiedRelemInstanceOffsetsbuf;

  etna::Buffer unifiedDrawInstanceIndicesbuf;

  etna::Buffer unifiedDrawCommandsbuf;
};
