#include "Baker.hpp"

extern "C"
{
#include "mikktspace.h"
}

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/basic_file_sink.h>

#define GLM_ENABLE_EXPERIMENTAL
#include "glm/gtx/string_cast.hpp"

#include "etna/Assert.hpp"


#define DEBUG_FILE_WRITE 0

static auto logger = spdlog::basic_logger_mt("file_logger", "logs.txt");


static uint32_t encodeNormalized(glm::vec4 normal)
{
  glm::float32_t scale = 127.0;
  int32_t newX = (std::lround(normal.x * scale) & 0x000000ff);
  int32_t newY = ((std::lround(normal.y * scale) & 0x000000ff) << 8);
  int32_t newZ = ((std::lround(normal.z * scale) & 0x000000ff) << 16);
  int32_t newW = ((std::lround(normal.w * scale) & 0x000000ff) << 24);

  int32_t res = newX | newY | newZ | newW;

  return std::bit_cast<uint32_t>(res);
}

static glm::vec4 decodeNormalized(uint32_t normal)
{
  const uint32_t aEncX = (normal & 0x000000ff);
  const uint32_t aEncY = ((normal & 0x0000ff00) >> 8);
  const uint32_t aEncZ = ((normal & 0x00ff0000) >> 16);
  const uint32_t aEncW = ((normal & 0xff000000) >> 24);

  glm::ivec4 intEnc = {aEncX, aEncY, aEncZ, aEncW};
  intEnc = ((intEnc + 128) % 256) - 128;
  glm::vec4 trueEnc = glm::vec4(intEnc);

  return glm::max(trueEnc / 127.0f, glm::vec4(-1.0f));
}


Baker::Baker(std::filesystem::path path)
  : filepath(path)
{
}

void Baker::run()
{
  auto maybeModel = loadFile();
  if (!maybeModel.has_value())
  {
    return;
  }

  auto model = *maybeModel;

  checkModelSuitability(model);

  auto meshes = processMeshes(model);
  if (reconstructTangents)
  {
    calculateTangents(meshes);
  }

  // makeIntermediateModel(model, meshes);

  auto bakedMeshes = bakeMeshes(meshes);
  // reconstructs tangents of ALL privitives by using packed normal (which I think is bad)
  // TODO: make reconstruction for individual primitives only
  // TODO: use actual normals, not packed

  changeBuffer(model, bakedMeshes);
  changeBufferViews(model, bakedMeshes);
  changeAccessors(model, bakedMeshes);

  saveFormatted(model);
}

void Baker::checkModelSuitability(tinygltf::Model& model)
{
  for (auto& image : model.images)
  {
    // image.name = image.uri;
    auto path = std::filesystem::path(image.uri);
    image.name = std::filesystem::path(image.uri).replace_extension().generic_string<char>();
    if (path.extension() == ".jpg")
    {
      image.mimeType = "image/jpeg";
    }
    else if (path.extension() == ".jpeg")
    {
      spdlog::warn(
        "Tinygltf does not support .jpeg images - {}, changing extention to .jpg", image.uri);
      path.extension() = ".jpg";
      // image.name = path.filename().generic_string<char>();
      image.mimeType = "image/jpeg";
    }
    else if (path.extension() == ".png")
    {
      image.mimeType = "image/png";
    }
    else if (path.extension() == ".bmp")
    {
      image.mimeType = "image/bmp";
    }
    else if (path.extension() == ".gif")
    {
      image.mimeType = "image/gif";
    }
    image.uri = "";
  }
}

std::optional<tinygltf::Model> Baker::loadFile()
{
  auto fileExt = filepath.extension();
  if (fileExt != ".gltf")
  {
    spdlog::error("glTF: Unknown glTF file extension: '{}'. Expected .gltf.");
    return std::nullopt;
  }

  tinygltf::Model model;

  std::string error;
  std::string warning;
  bool success = false;

  success = loader.LoadASCIIFromFile(&model, &error, &warning, filepath.string().c_str());

  if (!success)
  {
    spdlog::error("glTF: Failed to load model!");
    if (!error.empty())
      spdlog::error("glTF: {}", error);
  }

  if (!warning.empty())
    spdlog::warn("glTF: {}", warning);

  model.extensionsUsed.push_back("KHR_mesh_quantization");
  model.extensionsRequired.push_back("KHR_mesh_quantization");

  if (
    !model.extensions.empty() || !model.extensionsRequired.empty() || !model.extensionsUsed.empty())
    spdlog::warn("glTF: No glTF extensions are currently implemented!");

  return model;
}

