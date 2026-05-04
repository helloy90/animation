#include "AssimpSceneManager.hpp"

#include <assimp/postprocess.h>
#include <etna/RenderTargetStates.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <ozz/animation/offline/animation_builder.h>
#include <ozz/animation/offline/raw_animation.h>
#include <ozz/animation/offline/raw_skeleton.h>
#include <ozz/animation/offline/skeleton_builder.h>
#include <ozz/animation/runtime/local_to_model_job.h>
#include <ozz/animation/runtime/skeleton_utils.h>
#include <stb_image.h>
#include <string_view>
#include <tracy/Tracy.hpp>


#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/matrix_decompose.hpp>
#include <glm/gtx/string_cast.hpp>

#include "render_utils/Utilities.hpp"

static glm::mat4x4 mat4_cast(const aiMatrix4x4& mat)
{
  return glm::transpose(glm::make_mat4(&mat.a1));
}

static glm::vec3 vec3_cast(const aiVector3D& vec)
{
  return glm::vec3(vec.x, vec.y, vec.z);
}

static glm::vec4 color4_cast(const aiColor4D& col)
{
  return glm::vec4(col.r, col.g, col.b, col.a);
}

static uint32_t encode_normalized(glm::vec4 normal)
{
  glm::float32_t scale = 127.0;
  int32_t newX = (std::lround(normal.x * scale) & 0x000000ff);
  int32_t newY = ((std::lround(normal.y * scale) & 0x000000ff) << 8);
  int32_t newZ = ((std::lround(normal.z * scale) & 0x000000ff) << 16);
  int32_t newW = ((std::lround(normal.w * scale) & 0x000000ff) << 24);

  int32_t res = newX | newY | newZ | newW;

  return std::bit_cast<uint32_t>(res);
}

AssimpSceneManager::AssimpSceneManager()
  : baseColorPlaceholder(Texture2D::Id::Invalid)
  , metallicRoughnessPlaceholder(Texture2D::Id::Invalid)
  , normalPlaceholder(Texture2D::Id::Invalid)
  , materialPlaceholder(Material::Id::Invalid)
  , oneShotCommands{etna::get_context().createOneShotCmdMgr()}
  , transferHelper{etna::BlockingTransferHelper::CreateInfo{.stagingSize = 4096 * 4096 * 4}}
  // TODO - add sampler construction from gltf file
  , defaultSampler(
      etna::Sampler::CreateInfo{
        .filter = vk::Filter::eLinear,
        .addressMode = vk::SamplerAddressMode::eRepeat,
        .name = "default_sampler"})
{
}

void AssimpSceneManager::selectScene(
  const std::filesystem::path& path, const glm::mat4x4& scene_transform)
{
  ZoneScopedN("assimpSelectScene");

  importer.SetPropertyInteger(
    AI_CONFIG_PP_SBP_REMOVE, aiPrimitiveType_LINE | aiPrimitiveType_POINT);

  importer.SetPropertyFloat(AI_CONFIG_GLOBAL_SCALE_FACTOR_KEY, 1.0f);

  const aiScene* scene = importer.ReadFile(
    path.string(),
    aiProcess_CalcTangentSpace | aiProcess_Triangulate | aiProcess_JoinIdenticalVertices |
      aiProcess_SortByPType | aiProcess_ConvertToLeftHanded | aiProcess_LimitBoneWeights |
      aiProcess_GenBoundingBoxes | aiProcess_GlobalScale);

  if (nullptr == scene)
  {
    spdlog::error("Assimp: Failed to load model with path {}", path.string());
    spdlog::error("Assimp: {}", importer.GetErrorString());
    return;
  }

  generatePlaceholderMaterial();
  processMaterials(scene, path.parent_path() / "textures");

  auto [instMats, instMeshes, processedSc] = processNodes(scene);
  instanceMatrices = std::move(instMats);
  instanceMeshes = std::move(instMeshes);

  processedScene = std::move(processedSc);
  processedScene.transform = scene_transform;

  auto [verts, inds, relems, groups, bounds] = processMeshes(scene);

  renderElements = std::move(relems);
  relemsGroups = std::move(groups);
  renderElementsBounds = std::move(bounds);

  processedScene.setupWorker();

  uploadData(verts, inds);
}

void AssimpSceneManager::loadAnimationForScene(
  const std::pair<SceneState, std::filesystem::path>& animation)
{
  const auto& [state, path] = animation;

  const aiScene* scene = importer.ReadFile(
    path.string(),
    aiProcess_CalcTangentSpace | aiProcess_Triangulate | aiProcess_JoinIdenticalVertices |
      aiProcess_SortByPType | aiProcess_ConvertToLeftHanded | aiProcess_LimitBoneWeights |
      aiProcess_GenBoundingBoxes | aiProcess_GlobalScale);

  auto animations = processAnimations(scene);

  if (animations.size() > 1)
  {
    spdlog::warn("More than one animation was loaded from file, state mapping can be unexpected!");
  }

  for (auto& anim : animations)
  {
    processedScene.animations.emplace(state, std::move(anim));
  }
}

