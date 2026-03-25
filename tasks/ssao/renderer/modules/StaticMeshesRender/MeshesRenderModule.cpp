#include "MeshesRenderModule.hpp"
#include "RenderPacket.hpp"

#include <glm/ext/matrix_transform.hpp>
#include <tracy/Tracy.hpp>
#include <imgui.h>

#include <etna/GlobalContext.hpp>
#include <etna/PipelineManager.hpp>
#include <etna/Profiling.hpp>
#include <etna/RenderTargetStates.hpp>


MeshesRenderModule::MeshesRenderModule()
  : info{.translation = glm::translate(glm::identity<glm::mat4>(), glm::vec3(45, -20, -100))}
  , sceneMgr{std::make_unique<SceneManager>()}
{
}

void MeshesRenderModule::allocateResources()
{
  paramsBuffer = etna::get_context().createBuffer(
    etna::Buffer::CreateInfo{
      .size = sizeof(MeshesParams),
      .bufferUsage = vk::BufferUsageFlagBits::eUniformBuffer,
      .memoryUsage = VMA_MEMORY_USAGE_AUTO,
      .allocationCreate =
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
      .name = "meshesParams"});

  staticMeshSampler = etna::Sampler(
    etna::Sampler::CreateInfo{.filter = vk::Filter::eLinear, .name = "static_mesh_sampler"});
  oneShotCommands = etna::get_context().createOneShotCmdMgr();

  transferHelper = std::make_unique<etna::BlockingTransferHelper>(
    etna::BlockingTransferHelper::CreateInfo{.stagingSize = sizeof(MeshesParams)});
}

void MeshesRenderModule::loadShaders()
{
  etna::create_program(
    "static_mesh_material",
    {STATIC_MESHES_MODULE_SHADERS_ROOT "static_mesh.frag.spv",
     STATIC_MESHES_MODULE_SHADERS_ROOT "static_mesh.vert.spv"});
  etna::create_program(
    "static_mesh_shadow", {STATIC_MESHES_MODULE_SHADERS_ROOT "static_mesh_shadow.vert.spv"});

  etna::create_program("culling_meshes", {STATIC_MESHES_MODULE_SHADERS_ROOT "culling.comp.spv"});
  etna::create_program(
    "culling_shadow", {STATIC_MESHES_MODULE_SHADERS_ROOT "culling_shadow.comp.spv"});
}

void MeshesRenderModule::loadScene(std::filesystem::path path)
{
  sceneMgr->selectBakedScene(path);
}

