#include "SceneManager.hpp"
#include "etna/DescriptorSet.hpp"
#include "render_utils/Timer.hpp"

#include <stack>

#include <stb_image.h>
#include <spdlog/spdlog.h>
#include <fmt/std.h>
#include <tracy/Tracy.hpp>
#include <glm/gtc/type_precision.hpp>

#include <etna/GlobalContext.hpp>
#include <etna/OneShotCmdMgr.hpp>
#include <etna/Etna.hpp>
#include <etna/RenderTargetStates.hpp>
#include <vector>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/string_cast.hpp>

#include "render_utils/Utilities.hpp"


static std::uint32_t encode_normal(glm::vec3 normal)
{
  const std::int32_t x = static_cast<std::int32_t>(normal.x * 32767.0f);
  const std::int32_t y = static_cast<std::int32_t>(normal.y * 32767.0f);

  const std::uint32_t sign = normal.z >= 0 ? 0 : 1;
  const std::uint32_t sx = static_cast<std::uint32_t>(x & 0xfffe) | sign;
  const std::uint32_t sy = static_cast<std::uint32_t>(y & 0xffff) << 16;

  return sx | sy;
}

SceneManager::SceneManager()
  : baseColorPlaceholder(Texture2D::Id::Invalid)
  , metallicRoughnessPlaceholder(Texture2D::Id::Invalid)
  , normalPlaceholder(Texture2D::Id::Invalid)
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

std::optional<tinygltf::Model> SceneManager::loadModel(std::filesystem::path path)
{
  tinygltf::Model model;

  std::string error;
  std::string warning;
  bool success = false;

  auto ext = path.extension();
  if (ext == ".gltf")
    success = loader.LoadASCIIFromFile(&model, &error, &warning, path.string());
  else if (ext == ".glb")
    success = loader.LoadBinaryFromFile(&model, &error, &warning, path.string());
  else
  {
    spdlog::error("glTF: Unknown glTF file extension: '{}'. Expected .gltf or .glb.", ext);
    return std::nullopt;
  }

  if (!success)
  {
    spdlog::error("glTF: Failed to load model!");
    if (!error.empty())
      spdlog::error("glTF: {}", error);
    return std::nullopt;
  }

  if (!warning.empty())
    spdlog::warn("glTF: {}", warning);

  if (
    !model.extensions.empty() || !model.extensionsRequired.empty() || !model.extensionsUsed.empty())
    spdlog::warn("glTF: No glTF extensions are currently implemented!");

  return model;
}

std::vector<vk::Format> SceneManager::parseTextures(const tinygltf::Model& model)
{
  ZoneScopedN("parseTextures");
  std::vector<vk::Format> texturesInfo;
  texturesInfo.resize(model.images.size());

  bool oldExtention = false;
  for (const auto& extention : model.extensionsRequired)
  {
    if (extention == "KHR_materials_pbrSpecularGlossiness")
    {
      oldExtention = true;
      break;
    }
  }

  if (oldExtention)
  {
    for (auto& format : texturesInfo)
    {
      format = vk::Format::eR8G8B8A8Unorm;
    }
    return texturesInfo;
  }

  for (const auto& material : model.materials)
  {
    if (material.pbrMetallicRoughness.baseColorTexture.index != -1)
    {
      texturesInfo[material.pbrMetallicRoughness.baseColorTexture.index] =
        vk::Format::eR8G8B8A8Srgb;
    }
    if (material.pbrMetallicRoughness.metallicRoughnessTexture.index != -1)
    {
      texturesInfo[material.pbrMetallicRoughness.metallicRoughnessTexture.index] =
        vk::Format::eR8G8B8A8Unorm;
    }
    if (material.normalTexture.index != -1)
    {
      texturesInfo[material.normalTexture.index] = vk::Format::eR8G8B8A8Unorm;
    }
  }

  return texturesInfo;
}