void AssimpSceneManager::updateScene(float dt, const glm::mat4x4& scene_transform)
{
  processedScene.transform = scene_transform;

  AnimationsWorker& worker = processedScene.worker;

  if (worker.currentAnimation != nullptr)
  {
    ozz::animation::SamplingJob samplingJob;
    samplingJob.ratio = worker.currentProgress;
    ETNA_ASSERT(0.0f <= samplingJob.ratio && samplingJob.ratio <= 1.0f);
    ETNA_ASSERT(worker.currentAnimation->num_tracks() == processedScene.skeleton->num_joints());

    samplingJob.animation = worker.currentAnimation;
    samplingJob.context = worker.samplingContext.get();
    samplingJob.output = ozz::make_span(worker.localTransforms);

    ETNA_VERIFYF(
      samplingJob.Validate(),
      "Error when building sampling job for animation {} !",
      worker.currentAnimation->name());

    ETNA_VERIFYF(samplingJob.Run(), "Error when running sampling job!");

    worker.currentProgress += dt / worker.currentAnimation->duration();

    if (worker.currentProgress > 1.0f)
    {
      worker.currentProgress = 0.0f;
    }
  }
  else
  {
    auto restPose = processedScene.skeleton->joint_rest_poses();
    worker.localTransforms.assign(restPose.begin(), restPose.end());
  }

  ozz::animation::LocalToModelJob localToModelJob;
  localToModelJob.skeleton = processedScene.skeleton.get();
  localToModelJob.input = ozz::make_span(worker.localTransforms);
  localToModelJob.output = ozz::make_span(worker.worldTransforms);
  ozz::math::Float4x4 rootTransform;
  std::memcpy(&rootTransform, &processedScene.transform, sizeof(rootTransform));
  localToModelJob.root = &rootTransform;
  ETNA_VERIFYF(localToModelJob.Validate(), "Error when building localToModelJob!");
  ETNA_VERIFYF(localToModelJob.Run(), "Error when running localToModelJob!");

  std::span<const glm::mat4x4> worldTransforms = std::span(
    reinterpret_cast<const glm::mat4x4*>(processedScene.worker.worldTransforms.data()),
    processedScene.worker.worldTransforms.size());

  static std::stack<uint32_t> nodes;
  nodes.push(processedScene.boneRootNodeId);

  while (!nodes.empty())
  {
    uint32_t nodeId = nodes.top();
    nodes.pop();

    Node& node = processedScene.nodes[nodeId];
    Bone& bone = node.boneInfo.value();

    processedScene.boneGlobalTransforms[bone.matrixId] =
      worldTransforms[node.id] * bone.offsetMatrix;

    for (const uint32_t childId : bone.childrenNodeIds)
    {
      nodes.push(childId);
    }
  }
}

void AssimpSceneManager::prepareForDraw()
{
  auto& currentInstanceMatricesBuffer = unifiedInstanceMatricesbuf->get();
  currentInstanceMatricesBuffer.map();
  std::memcpy(
    currentInstanceMatricesBuffer.data(),
    instanceMatrices.data(),
    instanceMatrices.size() * sizeof(glm::mat4x4));
  currentInstanceMatricesBuffer.unmap();

  auto& currentBoneMatricesBuffer = unifiedBoneMatricesBuf->get();
  currentBoneMatricesBuffer.map();
  std::memcpy(
    currentBoneMatricesBuffer.data(),
    processedScene.boneGlobalTransforms.data(),
    processedScene.boneGlobalTransforms.size() * sizeof(glm::mat4x4));
  currentBoneMatricesBuffer.unmap();
}

void AssimpSceneManager::processMaterials(const aiScene* scene, std::filesystem::path textures_path)
{
  auto processTexture = [&](vk::Format format, std::filesystem::path path) {
    ZoneScoped;
    auto& ctx = etna::get_context();

    std::string resName = ("texture_" + path.filename().generic_string());
    Texture2D::Id resID = texture2dManager.tryGetResourceId(resName.c_str());

    if (resID != Texture2D::Id::Invalid)
    {
      return resID;
    }

    uint32_t layerCount = 1;
    int width, height, channels;

    auto filename = path.generic_string<char>();
    unsigned char* textureData =
      stbi_load(filename.c_str(), &width, &height, &channels, STBI_rgb_alpha);

    ETNA_VERIFYF(textureData != nullptr, "Texture {} is not loaded!", path.generic_string());

    uint32_t mipLevels = static_cast<uint32_t>(std::floor(std::log2(std::max(width, height)))) + 1;

    const vk::DeviceSize textureSize = width * height * 4;
    etna::Buffer textureBuffer = ctx.createBuffer(
      etna::Buffer::CreateInfo{
        .size = textureSize,
        .bufferUsage =
          vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst,
        .name = path.filename().generic_string() + "_buffer",
      });

    auto source = std::span<unsigned char>(textureData, textureSize);
    transferHelper.uploadBuffer(*oneShotCommands, textureBuffer, 0, std::as_bytes(source));

    etna::Image texture = ctx.createImage(
      etna::Image::CreateInfo{
        .extent = vk::Extent3D{static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1},
        .name = path.filename().generic_string() + "_texture",
        .format = format,
        .imageUsage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst |
          vk::ImageUsageFlagBits::eTransferSrc,
        .mipLevels = mipLevels});

    render_utility::local_copy_buffer_to_image(
      *oneShotCommands, textureBuffer, texture, layerCount);

    render_utility::generate_mipmaps_vk_style(*oneShotCommands, texture, mipLevels, layerCount);

    auto id = texture2dManager.loadResource(resName.c_str(), {.texture = std::move(texture)});
    spdlog::info(
      "New texture loaded from file {}, texture id = {}",
      path.filename().generic_string(),
      static_cast<uint32_t>(id));

    stbi_image_free(textureData);

    return id;
  };

  std::span<aiMaterial*> materials =
    std::span(scene->mMaterials, scene->mMaterials + scene->mNumMaterials);

  for (const aiMaterial* aiMat : materials)
  {
    Material material;

    material.baseColorFactor = glm::vec4(1, 1, 1, 1);
    material.metallicFactor = 0.0f;
    material.roughnessFactor = 1.0f;

    std::span props = std::span(aiMat->mProperties, aiMat->mProperties + aiMat->mNumProperties);
    if (aiMat->GetTextureCount(aiTextureType_DIFFUSE) > 0)
    {
      aiString name;
      aiMat->Get(AI_MATKEY_TEXTURE_DIFFUSE(0), name);

      std::filesystem::path path = name.C_Str();
      material.baseColorTexture =
        processTexture(vk::Format::eR8G8B8A8Unorm, textures_path / path.filename());
    }
    else
    {
      material.baseColorTexture = baseColorPlaceholder;
      aiColor4D diffuse;
      if (AI_SUCCESS == aiGetMaterialColor(aiMat, AI_MATKEY_COLOR_DIFFUSE, &diffuse))
      {
        material.baseColorFactor = color4_cast(diffuse);
      }
    }
    if (aiMat->GetTextureCount(aiTextureType_NORMALS) > 0)
    {
      aiString name;
      aiMat->Get(AI_MATKEY_TEXTURE_NORMALS(0), name);

      std::filesystem::path path = name.C_Str();
      material.normalTexture =
        processTexture(vk::Format::eR8G8B8A8Unorm, textures_path / path.filename());
    }
    else
    {
      material.normalTexture = normalPlaceholder;
    }
    material.metallicRoughnessTexture = metallicRoughnessPlaceholder;

    auto id = materialManager.loadResource(
      ("material_" + std::string(aiMat->GetName().C_Str())).c_str(), std::move(material));
    spdlog::info(
      "Material loaded, name - {}, material id = {}, used texture ids - [\n"
      "base color - {},\n"
      "metallic/roughness - {},\n"
      "normal - {}\n]",
      aiMat->GetName().C_Str(),
      static_cast<uint32_t>(id),
      static_cast<uint32_t>(material.baseColorTexture),
      static_cast<uint32_t>(material.metallicRoughnessTexture),
      static_cast<uint32_t>(material.normalTexture));
  }
}

