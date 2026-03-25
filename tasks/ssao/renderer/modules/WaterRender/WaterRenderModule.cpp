#include "WaterRenderModule.hpp"

#include <tracy/Tracy.hpp>

#include <imgui.h>

#include <etna/Etna.hpp>
#include <etna/PipelineManager.hpp>
#include <etna/Profiling.hpp>


WaterRenderModule::WaterRenderModule()
  : params(
      {.extent = shader_uvec2(1024),
       .chunk = shader_uvec2(16),
       .waterInChunks = shader_uvec2(128),
       .waterOffset = shader_vec2(-1024),
       .heightOffset = shader_float(-19.5)})
  , renderParams(
      {.scatterColor = shader_vec4(0.016, 0.0736, 0.16, 1),
       .bubbleColor = shader_vec4(0, 0.02, 0.016, 1),
       .foamColor = shader_vec4(0.6, 0.5568, 0.492, 1),
       .roughness = shader_float(0.1),
       .reflectionStrength = shader_float(0.9),
       .wavePeakScatterStrength = shader_float(2.2),
       .scatterStrength = shader_float(1),
       .scatterShadowStrength = shader_float(0.7),
       .bubbleDensity = shader_float(1.3),
       .transparencyStrength = shader_float(0.3)})
{
}

WaterRenderModule::WaterRenderModule(WaterParams par)
  : params(par)
  , renderParams(
      //{.color = shader_vec4(0.4627450980, 0.7137254902, 0.7686274510, 1),
      {.scatterColor = shader_vec4(0.016, 0.0736, 0.16, 1),
       .bubbleColor = shader_vec4(0, 0.02, 0.016, 1),
       .foamColor = shader_vec4(0.6, 0.5568, 0.0492, 1),
       .roughness = shader_float(0.3),
       .reflectionStrength = shader_float(0.5),
       .wavePeakScatterStrength = shader_float(1),
       .scatterStrength = shader_float(1),
       .scatterShadowStrength = shader_float(0.5),
       .bubbleDensity = shader_float(1),
       .transparencyStrength = shader_float(0.3)})
{
}

void WaterRenderModule::allocateResources()
{
  paramsBuffer = etna::get_context().createBuffer(
    etna::Buffer::CreateInfo{
      .size = sizeof(WaterParams),
      .bufferUsage = vk::BufferUsageFlagBits::eUniformBuffer,
      .memoryUsage = VMA_MEMORY_USAGE_AUTO,
      .allocationCreate =
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
      .name = "waterParams"});

  renderParamsBuffer = etna::get_context().createBuffer(
    etna::Buffer::CreateInfo{
      .size = sizeof(WaterRenderParams),
      .bufferUsage = vk::BufferUsageFlagBits::eUniformBuffer,
      .memoryUsage = VMA_MEMORY_USAGE_AUTO,
      .allocationCreate =
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
      .name = "waterRenderParams"});

  paramsBuffer.map();
  std::memcpy(paramsBuffer.data(), &params, sizeof(WaterParams));
  paramsBuffer.unmap();

  renderParamsBuffer.map();
  std::memcpy(renderParamsBuffer.data(), &renderParams, sizeof(WaterRenderParams));
  renderParamsBuffer.unmap();
}

void WaterRenderModule::loadShaders()
{
  etna::create_program(
    "water_render",
    {WATER_RENDER_MODULE_SHADERS_ROOT "chunk.vert.spv",
     WATER_RENDER_MODULE_SHADERS_ROOT "subdivide_chunk.tesc.spv",
     WATER_RENDER_MODULE_SHADERS_ROOT "process_chunk.tese.spv",
     WATER_RENDER_MODULE_SHADERS_ROOT "water.frag.spv"});
}