void SceneManager::processTextures(
  const tinygltf::Model& model, std::vector<vk::Format> textures_info, std::filesystem::path path)
{
  ZoneScopedN("processTextures");
  auto& ctx = etna::get_context();

  uint32_t layerCount = 1;
  int width, height, channels;
  for (uint32_t i = 0; i < model.images.size(); i++)
  {
    ZoneScoped;
    const auto& currentTextureImage = model.images[i];
    auto format = textures_info[i];

    auto filename = (path / currentTextureImage.uri).generic_string<char>();
    unsigned char* textureData =
      stbi_load(filename.c_str(), &width, &height, &channels, STBI_rgb_alpha);

    // maybe add recovery later
    ETNA_VERIFYF(textureData != nullptr, "Texture {} is not loaded!", currentTextureImage.uri);

    uint32_t mipLevels = static_cast<uint32_t>(std::floor(std::log2(std::max(width, height)))) + 1;

    const vk::DeviceSize textureSize = width * height * 4;
    etna::Buffer textureBuffer = ctx.createBuffer(
      etna::Buffer::CreateInfo{
        .size = textureSize,
        .bufferUsage =
          vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst,
        .name = currentTextureImage.uri + "_buffer",
      });

    auto source = std::span<unsigned char>(textureData, textureSize);
    transferHelper.uploadBuffer(*oneShotCommands, textureBuffer, 0, std::as_bytes(source));

    etna::Image texture = ctx.createImage(
      etna::Image::CreateInfo{
        .extent = vk::Extent3D{static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1},
        .name = currentTextureImage.uri + "_texture",
        .format = format,
        .imageUsage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst |
          vk::ImageUsageFlagBits::eTransferSrc,
        .mipLevels = mipLevels});

    render_utility::local_copy_buffer_to_image(
      *oneShotCommands, textureBuffer, texture, layerCount);

    render_utility::generate_mipmaps_vk_style(*oneShotCommands, texture, mipLevels, layerCount);

    auto id = texture2dManager.loadResource(
      ("texture_" + currentTextureImage.uri).c_str(), {.texture = std::move(texture)});
    spdlog::info(
      "New texture loaded from file {}, texture id = {}",
      currentTextureImage.uri,
      static_cast<uint32_t>(id));

    stbi_image_free(textureData);
  }
}

