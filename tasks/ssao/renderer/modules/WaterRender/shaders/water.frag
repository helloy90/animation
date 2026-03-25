#version 460
#extension GL_ARB_separate_shader_objects : enable
#extension GL_GOOGLE_include_directive : require

#include "WaterRenderParams.h"
#include "WaterParams.h"

#include "DirectionalLight.h"

// NOTE - for now set here
#define SHADOW_CASCADES 4

layout(location = 0) in VS_OUT
{
  vec4 currentPos;
  vec4 previousPos;
  vec3 worldPos;
  vec3 normal;
  vec2 texCoord;
};

layout(location = 0) out vec4 fragColor;
layout(location = 1) out vec2 gVelocity;

layout(binding = 0) uniform params_t
{
  WaterParams waterParams;
};

layout(binding = 1) uniform water_render_params_t
{
  WaterRenderParams params;
};

layout(binding = 2) uniform sampler2D heightMap;
layout(binding = 3) uniform sampler2D normalMap;
layout(binding = 4) uniform sampler2D gShadow[SHADOW_CASCADES];
layout(binding = 5) uniform samplerCube skybox;

// fighting alignment rules here
layout(binding = 6) readonly buffer shadow_casting_dir_lights_t
{
  vec3 shadowCastingDirLightDirection;
  float shadowCastingDirLightIntensity;
  vec3 shadowCastingDirLightColor;
  uint cascadesAmount;
  float nearPlanesBackwardOffset;
  float _padding[7];
  mat4 lightProjViews[SHADOW_CASCADES];
  float planes[SHADOW_CASCADES + 1];
};

layout(binding = 7) uniform render_params_t
{
  mat4 projView;
  mat4 previousProjView;
  vec2 currentJitter;
  vec2 previousJitter;
  vec3 cameraWorldPosition;
};

layout(push_constant) uniform push_constant_t
{
  mat4 viewMatrix;
  mat4 invViewMatrix;
};
// -----------------------------------------------

const float kPi = 3.1415926535897932384626433832795;

float clampedDot(vec3 x, vec3 y)
{
  return clamp(dot(x, y), 0.0, 1.0);
}

float D_GGX(float alphaRoughness, float NdotH)
{
  float alphaRoughnessSq = alphaRoughness * alphaRoughness;
  float denom = NdotH * NdotH * (alphaRoughnessSq - 1.0) + 1.0;
  return alphaRoughnessSq / (kPi * denom * denom);
}

float visibility_GGX_Correlated(float alphaRoughness, float NdotL, float NdotV)
{
  float alphaRoughnessSq = alphaRoughness * alphaRoughness;
  float GGXV = NdotL * sqrt(NdotV * NdotV * (1.0 - alphaRoughnessSq) + alphaRoughnessSq);
  float GGXL = NdotV * sqrt(NdotL * NdotL * (1.0 - alphaRoughnessSq) + alphaRoughnessSq);
  float GGX = GGXL + GGXV;
  if (GGX > 0.0)
  {
    return 0.5 / GGX;
  }
  return 0.0;
}

// https://google.github.io/filament/Filament.html#materialsystem/specularbrdf/geometricshadowing(specularg)
float smithGGXorrelatedMasking(float alphaRoughness, float AnyDotAny)
{
  float alphaRoughnessSq = alphaRoughness * alphaRoughness;
  return 0.5 *
    (sqrt(alphaRoughnessSq + (1 - alphaRoughnessSq) * AnyDotAny * AnyDotAny) / AnyDotAny - 1);
}

vec3 BRDFSpecular_GGX(float alphaRoughness, float NdotL, float NdotV, float NdotH)
{
  float visibility = visibility_GGX_Correlated(alphaRoughness, NdotL, NdotV);
  float distribution = D_GGX(alphaRoughness, NdotH);

  return vec3(visibility * distribution);
}

vec3 frenselSchlick(vec3 f0, float theta)
{
  return f0 + (vec3(1.0) - f0) * pow(clamp(1.0 - abs(theta), 0.0, 1.0), 5.0);
}

vec3 diffuseBrdf(vec3 color)
{
  return color / kPi;
}
// -----------------------------------------------