void MeshesRenderModule::setupPipelines(
  bool wireframe_enabled,
  std::vector<vk::Format> color_attachent_formats,
  vk::Format depth_attachment_format,
  vk::Format shadow_attachment_format)
{
  etna::VertexShaderInputDescription sceneVertexInputDesc{
    .bindings = {etna::VertexShaderInputDescription::Binding{
      .byteStreamDescription = sceneMgr->getVertexFormatDescription(),
    }},
  };

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

  staticMeshPipeline = pipelineManager.createGraphicsPipeline(
    "static_mesh_material",
    etna::GraphicsPipeline::CreateInfo{
      .vertexShaderInput = sceneVertexInputDesc,
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

  staticMeshShadowPipeline = pipelineManager.createGraphicsPipeline(
    "static_mesh_shadow",
    etna::GraphicsPipeline::CreateInfo{
      .vertexShaderInput = sceneVertexInputDesc,
      .rasterizationConfig =
        vk::PipelineRasterizationStateCreateInfo{
          .polygonMode = vk::PolygonMode::eFill,
          .cullMode = vk::CullModeFlagBits::eNone,
          .frontFace = vk::FrontFace::eCounterClockwise,
          .lineWidth = 1.f,
        },
      .fragmentShaderOutput =
        {
          .depthAttachmentFormat = shadow_attachment_format,
        },
    });

  cullingPipeline = pipelineManager.createComputePipeline("culling_meshes", {});
  cullingShadowPipeline = pipelineManager.createComputePipeline("culling_shadow", {});
}

void MeshesRenderModule::loadSet()
{
  auto shaderInfo = etna::get_shader_program("static_mesh_material");
  meshesDescriptorSet = etna::create_persistent_descriptor_set(
    shaderInfo.getDescriptorLayoutId(0), sceneMgr->getBindlessBindings(), true);

  auto commandBuffer = oneShotCommands->start();
  ETNA_CHECK_VK_RESULT(commandBuffer.begin(vk::CommandBufferBeginInfo{}));
  {
    meshesDescriptorSet->processBarriers(commandBuffer);
  }
  ETNA_CHECK_VK_RESULT(commandBuffer.end());
  oneShotCommands->submitAndWait(commandBuffer);

  params.instancesCount = static_cast<shader_uint>(sceneMgr->getInstanceMeshes().size());
  params.relemsCount = static_cast<shader_uint>(sceneMgr->getRenderElements().size());

  paramsBuffer.map();
  std::memcpy(paramsBuffer.data(), &params, sizeof(MeshesParams));
  paramsBuffer.unmap();
}

void MeshesRenderModule::executeRender(
  vk::CommandBuffer cmd_buf,
  const RenderPacket& packet,
  const etna::Buffer& heavy_packet_info_buffer,
  std::vector<etna::RenderTargetState::AttachmentParams> color_attachment_params,
  etna::RenderTargetState::AttachmentParams depth_attachment_params)
{
  cmd_buf.bindPipeline(vk::PipelineBindPoint::eCompute, cullingPipeline.getVkPipeline());
  cullMeshes(cmd_buf, cullingPipeline.getVkPipelineLayout(), packet.heavyInfo.projView);

  {
    ETNA_PROFILE_GPU(cmd_buf, renderScene);
    etna::RenderTargetState renderTargets(
      cmd_buf,
      {{0, 0}, {packet.resolution.x, packet.resolution.y}},
      color_attachment_params,
      depth_attachment_params);

    cmd_buf.bindPipeline(vk::PipelineBindPoint::eGraphics, staticMeshPipeline.getVkPipeline());
    renderScene(cmd_buf, staticMeshPipeline.getVkPipelineLayout(), heavy_packet_info_buffer);
  }
}

void MeshesRenderModule::executeShadowMapping(
  vk::CommandBuffer cmd_buf,
  vk::Extent2D extent,
  etna::BufferBinding light_info_binding,
  etna::RenderTargetState::AttachmentParams shadow_mapping_attachment_params)
{
  cmd_buf.bindPipeline(vk::PipelineBindPoint::eCompute, cullingShadowPipeline.getVkPipeline());
  cullMeshes(cmd_buf, cullingShadowPipeline.getVkPipelineLayout(), light_info_binding);

  {
    ETNA_PROFILE_GPU(cmd_buf, shadowMapScene);
    etna::RenderTargetState renderTargets(
      cmd_buf, {{0, 0}, extent}, {}, shadow_mapping_attachment_params);

    cmd_buf.bindPipeline(
      vk::PipelineBindPoint::eGraphics, staticMeshShadowPipeline.getVkPipeline());

    cmd_buf.bindVertexBuffers(0, {sceneMgr->getVertexBuffer()}, {0});
    cmd_buf.bindIndexBuffer(sceneMgr->getIndexBuffer(), 0, vk::IndexType::eUint32);

    auto shaderInfo = etna::get_shader_program("static_mesh_shadow");

    auto set = etna::create_descriptor_set(
      shaderInfo.getDescriptorLayoutId(0),
      cmd_buf,
      {etna::Binding{0, sceneMgr->getInstanceMatricesBuffer().genBinding()},
       etna::Binding{1, sceneMgr->getDrawInstanceIndicesBuffer().genBinding()},
       etna::Binding{2, light_info_binding}});

    cmd_buf.bindDescriptorSets(
      vk::PipelineBindPoint::eGraphics,
      staticMeshShadowPipeline.getVkPipelineLayout(),
      0,
      {set.getVkSet()},
      {});

    cmd_buf.drawIndexedIndirect(
      sceneMgr->getDrawCommandsBuffer().get(),
      0,
      static_cast<uint32_t>(sceneMgr->getRenderElements().size()),
      sizeof(vk::DrawIndexedIndirectCommand));
  }
}

void MeshesRenderModule::drawGui()
{
  ImGui::Begin("Application Settings");

  static glm::vec3 translation = glm::vec3(45, -20, -100);
  static bool translationChanged = false;

  if (ImGui::CollapsingHeader("Meshes"))
  {
    float currentTranslation[] = {translation.x, translation.y, translation.z};
    translationChanged = translationChanged ||
      ImGui::DragFloat3("Meshes translation", currentTranslation, 0.1f, -5000.0f, 5000.0f);
    translation = glm::vec3(currentTranslation[0], currentTranslation[1], currentTranslation[2]);
    info.translation = glm::translate(glm::identity<glm::mat4>(), translation);
  }

  if (translationChanged)
  {
    ETNA_CHECK_VK_RESULT(etna::get_context().getDevice().waitIdle());
    sceneMgr->updateMatrices(info.translation);
    translationChanged = false;
  }

  ImGui::End();
}

void MeshesRenderModule::cullMeshes(
  vk::CommandBuffer cmd_buf, vk::PipelineLayout pipeline_layout, const glm::mat4x4& proj_view)
{
  ETNA_PROFILE_GPU(cmd_buf, cullMeshesRender);
  {
    std::array bufferBarriers = {
      vk::BufferMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eVertexShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderWrite,
        .buffer = sceneMgr->getDrawInstanceIndicesBuffer().get(),
        .size = vk::WholeSize},
      vk::BufferMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eDrawIndirect,
        .srcAccessMask = vk::AccessFlagBits2::eIndirectCommandRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderWrite,
        .buffer = sceneMgr->getDrawCommandsBuffer().get(),
        .size = vk::WholeSize}};

    vk::DependencyInfo dependencyInfo = {
      .dependencyFlags = vk::DependencyFlagBits::eByRegion,
      .bufferMemoryBarrierCount = static_cast<uint32_t>(bufferBarriers.size()),
      .pBufferMemoryBarriers = bufferBarriers.data()};

    cmd_buf.pipelineBarrier2(dependencyInfo);
  }

  auto shaderInfo = etna::get_shader_program("culling_meshes");
  auto set = etna::create_descriptor_set(
    shaderInfo.getDescriptorLayoutId(0),
    cmd_buf,
    {etna::Binding{0, sceneMgr->getRelemsBuffer().genBinding()},
     etna::Binding{1, sceneMgr->getBoundsBuffer().genBinding()},
     etna::Binding{2, sceneMgr->getMeshesBuffer().genBinding()},
     etna::Binding{3, sceneMgr->getInstanceMeshesBuffer().genBinding()},
     etna::Binding{4, sceneMgr->getInstanceMatricesBuffer().genBinding()},
     etna::Binding{5, sceneMgr->getRelemInstanceOffsetsBuffer().genBinding()},
     etna::Binding{6, sceneMgr->getDrawInstanceIndicesBuffer().genBinding()},
     etna::Binding{7, sceneMgr->getDrawCommandsBuffer().genBinding()},
     etna::Binding{8, paramsBuffer.genBinding()}});
  auto vkSet = set.getVkSet();

  cmd_buf.bindDescriptorSets(
    vk::PipelineBindPoint::eCompute, pipeline_layout, 0, 1, &vkSet, 0, nullptr);

  // cmd_buf.pushConstants<glm::mat4x4>(
  //   pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0, {proj_view, info.translation});
  cmd_buf.pushConstants<glm::mat4x4>(
    pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0, {proj_view});

  cmd_buf.dispatch((static_cast<uint32_t>(sceneMgr->getInstanceMeshes().size()) + 127) / 128, 1, 1);

  {
    std::array bufferBarriers = {
      vk::BufferMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eVertexShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
        .buffer = sceneMgr->getDrawInstanceIndicesBuffer().get(),
        .size = vk::WholeSize},
      vk::BufferMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eDrawIndirect,
        .dstAccessMask = vk::AccessFlagBits2::eIndirectCommandRead,
        .buffer = sceneMgr->getDrawCommandsBuffer().get(),
        .size = vk::WholeSize}};

    vk::DependencyInfo dependencyInfo = {
      .dependencyFlags = vk::DependencyFlagBits::eByRegion,
      .bufferMemoryBarrierCount = static_cast<uint32_t>(bufferBarriers.size()),
      .pBufferMemoryBarriers = bufferBarriers.data()};

    cmd_buf.pipelineBarrier2(dependencyInfo);
  }
}

