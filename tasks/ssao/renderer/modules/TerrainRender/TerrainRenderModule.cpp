#include "TerrainRenderModule.hpp"

#include <tracy/Tracy.hpp>

#include <imgui.h>

#include <etna/Etna.hpp>
#include <etna/PipelineManager.hpp>
#include <etna/Profiling.hpp>

#include "shaders/TerrainParams.h"


TerrainRenderModule::TerrainRenderModule()
  : params({
      .extent = shader_uvec2(1024),
      .chunk = shader_uvec2(16),
      .terrainInChunks = shader_uvec2(64, 64),
      .terrainOffset = shader_vec2(-512, -512),
    })
{
}

TerrainRenderModule::TerrainRenderModule(TerrainParams par)
  : params(par)
{
}

void TerrainRenderModule::allocateResources()
{
  paramsBuffer = etna::get_context().createBuffer(
    etna::Buffer::CreateInfo{
      .size = sizeof(TerrainParams),
      .bufferUsage = vk::BufferUsageFlagBits::eUniformBuffer,
      .memoryUsage = VMA_MEMORY_USAGE_AUTO,
      .allocationCreate =
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
      .name = "terrainParams"});

  paramsBuffer.map();
  std::memcpy(paramsBuffer.data(), &params, sizeof(TerrainParams));
  paramsBuffer.unmap();

  oneShotCommands = etna::get_context().createOneShotCmdMgr();
}

void TerrainRenderModule::loadShaders()
{
  etna::create_program(
    "terrain_render",
    {TERRAIN_RENDER_MODULE_SHADERS_ROOT "chunk.vert.spv",
     TERRAIN_RENDER_MODULE_SHADERS_ROOT "subdivide_chunk.tesc.spv",
     TERRAIN_RENDER_MODULE_SHADERS_ROOT "process_chunk.tese.spv",
     TERRAIN_RENDER_MODULE_SHADERS_ROOT "terrain.frag.spv"});

  etna::create_program(
    "terrain_shadow",
    {TERRAIN_RENDER_MODULE_SHADERS_ROOT "chunk_shadow.vert.spv",
     TERRAIN_RENDER_MODULE_SHADERS_ROOT "subdivide_chunk_shadow.tesc.spv",
     TERRAIN_RENDER_MODULE_SHADERS_ROOT "process_chunk_shadow.tese.spv"});
}

void TerrainRenderModule::setupPipelines(
  bool wireframe_enabled,
  std::vector<vk::Format> color_attachent_formats,
  vk::Format depth_attachment_format,
  vk::Format shadow_attachment_format)
{
  auto& pipelineManager = etna::get_context().getPipelineManager();

  std::vector<vk::PipelineColorBlendAttachmentState> attachments;

  attachments.reserve(color_attachent_formats.size());

  for (std::size_t i = 0; i < color_attachent_formats.size(); i++)
  {
    attachments.emplace_back(
      vk::PipelineColorBlendAttachmentState{
        .blendEnable = vk::False,
        .colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
          vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA,
      });
  }

  terrainRenderPipeline = pipelineManager.createGraphicsPipeline(
    "terrain_render",
    etna::GraphicsPipeline::CreateInfo{
      .inputAssemblyConfig = {.topology = vk::PrimitiveTopology::ePatchList},
      .rasterizationConfig =
        vk::PipelineRasterizationStateCreateInfo{
          .polygonMode = (wireframe_enabled ? vk::PolygonMode::eLine : vk::PolygonMode::eFill),
          .cullMode = vk::CullModeFlagBits::eBack,
          .frontFace = vk::FrontFace::eCounterClockwise,
          .lineWidth = 1.f,
        },
      .blendingConfig =
        {
          .attachments = std::move(attachments),
          .logicOpEnable = false,
          .logicOp = {},
        },
      .depthConfig =
        {
          .depthTestEnable = vk::True,
          .depthWriteEnable = vk::True,
          .depthCompareOp = vk::CompareOp::eGreaterOrEqual,
          .maxDepthBounds = 1.f,
        },
      .fragmentShaderOutput =
        {
          .colorAttachmentFormats = color_attachent_formats,
          .depthAttachmentFormat = depth_attachment_format,
        },
    });

  terrainShadowPipeline = pipelineManager.createGraphicsPipeline(
    "terrain_shadow",
    etna::GraphicsPipeline::CreateInfo{
      .inputAssemblyConfig = {.topology = vk::PrimitiveTopology::ePatchList},
      .rasterizationConfig =
        vk::PipelineRasterizationStateCreateInfo{
          .polygonMode = vk::PolygonMode::eFill,
          .cullMode = vk::CullModeFlagBits::eBack,
          .frontFace = vk::FrontFace::eCounterClockwise,
          .lineWidth = 1.f,
        },
      .fragmentShaderOutput =
        {
          .depthAttachmentFormat = shadow_attachment_format,
        },
    });
}