void SceneManager::processMaterials(const tinygltf::Model& model)
{
  ZoneScopedN("processMaterials");

  bool oldExtention = false;
  for (const auto& extention : model.extensionsRequired)
  {
    if (extention == "KHR_materials_pbrSpecularGlossiness")
    {
      oldExtention = true;
      break;
    }
  }

  if (oldExtention)
  {
    // auto checkValue = [&](const tinygltf::Value& value) -> bool {
    //   return static_cast<int32_t>(value.Type()) != 0;
    // };
    for (const auto& modelMaterial : model.materials)
    {
      Material material;

      auto diffuseFactor =
        modelMaterial.extensions.at("KHR_materials_pbrSpecularGlossiness").Get("diffuseFactor");

      material.baseColorFactor = {
        diffuseFactor.Get(0).GetNumberAsDouble(),
        diffuseFactor.Get(1).GetNumberAsDouble(),
        diffuseFactor.Get(2).GetNumberAsDouble(),
        diffuseFactor.Get(3).GetNumberAsDouble()};

      auto glossinessFactor =
        modelMaterial.extensions.at("KHR_materials_pbrSpecularGlossiness").Get("glossinessFactor");
      material.roughnessFactor = 1.0f - static_cast<float>(glossinessFactor.GetNumberAsDouble());

      material.metallicFactor = 0.0f; // assume

      auto diffuseTexture =
        modelMaterial.extensions.at("KHR_materials_pbrSpecularGlossiness").Get("diffuseTexture");
      if (diffuseTexture.IsObject())
      {
        auto index = diffuseTexture.Get("index");
        material.baseColorTexture = static_cast<Texture2D::Id>(index.GetNumberAsInt());
      }
      else
      {
        if (baseColorPlaceholder == Texture2D::Id::Invalid)
        {
          baseColorPlaceholder = generatePlaceholderTexture(
            "base_color_placeholder", vk::Format::eR8G8B8A8Srgb, {1.0f, 1.0f, 1.0f, 1.0f});
        }
        material.baseColorTexture = baseColorPlaceholder;
      }

      if (metallicRoughnessPlaceholder == Texture2D::Id::Invalid)
      {
        metallicRoughnessPlaceholder = generatePlaceholderTexture(
          "metallic_roughness_placeholder", vk::Format::eR8G8B8A8Unorm, {0.0f, 1.0f, 1.0f, 1.0f});
      }
      material.metallicRoughnessTexture = metallicRoughnessPlaceholder;

      auto normalTextureInfo = modelMaterial.normalTexture;
      {
        if (normalPlaceholder == Texture2D::Id::Invalid)
        {
          normalPlaceholder = generatePlaceholderTexture(
            "normal_placeholder", vk::Format::eR8G8B8A8Snorm, {0.0f, 0.0f, 0.5f, 0.0f});
        }
        material.normalTexture = normalPlaceholder;
      }


      auto id = materialManager.loadResource(
        ("material_" + modelMaterial.name).c_str(), std::move(material));
      spdlog::info(
        "Material loaded, name - {}, material id = {}, used texture ids - [\n"
        "base color - {},\n"
        "metallic/roughness - {},\n"
        "normal - {}\n]",
        modelMaterial.name,
        static_cast<uint32_t>(id),
        static_cast<uint32_t>(material.baseColorTexture),
        static_cast<uint32_t>(material.metallicRoughnessTexture),
        static_cast<uint32_t>(material.normalTexture));
    }
    return;
  }

  for (const auto& modelMaterial : model.materials)
  {
    Material material;

    // always guaranteed by tinygltf loader to have 4 members in baseColorFactor vector
    material.baseColorFactor = {
      modelMaterial.pbrMetallicRoughness.baseColorFactor[0],
      modelMaterial.pbrMetallicRoughness.baseColorFactor[1],
      modelMaterial.pbrMetallicRoughness.baseColorFactor[2],
      modelMaterial.pbrMetallicRoughness.baseColorFactor[3]};

    material.roughnessFactor =
      static_cast<float>(modelMaterial.pbrMetallicRoughness.roughnessFactor);
    material.metallicFactor = static_cast<float>(modelMaterial.pbrMetallicRoughness.metallicFactor);

    // little bit ugly
    if (modelMaterial.pbrMetallicRoughness.baseColorTexture.index != -1)
    {
      material.baseColorTexture =
        static_cast<Texture2D::Id>(modelMaterial.pbrMetallicRoughness.baseColorTexture.index);
    }
    else
    {
      if (baseColorPlaceholder == Texture2D::Id::Invalid)
      {
        baseColorPlaceholder = generatePlaceholderTexture(
          "base_color_placeholder", vk::Format::eR8G8B8A8Srgb, {1.0f, 1.0f, 1.0f, 1.0f});
      }
      material.baseColorTexture = baseColorPlaceholder;
    }

    if (modelMaterial.pbrMetallicRoughness.metallicRoughnessTexture.index != -1)
    {
      material.metallicRoughnessTexture = static_cast<Texture2D::Id>(
        modelMaterial.pbrMetallicRoughness.metallicRoughnessTexture.index);
    }
    else
    {
      if (metallicRoughnessPlaceholder == Texture2D::Id::Invalid)
      {
        metallicRoughnessPlaceholder = generatePlaceholderTexture(
          "metallic_roughness_placeholder", vk::Format::eR8G8B8A8Unorm, {0.0f, 1.0f, 1.0f, 1.0f});
      }
      material.metallicRoughnessTexture = metallicRoughnessPlaceholder;
    }

    if (modelMaterial.normalTexture.index != -1)
    {

      material.normalTexture = static_cast<Texture2D::Id>(modelMaterial.normalTexture.index);
    }
    else
    {
      if (normalPlaceholder == Texture2D::Id::Invalid)
      {
        normalPlaceholder = generatePlaceholderTexture(
          "normal_placeholder", vk::Format::eR8G8B8A8Snorm, {0.0f, 0.0f, 0.5f, 0.0f});
      }
      material.normalTexture = normalPlaceholder;
    }

    auto id =
      materialManager.loadResource(("material_" + modelMaterial.name).c_str(), std::move(material));
    spdlog::info(
      "Material loaded, name - {}, material id = {}, used texture ids - [\n"
      "\tbase color - {},\n"
      "\tmetallic/roughness - {},\n"
      "\tnormal - {}\n]",
      modelMaterial.name,
      static_cast<uint32_t>(id),
      static_cast<uint32_t>(material.baseColorTexture),
      static_cast<uint32_t>(material.metallicRoughnessTexture),
      static_cast<uint32_t>(material.normalTexture));
  }
}

