#include "AntialiasingModule.hpp"

#include <glm/ext/matrix_transform.hpp>
#include <tracy/Tracy.hpp>
#include <imgui.h>

#include <etna/GlobalContext.hpp>
#include <etna/PipelineManager.hpp>
#include <etna/Profiling.hpp>

#include <render_utils/Utilities.hpp>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/string_cast.hpp>


AntialiasingModule::AntialiasingModule()
  : params(
      {.currentProjView = glm::identity<glm::mat4>(),
       .currentInvProjView = glm::identity<glm::mat4>(),
       .previousProjView = glm::identity<glm::mat4>(),
       .previousFrameUsage = 0.9f,
       .maxDepthDelta = 0.01f,
       .catmullRomBParam = 0.3f,
       .catmullRomCParam = 0.3f})
  , jitterIndex(0)
  , jitterDamping(4.0f)
  , jitterCutoffIndex(16)
  , currentJitter(0.0f, 0.0f)
  , previousJitter(0.0f, 0.0f)
{
}

void AntialiasingModule::allocateResources(const AllocationInfo& info)
{
  auto& ctx = etna::get_context();

  vk::Extent3D renderImagesExtent = {info.resolution.x, info.resolution.y, 1};

  previousTargetImage = ctx.createImage(
    etna::Image::CreateInfo{
      .extent = renderImagesExtent,
      .name = "previousTargetImage",
      .format = info.renderTargetFormat,
      .imageUsage = vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled |
        vk::ImageUsageFlagBits::eStorage,
      .memoryUsage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
      .allocationCreate = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT});

  previousDepthImage = ctx.createImage(
    etna::Image::CreateInfo{
      .extent = renderImagesExtent,
      .name = "previousDepthImage",
      .format = info.depthFormat,
      .imageUsage = vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled,
      .memoryUsage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
      .allocationCreate = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT});

  paramsBuffer.emplace(ctx.getMainWorkCount(), [&ctx](std::size_t i) {
    return ctx.createBuffer(
      etna::Buffer::CreateInfo{
        .size = sizeof(AntialiasingParams),
        .bufferUsage = vk::BufferUsageFlagBits::eUniformBuffer,
        .memoryUsage = VMA_MEMORY_USAGE_AUTO,
        .allocationCreate =
          VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
        .name = fmt::format("antialiasingConstants{}", i)});
  });

  depthSampler = etna::Sampler(
    etna::Sampler::CreateInfo{.filter = vk::Filter::eLinear, .name = "taaDepthSampler"});
  linearClampSampler = etna::Sampler(
    etna::Sampler::CreateInfo{
      .filter = vk::Filter::eLinear,
      .addressMode = vk::SamplerAddressMode::eClampToBorder,
      .name = "taaLinearClampSampler"});
}

void AntialiasingModule::loadShaders()
{
  etna::create_program("taa", {ANTIALIASING_MODULE_SHADERS_ROOT "taa.comp.spv"});
}

void AntialiasingModule::setupPipelines()
{
  auto& pipelineManager = etna::get_context().getPipelineManager();

  aaPipeline = pipelineManager.createComputePipeline("taa", {});
}

void AntialiasingModule::setBarriersForCopy(vk::CommandBuffer cmd_buf)
{
  etna::set_state(
    cmd_buf,
    previousTargetImage.get(),
    vk::PipelineStageFlagBits2::eTransfer,
    vk::AccessFlagBits2::eTransferWrite,
    vk::ImageLayout::eTransferDstOptimal,
    vk::ImageAspectFlagBits::eColor);

  etna::set_state(
    cmd_buf,
    previousDepthImage.get(),
    vk::PipelineStageFlagBits2::eTransfer,
    vk::AccessFlagBits2::eTransferWrite,
    vk::ImageLayout::eTransferDstOptimal,
    vk::ImageAspectFlagBits::eDepth);
}

void AntialiasingModule::setBarriersForExecute(vk::CommandBuffer cmd_buf)
{
  etna::set_state(
    cmd_buf,
    previousTargetImage.get(),
    vk::PipelineStageFlagBits2::eComputeShader,
    vk::AccessFlagBits2::eShaderSampledRead,
    vk::ImageLayout::eGeneral,
    vk::ImageAspectFlagBits::eColor);

  etna::set_state(
    cmd_buf,
    previousDepthImage.get(),
    vk::PipelineStageFlagBits2::eComputeShader,
    vk::AccessFlagBits2::eShaderSampledRead,
    vk::ImageLayout::eShaderReadOnlyOptimal,
    vk::ImageAspectFlagBits::eDepth);
}