void MeshesRenderModule::cullMeshes(
  vk::CommandBuffer cmd_buf,
  vk::PipelineLayout pipeline_layout,
  const etna::BufferBinding& proj_view_binding)
{
  ETNA_PROFILE_GPU(cmd_buf, cullMeshesShadow);
  {
    std::array bufferBarriers = {
      vk::BufferMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eVertexShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderWrite,
        .buffer = sceneMgr->getDrawInstanceIndicesBuffer().get(),
        .size = vk::WholeSize},
      vk::BufferMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eDrawIndirect,
        .srcAccessMask = vk::AccessFlagBits2::eIndirectCommandRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderWrite,
        .buffer = sceneMgr->getDrawCommandsBuffer().get(),
        .size = vk::WholeSize}};

    vk::DependencyInfo dependencyInfo = {
      .dependencyFlags = vk::DependencyFlagBits::eByRegion,
      .bufferMemoryBarrierCount = static_cast<uint32_t>(bufferBarriers.size()),
      .pBufferMemoryBarriers = bufferBarriers.data()};

    cmd_buf.pipelineBarrier2(dependencyInfo);
  }

  auto shaderInfo = etna::get_shader_program("culling_shadow");
  auto set = etna::create_descriptor_set(
    shaderInfo.getDescriptorLayoutId(0),
    cmd_buf,
    {etna::Binding{0, sceneMgr->getRelemsBuffer().genBinding()},
     etna::Binding{1, sceneMgr->getBoundsBuffer().genBinding()},
     etna::Binding{2, sceneMgr->getMeshesBuffer().genBinding()},
     etna::Binding{3, sceneMgr->getInstanceMeshesBuffer().genBinding()},
     etna::Binding{4, sceneMgr->getInstanceMatricesBuffer().genBinding()},
     etna::Binding{5, sceneMgr->getRelemInstanceOffsetsBuffer().genBinding()},
     etna::Binding{6, sceneMgr->getDrawInstanceIndicesBuffer().genBinding()},
     etna::Binding{7, sceneMgr->getDrawCommandsBuffer().genBinding()},
     etna::Binding{8, paramsBuffer.genBinding()},
     etna::Binding{9, proj_view_binding}});
  auto vkSet = set.getVkSet();

  cmd_buf.bindDescriptorSets(
    vk::PipelineBindPoint::eCompute, pipeline_layout, 0, 1, &vkSet, 0, nullptr);

  // cmd_buf.pushConstants<glm::mat4>(
  //   pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0, {info.translation});

  cmd_buf.dispatch((static_cast<uint32_t>(sceneMgr->getInstanceMeshes().size()) + 127) / 128, 1, 1);

  {
    std::array bufferBarriers = {
      vk::BufferMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eVertexShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
        .buffer = sceneMgr->getDrawInstanceIndicesBuffer().get(),
        .size = vk::WholeSize},
      vk::BufferMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eDrawIndirect,
        .dstAccessMask = vk::AccessFlagBits2::eIndirectCommandRead,
        .buffer = sceneMgr->getDrawCommandsBuffer().get(),
        .size = vk::WholeSize}};

    vk::DependencyInfo dependencyInfo = {
      .dependencyFlags = vk::DependencyFlagBits::eByRegion,
      .bufferMemoryBarrierCount = static_cast<uint32_t>(bufferBarriers.size()),
      .pBufferMemoryBarriers = bufferBarriers.data()};

    cmd_buf.pipelineBarrier2(dependencyInfo);
  }
}

