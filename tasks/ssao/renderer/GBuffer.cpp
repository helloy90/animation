#include "GBuffer.hpp"

#include <fmt/format.h>
#include <vector>

#include <etna/DescriptorSet.hpp>
#include <etna/Etna.hpp>
#include <etna/GlobalContext.hpp>


GBuffer::GBuffer(const CreateInfo& info)
{
  auto& ctx = etna::get_context();

  vk::Extent3D renderImagesExtent = {info.resolution.x, info.resolution.y, 1};
  vk::Extent3D shadowImagesExtent = {info.shadowMapsResolution.x, info.shadowMapsResolution.y, 1};

  albedo = ctx.createImage(
    etna::Image::CreateInfo{
      .extent = renderImagesExtent,
      .name = "gAlbedo",
      .format = info.renderTargetFormat,
      .imageUsage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled |
        vk::ImageUsageFlagBits::eStorage,
      .memoryUsage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
      .allocationCreate = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT});

  normal = ctx.createImage(
    etna::Image::CreateInfo{
      .extent = renderImagesExtent,
      .name = "gNormal",
      .format = info.normalsFormat,
      .imageUsage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled |
        vk::ImageUsageFlagBits::eStorage,
      .memoryUsage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
      .allocationCreate = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT});

  material = ctx.createImage(
    etna::Image::CreateInfo{
      .extent = renderImagesExtent,
      .name = "gMaterial",
      .format = vk::Format::eR8G8B8A8Unorm,
      .imageUsage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled |
        vk::ImageUsageFlagBits::eStorage,
      .memoryUsage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
      .allocationCreate = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT});

  velocity = ctx.createImage(
    etna::Image::CreateInfo{
      .extent = renderImagesExtent,
      .name = "gVelocity",
      .format = info.velocityBufferFormat,
      .imageUsage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled |
        vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eTransferDst,
      .memoryUsage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
      .allocationCreate = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT});

  occlusion = ctx.createImage(
    etna::Image::CreateInfo{
      .extent = renderImagesExtent,
      .name = "gOcclusion",
      .format = info.occlusionFormat,
      .imageUsage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled |
        vk::ImageUsageFlagBits::eStorage,
      .memoryUsage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
      .allocationCreate = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT});

  depth = ctx.createImage(
    etna::Image::CreateInfo{
      .extent = renderImagesExtent,
      .name = "gDepth",
      .format = info.depthFormat,
      .imageUsage = vk::ImageUsageFlagBits::eDepthStencilAttachment |
        vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferSrc,
      .memoryUsage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
      .allocationCreate = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT});

  shadows.reserve(info.shadowCascadesAmount);

  for (uint32_t i = 0; i < info.shadowCascadesAmount; i++)
  {
    shadows.emplace_back(ctx.createImage(
      etna::Image::CreateInfo{
        .extent = shadowImagesExtent,
        .name = fmt::format("gShadows{}", i),
        .format = info.shadowsFormat,
        .imageUsage =
          vk::ImageUsageFlagBits::eDepthStencilAttachment | vk::ImageUsageFlagBits::eSampled,
        .memoryUsage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        .allocationCreate = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT}));
  }

  depthSampler = etna::Sampler(
    etna::Sampler::CreateInfo{.filter = vk::Filter::eLinear, .name = "gBufferSampler"});
}

