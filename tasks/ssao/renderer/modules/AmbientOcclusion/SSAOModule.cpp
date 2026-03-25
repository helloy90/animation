#include "SSAOModule.hpp"

#include <random>

#include <tracy/Tracy.hpp>
#include <imgui.h>

#include <etna/Etna.hpp>
#include <etna/PipelineManager.hpp>
#include <etna/Profiling.hpp>

#include "render_utils/Utilities.hpp"


SSAOModule::SSAOModule() {}

void SSAOModule::allocateResources(const AllocationInfo& info)
{
  auto& ctx = etna::get_context();

  std::mt19937 generator(info.seed);
  std::uniform_real_distribution<float> range(0.0f, 1.0f);

  uint32_t noiseTextureTexelsAmount = info.noiseTextureSize.x * info.noiseTextureSize.y;
  std::vector<glm::vec2> noise;
  noise.reserve(noiseTextureTexelsAmount);

  for (uint32_t i = 0; i < noiseTextureTexelsAmount; i++)
  {
    noise.emplace_back(range(generator) * 2.0f - 1.0f, range(generator) * 2.0f - 1.0f);
  }

  pushConstants = {.kernelSize = info.kernelSize, .radius = 10.0f, .occlusionPower = 2.0f};

  std::vector<glm::vec4> kernel;
  kernel.reserve(pushConstants.kernelSize);

  for (uint32_t i = 0; i < pushConstants.kernelSize; i++)
  {
    glm::vec4 val = {
      glm::normalize(
        glm::vec3(
          range(generator) * 2.0f - 1.0f, range(generator) * 2.0f - 1.0f, range(generator))),
      0.0};

    float scale = static_cast<float>(i) / pushConstants.kernelSize;
    float scaleMult = std::lerp(0.1f, 1.0f, scale * scale);

    kernel.emplace_back(val * scaleMult);
  }

  noiseTexture = ctx.createImage(
    etna::Image::CreateInfo{
      .extent = vk::Extent3D{info.noiseTextureSize.x, info.noiseTextureSize.y, 1},
      .name = "ssaoNoiseTexture",
      .format = vk::Format::eR32G32Sfloat,
      .imageUsage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
    });

  ssaoKernel = ctx.createBuffer(
    etna::Buffer::CreateInfo{
      .size = pushConstants.kernelSize * sizeof(glm::vec4),
      .bufferUsage =
        vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferDst,
      .memoryUsage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
      .name = fmt::format("ssaoKernelBuffer")});

  matrices.emplace(ctx.getMainWorkCount(), [&ctx](std::size_t i) {
    return ctx.createBuffer(
      etna::Buffer::CreateInfo{
        .size = sizeof(glm::mat4) * 3,
        .bufferUsage = vk::BufferUsageFlagBits::eUniformBuffer,
        .memoryUsage = VMA_MEMORY_USAGE_AUTO,
        .allocationCreate =
          VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
        .name = fmt::format("ssaoMatrices{}", i)});
  });

  noiseSampler = etna::Sampler(
    etna::Sampler::CreateInfo{
      .filter = vk::Filter::eNearest,
      .addressMode = vk::SamplerAddressMode::eRepeat,
      .name = "ssaoNoiseSampler",
    });

  oneShotCommands = ctx.createOneShotCmdMgr();

  transferHelper = std::make_unique<etna::BlockingTransferHelper>(
    etna::BlockingTransferHelper::CreateInfo{.stagingSize = 4096 * 4096 * 4});

  etna::Buffer tempBuffer = ctx.createBuffer(
    etna::Buffer::CreateInfo{
      .size = noiseTextureTexelsAmount * sizeof(glm::vec2),
      .bufferUsage = vk::BufferUsageFlagBits::eStorageBuffer |
        vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eTransferSrc,
      .memoryUsage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
      .name = fmt::format("tempNoiseBufferSSAO")});


  transferHelper->uploadBuffer(*oneShotCommands, tempBuffer, 0, std::as_bytes(std::span(noise)));

  render_utility::local_copy_buffer_to_image(*oneShotCommands, tempBuffer, noiseTexture, 1);

  tempBuffer.reset();

  transferHelper->uploadBuffer(*oneShotCommands, ssaoKernel, 0, std::as_bytes(std::span(kernel)));

  auto cmdBuf = oneShotCommands->start();
  ETNA_CHECK_VK_RESULT(cmdBuf.begin(vk::CommandBufferBeginInfo{}));
  {
    etna::set_state(
      cmdBuf,
      noiseTexture.get(),
      vk::PipelineStageFlagBits2::eComputeShader,
      vk::AccessFlagBits2::eShaderSampledRead,
      vk::ImageLayout::eShaderReadOnlyOptimal,
      vk::ImageAspectFlagBits::eColor);

    etna::flush_barriers(cmdBuf);
  }
  ETNA_CHECK_VK_RESULT(cmdBuf.end());

  oneShotCommands->submitAndWait(cmdBuf);
}

void SSAOModule::loadShaders()
{
  etna::create_program("ssao", {AMBIENT_OCCLUSION_MODULE_SHADERS_ROOT "ssao.comp.spv"});
  etna::create_program("ssaoBlur", {AMBIENT_OCCLUSION_MODULE_SHADERS_ROOT "blur.comp.spv"});
}