void MeshesRenderModule::renderScene(
  vk::CommandBuffer cmd_buf,
  vk::PipelineLayout pipeline_layout,
  const etna::Buffer& heavy_packet_info_buffer)
{
  ZoneScoped;
  if (!sceneMgr->getVertexBuffer())
    return;

  cmd_buf.bindVertexBuffers(0, {sceneMgr->getVertexBuffer()}, {0});
  cmd_buf.bindIndexBuffer(sceneMgr->getIndexBuffer(), 0, vk::IndexType::eUint32);

  auto shaderInfo = etna::get_shader_program("static_mesh_material");

  // set 0 is persistent, for materials and textures
  auto set = etna::create_descriptor_set(
    shaderInfo.getDescriptorLayoutId(1),
    cmd_buf,
    {etna::Binding{0, sceneMgr->getRelemsBuffer().genBinding()},
     etna::Binding{1, sceneMgr->getInstanceMatricesBuffer().genBinding()},
     etna::Binding{2, sceneMgr->getDrawInstanceIndicesBuffer().genBinding()},
     etna::Binding{3, heavy_packet_info_buffer.genBinding()}});

  cmd_buf.bindDescriptorSets(
    vk::PipelineBindPoint::eGraphics,
    pipeline_layout,
    0,
    {meshesDescriptorSet->getVkSet(), set.getVkSet()},
    {});

  // cmd_buf.pushConstants<glm::mat4>(
  //   pipeline_layout, vk::ShaderStageFlagBits::eVertex, 0, {info.translation});

  cmd_buf.drawIndexedIndirect(
    sceneMgr->getDrawCommandsBuffer().get(),
    0,
    static_cast<uint32_t>(sceneMgr->getRenderElements().size()),
    sizeof(vk::DrawIndexedIndirectCommand));
}
