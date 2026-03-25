#pragma once

#include "etna/BlockingTransferHelper.hpp"
#include <etna/OneShotCmdMgr.hpp>
#include <etna/Buffer.hpp>
#include <etna/Image.hpp>
#include <etna/Etna.hpp>


namespace render_utility
{

void local_copy_buffer_to_image(
  etna::OneShotCmdMgr& one_shot_cmd_mgr,
  const etna::Buffer& buffer,
  const etna::Image& image,
  uint32_t layer_count);

void generate_mipmaps_vk_style(
  etna::OneShotCmdMgr& one_shot_cmd_mgr,
  const etna::Image& image,
  uint32_t mip_levels,
  uint32_t layer_count);

// assume images have the same resolution
// and they are 1 layer images
void blit_image(
  vk::CommandBuffer cmd_buf,
  vk::Image source_image,
  vk::Image target_image,
  vk::ImageAspectFlagBits aspect_mask,
  vk::Offset3D offset_size);

etna::Image load_texture(
  etna::BlockingTransferHelper& transfer_helper,
  etna::OneShotCmdMgr& one_shot_commands,
  std::filesystem::path filename,
  vk::Format format);

}; // namespace render_utility