Baker::Meshes Baker::processMeshes(const tinygltf::Model& model)
{
  Meshes result;
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
    result.vertices.reserve(vertexBytes / sizeof(RealVertex));
    result.indices.reserve(indexBytes / sizeof(std::uint32_t));
  }

  {
    std::size_t totalPrimitives = 0;
    for (const auto& mesh : model.meshes)
      totalPrimitives += mesh.primitives.size();
    result.relems.reserve(totalPrimitives);
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
          "Encountered a non-triangles primitive, these are not "
          "supported for now, skipping it!");
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

      const std::size_t vertexCount = accessors[1]->count;

      result.relems.push_back(
        RenderElement{
          .vertexOffset = static_cast<std::uint32_t>(result.vertices.size()),
          .vertexCount = static_cast<std::uint32_t>(vertexCount),
          .indexOffset = static_cast<std::uint32_t>(result.indices.size()),
          .indexCount = static_cast<std::uint32_t>(accessors[0]->count),
          .positionMinMax = std::array{accessors[1]->minValues, accessors[1]->maxValues},
          .texcoordMinMax =
            (hasTexcoord
               ? ((!accessors[4]->minValues.empty() && !accessors[4]->maxValues.empty())
                    ? std::optional(std::array{accessors[4]->minValues, accessors[4]->maxValues})
                    : std::nullopt)
               : std::nullopt)});

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
        glm::vec3 normal{0};
        glm::vec4 tangent{0, 0, 1, 1};
        glm::vec2 texcoord{0};
        std::memcpy(&pos, ptrs[1], sizeof(pos));

        // TODO: it's faster to do a template here with specializations for all
        // combinations than to do ifs at runtime. Also, SIMD should be used.
        // Try implementing this!
        if (hasNormals)
          std::memcpy(&normal, ptrs[2], sizeof(normal));
        if (hasTangents)
        {
          std::memcpy(&tangent, ptrs[3], sizeof(tangent));
        }
        else
        {
          reconstructTangents = true;
        }
        if (hasTexcoord)
          std::memcpy(&texcoord, ptrs[4], sizeof(texcoord));

        vtx.position = pos;
        vtx.normal = normal;
        vtx.tangent = tangent;
        vtx.texCoord = texcoord;

        ptrs[1] += strides[1];
        if (hasNormals)
          ptrs[2] += strides[2];
        if (hasTangents)
          ptrs[3] += strides[3];
        if (hasTexcoord)
          ptrs[4] += strides[4];
      }

      if (bufViews[0]->byteStride != 0)
      {
        spdlog::error("Something is wrong. Indices byte stride should be 0!");
      }
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

  spdlog::info(
    "Vertex processing complete! Tangent reconstruction needed? - {}", reconstructTangents);

  return result;
}