vec3 debugGetShadowCascadeColor(uint cascade)
{
  vec3 shadowColor = vec3(0);
  if (true)
  {
    return vec3(0);
  }

  switch (cascade)
  {
  case 0:
    shadowColor.rgb = vec3(1.0f, 0.25f, 0.25f);
    break;
  case 1:
    shadowColor.rgb = vec3(0.25f, 1.0f, 0.25f);
    break;
  case 2:
    shadowColor.rgb = vec3(0.25f, 0.25f, 1.0f);
    break;
  case 3:
    shadowColor.rgb = vec3(1.0f, 0.25f, 1.0f);
    break;
  }
  return shadowColor;
}

uint getShadowCascade(float depth, float offset)
{
  uint cascade = 0;
  for (; cascade < cascadesAmount - 1; cascade++)
  {
    if (depth < planes[cascade + 1] - offset)
    {
      break;
    }
  }

  return cascade;
}

float getCurrentCascadeInterpolator(float depth, uint currentCascade)
{
  return clamp(
    (depth - planes[currentCascade + 1] + nearPlanesBackwardOffset) / nearPlanesBackwardOffset,
    0.0,
    1.0);
}

vec3 getShadowCoordsAndDepth(uint cascade, vec4 position)
{
  const vec4 lightSpacePos = (lightProjViews[cascade]) * position;
  const vec3 lightSpaceNDCPos = lightSpacePos.xyz / lightSpacePos.w;

  const vec2 shadowTexCoord = lightSpaceNDCPos.xy * 0.5 + vec2(0.5);
  return vec3(shadowTexCoord, lightSpaceNDCPos.z);
}

float getShadowFromTexture(
  vec2 shadowTexCoord, vec2 offset, float depth, float bias, uint currentCascade)
{
  const vec2 currentTexCoord = shadowTexCoord + offset;
  const float lightDepth = texture(gShadow[currentCascade], currentTexCoord).x;
  float shadow = (depth < lightDepth + bias) ? 0.0 : 1.0;

  return shadow;
}

float computeShadow(
  vec2 shadowTexCoord, float depth, float bias, uint currentCascade, vec2 texelSize)
{
  if (depth > 1.0)
  {
    return 0.0;
  }

  float shadow = 0.0;

  int count = 0;
  // for now forget about pcf range
  for (int x = -1; x <= 1; x++)
  {
    for (int y = -1; y <= 1; y++)
    {
      shadow +=
        getShadowFromTexture(shadowTexCoord, vec2(x, y) * texelSize, depth, bias, currentCascade);
      count++;
    }
  }

  shadow /= float(count);

  return shadow;
}