Texture2D::Id AssimpSceneManager::generatePlaceholderTexture(
  std::string name, vk::Format format, vk::ClearColorValue clear_color)
{
  etna::Image texture = etna::get_context().createImage(
    etna::Image::CreateInfo{
      .extent = vk::Extent3D{1, 1, 1},
      .name = name + "_texture",
      .format = format,
      .imageUsage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled |
        vk::ImageUsageFlagBits::eTransferDst});

  auto commandBuffer = oneShotCommands->start();
  auto extent = texture.getExtent();

  ETNA_CHECK_VK_RESULT(commandBuffer.begin(vk::CommandBufferBeginInfo{}));
  {
    // needed for setting texture color
    {
      etna::RenderTargetState state(
        commandBuffer,
        {{}, {extent.width, extent.height}},
        {{.image = texture.get(), .view = texture.getView({}), .clearColorValue = clear_color}},
        {});
    }

    etna::set_state(
      commandBuffer,
      texture.get(),
      vk::PipelineStageFlagBits2::eFragmentShader,
      vk::AccessFlagBits2::eShaderSampledRead,
      vk::ImageLayout::eShaderReadOnlyOptimal,
      vk::ImageAspectFlagBits::eColor);

    etna::flush_barriers(commandBuffer);
  }
  ETNA_CHECK_VK_RESULT(commandBuffer.end());

  oneShotCommands->submitAndWait(commandBuffer);

  Texture2D::Id placeholder =
    texture2dManager.loadResource(("texture_" + name).c_str(), {.texture = std::move(texture)});
  spdlog::info(
    "Placeholder texture {} created , texture id = {}", name, static_cast<uint32_t>(placeholder));
  return placeholder;
}

void AssimpSceneManager::generatePlaceholderMaterial()
{
  if (materialPlaceholder != Material::Id::Invalid)
  {
    return;
  }

  if (baseColorPlaceholder == Texture2D::Id::Invalid)
  {
    baseColorPlaceholder = generatePlaceholderTexture(
      "base_color_placeholder", vk::Format::eR8G8B8A8Srgb, {1.0f, 1.0f, 1.0f, 1.0f});
  }
  if (metallicRoughnessPlaceholder == Texture2D::Id::Invalid)
  {
    metallicRoughnessPlaceholder = generatePlaceholderTexture(
      "metallic_roughness_placeholder", vk::Format::eR8G8B8A8Unorm, {0.0f, 1.0f, 1.0f, 1.0f});
  }
  if (normalPlaceholder == Texture2D::Id::Invalid)
  {
    normalPlaceholder = generatePlaceholderTexture(
      "normal_placeholder", vk::Format::eR8G8B8A8Snorm, {0.0f, 0.0f, 0.5f, 0.0f});
  }

  materialPlaceholder = materialManager.loadResource(
    "material_placeholder",
    {.baseColorFactor = {1.0f, 1.0f, 1.0f, 1.0f},
     .roughnessFactor = 1.0f,
     .metallicFactor = 1.0f,
     .baseColorTexture = baseColorPlaceholder,
     .metallicRoughnessTexture = metallicRoughnessPlaceholder,
     .normalTexture = normalPlaceholder});

  spdlog::info(
    "Placeholder material created, material id = {}, used texture ids - [\n"
    "\tbase color - {},\n"
    "\tmetallic/roughness - {},\n"
    "\tnormal - {}\n]",
    static_cast<uint32_t>(materialPlaceholder),
    static_cast<uint32_t>(baseColorPlaceholder),
    static_cast<uint32_t>(metallicRoughnessPlaceholder),
    static_cast<uint32_t>(normalPlaceholder));
}