Baker::BakedMeshes Baker::bakeMeshes(const Meshes& meshes)
{
  BakedMeshes result;

  result.vertices.reserve(meshes.vertices.size());
  result.indices.reserve(meshes.indices.size());
  result.relems.reserve(meshes.relems.size());
  result.meshes.reserve(meshes.meshes.size());

  for (const auto& mesh : meshes.meshes)
  {
    result.meshes.push_back(
      Mesh{
        .firstRelem = static_cast<std::uint32_t>(result.relems.size()),
        .relemCount = static_cast<std::uint32_t>(mesh.relemCount),
      });

    for (uint32_t i = mesh.firstRelem; i < mesh.firstRelem + mesh.relemCount; i++)
    {
      const auto& currentRelem = meshes.relems[i];

      result.relems.push_back(
        RenderElement{
          .vertexOffset = static_cast<std::uint32_t>(result.vertices.size()),
          .vertexCount = static_cast<std::uint32_t>(currentRelem.vertexCount),
          .indexOffset = static_cast<std::uint32_t>(result.indices.size()),
          .indexCount = static_cast<std::uint32_t>(currentRelem.indexCount),
          .positionMinMax = currentRelem.positionMinMax,
          .texcoordMinMax = currentRelem.texcoordMinMax});

      for (uint32_t j = currentRelem.vertexOffset;
           j < currentRelem.vertexCount + currentRelem.vertexOffset;
           j++)
      {
        const auto& realVertex = meshes.vertices[j];
        auto& vtx = result.vertices.emplace_back();

        vtx.positionAndNormal = glm::vec4(
          realVertex.position,
          std::bit_cast<float>(encodeNormalized(glm::vec4(realVertex.normal, 0))));
        vtx.texCoordAndTangentAndPadding = glm::vec4(
          realVertex.texCoord, std::bit_cast<float>(encodeNormalized(realVertex.tangent)), 0);
      }

      for (uint32_t j = currentRelem.indexOffset;
           j < currentRelem.indexCount + currentRelem.indexOffset;
           j++)
      {
        result.indices.push_back(meshes.indices[j]);
      }
    }
  }
  ETNA_VERIFYF(
    result.vertices.size() == meshes.vertices.size(),
    "Incorrect amount of vertices!, result.vertices.size() = {}, meshes.vertices.size() = {}",
    result.vertices.size(),
    meshes.vertices.size());
  ETNA_VERIFYF(
    result.indices.size() == meshes.indices.size(),
    "Incorrect amount of indices!, result.indices.size() = {}, meshes.indices.size() = {}",
    result.indices.size(),
    meshes.indices.size());
  ETNA_VERIFYF(
    result.relems.size() == meshes.relems.size(),
    "Incorrect amount of relems!,result.relems.size() = {}, meshes.relems.size() = {}",
    result.relems.size(),
    meshes.relems.size());
  ETNA_VERIFYF(
    result.meshes.size() == meshes.meshes.size(),
    "Incorrect amount of meshes!, result.meshes.size() = {}, meshes.meshes.size() = {}",
    result.meshes.size(),
    meshes.meshes.size());

  spdlog::info("Vertex baking complete!");
  return result;
}