void main()
{
  DirectionalLight shadowCastingDirLight = {
    shadowCastingDirLightDirection, shadowCastingDirLightIntensity, shadowCastingDirLightColor};

  const vec4 worldSpacePosition = vec4(worldPos, 1.0);

  vec4 viewSpacePosition = viewMatrix * worldSpacePosition;
  viewSpacePosition /= viewSpacePosition.w;

  const float roughness = params.roughness;

  const float alphaRoughness = roughness * roughness;

  const vec3 pointToLight = normalize((viewMatrix * vec4(-shadowCastingDirLight.direction, 0)).xyz);

  const vec3 fromPosToCamera = normalize(-viewSpacePosition.xyz);                           // V
  const vec3 fromPosToLight = normalize(pointToLight);                                      // L
  const vec3 surfaceNormal = normalize((transpose(invViewMatrix) * vec4(normal, 0.0)).xyz); // N
  const vec3 halfVector = normalize(fromPosToLight + fromPosToCamera);                      // H

  const float VdotH = clampedDot(fromPosToCamera, halfVector);
  const float HdotL = clampedDot(halfVector, fromPosToLight);
  const float NdotL = clampedDot(surfaceNormal, fromPosToLight);
  const float NdotV = clampedDot(surfaceNormal, fromPosToCamera) + 0.00001;
  const float NdotH = clampedDot(surfaceNormal, halfVector) + 0.00001;

  const vec3 reflectedDir = reflect(-fromPosToCamera, normal);
  vec3 reflection = texture(skybox, reflectedDir).rgb * params.reflectionStrength;

  uint currentCascade = getShadowCascade(viewSpacePosition.z, 0.0);
  uint overlappingCascade = getShadowCascade(viewSpacePosition.z, nearPlanesBackwardOffset);

  float interpolator = (overlappingCascade == currentCascade)
    ? 0.0
    : getCurrentCascadeInterpolator(viewSpacePosition.z, currentCascade);

  vec3 shadowColor = debugGetShadowCascadeColor(currentCascade);
  vec3 nextShadowColor = debugGetShadowCascadeColor(overlappingCascade);

  float shadowBias = 0.005;
  vec2 texelSize = 1.0 / vec2(textureSize(gShadow[0], 0));

  vec3 currentShadowTexCoordAndDepth = getShadowCoordsAndDepth(currentCascade, worldSpacePosition);
  vec3 nextShadowTexCoordAndDepth = getShadowCoordsAndDepth(overlappingCascade, worldSpacePosition);

  const float shadow =
    // (params.usePCF) ?
    computeShadow(
      currentShadowTexCoordAndDepth.xy,
      currentShadowTexCoordAndDepth.z,
      shadowBias,
      currentCascade,
      texelSize);
  //  :
  // getShadowFromTexture(
  //   currentShadowTexCoordAndDepth.xy,
  //   vec2(0, 0),
  //   currentShadowTexCoordAndDepth.z,
  //   shadowBias,
  //   currentCascade);
  const float nextShadow = (interpolator < 0.00001) ? 0.0 :
                                                    // ((params.usePCF) ?
    computeShadow(
      nextShadowTexCoordAndDepth.xy,
      nextShadowTexCoordAndDepth.z,
      shadowBias,
      overlappingCascade,
      texelSize);
  //  :
  // getShadowFromTexture(
  //   nextShadowTexCoordAndDepth.xy,
  //   vec2(0, 0),
  //   nextShadowTexCoordAndDepth.z,
  //   shadowBias,
  //   overlappingCascade));

  const float finalShadow = mix(shadow, nextShadow, interpolator);
  const vec3 finalShadowColor = mix(shadowColor, nextShadowColor, interpolator);

  const vec3 sunIrradiance = shadowCastingDirLight.intensity * shadowCastingDirLight.color;

  vec3 f0 = vec3(0.02);
  vec3 frensel = frenselSchlick(f0, NdotV);
  vec3 diffuse = diffuseBrdf(reflection);

  vec3 specular = sunIrradiance * NdotL * BRDFSpecular_GGX(alphaRoughness, NdotL, NdotV, NdotH);

  specular = clamp(specular, 0, 1);

  vec4 displacementAndFoam = texture(heightMap, texCoord);
  float height = max(0.0, displacementAndFoam.y);

  // fake subsurface scattering
  // see
  // https://gpuopen.com/gdc-presentations/2019/gdc-2019-agtd6-interactive-water-simulation-in-atlas.pdf
  // p.52
  float k1 = params.wavePeakScatterStrength * height *
    pow(clampedDot(pointToLight, -fromPosToCamera), 4.0) *
    pow(0.5 - 0.5 * dot(pointToLight, surfaceNormal), 3.0);
  float k2 = params.scatterStrength * pow(NdotV, 2.0);
  float k3 = params.scatterShadowStrength * NdotL;
  float k4 = params.bubbleDensity;

  float lightMask = smithGGXorrelatedMasking(alphaRoughness, NdotL);

  vec3 scatter = (k1 + k2) * params.scatterColor.xyz * sunIrradiance * 1 / (1 + lightMask);
  scatter +=
    k3 * params.scatterColor.xyz * sunIrradiance + k4 * params.bubbleColor.xyz * sunIrradiance;

  vec3 brdf = max(vec3(0.0), mix(scatter, diffuse, frensel) + specular * (1.0 - finalShadow));

  float foam = clamp(displacementAndFoam.w, 0.0, 1.0);

  fragColor = vec4(
    mix(brdf, params.foamColor.xyz, foam) * (1.0 - 0.3 * finalShadow) +
      finalShadowColor * finalShadow,
    clamp(mix(0.0, 1.0, 1.0 - NdotV * params.transparencyStrength), 0.0, 1.0));

  const vec3 currentPosNDC = currentPos.xyz / currentPos.w;
  const vec3 previousPosNDC = previousPos.xyz / previousPos.w;

  gVelocity = (currentPosNDC.xy - currentJitter) - (previousPosNDC.xy - previousJitter);
}