Texture2D::Id SceneManager::generatePlaceholderTexture(
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

void SceneManager::generatePlaceholderMaterial()
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

SceneManager::ProcessedInstances SceneManager::processInstances(const tinygltf::Model& model) const
{
  std::vector nodeTransforms(model.nodes.size(), glm::identity<glm::mat4x4>());

  for (std::size_t nodeIdx = 0; nodeIdx < model.nodes.size(); ++nodeIdx)
  {
    const auto& node = model.nodes[nodeIdx];
    auto& transform = nodeTransforms[nodeIdx];

    if (!node.matrix.empty())
    {
      for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
          transform[i][j] = static_cast<float>(node.matrix[4 * i + j]);
    }
    else
    {
      if (!node.scale.empty())
        transform = scale(
          transform,
          glm::vec3(
            static_cast<float>(node.scale[0]),
            static_cast<float>(node.scale[1]),
            static_cast<float>(node.scale[2])));

      if (!node.rotation.empty())
        transform *= mat4_cast(
          glm::quat(
            static_cast<float>(node.rotation[3]),
            static_cast<float>(node.rotation[0]),
            static_cast<float>(node.rotation[1]),
            static_cast<float>(node.rotation[2])));

      if (!node.translation.empty())
        transform = translate(
          transform,
          glm::vec3(
            static_cast<float>(node.translation[0]),
            static_cast<float>(node.translation[1]),
            static_cast<float>(node.translation[2])));
    }
  }

  std::stack<std::size_t> vertices;
  for (auto vert : model.scenes[model.defaultScene].nodes)
    vertices.push(vert);

  while (!vertices.empty())
  {
    auto vert = vertices.top();
    vertices.pop();

    for (auto child : model.nodes[vert].children)
    {
      nodeTransforms[child] = nodeTransforms[vert] * nodeTransforms[child];
      vertices.push(child);
    }
  }

  ProcessedInstances result;

  // Don't overallocate matrices, they are pretty chonky.
  {
    std::size_t totalNodesWithMeshes = 0;
    for (std::size_t i = 0; i < model.nodes.size(); ++i)
      if (model.nodes[i].mesh >= 0)
        ++totalNodesWithMeshes;
    result.matrices.reserve(totalNodesWithMeshes);
    result.meshes.reserve(totalNodesWithMeshes);
  }

  for (std::size_t i = 0; i < model.nodes.size(); ++i)
    if (model.nodes[i].mesh >= 0)
    {
      result.matrices.push_back(nodeTransforms[i]);
      result.meshes.push_back(model.nodes[i].mesh);
    }

  return result;
}

SceneManager::ProcessedMeshes SceneManager::processMeshes(const tinygltf::Model& model) const
{
  // NOTE: glTF assets can have pretty wonky data layouts which are not appropriate
  // for real-time rendering, so we have to press the data first. In serious engines
  // this is mitigated by storing assets on the disc in an engine-specific format that
  // is appropriate for GPU upload right after reading from disc.

  ProcessedMeshes result;

  // Pre-allocate enough memory so as not to hit the
  // allocator on the memcpy hotpath
  {
    std::size_t vertexBytes = 0;
    std::size_t indexBytes = 0;
    for (const auto& bufView : model.bufferViews)
    {
      switch (bufView.target)
      {
      case TINYGLTF_TARGET_ARRAY_BUFFER:
        vertexBytes += bufView.byteLength;
        break;
      case TINYGLTF_TARGET_ELEMENT_ARRAY_BUFFER:
        indexBytes += bufView.byteLength;
        break;
      default:
        break;
      }
    }
    result.vertices.reserve(vertexBytes / sizeof(Vertex));
    result.indices.reserve(indexBytes / sizeof(std::uint32_t));
  }

  {
    std::size_t totalPrimitives = 0;
    for (const auto& mesh : model.meshes)
      totalPrimitives += mesh.primitives.size();
    result.relems.reserve(totalPrimitives);
    result.bounds.reserve(totalPrimitives);
  }

  result.meshes.reserve(model.meshes.size());

  for (const auto& mesh : model.meshes)
  {
    result.meshes.push_back(
      Mesh{
        .firstRelem = static_cast<std::uint32_t>(result.relems.size()),
        .relemCount = static_cast<std::uint32_t>(mesh.primitives.size()),
      });

    for (const auto& prim : mesh.primitives)
    {
      if (prim.mode != TINYGLTF_MODE_TRIANGLES)
      {
        spdlog::warn(
          "Encountered a non-triangles primitive, these are not supported for now, skipping it!");
        --result.meshes.back().relemCount;
        continue;
      }

      const auto normalIt = prim.attributes.find("NORMAL");
      const auto tangentIt = prim.attributes.find("TANGENT");
      const auto texcoordIt = prim.attributes.find("TEXCOORD_0");

      const bool hasNormals = normalIt != prim.attributes.end();
      const bool hasTangents = tangentIt != prim.attributes.end();
      const bool hasTexcoord = texcoordIt != prim.attributes.end();
      std::array accessorIndices{
        prim.indices,
        prim.attributes.at("POSITION"),
        hasNormals ? normalIt->second : -1,
        hasTangents ? tangentIt->second : -1,
        hasTexcoord ? texcoordIt->second : -1,
      };

      std::array accessors{
        &model.accessors[prim.indices],
        &model.accessors[accessorIndices[1]],
        hasNormals ? &model.accessors[accessorIndices[2]] : nullptr,
        hasTangents ? &model.accessors[accessorIndices[3]] : nullptr,
        hasTexcoord ? &model.accessors[accessorIndices[4]] : nullptr,
      };

      std::array bufViews{
        &model.bufferViews[accessors[0]->bufferView],
        &model.bufferViews[accessors[1]->bufferView],
        hasNormals ? &model.bufferViews[accessors[2]->bufferView] : nullptr,
        hasTangents ? &model.bufferViews[accessors[3]->bufferView] : nullptr,
        hasTexcoord ? &model.bufferViews[accessors[4]->bufferView] : nullptr,
      };

      result.relems.push_back(
        RenderElement{
          .vertexOffset = static_cast<std::uint32_t>(result.vertices.size()),
          .indexOffset = static_cast<std::uint32_t>(result.indices.size()),
          .indexCount = static_cast<std::uint32_t>(accessors[0]->count),
          .material = static_cast<Material::Id>(prim.material)});


      const auto& positionAccessor = accessors[1];
      glm::vec4 minPos = {
        positionAccessor->minValues[0],
        positionAccessor->minValues[1],
        positionAccessor->minValues[2],
        0};
      glm::vec4 maxPos = {
        positionAccessor->maxValues[0],
        positionAccessor->maxValues[1],
        positionAccessor->maxValues[2],
        0};

      result.bounds.push_back(Bounds{minPos, maxPos});

      const std::size_t vertexCount = accessors[1]->count;

      std::array ptrs{
        reinterpret_cast<const std::byte*>(model.buffers[bufViews[0]->buffer].data.data()) +
          bufViews[0]->byteOffset + accessors[0]->byteOffset,
        reinterpret_cast<const std::byte*>(model.buffers[bufViews[1]->buffer].data.data()) +
          bufViews[1]->byteOffset + accessors[1]->byteOffset,
        hasNormals
          ? reinterpret_cast<const std::byte*>(model.buffers[bufViews[2]->buffer].data.data()) +
            bufViews[2]->byteOffset + accessors[2]->byteOffset
          : nullptr,
        hasTangents
          ? reinterpret_cast<const std::byte*>(model.buffers[bufViews[3]->buffer].data.data()) +
            bufViews[3]->byteOffset + accessors[3]->byteOffset
          : nullptr,
        hasTexcoord
          ? reinterpret_cast<const std::byte*>(model.buffers[bufViews[4]->buffer].data.data()) +
            bufViews[4]->byteOffset + accessors[4]->byteOffset
          : nullptr,
      };

      std::array strides{
        bufViews[0]->byteStride != 0
          ? bufViews[0]->byteStride
          : tinygltf::GetComponentSizeInBytes(accessors[0]->componentType) *
            tinygltf::GetNumComponentsInType(accessors[0]->type),
        bufViews[1]->byteStride != 0
          ? bufViews[1]->byteStride
          : tinygltf::GetComponentSizeInBytes(accessors[1]->componentType) *
            tinygltf::GetNumComponentsInType(accessors[1]->type),
        hasNormals ? (bufViews[2]->byteStride != 0
                        ? bufViews[2]->byteStride
                        : tinygltf::GetComponentSizeInBytes(accessors[2]->componentType) *
                          tinygltf::GetNumComponentsInType(accessors[2]->type))
                   : 0,
        hasTangents ? (bufViews[3]->byteStride != 0
                         ? bufViews[3]->byteStride
                         : tinygltf::GetComponentSizeInBytes(accessors[3]->componentType) *
                           tinygltf::GetNumComponentsInType(accessors[3]->type))
                    : 0,
        hasTexcoord ? (bufViews[4]->byteStride != 0
                         ? bufViews[4]->byteStride
                         : tinygltf::GetComponentSizeInBytes(accessors[4]->componentType) *
                           tinygltf::GetNumComponentsInType(accessors[4]->type))
                    : 0,
      };

      for (std::size_t i = 0; i < vertexCount; ++i)
      {
        auto& vtx = result.vertices.emplace_back();
        glm::vec3 pos;
        // Fall back to 0 in case we don't have something.
        // NOTE: if tangents are not available, one could use http://mikktspace.com/
        // NOTE: if normals are not available, reconstructing them is possible but will look ugly
        glm::vec3 normal{0};
        glm::vec3 tangent{0};
        glm::vec2 texcoord{0};
        std::memcpy(&pos, ptrs[1], sizeof(pos));

        // NOTE: it's faster to do a template here with specializations for all combinations than to
        // do ifs at runtime. Also, SIMD should be used. Try implementing this!
        if (hasNormals)
          std::memcpy(&normal, ptrs[2], sizeof(normal));
        if (hasTangents)
          std::memcpy(&tangent, ptrs[3], sizeof(tangent));
        if (hasTexcoord)
          std::memcpy(&texcoord, ptrs[4], sizeof(texcoord));


        vtx.positionAndNormal = glm::vec4(pos, std::bit_cast<float>(encode_normal(normal)));
        vtx.texCoordAndTangentAndPadding =
          glm::vec4(texcoord, std::bit_cast<float>(encode_normal(tangent)), 0);

        ptrs[1] += strides[1];
        if (hasNormals)
          ptrs[2] += strides[2];
        if (hasTangents)
          ptrs[3] += strides[3];
        if (hasTexcoord)
          ptrs[4] += strides[4];
      }

      // Indices are guaranteed to have no stride
      ETNA_VERIFY(bufViews[0]->byteStride == 0);
      const std::size_t indexCount = accessors[0]->count;
      if (accessors[0]->componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT)
      {
        for (std::size_t i = 0; i < indexCount; ++i)
        {
          std::uint16_t index;
          std::memcpy(&index, ptrs[0], sizeof(index));
          result.indices.push_back(index);
          ptrs[0] += 2;
        }
      }
      else if (accessors[0]->componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT)
      {
        const std::size_t lastTotalIndices = result.indices.size();
        result.indices.resize(lastTotalIndices + indexCount);
        std::memcpy(
          result.indices.data() + lastTotalIndices,
          ptrs[0],
          sizeof(result.indices[0]) * indexCount);
      }
    }
  }

  return result;
}

SceneManager::BakedMeshes SceneManager::processBakedMeshes(const tinygltf::Model& model) const
{

  BakedMeshes result;

  {
    std::size_t totalPrimitives = 0;
    for (const auto& mesh : model.meshes)
      totalPrimitives += mesh.primitives.size();
    result.relems.reserve(totalPrimitives);
    result.bounds.reserve(totalPrimitives);
  }

  result.meshes.reserve(model.meshes.size());

  for (const auto& mesh : model.meshes)
  {
    result.meshes.push_back(
      Mesh{
        .firstRelem = static_cast<std::uint32_t>(result.relems.size()),
        .relemCount = static_cast<std::uint32_t>(mesh.primitives.size()),
      });

    for (const auto& prim : mesh.primitives)
    {
      if (prim.mode != TINYGLTF_MODE_TRIANGLES)
      {
        spdlog::warn(
          "Encountered a non-triangles primitive, these are not supported for now, skipping it!");
        --result.meshes.back().relemCount;
        continue;
      }

      auto& indicesAccessor = model.accessors[prim.indices];
      auto& vertexAccessor = model.accessors[prim.attributes.at("POSITION")];

      result.relems.push_back(
        RenderElement{
          .vertexOffset = static_cast<uint32_t>(vertexAccessor.byteOffset / sizeof(Vertex)),
          .indexOffset = static_cast<uint32_t>(indicesAccessor.byteOffset / sizeof(uint32_t)),
          .indexCount = static_cast<uint32_t>(indicesAccessor.count),
          .material = static_cast<Material::Id>(prim.material)});

      glm::vec4 minPos = {
        vertexAccessor.minValues[0], vertexAccessor.minValues[1], vertexAccessor.minValues[2], 0};
      glm::vec4 maxPos = {
        vertexAccessor.maxValues[0], vertexAccessor.maxValues[1], vertexAccessor.maxValues[2], 0};

      result.bounds.push_back(Bounds{minPos, maxPos});

      auto positionBufferView = model.bufferViews[vertexAccessor.bufferView];
      auto positionPtr =
        reinterpret_cast<const std::byte*>(model.buffers[positionBufferView.buffer].data.data()) +
        positionBufferView.byteOffset + vertexAccessor.byteOffset;
      auto positionStride = positionBufferView.byteStride != 0
        ? positionBufferView.byteStride
        : tinygltf::GetComponentSizeInBytes(vertexAccessor.componentType) *
          tinygltf::GetNumComponentsInType(vertexAccessor.type);

      for (std::size_t i = 0; i < vertexAccessor.count; i++)
      {
        glm::vec3 pos;
        std::memcpy(&pos, positionPtr, sizeof(pos));

        positionPtr += positionStride;
      }
    }

    auto buffer = model.buffers[0].data.data();
    auto indicesAmount = model.bufferViews[0].byteLength / sizeof(uint32_t);
    auto vertexAmount = model.bufferViews[1].byteLength / sizeof(Vertex);

    result.indices = std::span(reinterpret_cast<const uint32_t*>(buffer), indicesAmount);
    result.vertices = std::span(
      reinterpret_cast<const Vertex*>(buffer + sizeof(uint32_t) * indicesAmount), vertexAmount);
  }

  return result;
}

void SceneManager::uploadData(
  std::span<const Vertex> vertices, std::span<const std::uint32_t> indices)
{
  auto& ctx = etna::get_context();

  unifiedVbuf = ctx.createBuffer(
    etna::Buffer::CreateInfo{
      .size = vertices.size_bytes(),
      .bufferUsage = vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eVertexBuffer,
      .memoryUsage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
      .name = "unifiedVbuf",
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


  unifiedRelemsbuf = ctx.createBuffer(
    etna::Buffer::CreateInfo{
      .size = renderElements.size() * sizeof(RenderElementGLSLCompat),
      .bufferUsage =
        vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eStorageBuffer,
      .memoryUsage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
      .name = "unifiedRelemsbuf"});

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
        .material = static_cast<std::uint32_t>(relem.material)});
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
      .size = meshes.size() * sizeof(Mesh),
      .bufferUsage =
        vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eStorageBuffer,
      .memoryUsage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
      .name = "unifiedMeshesbuf"});
  unifiedInstanceMatricesbuf = ctx.createBuffer(
    etna::Buffer::CreateInfo{
      .size = instanceMatrices.size() * sizeof(glm::mat4x4),
      .bufferUsage =
        vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eStorageBuffer,
      .memoryUsage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
      .name = "unifiedInstanceMatricesbuf"});
  unifiedInstanceMeshesbuf = ctx.createBuffer(
    etna::Buffer::CreateInfo{
      .size = instanceMeshes.size() * sizeof(std::uint32_t),
      .bufferUsage =
        vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eStorageBuffer,
      .memoryUsage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
      .name = "unifiedInstanceMeshesbuf"});

  transferHelper.uploadBuffer<Bounds>(
    *oneShotCommands, unifiedBoundsbuf, 0, std::span(renderElementsBounds));
  transferHelper.uploadBuffer<Mesh>(*oneShotCommands, unifiedMeshesbuf, 0, std::span(meshes));

  // TODO - fix hardcode in matrices init
  updateMatrices(glm::translate(glm::identity<glm::mat4>(), glm::vec3(45, -20, -100)));

  transferHelper.uploadBuffer<std::uint32_t>(
    *oneShotCommands, unifiedInstanceMeshesbuf, 0, std::span(instanceMeshes));

  // filled on GPU when culling
  unifiedDrawInstanceIndicesbuf = ctx.createBuffer(
    etna::Buffer::CreateInfo{
      .size = instanceMeshes.size() * sizeof(std::uint32_t),
      .bufferUsage =
        vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eStorageBuffer,
      .memoryUsage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
      .name = "unifiedDrawInstanceIndicesbuf"});

  unifiedRelemInstanceOffsetsbuf = ctx.createBuffer(
    etna::Buffer::CreateInfo{
      .size = renderElements.size() * sizeof(std::uint32_t),
      .bufferUsage =
        vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eStorageBuffer,
      .memoryUsage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
      .name = "unifiedRelemInstanceOffsetsbuf"});

  std::vector<std::uint32_t> relemInstanceOffsets(renderElements.size(), 0);
  // calculate total amounts first
  for (const auto& meshIdx : instanceMeshes)
  {
    const auto& currentMesh = meshes[meshIdx];
    for (std::uint32_t relemIdx = currentMesh.firstRelem;
         relemIdx < currentMesh.firstRelem + currentMesh.relemCount;
         relemIdx++)
    {
      relemInstanceOffsets[relemIdx]++;
    }
  }

  // then convert amounts to respective offsets
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