void GBuffer::prepareForRender(vk::CommandBuffer cmd_buf)
{
  etna::set_state(
    cmd_buf,
    albedo.get(),
    vk::PipelineStageFlagBits2::eColorAttachmentOutput,
    vk::AccessFlagBits2::eColorAttachmentWrite,
    vk::ImageLayout::eColorAttachmentOptimal,
    vk::ImageAspectFlagBits::eColor);
  etna::set_state(
    cmd_buf,
    normal.get(),
    vk::PipelineStageFlagBits2::eColorAttachmentOutput,
    vk::AccessFlagBits2::eColorAttachmentWrite,
    vk::ImageLayout::eColorAttachmentOptimal,
    vk::ImageAspectFlagBits::eColor);
  etna::set_state(
    cmd_buf,
    material.get(),
    vk::PipelineStageFlagBits2::eColorAttachmentOutput,
    vk::AccessFlagBits2::eColorAttachmentWrite,
    vk::ImageLayout::eColorAttachmentOptimal,
    vk::ImageAspectFlagBits::eColor);
  etna::set_state(
    cmd_buf,
    velocity.get(),
    vk::PipelineStageFlagBits2::eColorAttachmentOutput,
    vk::AccessFlagBits2::eColorAttachmentWrite,
    vk::ImageLayout::eColorAttachmentOptimal,
    vk::ImageAspectFlagBits::eColor);
  etna::set_state(
    cmd_buf,
    depth.get(),
    vk::PipelineStageFlagBits2::eEarlyFragmentTests |
      vk::PipelineStageFlagBits2::eLateFragmentTests,
    vk::AccessFlagBits2::eDepthStencilAttachmentWrite,
    vk::ImageLayout::eDepthStencilAttachmentOptimal,
    vk::ImageAspectFlagBits::eDepth);

  for (const auto& shadowMap : shadows)
  {
    etna::set_state(
      cmd_buf,
      shadowMap.get(),
      vk::PipelineStageFlagBits2::eEarlyFragmentTests |
        vk::PipelineStageFlagBits2::eLateFragmentTests,
      vk::AccessFlagBits2::eDepthStencilAttachmentWrite,
      vk::ImageLayout::eDepthStencilAttachmentOptimal,
      vk::ImageAspectFlagBits::eDepth);
  }
}

void GBuffer::prepareForDepthReadWrite(vk::CommandBuffer cmd_buf)
{
  etna::set_state(
    cmd_buf,
    depth.get(),
    vk::PipelineStageFlagBits2::eEarlyFragmentTests |
      vk::PipelineStageFlagBits2::eLateFragmentTests,
    vk::AccessFlagBits2::eDepthStencilAttachmentRead |
      vk::AccessFlagBits2::eDepthStencilAttachmentWrite,
    vk::ImageLayout::eDepthStencilAttachmentOptimal,
    vk::ImageAspectFlagBits::eDepth);
}

void GBuffer::prepareForDepthRead(
  vk::CommandBuffer cmd_buf, vk::PipelineStageFlagBits2 pipeline_stage)
{
  etna::set_state(
    cmd_buf,
    depth.get(),
    pipeline_stage,
    vk::AccessFlagBits2::eShaderSampledRead,
    vk::ImageLayout::eShaderReadOnlyOptimal,
    vk::ImageAspectFlagBits::eDepth);
}

void GBuffer::prepareForOcclusionExecute(vk::CommandBuffer cmd_buf)
{
  prepareForDepthRead(cmd_buf, vk::PipelineStageFlagBits2::eComputeShader);

  etna::set_state(
    cmd_buf,
    normal.get(),
    vk::PipelineStageFlagBits2::eComputeShader,
    vk::AccessFlagBits2::eShaderStorageRead,
    vk::ImageLayout::eGeneral,
    vk::ImageAspectFlagBits::eColor);

  etna::set_state(
    cmd_buf,
    occlusion.get(),
    vk::PipelineStageFlagBits2::eComputeShader,
    vk::AccessFlagBits2::eShaderStorageWrite,
    vk::ImageLayout::eGeneral,
    vk::ImageAspectFlagBits::eColor);
}