AssimpSceneManager::ProcessedInstances AssimpSceneManager::processNodes(const aiScene* scene) const
{
  // NOTE - maybe super ineffectient because of four traversals, but trying to
  // minimize reallocations
  std::size_t nodesCount = 1;
  std::size_t meshesInstancesCount = 0;
  {
    std::queue<const aiNode*> nodes;

    nodes.push(scene->mRootNode);

    while (!nodes.empty())
    {
      const aiNode* node = nodes.front();
      nodes.pop();

      meshesInstancesCount += node->mNumMeshes;

      std::span children = std::span(node->mChildren, node->mChildren + node->mNumChildren);

      nodesCount += node->mNumChildren;

      for (const auto child : children)
      {
        nodes.push(child);
      }
    }
  }

  ProcessedInstances result;

  result.matrices.reserve(meshesInstancesCount);
  result.meshes.reserve(meshesInstancesCount);

  result.scene.nodes.reserve(nodesCount);
  result.scene.nodeGlobalTransforms.resize(nodesCount, glm::identity<glm::mat4x4>());

  auto buildProcessedScene = [&scene, &result]() -> void {
    auto buildImpl = [&result](const aiNode* node, uint32_t parent, auto& traverse_impl) -> void {
      uint32_t nodeIdx = result.scene.nodes.size();
      std::string nodeName = node->mName.C_Str();
      result.scene.nodeNameToIdx.emplace(nodeName, nodeIdx);

      Node& sceneNode = result.scene.nodes.emplace_back(
        Node{
          .name = nodeName,
          .id = nodeIdx,
          .parentId = parent,
          .childrenIds = {},
          .localTransform = mat4_cast(node->mTransformation),
        });

      sceneNode.childrenIds.reserve(node->mNumChildren);

      if (parent != INVALID_INDEX) [[likely]]
      {
        result.scene.nodes[parent].childrenIds.emplace_back(nodeIdx);
        result.scene.nodeGlobalTransforms[nodeIdx] =
          result.scene.nodeGlobalTransforms[parent] * sceneNode.localTransform;
      }
      else
      {
        result.scene.nodeGlobalTransforms[nodeIdx] = sceneNode.localTransform;
      }

      for (uint32_t i = 0; i < node->mNumMeshes; i++)
      {
        result.matrices.push_back(
          glm::scale(result.scene.nodeGlobalTransforms[nodeIdx], glm::vec3(1, 1, 1)));
        result.meshes.push_back(node->mMeshes[i]);
      }

      for (uint32_t i = 0; i < node->mNumChildren; i++)
      {
        traverse_impl(node->mChildren[i], nodeIdx, traverse_impl);
      }
    };

    buildImpl(scene->mRootNode, INVALID_INDEX, buildImpl);
  };

  buildProcessedScene();

  return result;
}