void SSAOModule::setupPipelines()
{
  auto& pipelineManager = etna::get_context().getPipelineManager();

  ssaoPipeline = pipelineManager.createComputePipeline("ssao", {});
  blurPipeline = pipelineManager.createComputePipeline("ssaoBlur", {});
}

void SSAOModule::prepareForExecute(
  const glm::mat4& proj_matrix, const glm::mat4& inv_proj_matrix, const glm::mat4& inv_view_matrix)
{
  auto& currentMatricesBuffer = matrices->get();
  currentMatricesBuffer.map();
  std::memcpy(currentMatricesBuffer.data(), &proj_matrix, sizeof(glm::mat4));
  std::memcpy(
    currentMatricesBuffer.data() + sizeof(glm::mat4), &inv_proj_matrix, sizeof(glm::mat4));
  std::memcpy(
    currentMatricesBuffer.data() + 2 * sizeof(glm::mat4), &inv_view_matrix, sizeof(glm::mat4));
  currentMatricesBuffer.unmap();
}

void SSAOModule::execute(
  vk::CommandBuffer cmd_buf,
  const etna::Image& normal_texture,
  const etna::Image& depth_texture,
  const etna::Sampler& depth_sampler,
  const etna::Image& occlusion_texture)
{
  ZoneScoped;

  auto& currentMatricesBuffer = matrices->get();

  {
    ETNA_PROFILE_GPU(cmd_buf, ssaoMain);

    cmd_buf.bindPipeline(vk::PipelineBindPoint::eCompute, ssaoPipeline.getVkPipeline());
    occlude(
      cmd_buf,
      currentMatricesBuffer,
      normal_texture,
      depth_texture,
      depth_sampler,
      occlusion_texture);
  }

  etna::set_state(
    cmd_buf,
    occlusion_texture.get(),
    vk::PipelineStageFlagBits2::eComputeShader,
    vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite,
    vk::ImageLayout::eGeneral,
    vk::ImageAspectFlagBits::eColor);

  etna::flush_barriers(cmd_buf);

  {
    ETNA_PROFILE_GPU(cmd_buf, ssaoBlur);

    cmd_buf.bindPipeline(vk::PipelineBindPoint::eCompute, blurPipeline.getVkPipeline());
    blur(cmd_buf, occlusion_texture);
  }
}

void SSAOModule::drawGui()
{
  ImGui::Begin("Application Settings");

  if (ImGui::CollapsingHeader("Ambient Occlusion"))
  {
    ImGui::DragFloat("Sample radius", &pushConstants.radius, 0.01f, 0.0f, 128.0f);
    ImGui::DragFloat("Occlusion power", &pushConstants.occlusionPower, 0.01f, 0.0f, 10.0f);
  }

  ImGui::End();
}

void SSAOModule::occlude(
  vk::CommandBuffer cmd_buf,
  const etna::Buffer& current_matrices,
  const etna::Image& normal_texture,
  const etna::Image& depth_texture,
  const etna::Sampler& depth_sampler,
  const etna::Image& occlusion_texture)
{
  auto extent = occlusion_texture.getExtent();

  auto shaderInfo = etna::get_shader_program("ssao");

  auto set = etna::create_descriptor_set(
    shaderInfo.getDescriptorLayoutId(0),
    cmd_buf,
    {etna::Binding{0, normal_texture.genBinding({}, vk::ImageLayout::eGeneral)},
     etna::Binding{
       1, depth_texture.genBinding(depth_sampler.get(), vk::ImageLayout::eShaderReadOnlyOptimal)},
     etna::Binding{2, occlusion_texture.genBinding({}, vk::ImageLayout::eGeneral)},
     etna::Binding{
       3, noiseTexture.genBinding(noiseSampler.get(), vk::ImageLayout::eShaderReadOnlyOptimal)},
     etna::Binding{4, ssaoKernel.genBinding()},
     etna::Binding{5, current_matrices.genBinding()}});

  cmd_buf.bindDescriptorSets(
    vk::PipelineBindPoint::eCompute, ssaoPipeline.getVkPipelineLayout(), 0, {set.getVkSet()}, {});

  cmd_buf.pushConstants<PushConstants>(
    ssaoPipeline.getVkPipelineLayout(), vk::ShaderStageFlagBits::eCompute, 0, {pushConstants});

  cmd_buf.dispatch(
    (static_cast<uint32_t>(extent.width) + 31) / 32,
    (static_cast<uint32_t>(extent.height) + 31) / 32,
    1);
}

void SSAOModule::blur(vk::CommandBuffer cmd_buf, const etna::Image& occlusion_texture)
{
  auto extent = occlusion_texture.getExtent();

  auto shaderInfo = etna::get_shader_program("ssaoBlur");

  auto set = etna::create_descriptor_set(
    shaderInfo.getDescriptorLayoutId(0),
    cmd_buf,
    {etna::Binding{0, occlusion_texture.genBinding({}, vk::ImageLayout::eGeneral)}});

  cmd_buf.bindDescriptorSets(
    vk::PipelineBindPoint::eCompute, blurPipeline.getVkPipelineLayout(), 0, {set.getVkSet()}, {});

  cmd_buf.dispatch(
    (static_cast<uint32_t>(extent.width) + 31) / 32,
    (static_cast<uint32_t>(extent.height) + 31) / 32,
    1);
}
