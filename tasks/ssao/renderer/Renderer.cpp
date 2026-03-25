#include "Renderer.hpp"

#include <etna/GlobalContext.hpp>
#include <etna/Etna.hpp>
#include <etna/RenderTargetStates.hpp>
#include <etna/PipelineManager.hpp>
#include <etna/Profiling.hpp>

#include <imgui.h>


Renderer::Renderer(glm::uvec2 res)
  : resolution{res}
{
}

void Renderer::initVulkan(std::span<const char*> instance_extensions)
{
  std::vector<const char*> instanceExtensions;

  for (auto ext : instance_extensions)
    instanceExtensions.push_back(ext);

  std::vector<const char*> deviceExtensions;

  deviceExtensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
  deviceExtensions.push_back(VK_KHR_SHADER_DRAW_PARAMETERS_EXTENSION_NAME);

  etna::initialize(
    etna::InitParams{
      .applicationName = "project_renderer",
      .applicationVersion = VK_MAKE_VERSION(0, 1, 0),
      .instanceExtensions = instanceExtensions,
      .deviceExtensions = deviceExtensions,
      .features =
        {.features =
           {.independentBlend = vk::True,
            .tessellationShader = vk::True,
            .multiDrawIndirect = vk::True,
            .drawIndirectFirstInstance = vk::True,
            .fillModeNonSolid = vk::True /*debug*/,
            .fragmentStoresAndAtomics = vk::True}},
      .descriptorIndexingFeatures =
        {
          .shaderSampledImageArrayNonUniformIndexing = vk::True,
          .descriptorBindingPartiallyBound = vk::True,
          .descriptorBindingVariableDescriptorCount = vk::True,
          .runtimeDescriptorArray = vk::True,
        },
      .physicalDeviceIndexOverride = {},
      .numFramesInFlight = 2,
    });
}

void Renderer::initFrameDelivery(vk::UniqueSurfaceKHR a_surface, ResolutionProvider res_provider)
{
  resolutionProvider = std::move(res_provider);

  auto& ctx = etna::get_context();

  commandManager = ctx.createPerFrameCmdMgr();

  window = ctx.createWindow(
    etna::Window::CreateInfo{
      .surface = std::move(a_surface),
    });

  auto [w, h] = window->recreateSwapchain(
    etna::Window::DesiredProperties{
      .resolution = {resolution.x, resolution.y},
      .vsync = useVsync,
    });

  resolution = {w, h};

  worldRenderer = std::make_unique<WorldRenderer>(WorldRenderer::InitInfo{
    .renderTargetFormat = vk::Format::eB10G11R11UfloatPack32,
    .shadowCascadesAmount = 4,
    .wireframeEnabled = false,
    .tonemappingEnabled = false,
    .timeStopped = false,
    .taaEnabled = true,
    .ssaoEnabled = true});

  guiRenderer = std::make_unique<ImGuiRenderer>(window->getCurrentFormat());

  worldRenderer->allocateResources(resolution);
  worldRenderer->loadShaders();
  worldRenderer->setupRenderPipelines();
  worldRenderer->loadCubemap();
}

// little bit ugly
void Renderer::loadScene(std::filesystem::path path, float near_plane, float far_plane)
{
  worldRenderer->loadScene(path, near_plane, far_plane);
}

void Renderer::recreateSwapchain(glm::uvec2 res)
{
  if (!worldRenderer) [[unlikely]]
  {
    return;
  }

  auto& ctx = etna::get_context();

  spdlog::info("recreating swapchain");

  ETNA_CHECK_VK_RESULT(ctx.getDevice().waitIdle());

  auto [w, h] = window->recreateSwapchain(
    etna::Window::DesiredProperties{
      .resolution = {res.x, res.y},
      .vsync = useVsync,
    });
  resolution = {w, h};

  worldRenderer->allocateResources(resolution);
  worldRenderer->setupRenderPipelines();
  worldRenderer->loadInfo();
}

void Renderer::debugInput(const Keyboard& kb)
{
  if (kb[KeyboardKey::kB] == ButtonState::Falling)
  {
    reloadShaders();
  }

  worldRenderer->debugInput(kb);
}

void Renderer::update(const FramePacket& packet)
{
  if (!worldRenderer) [[unlikely]]
  {
    return;
  }

  worldRenderer->update(packet);
}

void Renderer::drawGui()
{
  ImGui::Begin("Application Settings");

  worldRenderer->drawGui();

  if (ImGui::Checkbox("Use Vsync", &useVsync))
  {
    swapchainRecreationNeeded = true;
  }

  if (ImGui::Button("Reload shaders"))
  {
    reloadShaders();
  }

  ImGui::End();
}

void Renderer::drawFrame()
{
  ZoneScoped;

  if (!worldRenderer) [[unlikely]]
  {
    return;
  }

  {
    ZoneScopedN("drawGui");
    guiRenderer->nextFrame();
    ImGui::NewFrame();
    drawGui();
    ImGui::Render();
  }

  auto currentCmdBuf = commandManager->acquireNext();

  etna::begin_frame();

  auto nextSwapchainImage = window->acquireNext();

  if (nextSwapchainImage)
  {
    auto [image, view, availableSem, readyForPresentSem] = *nextSwapchainImage;

    ETNA_CHECK_VK_RESULT(currentCmdBuf.begin(vk::CommandBufferBeginInfo{}));
    {
      ETNA_PROFILE_GPU(currentCmdBuf, renderFrame);

      worldRenderer->renderWorld(currentCmdBuf, image); // view);

      etna::set_state(
        currentCmdBuf,
        image,
        vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        vk::AccessFlagBits2::eColorAttachmentRead,
        vk::ImageLayout::eColorAttachmentOptimal,
        vk::ImageAspectFlagBits::eColor);

      {
        ImDrawData* pDrawData = ImGui::GetDrawData();
        guiRenderer->render(
          currentCmdBuf, {{0, 0}, {resolution.x, resolution.y}}, image, view, pDrawData);
      }

      etna::set_state(
        currentCmdBuf,
        image,
        vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        {},
        vk::ImageLayout::ePresentSrcKHR,
        vk::ImageAspectFlagBits::eColor);

      etna::flush_barriers(currentCmdBuf);

      ETNA_READ_BACK_GPU_PROFILING(currentCmdBuf);
    }
    ETNA_CHECK_VK_RESULT(currentCmdBuf.end());

    auto renderingDone = commandManager->submit(
      std::move(currentCmdBuf), std::move(availableSem), std::move(readyForPresentSem));

    const bool presented = window->present(std::move(renderingDone), view);

    if (!presented)
      nextSwapchainImage = std::nullopt;

    if (swapchainRecreationNeeded)
    {
      nextSwapchainImage = std::nullopt;
      swapchainRecreationNeeded = false;
    }
  }

  if (!nextSwapchainImage)
  {
    auto res = resolutionProvider();
    if (res.x != 0 && res.y != 0)
    {
      recreateSwapchain(resolution);
    }
  }

  etna::end_frame();
}

void Renderer::reloadShaders()
{
  const int retval = std::system(
    "cd " GRAPHICS_COURSE_ROOT "/build"
    " && cmake --build . --target project_renderer_shaders");
  if (retval != 0)
    spdlog::warn("Shader recompilation returned a non-zero return code!");
  else
  {
    ETNA_CHECK_VK_RESULT(etna::get_context().getDevice().waitIdle());
    etna::reload_shaders();
    spdlog::info("Successfully reloaded shaders!");
  }
}

Renderer::~Renderer()
{
  ETNA_CHECK_VK_RESULT(etna::get_context().getDevice().waitIdle());
}