void SceneManager::selectScene(std::filesystem::path path)
{
  auto maybeModel = loadModel(path);
  if (!maybeModel.has_value())
    return;

  auto model = std::move(*maybeModel);

  processTextures(model, parseTextures(model), path.parent_path());
  processMaterials(model);
  generatePlaceholderMaterial();

  // By aggregating all SceneManager fields mutations here,
  // we guarantee that we don't forget to clear something
  // when re-loading a scene.

  // NOTE: you might want to store these on the GPU for GPU-driven rendering.
  auto [instMats, instMeshes] = processInstances(model);
  instanceMatrices = std::move(instMats);
  instanceMeshes = std::move(instMeshes);

  auto [verts, inds, relems, meshs, bounds] = processMeshes(model);

  renderElements = std::move(relems);
  meshes = std::move(meshs);
  renderElementsBounds = std::move(bounds);

  uploadData(verts, inds);
}

void SceneManager::selectBakedScene(std::filesystem::path path)
{
  ZoneScopedN("selectBakedScene");

  Timer timer;
  TIMER_START(timer, loadModel)
  auto maybeModel = loadModel(path);
  if (!maybeModel.has_value())
    return;
  TIMER_END(timer)

  auto model = std::move(*maybeModel);

  processTextures(model, parseTextures(model), path.parent_path());
  processMaterials(model);
  generatePlaceholderMaterial();

  auto [instMats, instMeshes] = processInstances(model);
  instanceMatrices = std::move(instMats);

  // TODO - fix hardcode for lighthouse model
  float scale = 0.5f;
  for (auto& matrix : instanceMatrices)
  {
    matrix[0][0] *= scale;
    matrix[1][1] *= scale;
    matrix[2][2] *= scale;
  }

  instanceMeshes = std::move(instMeshes);

  auto [verts, inds, relems, meshs, bounds] = processBakedMeshes(model);

  renderElements = std::move(relems);
  meshes = std::move(meshs);
  renderElementsBounds = std::move(bounds);

  uploadData(verts, inds);
}

std::vector<etna::Binding> SceneManager::getBindlessBindings() const
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

etna::VertexByteStreamFormatDescription SceneManager::getVertexFormatDescription()
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
    }};
}

void SceneManager::updateMatrices(const glm::mat4& transform)
{
  for (auto& matrix : instanceMatrices)
  {
    matrix[3][0] = transform[3][0];
    matrix[3][1] = transform[3][1];
    matrix[3][2] = transform[3][2];
  }

  transferHelper.uploadBuffer<glm::mat4x4>(
    *oneShotCommands, unifiedInstanceMatricesbuf, 0, std::span(instanceMatrices));
}
