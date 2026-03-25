#pragma once

#include <glm/fwd.hpp>
#include <glm/glm.hpp>

#include <etna/GraphicsPipeline.hpp>
#include <etna/RenderTargetStates.hpp>
#include <etna/Buffer.hpp>
#include <etna/Sampler.hpp>
#include <etna/OneShotCmdMgr.hpp>

#include "shaders/TerrainParams.h"
#include "../RenderPacket.hpp"


class TerrainRenderModule
{
public:
  TerrainRenderModule();
  explicit TerrainRenderModule(TerrainParams par);

  void allocateResources();
  void loadShaders();
  void setupPipelines(
    bool wireframe_enabled,
    std::vector<vk::Format> color_attachent_formats,
    vk::Format depth_attachment_format,
    vk::Format shadow_attachment_format);

  void loadMaps(const std::vector<etna::Binding>& terrain_bindings);

  void executeRender(
    vk::CommandBuffer cmd_buf,
    const RenderPacket& packet,
    const etna::Buffer& heavy_packet_info_buffer,
    std::vector<etna::RenderTargetState::AttachmentParams> color_attachment_params,
    etna::RenderTargetState::AttachmentParams depth_attachment_params);

  void executeShadowMapping(
    vk::CommandBuffer cmd_buf,
    const RenderPacket& packet,
    vk::Extent2D extent,
    etna::BufferBinding light_info_binding,
    etna::RenderTargetState::AttachmentParams shadow_mapping_attachment_params);

  void drawGui();

private:
  struct PushConstants
  {
    glm::mat4x4 projView;
    glm::vec3 cameraWorldPosition;
  };

private:
  void renderTerrain(
    vk::CommandBuffer cmd_buf,
    vk::PipelineLayout pipeline_layout,
    const etna::Buffer& heavy_packet_info_buffer);

private:
  TerrainParams params;

  etna::Buffer paramsBuffer;

  std::unique_ptr<etna::PersistentDescriptorSet> terrainSet;
  std::unique_ptr<etna::PersistentDescriptorSet> terrainShadowSet;

  etna::GraphicsPipeline terrainRenderPipeline;
  etna::GraphicsPipeline terrainShadowPipeline;

  std::unique_ptr<etna::OneShotCmdMgr> oneShotCommands;
};
