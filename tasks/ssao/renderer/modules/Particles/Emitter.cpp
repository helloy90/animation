#include "Emitter.hpp"

#include <algorithm>


Emitter::Emitter()
  : maxParticlesAmount(1024)
  , info({.position = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f), .particleType = ParticleType::Pixel})
  , spawnInfo(
      {.spawnpoint = glm::vec3(0.0f),
       .spawnRadius = 0.0f,
       .direction = glm::vec3(0.0f, 1.0f, 0.0f),
       .directionRandomness = glm::vec3(0.0f),
       .initialVelocity = 1.0f,
       .speedRandomness = 0.0f,
       .lifetime = 1.0f,
       .spawnRate = 1.0f})
  , randomGenerator(1024, 0.0f, 1.0f)
{
}

Emitter::Emitter(std::uint32_t max_amount)
  : maxParticlesAmount(max_amount)
  , info({.position = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f), .particleType = ParticleType::Pixel})
  , spawnInfo(
      {.spawnpoint = glm::vec3(0.0f),
       .spawnRadius = 0.0f,
       .direction = glm::vec3(0.0f, 1.0f, 0.0f),
       .directionRandomness = glm::vec3(0.0f),
       .initialVelocity = 1.0f,
       .speedRandomness = 0.0f,
       .lifetime = 1.0f,
       .spawnRate = 1.0f})
  , randomGenerator(1024, 0.0f, 1.0f)
{
}

void Emitter::update(const glm::vec4& z_view, float detla_time)
{
  depthLayer = glm::dot(info.position, z_view);

  for (std::size_t i = 0; i < particles.size(); i++)
  {
    particles[i].timeLeft -= detla_time;
    if (particles[i].timeLeft <= 0)
    {
      despawn(i);
      i--;
      continue;
    }
    auto& currentParticle = particles[i];
    currentParticle.position += currentParticle.velocity * detla_time;

    timeSinceLastSpawn += detla_time;
    if (timeSinceLastSpawn >= spawnInfo.spawnRate)
    {
      spawn(
        {spawnInfo.spawnpoint + glm::vec3(info.position),
         spawnInfo.lifetime,
         glm::normalize(
           (spawnInfo.direction +
            spawnInfo.directionRandomness *
              glm::vec3(randomGenerator.get(), randomGenerator.get(), randomGenerator.get()))) *
           (spawnInfo.initialVelocity + randomGenerator.get() * spawnInfo.speedRandomness)});
    }

    sort(z_view);
  }
}

void Emitter::drawGui() {}

auto Emitter::operator<=>(const Emitter& other) const
{
  return depthLayer <=> other.depthLayer;
}

void Emitter::despawn(std::size_t particle_index)
{
  std::swap(particles.back(), particles[particle_index]);
  particles.pop_back();
}

void Emitter::spawn(ParticleCPU particle)
{
  particles.emplace_back(particle);
}

void Emitter::sort(const glm::vec4& z_view)
{
  std::stable_sort(
    particles.begin(),
    particles.end(),
    [z_view](const ParticleCPU& first, const ParticleCPU& second) {
      float firstDepth = glm::dot(glm::vec4(first.position, 1.0f), z_view);
      float secondDepth = glm::dot(glm::vec4(second.position, 1.0f), z_view);
      return firstDepth < secondDepth;
    });
}
