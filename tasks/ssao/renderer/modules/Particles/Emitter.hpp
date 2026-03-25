#pragma once

#include <vector>

#include <glm/glm.hpp>

#include "render_utils/RandomGenerator.hpp"


enum ParticleType : uint32_t
{
  Invalid = ~uint32_t(0),
  Voxel = 0,
  Pixel = 1
};

class Emitter
{
private:
  struct ParticleCPU
  {
    glm::vec3 position;
    float timeLeft;
    glm::vec3 velocity;
  };

  struct EmitterInfo
  {
    glm::vec4 position;
    ParticleType particleType;
  };

  struct SpawnInfo
  {
    glm::vec3 spawnpoint; // relative to emitter
    float spawnRadius;    // 0 for point spawn
    glm::vec3 direction;
    glm::vec3 directionRandomness;
    float initialVelocity;
    float speedRandomness; // dispersion from initial
    float lifetime;
    float spawnRate; // time between consecutive spawns
  };

public:
  Emitter();

  explicit Emitter(EmitterInfo info);
  explicit Emitter(SpawnInfo info);
  explicit Emitter(std::uint32_t max_particles_amount);

  Emitter(EmitterInfo info, SpawnInfo spawn_info, std::uint32_t max_particles_amount);

  void update(const glm::vec4& z_view, float detla_time);

  void drawGui();

  auto operator<=>(const Emitter& other) const;

private:
  void despawn(std::size_t particle_index);

  void spawn(ParticleCPU particle);

  void sort(const glm::vec4& z_view);

private:
  std::uint32_t maxParticlesAmount;
  float timeSinceLastSpawn;
  float depthLayer;

  EmitterInfo info;
  SpawnInfo spawnInfo;

  RandomGenerator randomGenerator;

  std::vector<ParticleCPU> particles;
};