void GBuffer::prepareForRead(vk::CommandBuffer cmd_buf)
{
  etna::set_state(
    cmd_buf,
    albedo.get(),
    vk::PipelineStageFlagBits2::eFragmentShader,
    vk::AccessFlagBits2::eShaderStorageRead,
    vk::ImageLayout::eGeneral,
    vk::ImageAspectFlagBits::eColor);
  etna::set_state(
    cmd_buf,
    normal.get(),
    vk::PipelineStageFlagBits2::eFragmentShader,
    vk::AccessFlagBits2::eShaderStorageRead,
    vk::ImageLayout::eGeneral,
    vk::ImageAspectFlagBits::eColor);
  etna::set_state(
    cmd_buf,
    material.get(),
    vk::PipelineStageFlagBits2::eFragmentShader,
    vk::AccessFlagBits2::eShaderStorageRead,
    vk::ImageLayout::eGeneral,
    vk::ImageAspectFlagBits::eColor);
  etna::set_state(
    cmd_buf,
    occlusion.get(),
    vk::PipelineStageFlagBits2::eFragmentShader,
    vk::AccessFlagBits2::eShaderStorageRead,
    vk::ImageLayout::eGeneral,
    vk::ImageAspectFlagBits::eColor);

  prepareForDepthRead(cmd_buf, vk::PipelineStageFlagBits2::eFragmentShader);

  for (const auto& shadowMap : shadows)
  {
    etna::set_state(
      cmd_buf,
      shadowMap.get(),
      vk::PipelineStageFlagBits2::eFragmentShader,
      vk::AccessFlagBits2::eShaderSampledRead,
      vk::ImageLayout::eShaderReadOnlyOptimal,
      vk::ImageAspectFlagBits::eDepth);
  }
}

void GBuffer::prepareForDepthCopy(vk::CommandBuffer cmd_buf)
{
  etna::set_state(
    cmd_buf,
    depth.get(),
    vk::PipelineStageFlagBits2::eTransfer,
    vk::AccessFlagBits2::eTransferRead,
    vk::ImageLayout::eTransferSrcOptimal,
    vk::ImageAspectFlagBits::eDepth);
}

void GBuffer::prepareForTaaExecute(vk::CommandBuffer cmd_buf)
{
  prepareForDepthRead(cmd_buf, vk::PipelineStageFlagBits2::eComputeShader);

  etna::set_state(
    cmd_buf,
    velocity.get(),
    vk::PipelineStageFlagBits2::eComputeShader,
    vk::AccessFlagBits2::eShaderStorageRead,
    vk::ImageLayout::eGeneral,
    vk::ImageAspectFlagBits::eColor);
}

void GBuffer::prepareForVelocityReset(vk::CommandBuffer cmd_buf)
{
  etna::set_state(
    cmd_buf,
    velocity.get(),
    vk::PipelineStageFlagBits2::eTransfer,
    vk::AccessFlagBits2::eTransferWrite,
    vk::ImageLayout::eTransferDstOptimal,
    vk::ImageAspectFlagBits::eColor);
}

void GBuffer::resetVelocityTexture(vk::CommandBuffer cmd_buf)
{
  cmd_buf.clearColorImage(
    velocity.get(),
    vk::ImageLayout::eTransferDstOptimal,
    {0.0f, 0.0f, 0.0f, 0.0f},
    vk::ImageSubresourceRange{
      .aspectMask = vk::ImageAspectFlagBits::eColor,
      .baseMipLevel = 0,
      .levelCount = vk::RemainingMipLevels,
      .baseArrayLayer = 0,
      .layerCount = vk::RemainingArrayLayers});
}

std::vector<etna::RenderTargetState::AttachmentParams> GBuffer::genColorAttachmentParams(
  vk::AttachmentLoadOp load_op)
{
  // little bit ugly
  return {
    {.image = albedo.get(), .view = albedo.getView({}), .loadOp = load_op},
    {.image = normal.get(), .view = normal.getView({}), .loadOp = load_op},
    {.image = material.get(), .view = material.getView({}), .loadOp = load_op},
    {.image = velocity.get(), .view = velocity.getView({}), .loadOp = load_op}};
}

etna::RenderTargetState::AttachmentParams GBuffer::genDepthAttachmentParams(
  vk::AttachmentLoadOp load_op, vk::AttachmentStoreOp store_op)
{
  return {
    .image = depth.get(),
    .view = depth.getView({}),
    .loadOp = load_op,
    .storeOp = store_op,
    .clearDepthStencilValue = {0.0f, 0}};
}

etna::RenderTargetState::AttachmentParams GBuffer::genShadowMappingAttachmentParams(
  uint32_t index, vk::AttachmentLoadOp load_op, vk::AttachmentStoreOp store_op)
{
  return {
    .image = shadows[index].get(),
    .view = shadows[index].getView({}),
    .loadOp = load_op,
    .storeOp = store_op};
}