AssimpSceneManager::ProcessedMeshes AssimpSceneManager::processMeshes(const aiScene* scene)
{
  ProcessedMeshes result;

  {
    std::size_t verticesCount = 0;
    std::size_t indicesCount = 0;

    for (uint32_t meshIdx = 0; meshIdx < scene->mNumMeshes; meshIdx++)
    {
      const aiMesh* mesh = scene->mMeshes[meshIdx];

      verticesCount += mesh->mNumVertices;
      // NOTE - maybe redundant and could be calculated in one go
      for (uint32_t primIdx = 0; primIdx < mesh->mNumFaces; primIdx++)
      {
        const aiFace& face = mesh->mFaces[primIdx];

        indicesCount += face.mNumIndices;
      }
    }

    result.vertices.reserve(verticesCount);
    result.indices.reserve(indicesCount);
  }

  result.relems.reserve(scene->mNumMeshes);
  result.bounds.reserve(scene->mNumMeshes);
  // it seems that meshes is the same as relems in assimp
  // (not the case in gltf where one mesh can have several render elements)
  // leaving it for compatibility with already present rendering
  result.relemsGroup.reserve(scene->mNumMeshes);

  bool boneSetupCalled = false;
  for (uint32_t meshIdx = 0; meshIdx < scene->mNumMeshes; meshIdx++)
  {
    const aiMesh* mesh = scene->mMeshes[meshIdx];
    result.relemsGroup.push_back(
      RelemsGroup{
        .firstRelem = static_cast<std::uint32_t>(result.relems.size()),
        .relemCount = static_cast<std::uint32_t>(1),
      });

    uint32_t currentVertexOffset = static_cast<std::uint32_t>(result.vertices.size());

    result.relems.push_back(
      RenderElement{
        .vertexOffset = currentVertexOffset,
        .indexOffset = static_cast<std::uint32_t>(result.indices.size()),
        .indexCount = 0, // filled in next loop
        .material =
          static_cast<Material::Id>(mesh->mMaterialIndex + 1)}); // placeholder material is first

    result.bounds.push_back(
      Bounds{
        .minPos = glm::vec4(vec3_cast(mesh->mAABB.mMin), 0),
        .maxPos = glm::vec4(vec3_cast(mesh->mAABB.mMax), 0),
      });

    for (uint32_t vertIdx = 0; vertIdx < mesh->mNumVertices; vertIdx++)
    {
      const aiVector3D& meshVertex = mesh->mVertices[vertIdx];
      // NOTE - may blow up for models that dont have normals/tangents/texcoords
      const aiVector3D& meshNormal = mesh->mNormals[vertIdx];

      auto& vertex = result.vertices.emplace_back();

      vertex.positionAndNormal = glm::vec4(
        meshVertex.x,
        meshVertex.y,
        meshVertex.z,
        std::bit_cast<float>(
          encode_normalized(glm::vec4(glm::vec3(meshNormal.x, meshNormal.y, meshNormal.z), 0))));

      vertex.texCoordAndTangentAndPadding = glm::vec4(0, 0, 0, 0);
      if (mesh->mTangents != nullptr)
      {
        const aiVector3D& meshTangent = mesh->mTangents[vertIdx];
        vertex.texCoordAndTangentAndPadding.z = std::bit_cast<float>(
          encode_normalized(glm::vec4(meshTangent.x, meshTangent.y, meshTangent.z, 1)));
      }
      else
      {
        vertex.texCoordAndTangentAndPadding.z =
          std::bit_cast<float>(encode_normalized(glm::vec4(0, 0, 1, 1)));
      }
      if (mesh->mTextureCoords[0] != nullptr)
      {
        const aiVector3D& meshTexcoord = mesh->mTextureCoords[0][vertIdx];
        vertex.texCoordAndTangentAndPadding.x = meshTexcoord.x;
        vertex.texCoordAndTangentAndPadding.y = meshTexcoord.y;
      }

      vertex.boneIds = glm::uvec4(0, 0, 0, 0);
      vertex.boneWeights = glm::vec4(0, 0, 0, 0);
    }

    std::uint32_t indexCount = 0;
    for (uint32_t primIdx = 0; primIdx < mesh->mNumFaces; primIdx++)
    {
      const aiFace& face = mesh->mFaces[primIdx];

      const std::size_t lastIndicesCount = result.indices.size();
      result.indices.resize(lastIndicesCount + face.mNumIndices);

      std::memcpy(
        result.indices.data() + lastIndicesCount,
        face.mIndices,
        sizeof(result.indices[0]) * face.mNumIndices);

      indexCount += face.mNumIndices;
    }

    result.relems.back().indexCount = indexCount;

    // NOTE - assuming that all meshes have the same skeleton
    if (mesh->HasBones())
    {
      if (!boneSetupCalled)
      {
        processedScene.boneGlobalTransforms.resize(mesh->mNumBones, glm::identity<glm::mat4x4>());
        processedScene.boneIds.reserve(mesh->mNumBones);
      }

      std::vector<uint32_t> weightOffset(mesh->mNumVertices, 0);

      for (uint32_t i = 0; i < mesh->mNumBones; i++)
      {
        const aiBone* bone = mesh->mBones[i];
        if (!boneSetupCalled)
        {
          processedScene.nodes[processedScene.nodeNameToIdx.at(bone->mName.C_Str())]
            .boneInfo.emplace(
              Bone{
                .matrixId = i,
                .parentNodeId = INVALID_INDEX,
                .childrenNodeIds = std::vector<uint32_t>(),
                .offsetMatrix = mat4_cast(bone->mOffsetMatrix),
              });
          processedScene.boneIds.emplace_back(processedScene.nodeNameToIdx.at(bone->mName.C_Str()));
        }

        std::span weights = std::span(bone->mWeights, bone->mWeights + bone->mNumWeights);
        for (const auto& [vertexId, weight] : weights)
        {
          uint32_t offset = weightOffset[vertexId]++;
          ETNA_VERIFY(offset < 4);
          result.vertices[currentVertexOffset + vertexId].boneWeights[offset] = weight;
          result.vertices[currentVertexOffset + vertexId].boneIds[offset] = i;
        }
      }

      for (auto& vertex : result.vertices)
      {
        const glm::vec4& weights = vertex.boneWeights;
        float sum = weights.x + weights.y + weights.z + weights.w;
        vertex.boneWeights *= 1.0f / sum;
      }

      if (boneSetupCalled)
      {
        continue;
      }

      auto buildSkeletonInfo = [&scene, this]() -> void {
        auto buildImpl =
          [this](const aiNode* node, uint32_t bone_parent, auto& traverse_impl) -> void {
          Node& sceneNode =
            processedScene.nodes[processedScene.nodeNameToIdx.at(node->mName.C_Str())];

          if (sceneNode.boneInfo.has_value())
          {
            Bone& bone = sceneNode.boneInfo.value();

            if (bone_parent != INVALID_INDEX) [[likely]]
            {
              bone.parentNodeId = bone_parent;
              processedScene.nodes[bone_parent].boneInfo->childrenNodeIds.emplace_back(
                sceneNode.id);
            }
            else
            {
              processedScene.boneRootNodeId = sceneNode.id;
            }

            bone_parent = processedScene.nodeNameToIdx.at(node->mName.C_Str());
          }

          for (uint32_t i = 0; i < node->mNumChildren; i++)
          {
            traverse_impl(node->mChildren[i], bone_parent, traverse_impl);
          }
        };

        buildImpl(scene->mRootNode, INVALID_INDEX, buildImpl);
      };

      buildSkeletonInfo();

      auto buildOzzSkeleton = [this]() {
        using RawSkeleton = ozz::animation::offline::RawSkeleton;
        using Joint = ozz::animation::offline::RawSkeleton::Joint;

        RawSkeleton rawSkeleton;
        rawSkeleton.roots.resize(1);

        auto buildImpl = [this](Joint& joint, uint32_t bone_node_id, auto& traverse_impl) -> void {
          Node& boneNode = processedScene.nodes[bone_node_id];

          joint.name = boneNode.name;

          const glm::mat4x4& localTransform = boneNode.localTransform;

          glm::vec3 scale;
          glm::quat rotation;
          glm::vec3 translation;
          glm::vec3 skew;
          glm::vec4 perspective;

          glm::decompose(localTransform, scale, rotation, translation, skew, perspective);

          joint.transform.translation =
            ozz::math::Float3(translation.x, translation.y, translation.z);
          joint.transform.rotation =
            ozz::math::Quaternion(rotation.x, rotation.y, rotation.z, rotation.w);
          joint.transform.scale = ozz::math::Float3(scale.x, scale.y, scale.z);

          joint.children.resize(boneNode.childrenIds.size());
          for (uint32_t i = 0; i < boneNode.childrenIds.size(); i++)
          {
            traverse_impl(joint.children[i], boneNode.childrenIds[i], traverse_impl);
          }
        };

        buildImpl(rawSkeleton.roots[0], 0, buildImpl);

        ETNA_VERIFYF(rawSkeleton.Validate(), "Error after building ozz skeleton!");

        return rawSkeleton;
      };

      auto rawSkeleton = buildOzzSkeleton();

      ozz::animation::offline::SkeletonBuilder builder;

      processedScene.skeleton = builder(rawSkeleton);

      boneSetupCalled = true;
    }
  }

  return result;
}

