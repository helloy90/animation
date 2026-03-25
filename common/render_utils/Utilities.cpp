#include "Utilities.hpp"

#include <tracy/Tracy.hpp>
#include <stb_image.h>

#include <etna/Profiling.hpp>


namespace render_utility
{

void local_copy_buffer_to_image(
  etna::OneShotCmdMgr& one_shot_cmd_mgr,
  const etna::Buffer& buffer,
  const etna::Image& image,
  uint32_t layer_count)
{
  auto commandBuffer = one_shot_cmd_mgr.start();

  auto extent = image.getExtent();

  ETNA_CHECK_VK_RESULT(commandBuffer.begin(vk::CommandBufferBeginInfo{}));
  {
    etna::set_state(
      commandBuffer,
      image.get(),
      vk::PipelineStageFlagBits2::eTransfer,
      vk::AccessFlagBits2::eTransferWrite,
      vk::ImageLayout::eTransferDstOptimal,
      vk::ImageAspectFlagBits::eColor);

    etna::flush_barriers(commandBuffer);

    vk::BufferImageCopy copyRegion = {
      .bufferOffset = 0,
      .bufferRowLength = 0,
      .bufferImageHeight = 0,
      .imageSubresource =
        {.aspectMask = vk::ImageAspectFlagBits::eColor,
         .mipLevel = 0,
         .baseArrayLayer = 0,
         .layerCount = layer_count},
      .imageExtent =
        vk::Extent3D{static_cast<uint32_t>(extent.width), static_cast<uint32_t>(extent.height), 1}};

    commandBuffer.copyBufferToImage(
      buffer.get(), image.get(), vk::ImageLayout::eTransferDstOptimal, 1, &copyRegion);
  }
  ETNA_CHECK_VK_RESULT(commandBuffer.end());

  one_shot_cmd_mgr.submitAndWait(commandBuffer);
}

void generate_mipmaps_vk_style(
  etna::OneShotCmdMgr& one_shot_cmd_mgr,
  const etna::Image& image,
  uint32_t mip_levels,
  uint32_t layer_count)
{
  ZoneScoped;

  auto extent = image.getExtent();

  auto commandBuffer = one_shot_cmd_mgr.start();

  auto vkImage = image.get();

  ETNA_CHECK_VK_RESULT(commandBuffer.begin(vk::CommandBufferBeginInfo{}));
  {
    int32_t mipWidth = extent.width;
    int32_t mipHeight = extent.height;

    vk::ImageMemoryBarrier barrier{
      .srcQueueFamilyIndex = vk::QueueFamilyIgnored,
      .dstQueueFamilyIndex = vk::QueueFamilyIgnored,
      .image = vkImage,
      .subresourceRange = {
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .levelCount = 1,
        .baseArrayLayer = 0,
        .layerCount = layer_count,
      }};

    for (uint32_t i = 1; i < mip_levels; i++)
    {
      barrier.subresourceRange.baseMipLevel = i - 1;
      barrier.oldLayout = vk::ImageLayout::eTransferDstOptimal;
      barrier.newLayout = vk::ImageLayout::eTransferSrcOptimal;
      barrier.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
      barrier.dstAccessMask = vk::AccessFlagBits::eTransferRead;

      commandBuffer.pipelineBarrier(
        vk::PipelineStageFlagBits::eTransfer,
        vk::PipelineStageFlagBits::eTransfer,
        vk::DependencyFlagBits::eByRegion,
        0,
        nullptr,
        0,
        nullptr,
        1,
        &barrier);

      std::array srcOffset = {vk::Offset3D{0, 0, 0}, vk::Offset3D{mipWidth, mipHeight, 1}};

      auto srcImageSubrecourceLayers = vk::ImageSubresourceLayers{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .mipLevel = i - 1,
        .baseArrayLayer = 0,
        .layerCount = layer_count};

      std::array dstOffset = {
        vk::Offset3D{0, 0, 0},
        vk::Offset3D{mipWidth > 1 ? mipWidth / 2 : 1, mipHeight > 1 ? mipHeight / 2 : 1, 1}};

      auto dstImageSubrecourceLayers = vk::ImageSubresourceLayers{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .mipLevel = i,
        .baseArrayLayer = 0,
        .layerCount = layer_count};

      auto imageBlit = vk::ImageBlit{
        .srcSubresource = srcImageSubrecourceLayers,
        .srcOffsets = srcOffset,
        .dstSubresource = dstImageSubrecourceLayers,
        .dstOffsets = dstOffset};

      commandBuffer.blitImage(
        vkImage,
        vk::ImageLayout::eTransferSrcOptimal,
        vkImage,
        vk::ImageLayout::eTransferDstOptimal,
        1,
        &imageBlit,
        vk::Filter::eLinear);

      barrier.oldLayout = vk::ImageLayout::eTransferSrcOptimal;
      barrier.newLayout = vk::ImageLayout::eTransferDstOptimal;
      barrier.srcAccessMask = vk::AccessFlagBits::eTransferRead;
      barrier.dstAccessMask = vk::AccessFlagBits::eTransferWrite;

      commandBuffer.pipelineBarrier(
        vk::PipelineStageFlagBits::eTransfer,
        vk::PipelineStageFlagBits::eTransfer,
        vk::DependencyFlagBits::eByRegion,
        0,
        nullptr,
        0,
        nullptr,
        1,
        &barrier);

      if (mipWidth > 1)
      {
        mipWidth /= 2;
      }
      if (mipHeight > 1)
      {
        mipHeight /= 2;
      }
    }

    etna::set_state(
      commandBuffer,
      image.get(),
      vk::PipelineStageFlagBits2::eFragmentShader,
      vk::AccessFlagBits2::eShaderSampledRead,
      vk::ImageLayout::eShaderReadOnlyOptimal,
      vk::ImageAspectFlagBits::eColor);

    etna::flush_barriers(commandBuffer);
  }
  ETNA_CHECK_VK_RESULT(commandBuffer.end());

  one_shot_cmd_mgr.submitAndWait(commandBuffer);
}

void blit_image(
  vk::CommandBuffer cmd_buf,
  vk::Image source_image,
  vk::Image target_image,
  vk::ImageAspectFlagBits aspect_mask,
  vk::Offset3D offset_size)
{
  ETNA_PROFILE_GPU(cmd_buf, blitImage);

  std::array srcOffset = {vk::Offset3D{}, offset_size};
  auto srdImageSubrecourceLayers = vk::ImageSubresourceLayers{
    .aspectMask = aspect_mask, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1};

  std::array dstOffset = {vk::Offset3D{}, offset_size};
  auto dstImageSubrecourceLayers = vk::ImageSubresourceLayers{
    .aspectMask = aspect_mask, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1};

  auto imageBlit = vk::ImageBlit2{
    .sType = vk::StructureType::eImageBlit2,
    .pNext = nullptr,
    .srcSubresource = srdImageSubrecourceLayers,
    .srcOffsets = srcOffset,
    .dstSubresource = dstImageSubrecourceLayers,
    .dstOffsets = dstOffset};

  auto blitInfo = vk::BlitImageInfo2{
    .sType = vk::StructureType::eBlitImageInfo2,
    .pNext = nullptr,
    .srcImage = source_image,
    .srcImageLayout = vk::ImageLayout::eTransferSrcOptimal,
    .dstImage = target_image,
    .dstImageLayout = vk::ImageLayout::eTransferDstOptimal,
    .regionCount = 1,
    .pRegions = &imageBlit,
    .filter = (aspect_mask == vk::ImageAspectFlagBits::eColor) ? vk::Filter::eLinear
                                                               : vk::Filter::eNearest};

  cmd_buf.blitImage2(&blitInfo);
}

etna::Image load_texture(
  etna::BlockingTransferHelper& transfer_helper,
  etna::OneShotCmdMgr& one_shot_commands,
  std::filesystem::path path,
  vk::Format format)
{
  ZoneScoped;
  auto& ctx = etna::get_context();

  uint32_t layerCount = 1;
  int width, height, channels;

  auto filepathString = path.generic_string<char>();
  unsigned char* textureData =
    stbi_load(filepathString.c_str(), &width, &height, &channels, STBI_rgb_alpha);

  ETNA_VERIFYF(textureData != nullptr, "Texture {} is not loaded!", filepathString.c_str());

  uint32_t mipLevels = static_cast<uint32_t>(std::floor(std::log2(std::max(width, height)))) + 1;

  auto filenameString = path.filename().generic_string<char>();
  const vk::DeviceSize textureSize = width * height * 4;
  etna::Buffer textureBuffer = ctx.createBuffer(
    etna::Buffer::CreateInfo{
      .size = textureSize,
      .bufferUsage = vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst,
      .name = filenameString + "_buffer",
    });

  auto source = std::span<unsigned char>(textureData, textureSize);
  transfer_helper.uploadBuffer(one_shot_commands, textureBuffer, 0, std::as_bytes(source));

  etna::Image texture = ctx.createImage(
    etna::Image::CreateInfo{
      .extent = vk::Extent3D{static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1},
      .name = filenameString + "_texture",
      .format = format,
      .imageUsage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst |
        vk::ImageUsageFlagBits::eTransferSrc,
      .mipLevels = mipLevels});

  render_utility::local_copy_buffer_to_image(one_shot_commands, textureBuffer, texture, layerCount);

  render_utility::generate_mipmaps_vk_style(one_shot_commands, texture, mipLevels, layerCount);

  stbi_image_free(textureData);

  return texture;
}

} // namespace render_utility