void Baker::calculateTangents(Meshes& mesh)
{
  auto getNumFaces = [](const SMikkTSpaceContext* context) -> int32_t {
    auto* meshData = static_cast<Meshes*>(context->m_pUserData);
    float floatSize = static_cast<float>(meshData->indices.size() / 3.0f);
    int32_t integerSize = static_cast<int32_t>(meshData->indices.size() / 3);
    ETNA_VERIFYF(
      (floatSize - static_cast<float>(integerSize)) == 0.0f,
      "Wrong amount of faces!, float size - {}, int size - {}",
      floatSize,
      integerSize);
    if (DEBUG_FILE_WRITE)
    {
      logger->info("number of faces - {}", integerSize);
    }
    return integerSize;
  };

  auto genNumVerticesOfFace = [](
                                [[maybe_unused]] const SMikkTSpaceContext* context,
                                [[maybe_unused]] const int32_t face_index) -> int32_t {
    return 3; // only triangles
  };

  auto getPosition = [](
                       const SMikkTSpaceContext* context,
                       float position_out[],
                       const int32_t face_index,
                       const int32_t vertex_index) -> void {
    auto* meshData = static_cast<Meshes*>(context->m_pUserData);
    auto index = meshData->indices[face_index * 3 + vertex_index];
    const auto& position = meshData->vertices[index].position;
    position_out[0] = position.x;
    position_out[1] = position.y;
    position_out[2] = position.z;
    if (DEBUG_FILE_WRITE)
    {
      logger->info("vertex index - {}, position - {}", index, glm::to_string(position));
    }
  };

  auto getNormal = [](
                     const SMikkTSpaceContext* context,
                     float normal_out[],
                     const int32_t face_index,
                     const int32_t vertex_index) -> void {
    auto* meshData = static_cast<Meshes*>(context->m_pUserData);
    auto index = meshData->indices[face_index * 3 + vertex_index];
    const auto& normal = meshData->vertices[index].normal;
    normal_out[0] = normal.x;
    normal_out[1] = normal.y;
    normal_out[2] = normal.z;
    if (DEBUG_FILE_WRITE)
    {
      logger->info("vertex index - {}, normal - {}", index, glm::to_string(normal));
    }
  };

  auto getTexCoord = [](
                       const SMikkTSpaceContext* context,
                       float tex_coord_out[],
                       const int32_t face_index,
                       const int32_t vertex_index) -> void {
    auto* meshData = static_cast<Meshes*>(context->m_pUserData);
    auto index = meshData->indices[face_index * 3 + vertex_index];
    const auto& texCoord = meshData->vertices[index].texCoord;
    tex_coord_out[0] = texCoord.x;
    tex_coord_out[1] = texCoord.y;
    if (DEBUG_FILE_WRITE)
    {
      logger->info("vertex index - {}, texcoord - {}", index, glm::to_string(texCoord));
    }
  };

  auto setTSpaceBasic = [](
                          const SMikkTSpaceContext* context,
                          const float tangent[],
                          const float sign,
                          const int32_t face_index,
                          const int32_t vertex_index) -> void {
    auto* meshData = static_cast<Meshes*>(context->m_pUserData);
    auto index = meshData->indices[face_index * 3 + vertex_index];
    glm::vec3 tangentVec(tangent[0], tangent[1], tangent[2]);
    meshData->vertices[index].tangent = glm::vec4(tangentVec, sign);
    if (DEBUG_FILE_WRITE)
    {
      logger->info(
        "vertex index - {}, tangent - {}, sign - {}", index, glm::to_string(tangentVec), sign);
    }
  };

  SMikkTSpaceInterface interface = {
    .m_getNumFaces = getNumFaces,
    .m_getNumVerticesOfFace = genNumVerticesOfFace,
    .m_getPosition = getPosition,
    .m_getNormal = getNormal,
    .m_getTexCoord = getTexCoord,
    .m_setTSpaceBasic = setTSpaceBasic,
    .m_setTSpace = nullptr};

  SMikkTSpaceContext context = {.m_pInterface = &interface, .m_pUserData = &mesh};

  auto result = genTangSpaceDefault(&context);
  ETNA_VERIFYF(result != 0, "Tangent space construction error!");
  spdlog::info("Tangent space construction complete!");
}

