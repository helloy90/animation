#pragma once

#include "Renderer.hpp"
#include "scene/ArcballCamera.hpp"
#include "scene/SceneState.hpp"
#include "wsi/OsWindowingManager.hpp"


class App
{
public:
  App();

  void run();

private:
  void processInput(float dt);
  void drawFrame(float dt);

  void moveScene(const Keyboard& kb, float dt);
  // void moveCam(Camera& cam, const Keyboard& kb, float dt);
  void updateCam(ArcballCamera& cam, const Mouse& ms, float dt);

private:
  OsWindowingManager windowing;
  std::unique_ptr<OsWindow> mainWindow;

  SceneState sceneState = SceneState::None;

  glm::mat4x4 sceneTransform;
  glm::vec3 scenePosition = glm::vec3(0, 0, 0);
  float sceneYaw = glm::pi<float>();
  float speed = 2.0f;

  float camMoveSpeed = 1;
  float camRotateSpeed = 0.1f;
  float zoomSensitivity = 2.0f;
  ArcballCamera mainCam;

  std::unique_ptr<Renderer> renderer;
};