ozz::vector<ozz::unique_ptr<ozz::animation::Animation>> AssimpSceneManager::processAnimations(
  const aiScene* scene)
{
  auto createAnimation = [this](const aiAnimation* animation) {
    using RawAnimation = ozz::animation::offline::RawAnimation;
    using JointTrack = ozz::animation::offline::RawAnimation::JointTrack;

    RawAnimation rawAnimation;

    rawAnimation.name = animation->mName.C_Str();

    rawAnimation.duration = animation->mDuration / animation->mTicksPerSecond;

    ozz::animation::Skeleton& skeleton = *processedScene.skeleton;

    rawAnimation.tracks.resize(skeleton.num_joints());

    for (int jointIdx = 0; jointIdx < skeleton.num_joints(); jointIdx++)
    {
      std::string_view jointName = skeleton.joint_names()[jointIdx];

      uint32_t currentChannelIdx = INVALID_INDEX;
      for (uint32_t channelIdx = 0; channelIdx < animation->mNumChannels; channelIdx++)
      {
        if (jointName == animation->mChannels[channelIdx]->mNodeName.C_Str())
        {
          currentChannelIdx = channelIdx;
          break;
        }
      }

      JointTrack& track = rawAnimation.tracks[jointIdx];
      if (currentChannelIdx == INVALID_INDEX)
      {
        ozz::math::Transform transform = ozz::animation::GetJointLocalRestPose(skeleton, jointIdx);

        track.translations.resize(1);
        track.translations[0].time = 0.0f;
        track.translations[0].value = transform.translation;

        track.rotations.resize(1);
        track.rotations[0].time = 0.0f;
        track.rotations[0].value = transform.rotation;

        track.scales.resize(1);
        track.scales[0].time = 0.0f;
        track.scales[0].value = transform.scale;
      }
      else
      {
        const aiNodeAnim& channel = *animation->mChannels[currentChannelIdx];

        track.translations.resize(channel.mNumPositionKeys);
        for (uint32_t posKeyIdx = 0; posKeyIdx < channel.mNumPositionKeys; posKeyIdx++)
        {
          const aiVectorKey& key = channel.mPositionKeys[posKeyIdx];
          const float time = key.mTime / animation->mTicksPerSecond;
          track.translations[posKeyIdx].time = time;
          track.translations[posKeyIdx].value =
            ozz::math::Float3(key.mValue.x, key.mValue.y, key.mValue.z);

          ETNA_ASSERT(time >= 0.0f && time <= rawAnimation.duration);
          if (posKeyIdx > 0)
          {
            ETNA_ASSERT(key.mTime >= channel.mPositionKeys[posKeyIdx - 1].mTime);
          }
        }

        track.rotations.resize(channel.mNumRotationKeys);
        for (uint32_t quatKeyIdx = 0; quatKeyIdx < channel.mNumRotationKeys; quatKeyIdx++)
        {
          const aiQuatKey& key = channel.mRotationKeys[quatKeyIdx];
          const float time = key.mTime / animation->mTicksPerSecond;
          track.rotations[quatKeyIdx].time = time;
          track.rotations[quatKeyIdx].value =
            ozz::math::Quaternion(key.mValue.x, key.mValue.y, key.mValue.z, key.mValue.w);

          ETNA_ASSERT(time >= 0.0f && time <= rawAnimation.duration);
          if (quatKeyIdx > 0)
          {
            ETNA_ASSERT(key.mTime >= channel.mRotationKeys[quatKeyIdx - 1].mTime);
          }
        }

        track.scales.resize(channel.mNumScalingKeys);
        for (uint32_t scaleKeyIdx = 0; scaleKeyIdx < channel.mNumScalingKeys; scaleKeyIdx++)
        {
          const aiVectorKey& key = channel.mScalingKeys[scaleKeyIdx];
          const float time = key.mTime / animation->mTicksPerSecond;
          track.scales[scaleKeyIdx].time = time;
          track.scales[scaleKeyIdx].value =
            ozz::math::Float3(key.mValue.x, key.mValue.y, key.mValue.z);

          ETNA_ASSERT(time >= 0.0f && time <= rawAnimation.duration);
          if (scaleKeyIdx > 0)
          {
            ETNA_ASSERT(key.mTime >= channel.mScalingKeys[scaleKeyIdx - 1].mTime);
          }
        }
      }
    }

    ETNA_VERIFYF(rawAnimation.Validate(), "Error after building ozz animation!");

    ozz::animation::offline::AnimationBuilder builder;

    ozz::unique_ptr<ozz::animation::Animation> processedAnimation = builder(rawAnimation);

    spdlog::info("Loaded animation {}", animation->mName.C_Str());

    return processedAnimation;
  };

  ozz::vector<ozz::unique_ptr<ozz::animation::Animation>> animations;

  animations.resize(scene->mNumAnimations);

  for (uint32_t animIdx = 0; animIdx < scene->mNumAnimations; animIdx++)
  {
    animations[animIdx] = createAnimation(scene->mAnimations[animIdx]);
  }

  return animations;
}

