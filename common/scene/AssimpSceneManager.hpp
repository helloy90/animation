#pragma once

#include <filesystem>

#include <glm/glm.hpp>
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <etna/Buffer.hpp>
#include <etna/BlockingTransferHelper.hpp>
#include <etna/VertexInput.hpp>
#include <vector>

#include "etna/Sampler.hpp"
#include "resource/ResourceManager.hpp"
#include "etna/DescriptorSet.hpp"
#include "resource/Material.hpp"
#include "resource/Texture2D.hpp"

namespace
{
struct StringHash
{
  using is_transparent = void; // NOLINT
  [[nodiscard]] size_t operator()(const char* txt) const
  {
    return std::hash<std::string_view>{}(txt);
  }
  [[nodiscard]] size_t operator()(std::string_view txt) const
  {
    return std::hash<std::string_view>{}(txt);
  }
  [[nodiscard]] size_t operator()(const std::string& txt) const
  {
    return std::hash<std::string>{}(txt);
  }
};
} // namespace

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

// struct Skeleton
// {
//   std::vector<glm::mat4x4> boneLocalMatrices;
//   std::vector<glm::mat4x4> boneWorldMatrices;
//   std::unordered_map<std::string, uint32_t, StringHash, std::equal_to<>> boneNames;
//   std::unordered_map<uint32_t, uint32_t> parents;
// };

struct Bone
{
  uint32_t matrixId;
  uint32_t parentNodeId = ~uint32_t(0);
  std::vector<uint32_t> childrenNodeIds;
  glm::mat4x4 offsetMatrix;
};

struct Node
{
  std::string name;
  uint32_t id;                       // to globalTransform and node arrays
  uint32_t parentId = ~uint32_t(0);  // to globalTransform and node arrays
  std::vector<uint32_t> childrenIds; // to globalTransform and node arrays

  glm::mat4x4 localTransform;
  // glm::mat4x4 globalTransform;

  std::optional<Bone> boneInfo = std::nullopt;
};

struct Scene
{
  std::vector<Node> nodes;
  std::vector<uint32_t> boneIds; // to node array
  std::unordered_map<std::string, uint32_t, StringHash, std::equal_to<>> nodeNameToIdx;
  std::vector<glm::mat4x4> nodeGlobalTransforms;
  std::vector<glm::mat4x4> boneGlobalTransforms;

  __forceinline const Bone& getBone(uint32_t bone_id) const
  {
    ETNA_ASSERT(nodes[bone_id].boneInfo.has_value());
    return nodes[bone_id].boneInfo.value();
  }
};
// A mesh is a collection of relems. A scene may have the same mesh
// located in several different places, so a scene consists of **instances**,
// not meshes.
struct RelemsGroup
{
  std::uint32_t firstRelem;
  std::uint32_t relemCount;
};

class AssimpSceneManager
{
public:
  AssimpSceneManager();

  void selectScene(std::filesystem::path path);

  void prepareForDraw();

  // Every instance is a mesh drawn with a certain transform
  // NOTE: maybe you can pass some additional data through unused matrix entries?
  std::span<const glm::mat4x4> getInstanceMatrices() { return instanceMatrices; }
  std::span<const std::uint32_t> getInstanceMeshes() { return instanceMeshes; }

  // Every relem is a single draw call
  std::span<const RenderElement> getRenderElements() { return renderElements; }

  const Texture2D& getTexture(Texture2D::Id id) const { return texture2dManager.getResource(id); }
  const Material& getMaterial(Material::Id id) const { return materialManager.getResource(id); }

  std::span<const Bounds> getRenderElementsBounds() { return renderElementsBounds; }

  vk::Buffer getVertexBuffer() { return unifiedVbuf.get(); }
  vk::Buffer getIndexBuffer() { return unifiedIbuf.get(); }

  const Scene& getScene() const { return processedScene; }
  void updateSceneMatrices();

  etna::Buffer& getMaterialBuffer() { return unifiedMaterialsbuf; }