void AntialiasingModule::execute(
  vk::CommandBuffer cmd_buf,
  const etna::Image& render_target,
  const etna::Image& depth_image,
  const etna::Image& velocity_image,
  const RenderPacket& packet,
  const glm::mat4& inv_proj_view)
{
  ZoneScoped;

  // skip TAA only for the first frame
  if (!enable) [[unlikely]]
  {
    enable = true;
    return;
  }

  // NOTE - maybe suboptiomal
  params.currentProjView = packet.heavyInfo.projView;
  params.currentInvProjView = inv_proj_view;

  auto& currentConstants = paramsBuffer->get();
  currentConstants.map();
  std::memcpy(currentConstants.data(), &params, sizeof(AntialiasingParams));
  currentConstants.unmap();

  auto extent = render_target.getExtent();

  {
    ETNA_PROFILE_GPU(cmd_buf, temporalAA);

    cmd_buf.bindPipeline(vk::PipelineBindPoint::eCompute, aaPipeline.getVkPipeline());

    auto shaderInfo = etna::get_shader_program("taa");

    auto set = etna::create_descriptor_set(
      shaderInfo.getDescriptorLayoutId(0),
      cmd_buf,
      {etna::Binding{
         0, previousTargetImage.genBinding(linearClampSampler.get(), vk::ImageLayout::eGeneral)},
       etna::Binding{
         1,
         previousDepthImage.genBinding(
           depthSampler.get(), vk::ImageLayout::eShaderReadOnlyOptimal)},
       etna::Binding{2, render_target.genBinding({}, vk::ImageLayout::eGeneral)},
       etna::Binding{
         3, depth_image.genBinding(depthSampler.get(), vk::ImageLayout::eShaderReadOnlyOptimal)},
       etna::Binding{4, velocity_image.genBinding({}, vk::ImageLayout::eGeneral)},
       etna::Binding{5, currentConstants.genBinding()}});

    cmd_buf.bindDescriptorSets(
      vk::PipelineBindPoint::eCompute, aaPipeline.getVkPipelineLayout(), 0, {set.getVkSet()}, {});

    cmd_buf.pushConstants<glm::vec2>(
      aaPipeline.getVkPipelineLayout(),
      vk::ShaderStageFlagBits::eCompute,
      0,
      {currentJitter * glm::vec2(extent.width, extent.height),
       glm::vec2(packet.nearPlane, packet.farPlane)});

    cmd_buf.dispatch(
      (static_cast<uint32_t>(extent.width) + 31) / 32,
      (static_cast<uint32_t>(extent.height) + 31) / 32,
      1);
  }
}

void AntialiasingModule::copyPreviousImages(
  vk::CommandBuffer cmd_buf, const etna::Image& prev_target, const etna::Image& prev_depth_image)
{
  vk::Extent3D extent = prev_target.getExtent();

  render_utility::blit_image(
    cmd_buf,
    prev_target.get(),
    previousTargetImage.get(),
    vk::ImageAspectFlagBits::eColor,
    {.x = static_cast<int32_t>(extent.width), .y = static_cast<int32_t>(extent.height), .z = 1});

  render_utility::blit_image(
    cmd_buf,
    prev_depth_image.get(),
    previousDepthImage.get(),
    vk::ImageAspectFlagBits::eDepth,
    {.x = static_cast<int32_t>(extent.width), .y = static_cast<int32_t>(extent.height), .z = 1});
}

void AntialiasingModule::copyPreviousProjView(const glm::mat4& previous_proj_view)
{
  params.previousProjView = previous_proj_view;
}

void AntialiasingModule::drawGui()
{
  ImGui::Begin("Application Settings");

  if (ImGui::CollapsingHeader("Temporal Antialiasing"))
  {
    ImGui::DragFloat("Previous frame usage", &params.previousFrameUsage, 0.001f, 0.0f, 1.0f);
    ImGui::DragFloat("Jitter damping", &jitterDamping, 0.01f, 1.0f, 10.0f);
    ImGui::DragInt("Jitter cutoff index", &jitterCutoffIndex, 1.0f, 1, 256);
    ImGui::DragFloat(
      "Maximum depth difference", &params.maxDepthDelta, 0.0001f, 0.0f, 1.0f, "%.10f");
    ImGui::DragFloat(
      "Catmull-Rom sampling B parameter", &params.catmullRomBParam, 0.001f, 0.0f, 1.0f);
    ImGui::DragFloat(
      "Catmull-Rom sampling C parameter", &params.catmullRomCParam, 0.001f, 0.0f, 1.0f);
  }

  ImGui::End();
}

void AntialiasingModule::updateJitter()
{
  previousJitter = currentJitter;

  float haltonX = 2.0f * haltonJitter(jitterIndex + 1, 2) - 1.0f;
  float haltonY = 2.0f * haltonJitter(jitterIndex + 1, 3) - 1.0f;

  jitterIndex++;
  jitterIndex = jitterIndex % jitterCutoffIndex;

  auto extent = previousTargetImage.getExtent();

  glm::vec2 resolution = glm::vec2(extent.width, extent.height);
  currentJitter = glm::vec2(haltonX / jitterDamping, haltonY / jitterDamping) / resolution;
}

float AntialiasingModule::haltonJitter(uint32_t index, uint32_t base)
{
  float result = 0.0f;
  float current = 1.0f;

  while (index > 0)
  {
    current /= static_cast<float>(base);
    result += current * static_cast<float>(index % base);
    index = static_cast<uint32_t>(glm::floor(static_cast<float>(index) / static_cast<float>(base)));
  }

  return result;
}
