#include "AssimpSceneManager.hpp"

#include <glm/ext/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <tracy/Tracy.hpp>
#include <stb_image.h>
#include <assimp/postprocess.h>

#include "etna/RenderTargetStates.hpp"

#include "render_utils/Utilities.hpp"

static glm::mat4x4 mat4_cast(const aiMatrix4x4& mat)
{
  return glm::transpose(glm::make_mat4(&mat.a1));
}

static glm::vec3 vec3_cast(const aiVector3D& vec)
{
  return glm::vec3(vec.x, vec.y, vec.z);
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

void AssimpSceneManager::selectScene(std::filesystem::path path)
{
  ZoneScopedN("assimpSelectScene");

  Assimp::Importer importer;

  importer.SetPropertyInteger(
    AI_CONFIG_PP_SBP_REMOVE, aiPrimitiveType_LINE | aiPrimitiveType_POINT);

  const aiScene* scene = importer.ReadFile(
    path.string(),
    aiProcess_CalcTangentSpace | aiProcess_Triangulate | aiProcess_JoinIdenticalVertices |
      aiProcess_SortByPType | aiProcess_ConvertToLeftHanded);

  if (nullptr == scene)
  {
    spdlog::error("Assimp: Failed to load model with path {}", path.string());
    spdlog::error("Assimp: {}", importer.GetErrorString());
    return;
  }

  generatePlaceholderMaterial();
  processMaterials(scene, path.parent_path() / "textures");

  auto [instMats, instMeshes] = processInstances(scene);
  instanceMatrices = std::move(instMats);

  instanceMeshes = std::move(instMeshes);

  auto [verts, inds, boneMatrices, relems, groups, bounds] = processMeshes(scene);

  meshesBoneMatrices = std::move(boneMatrices);
  renderElements = std::move(relems);
  relemsGroups = std::move(groups);
  renderElementsBounds = std::move(bounds);

  uploadData(verts, inds);
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

AssimpSceneManager::ProcessedInstances AssimpSceneManager::processInstances(
  const aiScene* scene) const
{

  // NOTE - maybe super ineffectient because of four traversals, but trying to minimize
  // reallocations
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

  std::vector nodeTransforms(nodesCount, glm::identity<glm::mat4x4>());

  std::unordered_map<const aiNode*, uint32_t> nodeToIdx;
  {
    std::queue<const aiNode*> nodes;
    std::uint32_t nodeIdx = 0;
    nodes.push(scene->mRootNode);

    while (!nodes.empty())
    {
      const aiNode* node = nodes.front();
      nodes.pop();

      nodeToIdx.emplace(node, nodeIdx);
      ++nodeIdx;

      nodeTransforms[nodeToIdx.at(node)] = mat4_cast(node->mTransformation);

      std::span children = std::span(node->mChildren, node->mChildren + node->mNumChildren);

      for (const auto child : children)
      {
        nodes.push(child);
      }
    }
  }

  {
    std::stack<const aiNode*> nodes;
    nodes.push(scene->mRootNode);

    while (!nodes.empty())
    {
      const aiNode* node = nodes.top();
      nodes.pop();

      std::span children = std::span(node->mChildren, node->mChildren + node->mNumChildren);

      for (const auto child : children)
      {
        nodeTransforms[nodeToIdx.at(child)] =
          nodeTransforms[nodeToIdx.at(node)] * nodeTransforms[nodeToIdx.at(child)];
        nodes.push(child);
      }
    }
  }

  ProcessedInstances result;

  result.matrices.reserve(meshesInstancesCount);
  result.meshes.reserve(meshesInstancesCount);

  {
    std::queue<const aiNode*> nodes;
    nodes.push(scene->mRootNode);

    while (!nodes.empty())
    {
      const aiNode* node = nodes.front();
      nodes.pop();

      for (uint32_t i = 0; i < node->mNumMeshes; i++)
      {
        result.matrices.push_back(
          glm::scale(nodeTransforms[nodeToIdx.at(node)], glm::vec3(0.01, 0.01, 0.01)));
        result.meshes.push_back(node->mMeshes[i]);
      }

      std::span children = std::span(node->mChildren, node->mChildren + node->mNumChildren);

      for (const auto child : children)
      {
        nodes.push(child);
      }
    }
  }

  return result;
}

AssimpSceneManager::ProcessedMeshes AssimpSceneManager::processMeshes(const aiScene* scene) const
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

  result.boneMatrices.resize(scene->mNumMeshes);

  result.relems.reserve(scene->mNumMeshes);
  result.bounds.reserve(scene->mNumMeshes);
  // it seems that meshes is the same as relems in assimp
  // (not the case in gltf where one mesh can have several render elements)
  // leaving it for compatibility with already present rendering
  result.relemsGroup.reserve(scene->mNumMeshes);

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
      const aiVector3D& meshTangent = mesh->mTangents[vertIdx];
      const aiVector3D& meshTexcoord = mesh->mTextureCoords[0][vertIdx];

      auto& vertex = result.vertices.emplace_back();

      vertex.positionAndNormal = glm::vec4(
        meshVertex.x,
        meshVertex.y,
        meshVertex.z,
        std::bit_cast<float>(
          encode_normalized(glm::vec4(glm::vec3(meshNormal.x, meshNormal.y, meshNormal.z), 0))));
      vertex.texCoordAndTangentAndPadding = glm::vec4(
        meshTexcoord.x,
        meshTexcoord.y,
        std::bit_cast<float>(
          encode_normalized(glm::vec4(meshTangent.x, meshTangent.y, meshTangent.z, 1))),
        0);
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

    if (mesh->HasBones())
    {
      result.boneMatrices[meshIdx].reserve(mesh->mNumBones);
      std::vector<uint32_t> weightOffset(mesh->mNumVertices, 0);

      for (uint32_t i = 0; i < mesh->mNumBones; i++)
      {
        const aiBone* bone = mesh->mBones[i];
        result.boneMatrices[meshIdx].push_back(mat4_cast(bone->mOffsetMatrix));

        std::span weights = std::span(bone->mWeights, bone->mWeights + bone->mNumWeights);
        for (const auto& [vertexId, weight] : weights)
        {
          uint32_t offset = weightOffset[vertexId];
          ETNA_VERIFY(offset < 4);
          result.vertices[currentVertexOffset + vertexId].boneWeights[offset] = weight;
          result.vertices[currentVertexOffset + vertexId].boneIds[offset] = i;
        }
      }
    }
  }

  return result;
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

void AssimpSceneManager::updateMatrices([[maybe_unused]] const glm::mat4x4& transform) {}