void Baker::makeIntermediateModel(const tinygltf::Model& old_model, Meshes& meshes)
{
  auto model = old_model;

  auto name = filepath.parent_path();
  name /= filepath.stem();
  auto newFile = name.string() + "_tangent_fix.bin";

  std::size_t indicesBufferSize = meshes.indices.size() * sizeof(int32_t);
  std::size_t vertexBufferSize = meshes.vertices.size() * sizeof(RealVertex);

  model.buffers.clear();

  tinygltf::Buffer bakedBuffer{};
  bakedBuffer.name = name.stem().string();
  bakedBuffer.uri = name.filename().string() + "_tangent_fix.bin";
  bakedBuffer.data.resize(indicesBufferSize + vertexBufferSize);

  std::memcpy(
    bakedBuffer.data.data(), meshes.indices.data(), meshes.indices.size() * sizeof(int32_t));
  std::memcpy(
    bakedBuffer.data.data() + indicesBufferSize,
    meshes.vertices.data(),
    meshes.vertices.size() * sizeof(RealVertex));
  model.buffers.push_back(bakedBuffer);

  model.bufferViews.clear();

  tinygltf::BufferView indicesBufferView{};
  indicesBufferView.name = "indices_tangent_fix";
  indicesBufferView.buffer = 0;
  indicesBufferView.byteOffset = 0;
  indicesBufferView.byteLength = meshes.indices.size() * sizeof(int32_t);
  indicesBufferView.byteStride = 0;
  indicesBufferView.target = TINYGLTF_TARGET_ELEMENT_ARRAY_BUFFER;

  tinygltf::BufferView vertexBufferView{};
  vertexBufferView.name = "vertices_tangent_fix";
  vertexBufferView.buffer = 0;
  vertexBufferView.byteOffset = meshes.indices.size() * sizeof(int32_t);
  vertexBufferView.byteLength = meshes.vertices.size() * sizeof(RealVertex);
  vertexBufferView.byteStride = sizeof(RealVertex);
  vertexBufferView.target = TINYGLTF_TARGET_ARRAY_BUFFER;

  model.bufferViews.push_back(indicesBufferView);
  model.bufferViews.push_back(vertexBufferView);

  tinygltf::Accessor indicesAccessor{};
  indicesAccessor.bufferView = 0;
  indicesAccessor.byteOffset = 0;
  indicesAccessor.componentType = TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT;
  indicesAccessor.type = TINYGLTF_TYPE_SCALAR;

  tinygltf::Accessor positionAccessor{};
  positionAccessor.bufferView = 1;
  positionAccessor.byteOffset = 0;
  positionAccessor.componentType = TINYGLTF_COMPONENT_TYPE_FLOAT;
  positionAccessor.type = TINYGLTF_TYPE_VEC3;

  tinygltf::Accessor normalAccessor{};
  normalAccessor.bufferView = 1;
  normalAccessor.byteOffset = 12;
  normalAccessor.componentType = TINYGLTF_COMPONENT_TYPE_FLOAT;
  normalAccessor.type = TINYGLTF_TYPE_VEC3;

  tinygltf::Accessor tangentAccessor{};
  tangentAccessor.bufferView = 1;
  tangentAccessor.byteOffset = 24;
  tangentAccessor.componentType = TINYGLTF_COMPONENT_TYPE_FLOAT;
  tangentAccessor.type = TINYGLTF_TYPE_VEC4;

  tinygltf::Accessor texcoordAccessor{};
  texcoordAccessor.bufferView = 1;
  texcoordAccessor.byteOffset = 40;
  texcoordAccessor.componentType = TINYGLTF_COMPONENT_TYPE_FLOAT;
  texcoordAccessor.type = TINYGLTF_TYPE_VEC2;

  std::map<std::string, tinygltf::Accessor> accessors = {
    {"POSITION", positionAccessor},
    {"NORMAL", normalAccessor},
    {"TANGENT", tangentAccessor},
    {"TEXCOORD_0", texcoordAccessor}};

  model.accessors.clear();

  for (std::size_t i = 0; i < model.meshes.size(); i++)
  {
    auto& mesh = model.meshes[i];
    for (std::size_t j = 0; j < mesh.primitives.size(); j++)
    {
      auto& primitive = mesh.primitives[j];
      if (primitive.mode != TINYGLTF_MODE_TRIANGLES)
      {
        spdlog::warn(
          "Encountered a non-triangles primitive in accessors modification, these are not "
          "supported for now, skipping it!");
        continue;
      }
      std::erase_if(primitive.attributes, [&](const auto& attribute) {
        for (const auto& accessor : accessors)
        {
          if (attribute.first == accessor.first)
          {
            return false;
          }
        }
        return true;
      });

      auto& renderElement = meshes.relems[meshes.meshes[i].firstRelem + j];

      primitive.indices = static_cast<int>(model.accessors.size());
      auto& currentIndicesAccessor = model.accessors.emplace_back(indicesAccessor);
      currentIndicesAccessor.byteOffset += renderElement.indexOffset * sizeof(int32_t);
      currentIndicesAccessor.count = renderElement.indexCount;

      accessors.at("POSITION").minValues = renderElement.positionMinMax->at(0);
      accessors.at("POSITION").maxValues = renderElement.positionMinMax->at(1);

      if (renderElement.texcoordMinMax.has_value())
      {
        accessors.at("TEXCOORD_0").minValues = renderElement.texcoordMinMax->at(0);
        accessors.at("TEXCOORD_0").maxValues = renderElement.texcoordMinMax->at(1);
      }
      for (const auto& accessor : accessors)
      {
        // adding missing tangents
        // if (!primitive.attributes.contains(accessor.first))
        // {
        //   spdlog::info("skipping accessor - {}", accessor.first);
        //   continue;
        // }
        primitive.attributes[accessor.first] = static_cast<int>(model.accessors.size());
        auto& currentAccessor = model.accessors.emplace_back(accessor.second);
        currentAccessor.byteOffset += renderElement.vertexOffset * sizeof(RealVertex);
        currentAccessor.count = renderElement.vertexCount;
      }
    }
  }

  name = filepath.parent_path();
  name /= filepath.stem();
  auto newFileGltf = name.string() + "_tangent_fix.gltf";

  if (!loader.WriteGltfSceneToFile(&model, newFileGltf, false, false, true, false))
  {
    spdlog::error("Error occured when saving formatted file!\n Location - {}", newFileGltf);
  }
  else
  {
    spdlog::info("Intermediate model complete!");
  }
}