  etna::Buffer& getRelemsBuffer() { return unifiedRelemsbuf; }
  etna::Buffer& getBoundsBuffer() { return unifiedBoundsbuf; }
  etna::Buffer& getMeshesBuffer() { return unifiedMeshesbuf; }
  etna::Buffer& getInstanceMeshesBuffer() { return unifiedInstanceMeshesbuf; }
  etna::Buffer& getInstanceMatricesBuffer() { return unifiedInstanceMatricesbuf->get(); }
  etna::Buffer& getBoneMatricesBuffer() { return unifiedBoneMatricesBuf->get(); }
  etna::Buffer& getRelemInstanceOffsetsBuffer() { return unifiedRelemInstanceOffsetsbuf; }
  etna::Buffer& getDrawInstanceIndicesBuffer() { return unifiedDrawRelemsInstanceIndicesbuf; }
  etna::Buffer& getDrawCommandsBuffer() { return unifiedDrawCommandsbuf; }

  std::vector<etna::Binding> getBindlessBindings() const;

  etna::VertexByteStreamFormatDescription getVertexFormatDescription();

  void updateMatrices(const glm::mat4& transform);

public:
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

  // struct Scene
  // {
  //   std::vector<Node> nodes;
  //   std::unordered_map<std::string, uint32_t, StringHash, std::equal_to<>> nodeNameToIdx;
  //   std::vector<glm::mat4x4> nodeGlobalTransforms;
  //   std::vector<glm::mat4x4> boneGlobalTransforms;
  // };

  // struct NodeTraversal
  // {
  //   std::vector<glm::mat4x4> nodeTransforms;
  //   std::unordered_map<std::string, uint32_t, StringHash, std::equal_to<>> nodeToIdx;
  //   std::string rootNodeName;
  // };

  struct ProcessedInstances
  {
    std::vector<glm::mat4x4> matrices;
    std::vector<std::uint32_t> meshes;
    Scene scene;
  };

  struct Vertex
  {
    // First 3 floats are position, 4th float is a packed normal
    glm::vec4 positionAndNormal;
    // First 2 floats are tex coords, 3rd is a packed tangent, 4th is padding
    glm::vec4 texCoordAndTangentAndPadding;

    glm::uvec4 boneIds;
    glm::vec4 boneWeights;
  };

  struct ProcessedMeshes
  {
    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<RenderElement> relems;
    std::vector<RelemsGroup> relemsGroup;
    std::vector<Bounds> bounds;
  };

private:
  void processMaterials(const aiScene* scene, std::filesystem::path path);

  Texture2D::Id generatePlaceholderTexture(
    std::string name, vk::Format format, vk::ClearColorValue clear_color);

  void generatePlaceholderMaterial();

  ProcessedInstances processNodes(const aiScene* scene) const;
  // not const - updates Scene
  ProcessedMeshes processMeshes(const aiScene* scene);
  void uploadData(std::span<const Vertex> vertices, std::span<const std::uint32_t> indices);

private:
  std::unique_ptr<etna::OneShotCmdMgr> oneShotCommands;
  etna::BlockingTransferHelper transferHelper;

  // NodeTraversal traversedNodes;
  Scene processedScene;

  // std::vector<Skeleton> skeletons;
  std::vector<RenderElement> renderElements;
  std::vector<RelemsGroup> relemsGroups;
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

  std::optional<etna::GpuSharedResource<etna::Buffer>> unifiedInstanceMatricesbuf;
  // std::optional<etna::GpuSharedResource<etna::Buffer>> unifiedBoneMatricesbuf;
  std::optional<etna::GpuSharedResource<etna::Buffer>> unifiedBoneMatricesBuf;
  etna::Buffer unifiedInstanceMeshesbuf;
  etna::Buffer unifiedRelemInstanceOffsetsbuf;

  etna::Buffer unifiedDrawRelemsInstanceIndicesbuf;

  etna::Buffer unifiedDrawCommandsbuf;
};