void AssimpSceneManager::uploadData(
  std::span<const Vertex> vertices, std::span<const std::uint32_t> indices)
{
  auto& ctx = etna::get_context();

  unifiedVbuf = ctx.createBuffer(
    etna::Buffer::CreateInfo{
      .size = vertices.size_bytes(),
      .bufferUsage = vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eVertexBuffer,
      .memoryUsage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
      .name = "unified=Vbuf",
    });

  unifiedIbuf = ctx.createBuffer(
    etna::Buffer::CreateInfo{
      .size = indices.size_bytes(),
      .bufferUsage = vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eIndexBuffer,
      .memoryUsage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
      .name = "unifiedIbuf",
    });

  transferHelper.uploadBuffer<Vertex>(*oneShotCommands, unifiedVbuf, 0, vertices);
  transferHelper.uploadBuffer<std::uint32_t>(*oneShotCommands, unifiedIbuf, 0, indices);

  unifiedRelemsbuf = ctx.createBuffer(
    etna::Buffer::CreateInfo{
      .size = renderElements.size() * sizeof(RenderElementGLSLCompat),
      .bufferUsage =
        vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eStorageBuffer,
      .memoryUsage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
      .name = "unifiedRelemsbuf"});

  unifiedMaterialsbuf = ctx.createBuffer(
    etna::Buffer::CreateInfo{
      .size = materialManager.size() * sizeof(MaterialGLSLCompat),
      .bufferUsage =
        vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eStorageBuffer,
      .memoryUsage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
      .name = "unifiedMaterialbuf"});

  // maybe unnesessary
  std::vector<MaterialGLSLCompat> materialData;
  materialData.reserve(materialManager.size());
  for (const auto& material : materialManager)
  {
    materialData.emplace_back(
      MaterialGLSLCompat{
        .baseColorFactor = material.baseColorFactor,
        .roughnessFactor = material.roughnessFactor,
        .metallicFactor = material.metallicFactor,
        .baseColorTexture = static_cast<uint32_t>(material.baseColorTexture),
        .metallicRoughnessTexture = static_cast<uint32_t>(material.metallicRoughnessTexture),
        .normalTexture = static_cast<uint32_t>(material.normalTexture)});
  }

  transferHelper.uploadBuffer<MaterialGLSLCompat>(
    *oneShotCommands, unifiedMaterialsbuf, 0, std::span(materialData));

  // maybe unnesessary
  std::vector<RenderElementGLSLCompat> renderElementsData;
  renderElementsData.reserve(renderElements.size());
  for (const auto& relem : renderElements)
  {
    renderElementsData.emplace_back(
      RenderElementGLSLCompat{
        .vertexOffset = relem.vertexOffset,
        .indexOffset = relem.indexOffset,
        .indexCount = relem.indexCount,
        .material = static_cast<std::uint32_t>(relem.material),
      });
  }

  transferHelper.uploadBuffer<RenderElementGLSLCompat>(
    *oneShotCommands, unifiedRelemsbuf, 0, std::span(renderElementsData));

  unifiedBoundsbuf = ctx.createBuffer(
    etna::Buffer::CreateInfo{
      .size = renderElementsBounds.size() * sizeof(Bounds),
      .bufferUsage =
        vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eStorageBuffer,
      .memoryUsage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
      .name = "unifiedBoundsbuf"});
  unifiedMeshesbuf = ctx.createBuffer(
    etna::Buffer::CreateInfo{
      .size = relemsGroups.size() * sizeof(RelemsGroup),
      .bufferUsage =
        vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eStorageBuffer,
      .memoryUsage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
      .name = "unifiedMeshesbuf"});

  unifiedInstanceMatricesbuf.emplace(
    ctx.getMainWorkCount(),
    [&ctx, instanceMatricesSize = this->instanceMatrices.size()](std::size_t i) {
      return ctx.createBuffer(
        etna::Buffer::CreateInfo{
          .size = instanceMatricesSize * sizeof(glm::mat4x4),
          .bufferUsage =
            vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eStorageBuffer,
          .memoryUsage = VMA_MEMORY_USAGE_AUTO,
          .allocationCreate = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
            VMA_ALLOCATION_CREATE_MAPPED_BIT,
          .name = fmt::format("unifiedInstanceMatricesbuf{}", i)});
    });

  spdlog::info(
    "{} - relem bounds size, {} - instance matrices size",
    renderElementsBounds.size(),
    instanceMatrices.size());

  unifiedBoneMatricesBuf.emplace(
    ctx.getMainWorkCount(),
    [&ctx, boneMatricesSize = processedScene.boneGlobalTransforms.size()](std::size_t i) {
      return ctx.createBuffer(
        etna::Buffer::CreateInfo{
          .size = boneMatricesSize * sizeof(glm::mat4x4),
          .bufferUsage =
            vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eStorageBuffer,
          .memoryUsage = VMA_MEMORY_USAGE_AUTO,
          .allocationCreate = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
            VMA_ALLOCATION_CREATE_MAPPED_BIT,
          .name = fmt::format("unifiedBoneMatricesbuf{}", i)});
    });

  unifiedInstanceMeshesbuf = ctx.createBuffer(
    etna::Buffer::CreateInfo{
      .size = instanceMeshes.size() * sizeof(std::uint32_t),
      .bufferUsage =
        vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eStorageBuffer,
      .memoryUsage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
      .name = "unifiedInstanceMeshesbuf"});

  transferHelper.uploadBuffer<Bounds>(
    *oneShotCommands, unifiedBoundsbuf, 0, std::span(renderElementsBounds));
  transferHelper.uploadBuffer<RelemsGroup>(
    *oneShotCommands, unifiedMeshesbuf, 0, std::span(relemsGroups));

  transferHelper.uploadBuffer<std::uint32_t>(
    *oneShotCommands, unifiedInstanceMeshesbuf, 0, std::span(instanceMeshes));

  std::size_t drawRelemsInstancesIndicesSize = 0;
  for (auto meshIndex : instanceMeshes)
  {
    drawRelemsInstancesIndicesSize += relemsGroups[meshIndex].relemCount;
  }

  // filled on GPU when culling
  unifiedDrawRelemsInstanceIndicesbuf = ctx.createBuffer(
    etna::Buffer::CreateInfo{
      .size = drawRelemsInstancesIndicesSize * sizeof(std::uint32_t),
      .bufferUsage =
        vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eStorageBuffer,
      .memoryUsage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
      .name = "unifiedDrawRelemsInstanceIndicesbuf"});

  unifiedRelemInstanceOffsetsbuf = ctx.createBuffer(
    etna::Buffer::CreateInfo{
      .size = renderElements.size() * sizeof(std::uint32_t),
      .bufferUsage =
        vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eStorageBuffer,
      .memoryUsage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
      .name = "unifiedRelemInstanceOffsetsbuf"});

  std::vector<std::uint32_t> relemInstanceOffsets(renderElements.size(), 0);
  for (const auto& meshIdx : instanceMeshes)
  {
    const auto& currentMesh = relemsGroups[meshIdx];
    for (std::uint32_t relemIdx = currentMesh.firstRelem;
         relemIdx < currentMesh.firstRelem + currentMesh.relemCount;
         relemIdx++)
    {
      relemInstanceOffsets[relemIdx]++;
    }
  }

  std::uint32_t offset = 0;
  std::uint32_t previousAmount = 0;
  for (auto& amount : relemInstanceOffsets)
  {
    previousAmount = amount;
    amount = offset;
    offset += previousAmount;
  }

  transferHelper.uploadBuffer<std::uint32_t>(
    *oneShotCommands, unifiedRelemInstanceOffsetsbuf, 0, std::span(relemInstanceOffsets));

  unifiedDrawCommandsbuf = ctx.createBuffer(
    etna::Buffer::CreateInfo{
      .size = renderElements.size() * sizeof(vk::DrawIndexedIndirectCommand),
      .bufferUsage = vk::BufferUsageFlagBits::eTransferDst |
        vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eIndirectBuffer,
      .memoryUsage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
      .name = "unifiedDrawCommandsbuf"});

  std::vector<vk::DrawIndexedIndirectCommand> drawCommands;
  drawCommands.reserve(renderElements.size());
  for (uint32_t i = 0; i < renderElements.size(); i++)
  {
    drawCommands.emplace_back(
      vk::DrawIndexedIndirectCommand{
        .indexCount = renderElements[i].indexCount,
        .instanceCount = 0,
        .firstIndex = renderElements[i].indexOffset,
        .vertexOffset = static_cast<std::int32_t>(renderElements[i].vertexOffset),
        .firstInstance = relemInstanceOffsets[i]});
  }

  transferHelper.uploadBuffer<vk::DrawIndexedIndirectCommand>(
    *oneShotCommands, unifiedDrawCommandsbuf, 0, std::span(drawCommands));
}