void Baker::changeBuffer(tinygltf::Model& model, BakedMeshes& baked_meshes)
{
  auto name = filepath.parent_path();
  name /= filepath.stem();
  auto newFile = name.string() + "_baked.bin";

  std::size_t indicesBufferSize = baked_meshes.indices.size() * sizeof(uint32_t);
  std::size_t vertexBufferSize = baked_meshes.vertices.size() * sizeof(Vertex);

  model.buffers.clear();

  tinygltf::Buffer bakedBuffer{};
  bakedBuffer.name = name.stem().string();
  bakedBuffer.uri = name.filename().string() + "_baked.bin";
  bakedBuffer.data.resize(indicesBufferSize + vertexBufferSize);

  std::memcpy(
    bakedBuffer.data.data(),
    baked_meshes.indices.data(),
    baked_meshes.indices.size() * sizeof(uint32_t));
  std::memcpy(
    bakedBuffer.data.data() + indicesBufferSize,
    baked_meshes.vertices.data(),
    baked_meshes.vertices.size() * sizeof(Vertex));
  model.buffers.push_back(bakedBuffer);
}

void Baker::changeBufferViews(tinygltf::Model& model, BakedMeshes& baked_meshes)
{
  model.bufferViews.clear();

  tinygltf::BufferView indicesBufferView{};
  indicesBufferView.name = "indices_baked";
  indicesBufferView.buffer = 0;
  indicesBufferView.byteOffset = 0;
  indicesBufferView.byteLength = baked_meshes.indices.size() * sizeof(uint32_t);
  indicesBufferView.byteStride = 0;
  indicesBufferView.target = TINYGLTF_TARGET_ELEMENT_ARRAY_BUFFER;

  tinygltf::BufferView vertexBufferView{};
  vertexBufferView.name = "vertices_baked";
  vertexBufferView.buffer = 0;
  vertexBufferView.byteOffset = baked_meshes.indices.size() * sizeof(uint32_t);
  vertexBufferView.byteLength = baked_meshes.vertices.size() * sizeof(Vertex);
  vertexBufferView.byteStride = sizeof(Vertex);
  vertexBufferView.target = TINYGLTF_TARGET_ARRAY_BUFFER;

  model.bufferViews.push_back(indicesBufferView);
  model.bufferViews.push_back(vertexBufferView);
}