void WaterRenderModule::setupPipelines(
  bool wireframe_enabled,
  std::vector<vk::Format> color_attachent_formats,
  vk::Format depth_attachment_format)
{
  auto& pipelineManager = etna::get_context().getPipelineManager();

  waterRenderPipeline =
    pipelineManager.createGraphicsPipeline(
      "water_render",
      etna::GraphicsPipeline::CreateInfo{
        .inputAssemblyConfig = {.topology = vk::PrimitiveTopology::ePatchList},
        .rasterizationConfig =
          vk::PipelineRasterizationStateCreateInfo{
            .polygonMode = (wireframe_enabled ? vk::PolygonMode::eLine : vk::PolygonMode::eFill),
            .cullMode = vk::CullModeFlagBits::eNone,
            .frontFace = vk::FrontFace::eCounterClockwise,
            .lineWidth = 1.f,
          },
        .blendingConfig =
          {
            .attachments =
              {{
                 .blendEnable = vk::True,
                 .srcColorBlendFactor = vk::BlendFactor::eSrcAlpha,
                 .dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha,
                 .colorBlendOp = vk::BlendOp::eAdd,
                 .srcAlphaBlendFactor = vk::BlendFactor::eOne,
                 .dstAlphaBlendFactor = vk::BlendFactor::eZero,
                 .alphaBlendOp = vk::BlendOp::eAdd,
                 .colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                   vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA,
               },
               {
                 .blendEnable = vk::False,
                 .colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                   vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA,
               }},
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
}

void WaterRenderModule::executeRender(
  vk::CommandBuffer cmd_buf,
  const RenderPacket& packet,
  const etna::Buffer& heavy_packet_info,
  const glm::mat4& view_matrix,
  const glm::mat4& inv_view_matrix,
  std::vector<etna::RenderTargetState::AttachmentParams> color_attachment_params,
  etna::RenderTargetState::AttachmentParams depth_attachment_params,
  const etna::Image& water_map,
  const etna::Image& water_normal_map,
  const etna::Sampler& water_sampler,
  const std::vector<etna::Image>& shadows,
  const etna::Sampler& shadow_sampler,
  const etna::Buffer& shadow_casting_lights,
  const etna::Image& cubemap)
{
  {
    ETNA_PROFILE_GPU(cmd_buf, renderWater);
    etna::RenderTargetState renderTargets(
      cmd_buf,
      {{0, 0}, {packet.resolution.x, packet.resolution.y}},
      color_attachment_params,
      depth_attachment_params);

    cmd_buf.bindPipeline(vk::PipelineBindPoint::eGraphics, waterRenderPipeline.getVkPipeline());
    renderWater(
      cmd_buf,
      waterRenderPipeline.getVkPipelineLayout(),
      heavy_packet_info,
      view_matrix,
      inv_view_matrix,
      water_map,
      water_normal_map,
      water_sampler,
      shadows,
      shadow_sampler,
      shadow_casting_lights,
      cubemap);
  }
}

void WaterRenderModule::drawGui()
{
  ImGui::Begin("Application Settings");

  static bool paramsChanged = false;
  static bool renderParamsChanged = false;

  if (ImGui::CollapsingHeader("Water Render"))
  {
    ImGui::SeparatorText("Water parameters");

    float heightOffset = params.heightOffset;
    paramsChanged =
      paramsChanged || ImGui::DragFloat("Height Offset", &heightOffset, 0.1f, -500.0f, 500.0f);
    params.heightOffset = heightOffset;

    ImGui::SeparatorText("Render parameters");

    ImGuiColorEditFlags colorFlags =
      ImGuiColorEditFlags_HDR | ImGuiColorEditFlags_Float | ImGuiColorEditFlags_NoAlpha;

    float scatterColor[] = {
      renderParams.scatterColor.r,
      renderParams.scatterColor.g,
      renderParams.scatterColor.b,
    };
    float bubbleColor[] = {
      renderParams.bubbleColor.r,
      renderParams.bubbleColor.g,
      renderParams.bubbleColor.b,
    };
    float foamColor[] = {
      renderParams.foamColor.r,
      renderParams.foamColor.g,
      renderParams.foamColor.b,
    };

    float roughness = renderParams.roughness;
    float reflectionStrength = renderParams.reflectionStrength;
    float wavePeakScatterStrength = renderParams.wavePeakScatterStrength;
    float scatterStrength = renderParams.scatterStrength;
    float scatterShadowStrength = renderParams.scatterShadowStrength;
    float bubbleDensity = renderParams.bubbleDensity;
    float transparencyStrength = renderParams.transparencyStrength;

    renderParamsChanged =
      renderParamsChanged || ImGui::ColorEdit3("Scatter color", scatterColor, colorFlags);
    renderParams.scatterColor = shader_vec4(scatterColor[0], scatterColor[1], scatterColor[2], 1);
    renderParamsChanged =
      renderParamsChanged || ImGui::ColorEdit3("Bubbles color", bubbleColor, colorFlags);
    renderParams.bubbleColor = shader_vec4(bubbleColor[0], bubbleColor[1], bubbleColor[2], 1);
    renderParamsChanged =
      renderParamsChanged || ImGui::ColorEdit3("Foam color", foamColor, colorFlags);
    renderParams.foamColor = shader_vec4(foamColor[0], foamColor[1], foamColor[2], 1);
    renderParamsChanged =
      renderParamsChanged || ImGui::DragFloat("Roughness", &roughness, 0.001f, 0.0f, 1.0f);
    renderParams.roughness = roughness;
    renderParamsChanged = renderParamsChanged ||
      ImGui::DragFloat("Reflection strength", &reflectionStrength, 0.1f, 0.0f, 500.0f);
    renderParams.reflectionStrength = reflectionStrength;
    renderParamsChanged = renderParamsChanged ||
      ImGui::DragFloat("Wave peak scatter strength", &wavePeakScatterStrength, 0.1f, 0.0f, 500.0f);
    renderParams.wavePeakScatterStrength = wavePeakScatterStrength;
    renderParamsChanged = renderParamsChanged ||
      ImGui::DragFloat("Scatter strength", &scatterStrength, 0.1f, 0.0f, 500.0f);
    renderParams.scatterStrength = scatterStrength;
    renderParamsChanged = renderParamsChanged ||
      ImGui::DragFloat("Scatter shadow strength", &scatterShadowStrength, 0.1f, 0.0f, 500.0f);
    renderParams.scatterShadowStrength = scatterShadowStrength;
    renderParamsChanged = renderParamsChanged ||
      ImGui::DragFloat("Bubbles density", &bubbleDensity, 0.1f, 0.0f, 500.0f);
    renderParams.bubbleDensity = bubbleDensity;
    renderParamsChanged = renderParamsChanged ||
      ImGui::DragFloat("Transparency strength", &transparencyStrength, 0.001f, 0.0f, 1.0f);
    renderParams.transparencyStrength = transparencyStrength;
  }

  if (paramsChanged)
  {
    paramsBuffer.map();
    std::memcpy(paramsBuffer.data(), &params, sizeof(WaterParams));
    paramsBuffer.unmap();
    paramsChanged = false;
  }

  if (renderParamsChanged)
  {
    renderParamsBuffer.map();
    std::memcpy(renderParamsBuffer.data(), &renderParams, sizeof(WaterRenderParams));
    renderParamsBuffer.unmap();
    renderParamsChanged = false;
  }

  ImGui::End();
}

void WaterRenderModule::renderWater(
  vk::CommandBuffer cmd_buf,
  vk::PipelineLayout pipeline_layout,
  const etna::Buffer& heavy_packet_info,
  const glm::mat4& view_matrix,
  const glm::mat4& inv_view_matrix,
  const etna::Image& water_map,
  const etna::Image& water_normal_map,
  const etna::Sampler& water_sampler,
  const std::vector<etna::Image>& shadows,
  const etna::Sampler& shadow_sampler,
  const etna::Buffer& shadow_casting_lights,
  const etna::Image& cubemap)
{
  ZoneScoped;

  auto shaderInfo = etna::get_shader_program("water_render");

  std::vector<etna::Binding> bindings;
  bindings.reserve(7 + shadows.size());

  bindings.emplace_back(etna::Binding{0, paramsBuffer.genBinding()});
  bindings.emplace_back(etna::Binding{1, renderParamsBuffer.genBinding()});
  bindings.emplace_back(
    etna::Binding{
      2, water_map.genBinding(water_sampler.get(), vk::ImageLayout::eShaderReadOnlyOptimal)});
  bindings.emplace_back(
    etna::Binding{
      3,
      water_normal_map.genBinding(water_sampler.get(), vk::ImageLayout::eShaderReadOnlyOptimal)});

  for (uint32_t i = 0; i < static_cast<uint32_t>(shadows.size()); i++)
  {
    bindings.emplace_back(
      etna::Binding{
        4,
        shadows[i].genBinding(shadow_sampler.get(), vk::ImageLayout::eShaderReadOnlyOptimal),
        i});
  }

  bindings.emplace_back(
    etna::Binding{
      5,
      cubemap.genBinding(
        water_sampler.get(),
        vk::ImageLayout::eShaderReadOnlyOptimal,
        {.type = vk::ImageViewType::eCube})});
  bindings.emplace_back(etna::Binding{6, shadow_casting_lights.genBinding()});
  bindings.emplace_back(etna::Binding{7, heavy_packet_info.genBinding()});

  auto set =
    etna::create_descriptor_set(shaderInfo.getDescriptorLayoutId(0), cmd_buf, std::move(bindings));

  auto vkSet = set.getVkSet();

  cmd_buf.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipeline_layout, 0, {vkSet}, {});

  cmd_buf.pushConstants<glm::mat4>(
    pipeline_layout, vk::ShaderStageFlagBits::eFragment, 0, {view_matrix, inv_view_matrix});

  cmd_buf.draw(4, params.waterInChunks.x * params.waterInChunks.y, 0, 0);
}