std::vector<etna::Binding> AssimpSceneManager::getBindlessBindings() const
{
  std::vector<etna::Binding> bindings;
  bindings.reserve(texture2dManager.size() + 1);

  bindings.emplace_back(etna::Binding{0, unifiedMaterialsbuf.genBinding()});

  for (uint32_t i = 0; i < texture2dManager.size(); i++)
  {
    auto& currentTexture = texture2dManager.getResource(static_cast<Texture2D::Id>(i));
    bindings.emplace_back(
      etna::Binding{
        1,
        currentTexture.texture.genBinding(
          defaultSampler.get(), vk::ImageLayout::eShaderReadOnlyOptimal),
        i});
  }

  return bindings;
}

etna::VertexByteStreamFormatDescription AssimpSceneManager::getVertexFormatDescription()
{
  return etna::VertexByteStreamFormatDescription{
    .stride = sizeof(Vertex),
    .attributes = {
      etna::VertexByteStreamFormatDescription::Attribute{
        .format = vk::Format::eR32G32B32A32Sfloat,
        .offset = 0,
      },
      etna::VertexByteStreamFormatDescription::Attribute{
        .format = vk::Format::eR32G32B32A32Sfloat,
        .offset = sizeof(glm::vec4),
      },
      etna::VertexByteStreamFormatDescription::Attribute{
        .format = vk::Format::eR32G32B32A32Uint,
        .offset = 2 * sizeof(glm::vec4),
      },
      etna::VertexByteStreamFormatDescription::Attribute{
        .format = vk::Format::eR32G32B32A32Sfloat,
        .offset = 2 * sizeof(glm::vec4) + sizeof(glm::uvec4),
      },
    }};
}
