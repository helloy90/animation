#pragma once

#include <etna/GlobalContext.hpp>
#include <etna/PerFrameCmdMgr.hpp>
#include <function2/function2.hpp>
#include <glm/glm.hpp>

#include "FramePacket.hpp"
#include "WorldRenderer.hpp"
#include "gui/ImGuiRenderer.hpp"
#include "scene/SceneState.hpp"
#include "wsi/Keyboard.hpp"


using ResolutionProvider = fu2::unique_function<glm::uvec2() const>;

class Renderer
{
public:
  explicit Renderer(glm::uvec2 resolution);
  ~Renderer();

  void initVulkan(std::span<const char*> instance_extensions);
  void initFrameDelivery(vk::UniqueSurfaceKHR surface, ResolutionProvider res_provider);
  void recreateSwapchain(glm::uvec2 res);
  void loadScene(
    const std::filesystem::path& scene_path,
    const std::vector<std::pair<SceneState, std::filesystem::path>>& animations,
    const glm::mat4x4& scene_transform,
    float near_plane,
    float far_plane);

  void debugInput(const Keyboard& kb);
  void update(const FramePacket& packet);
  void drawGui();
  void drawFrame();

  void reloadShaders();

private:
  ResolutionProvider resolutionProvider;

  std::unique_ptr<etna::Window> window;
  std::unique_ptr<etna::PerFrameCmdMgr> commandManager;

  glm::uvec2 resolution;
  bool useVsync = false;

  bool swapchainRecreationNeeded = false;

  std::unique_ptr<ImGuiRenderer> guiRenderer;

  std::unique_ptr<WorldRenderer> worldRenderer;
};
