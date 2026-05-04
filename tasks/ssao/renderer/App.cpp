#include "App.hpp"

#include <tracy/Tracy.hpp>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/matrix_decompose.hpp>
#include <glm/gtx/vector_angle.hpp>

#include "gui/ImGuiRenderer.hpp"


App::App()
{
  glm::uvec2 initialRes = {1280, 720};
  mainWindow = windowing.createWindow(
    OsWindow::CreateInfo{
      .resolution = initialRes,
      .resizeable = true,
      .refreshCb =
        [this]() {
          // NOTE: this is only called when the window is being resized.
          if (!renderer) [[unlikely]]
          {
            return;
          }

          drawFrame(0);
          FrameMark;
        },
      .resizeCb =
        [this](glm::uvec2 res) {
          if (res.x == 0 || res.y == 0)
            return;

          if (!renderer) [[unlikely]]
          {
            return;
          }

          renderer->recreateSwapchain(res);
        },
    });

  renderer.reset(new Renderer(initialRes));

  auto instExts = windowing.getRequiredVulkanInstanceExtensions();
  renderer->initVulkan(instExts);

  auto surface = mainWindow->createVkSurface(etna::get_context().getInstance());

  renderer->initFrameDelivery(std::move(surface), [this]() { return mainWindow->getResolution(); });

  mainCam.lookAt({2, 1, 2}, {0, 1, 0}, {0, 1, 0});

  // note - maybe bad
  ImGuiRenderer::enableImGuiForWindow(mainWindow->native());

  // NOTE - doing it like this because the mesh is identical, but node hierarchy is not
  renderer->loadScene(
    GRAPHICS_COURSE_RESOURCES_ROOT "/Animations/IPC/MOB1_Stand_Relaxed_Idle_IPC.fbx",
    {
      {SceneState::Idle,
       GRAPHICS_COURSE_RESOURCES_ROOT "/Animations/IPC/MOB1_Stand_Relaxed_Idle_IPC.fbx"},
      {SceneState::Walking,
       GRAPHICS_COURSE_RESOURCES_ROOT "/Animations/IPC/MOB1_Walk_F_Loop_IPC.fbx"},
      {SceneState::Running,
       GRAPHICS_COURSE_RESOURCES_ROOT "/Animations/IPC/MOB1_Run_F_Loop_IPC.fbx"},
    },
    glm::identity<glm::mat4x4>(),
    mainCam.zNear,
    mainCam.zFar);
}

void App::run()
{
  double lastTime = windowing.getTime();
  while (!mainWindow->isBeingClosed())
  {
    const double currTime = windowing.getTime();
    const float diffTime = static_cast<float>(currTime - lastTime);
    lastTime = currTime;

    windowing.poll();

    processInput(diffTime);

    drawFrame(diffTime);

    FrameMark;
  }
}
void App::moveScene(const Keyboard& kb, float dt)
{
  glm::vec3 forward = glm::rotateY(glm::vec3(0, 0, -1), sceneYaw);
  glm::vec3 right = glm::rotateY(glm::vec3(1, 0, 0), sceneYaw);

  glm::vec3 dir = {0, 0, 0};
  float yawDir = 1.0f;

  float rotationSpeed = 5.0f;
  bool running = false;

  if (is_held_down(kb[KeyboardKey::kLeftShift]))
  {
    running = true;
    speed = 6.0f;
  }
  else
  {
    running = false;
    speed = 2.0f;
  }

  if (is_held_down(kb[KeyboardKey::kS]))
  {
    dir -= forward;
    rotationSpeed /= 2.0f;
  }

  if (is_held_down(kb[KeyboardKey::kW]))
    dir += forward;

  if (is_held_down(kb[KeyboardKey::kA]))
  {
    dir -= right;
    yawDir = 1.0f;
  }

  if (is_held_down(kb[KeyboardKey::kD]))
  {
    dir += right;
    yawDir = -1.0f;
  }


  if (length(dir) > 1e-9)
  {
    sceneState = running ? SceneState::Running : SceneState::Walking;

    float angle = glm::angle(glm::normalize(dir), glm::normalize(forward));

    if (glm::dot(dir, forward) > glm::epsilon<float>())
    {
      scenePosition += glm::vec3(dt * speed * normalize(dir));
    }
    else
    {
      sceneState = SceneState::Walking;
    }

    sceneYaw += rotationSpeed * yawDir * angle * dt;
    sceneYaw = glm::mod(sceneYaw, 2 * glm::pi<float>());
  }
  else
  {
    sceneState = SceneState::Idle;
  }

  mainCam.lookAtPos = scenePosition;
  mainCam.lookAtPos.y = 1;
}

void App::processInput(float dt)
{
  ZoneScoped;

  if (mainWindow->keyboard[KeyboardKey::kEscape] == ButtonState::Falling)
    mainWindow->askToClose();

  if (is_held_down(mainWindow->keyboard[KeyboardKey::kLeftShift]))
    camMoveSpeed = 50;
  else
    camMoveSpeed = 2;

  if (mainWindow->mouse[MouseButton::mbRight] == ButtonState::Rising)
    mainWindow->captureMouse = !mainWindow->captureMouse;

  moveScene(mainWindow->keyboard, dt);
  if (mainWindow->captureMouse)
    updateCam(mainCam, mainWindow->mouse, dt);

  renderer->debugInput(mainWindow->keyboard);
}

void App::drawFrame(float dt)
{
  ZoneScoped;

  sceneTransform = glm::translate(glm::identity<glm::mat4x4>(), scenePosition) *
    glm::yawPitchRoll(sceneYaw, 0.0f, 0.0f);

  renderer->update(
    FramePacket{
      .mainCam = mainCam,
      .sceneTransform = sceneTransform,
      .currentState = sceneState,
      .currentTime = static_cast<float>(windowing.getTime()),
      .deltaTime = dt});
  renderer->drawFrame();
}

void App::updateCam(ArcballCamera& cam, const Mouse& ms, float dt)
{
  // Rotate camera based on mouse movement
  cam.rotate(-camRotateSpeed * ms.capturedPosDelta.y, camRotateSpeed * ms.capturedPosDelta.x);

  // Increase or decrease field of view based on mouse wheel
  cam.fov -= zoomSensitivity * ms.scrollDelta.y;
  if (cam.fov < 1.0f)
    cam.fov = 1.0f;
  if (cam.fov > 120.0f)
    cam.fov = 120.0f;

  cam.update(dt);
}