void Baker::changeAccessors(tinygltf::Model& model, BakedMeshes& baked_meshes)
{
  tinygltf::Accessor indicesAccessor{};
  indicesAccessor.bufferView = 0;
  indicesAccessor.byteOffset = 0;
  indicesAccessor.componentType = TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT;
  indicesAccessor.type = TINYGLTF_TYPE_SCALAR;

  tinygltf::Accessor positionAccessor{};
  positionAccessor.bufferView = 1;
  positionAccessor.byteOffset = 0;
  positionAccessor.componentType = TINYGLTF_COMPONENT_TYPE_FLOAT;
  positionAccessor.type = TINYGLTF_TYPE_VEC3;

  tinygltf::Accessor normalAccessor{};
  normalAccessor.bufferView = 1;
  normalAccessor.byteOffset = 12;
  normalAccessor.normalized = true;
  normalAccessor.componentType = TINYGLTF_COMPONENT_TYPE_BYTE;
  normalAccessor.type = TINYGLTF_TYPE_VEC3;

  tinygltf::Accessor tangentAccessor{};
  tangentAccessor.bufferView = 1;
  tangentAccessor.byteOffset = 24;
  tangentAccessor.normalized = true;
  tangentAccessor.componentType = TINYGLTF_COMPONENT_TYPE_BYTE;
  tangentAccessor.type = TINYGLTF_TYPE_VEC4;

  tinygltf::Accessor texcoordAccessor{};
  texcoordAccessor.bufferView = 1;
  texcoordAccessor.byteOffset = 16;
  texcoordAccessor.componentType = TINYGLTF_COMPONENT_TYPE_FLOAT;
  texcoordAccessor.type = TINYGLTF_TYPE_VEC2;

  std::map<std::string, tinygltf::Accessor> accessors = {
    {"POSITION", positionAccessor},
    {"NORMAL", normalAccessor},
    {"TANGENT", tangentAccessor},
    {"TEXCOORD_0", texcoordAccessor}};

  model.accessors.clear();

  for (std::size_t i = 0; i < model.meshes.size(); i++)
  {
    auto& mesh = model.meshes[i];
    for (std::size_t j = 0; j < mesh.primitives.size(); j++)
    {
      auto& primitive = mesh.primitives[j];
      if (primitive.mode != TINYGLTF_MODE_TRIANGLES)
      {
        spdlog::warn(
          "Encountered a non-triangles primitive in accessors modification, these are not "
          "supported for now, skipping it!");
        continue;
      }
      std::erase_if(primitive.attributes, [&](const auto& attribute) {
        for (const auto& accessor : accessors)
        {
          if (attribute.first == accessor.first)
          {
            return false;
          }
        }
        return true;
      });

      auto& renderElement = baked_meshes.relems[baked_meshes.meshes[i].firstRelem + j];

      primitive.indices = static_cast<int>(model.accessors.size());
      auto& currentIndicesAccessor = model.accessors.emplace_back(indicesAccessor);
      currentIndicesAccessor.byteOffset += renderElement.indexOffset * sizeof(uint32_t);
      currentIndicesAccessor.count = renderElement.indexCount;

      accessors.at("POSITION").minValues = renderElement.positionMinMax->at(0);
      accessors.at("POSITION").maxValues = renderElement.positionMinMax->at(1);

      if (renderElement.texcoordMinMax.has_value())
      {
        accessors.at("TEXCOORD_0").minValues = renderElement.texcoordMinMax->at(0);
        accessors.at("TEXCOORD_0").maxValues = renderElement.texcoordMinMax->at(1);
      }
      for (const auto& accessor : accessors)
      {
        // adding missing tangents
        // if (!primitive.attributes.contains(accessor.first))
        // {
        //   spdlog::info("skipping accessor - {}", accessor.first);
        //   continue;
        // }
        primitive.attributes[accessor.first] = static_cast<int>(model.accessors.size());
        auto& currentAccessor = model.accessors.emplace_back(accessor.second);
        currentAccessor.byteOffset += renderElement.vertexOffset * sizeof(Vertex);
        currentAccessor.count = renderElement.vertexCount;
      }
    }
  }
}

void Baker::saveFormatted(tinygltf::Model& model)
{
  auto name = filepath.parent_path();
  name /= filepath.stem();
  auto newFile = name.string() + "_baked.gltf";

  if (!loader.WriteGltfSceneToFile(&model, newFile, false, false, true, false))
  {
    spdlog::error("Error occured when saving formatted file!\n Location - {}", newFile);
  }
  else
  {
    spdlog::info("Bakery complete!");
  }
}