void TerrainRenderModule::loadMaps(const std::vector<etna::Binding>& terrain_bindings)
{
  auto shaderInfo = etna::get_shader_program("terrain_render");
  terrainSet =
    std::make_unique<etna::PersistentDescriptorSet>(etna::create_persistent_descriptor_set(
      shaderInfo.getDescriptorLayoutId(0), terrain_bindings, true));

  auto shaderShadowInfo = etna::get_shader_program("terrain_shadow");
  terrainShadowSet =
    std::make_unique<etna::PersistentDescriptorSet>(etna::create_persistent_descriptor_set(
      shaderShadowInfo.getDescriptorLayoutId(0), {terrain_bindings[0]}, true));

  auto commandBuffer = oneShotCommands->start();
  ETNA_CHECK_VK_RESULT(commandBuffer.begin(vk::CommandBufferBeginInfo{}));
  {
    terrainSet->processBarriers(commandBuffer);
    terrainShadowSet->processBarriers(commandBuffer);

    etna::flush_barriers(commandBuffer);
  }
  ETNA_CHECK_VK_RESULT(commandBuffer.end());
  oneShotCommands->submitAndWait(commandBuffer);
}

void TerrainRenderModule::executeRender(
  vk::CommandBuffer cmd_buf,
  const RenderPacket& packet,
  const etna::Buffer& heavy_packet_info_buffer,
  std::vector<etna::RenderTargetState::AttachmentParams> color_attachment_params,
  etna::RenderTargetState::AttachmentParams depth_attachment_params)
{
  {
    ETNA_PROFILE_GPU(cmd_buf, renderTerrain);
    etna::RenderTargetState renderTargets(
      cmd_buf,
      {{0, 0}, {packet.resolution.x, packet.resolution.y}},
      color_attachment_params,
      depth_attachment_params);

    cmd_buf.bindPipeline(vk::PipelineBindPoint::eGraphics, terrainRenderPipeline.getVkPipeline());
    renderTerrain(cmd_buf, terrainRenderPipeline.getVkPipelineLayout(), heavy_packet_info_buffer);
  }
}

void TerrainRenderModule::executeShadowMapping(
  vk::CommandBuffer cmd_buf,
  const RenderPacket& packet,
  vk::Extent2D extent,
  etna::BufferBinding light_info_binding,
  etna::RenderTargetState::AttachmentParams shadow_mapping_attachment_params)
{
  {
    ETNA_PROFILE_GPU(cmd_buf, shadowTerrain);
    etna::RenderTargetState renderTargets(
      cmd_buf, {{0, 0}, extent}, {}, shadow_mapping_attachment_params);

    cmd_buf.bindPipeline(vk::PipelineBindPoint::eGraphics, terrainShadowPipeline.getVkPipeline());
    auto shaderInfo = etna::get_shader_program("terrain_shadow");
    auto set = etna::create_descriptor_set(
      shaderInfo.getDescriptorLayoutId(1),
      cmd_buf,
      {etna::Binding{0, paramsBuffer.genBinding()}, etna::Binding{1, light_info_binding}});

    auto vkSet = set.getVkSet();

    cmd_buf.bindDescriptorSets(
      vk::PipelineBindPoint::eGraphics,
      terrainShadowPipeline.getVkPipelineLayout(),
      0,
      {terrainShadowSet->getVkSet(), vkSet},
      {});

    cmd_buf.pushConstants<glm::vec3>(
      terrainShadowPipeline.getVkPipelineLayout(),
      vk::ShaderStageFlagBits::eTessellationControl |
        vk::ShaderStageFlagBits::eTessellationEvaluation,
      0,
      {packet.heavyInfo.cameraWorldPosition});

    cmd_buf.draw(4, params.terrainInChunks.x * params.terrainInChunks.y, 0, 0);
  }
}

void TerrainRenderModule::drawGui() {}

void TerrainRenderModule::renderTerrain(
  vk::CommandBuffer cmd_buf,
  vk::PipelineLayout pipeline_layout,
  const etna::Buffer& heavy_packet_info_buffer)
{
  ZoneScoped;

  auto shaderInfo = etna::get_shader_program("terrain_render");
  auto set = etna::create_descriptor_set(
    shaderInfo.getDescriptorLayoutId(1),
    cmd_buf,
    {
      etna::Binding{0, paramsBuffer.genBinding()},
      etna::Binding{1, heavy_packet_info_buffer.genBinding()},
    });

  auto vkSet = set.getVkSet();

  cmd_buf.bindDescriptorSets(
    vk::PipelineBindPoint::eGraphics, pipeline_layout, 0, {terrainSet->getVkSet(), vkSet}, {});

  cmd_buf.draw(4, params.terrainInChunks.x * params.terrainInChunks.y, 0, 0);
}
