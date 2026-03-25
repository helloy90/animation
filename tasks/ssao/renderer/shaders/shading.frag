#version 460
#extension GL_ARB_separate_shader_objects : enable
#extension GL_GOOGLE_include_directive : require

#include "UniformParams.h"
#include "Light.h"
#include "DirectionalLight.h"

// NOTE - for now set here
#define SHADOW_CASCADES 4

layout(location = 0) out vec4 fragColor;

layout(set = 0, binding = 0, r11f_g11f_b10f) uniform image2D gAlbedo;
layout(set = 0, binding = 1, rgba16_snorm) uniform image2D gNormal;
layout(set = 0, binding = 2, rgba8) uniform image2D gMaterial;
layout(set = 0, binding = 3, r32f) uniform image2D gOcclusion;
layout(set = 0, binding = 4) uniform sampler2D gDepth;
layout(set = 0, binding = 5) uniform sampler2D gShadow[SHADOW_CASCADES];

layout(set = 1, binding = 0) uniform params_t
{
  UniformParams params;
};

layout(set = 1, binding = 1) readonly buffer lights_t
{
  Light lightsBuffer[];
};

layout(set = 1, binding = 2) readonly buffer directional_lights_t
{
  DirectionalLight directionalLightsBuffer[];
};

// fighting alignment rules here
layout(set = 1, binding = 3) readonly buffer shadow_casting_dir_lights_t
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


layout(set = 1, binding = 4) readonly uniform light_params_t
{
  uint lightsAmount;
  uint directionalLightsAmount;
  uint shadowCastingDirLightsAmount; // 1 for now
  float[] _;
};

layout(set = 1, binding = 5) uniform samplerCube cubemap;

layout(push_constant) uniform resolution_t
{
  uvec2 resolution;
};


// PBR -------------------------------------------
const float PI = 3.14159265359;

float clampedDot(vec3 x, vec3 y)
{
  return clamp(dot(x, y), 0.0, 1.0);
}

float heaviside(float x)
{
  if (x > 0.0)
  {
    return 1.0;
  }
  return 0.0;
}

float getAttenuation(float range, float pointDistance)
{
  float distanceSq = pointDistance * pointDistance;
  if (range <= 0)
  {
    return 1.0 / (distanceSq);
  }
  return max(min(1.0 - pow(pointDistance / range, 4.0), 1.0), 0.0) / (distanceSq);
}

vec3 getLightIntensity(Light light, vec3 pointToLight)
{
  float attenuation = getAttenuation(light.radius, length(pointToLight));

  return attenuation * light.intensity * light.color;
}

vec3 getLightIntensity(DirectionalLight light, vec3 pointToLight)
{
  float attenuation = 1.0f;

  return attenuation * light.intensity * light.color;
}

vec3 getIBLGGXFrensel(vec3 normal, vec3 viewDir, float roughness, vec3 f0, float specularWeight)
{
  return vec3(0);
}

vec3 IBL()
{
  return vec3(0);
}

float D_GGX(float alphaRoughness, float NdotH)
{
  float alphaRoughnessSq = alphaRoughness * alphaRoughness;
  float denom = NdotH * NdotH * (alphaRoughnessSq - 1.0) + 1.0;
  return alphaRoughnessSq / (PI * denom * denom);
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
  return color / PI;
}

// everything, except light, should be in the same space
vec3 computeLightPBR(
  vec3 base_color,
  vec3 pos,
  Light light,
  vec3 normal,
  vec3 camera_position,
  vec3 reflection,
  vec4 material,
  bool view_space)
{
  const float roughness = material.g;
  const float metallic = material.b;

  const float alphaRoughness = roughness * roughness;

  const vec3 pointToLight =
    view_space ? ((params.view * vec4(light.pos, 1.0)).xyz - pos) : (light.pos - pos);

  const vec3 fromPosToCamera = normalize(camera_position - pos);       // V
  const vec3 fromPosToLight = normalize(pointToLight);                 // L
  const vec3 surfaceNormal = normalize(normal);                        // N
  const vec3 halfVector = normalize(fromPosToLight + fromPosToCamera); // H

  const float VdotH = clampedDot(fromPosToCamera, halfVector);
  const float HdotL = clampedDot(halfVector, fromPosToLight);
  const float NdotL = clampedDot(surfaceNormal, fromPosToLight);
  const float NdotV = clampedDot(surfaceNormal, fromPosToCamera) + 0.00001;
  const float NdotH = clampedDot(surfaceNormal, halfVector);

  vec3 lightIntensity = getLightIntensity(light, pointToLight);

  vec3 f0 = vec3(0.04);
  vec3 dielectricFrensel = frenselSchlick(f0, VdotH);
  vec3 metalFrensel = frenselSchlick(base_color * reflection, VdotH);
  vec3 diffuse = lightIntensity * NdotL * diffuseBrdf(base_color);

  vec3 specularMetal =
    lightIntensity * NdotL * BRDFSpecular_GGX(alphaRoughness, NdotL, NdotV, NdotH);
  vec3 specularDielectric = specularMetal;

  vec3 metalBrdf = metalFrensel * specularMetal;
  vec3 dielectricBrdf = mix(diffuse, specularDielectric, dielectricFrensel);

  return mix(dielectricBrdf, metalBrdf, metallic);
}

// everything, except light, should be in the same space
vec3 computeLightPBR(
  vec3 base_color,
  vec3 pos,
  DirectionalLight light,
  vec3 normal,
  vec3 camera_position,
  vec3 reflection,
  vec4 material,
  bool view_space)
{
  const float roughness = material.g;
  const float metallic = material.b;

  const float alphaRoughness = roughness * roughness;

  const vec3 pointToLight =
    view_space ? (params.view * vec4(-light.direction, 0)).xyz : (-light.direction);

  const vec3 fromPosToCamera = normalize(camera_position - pos);       // V
  const vec3 fromPosToLight = normalize(pointToLight);                 // L
  const vec3 surfaceNormal = normalize(normal);                        // N
  const vec3 halfVector = normalize(fromPosToLight + fromPosToCamera); // H

  const float VdotH = clampedDot(fromPosToCamera, halfVector);
  const float HdotL = clampedDot(halfVector, fromPosToLight);
  const float NdotL = clampedDot(surfaceNormal, fromPosToLight);
  const float NdotV = clampedDot(surfaceNormal, fromPosToCamera) + 0.00001;
  const float NdotH = clampedDot(surfaceNormal, halfVector);

  vec3 lightIntensity = getLightIntensity(light, pointToLight);

  vec3 f0 = vec3(0.04);
  vec3 dielectricFrensel = frenselSchlick(f0, VdotH);
  vec3 metalFrensel = frenselSchlick(base_color * reflection, VdotH);
  vec3 diffuse = lightIntensity * NdotL * diffuseBrdf(base_color);

  vec3 specularMetal =
    lightIntensity * NdotL * BRDFSpecular_GGX(alphaRoughness, NdotL, NdotV, NdotH);
  vec3 specularDielectric = specularMetal;

  vec3 metalBrdf = metalFrensel * specularMetal;
  vec3 dielectricBrdf = mix(diffuse, specularDielectric, dielectricFrensel);

  return mix(dielectricBrdf, metalBrdf, metallic);
}
// -----------------------------------------------

vec3 debugGetShadowCascadeColor(uint cascade)
{
  vec3 shadowColor = vec3(0);
  if (!params.colorShadows)
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

  for (int x = -params.pcfRange; x <= params.pcfRange; x++)
  {
    for (int y = -params.pcfRange; y <= params.pcfRange; y++)
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
  const ivec2 texCoord = ivec2(gl_FragCoord.xy);
  const vec2 uvTexCoord = vec2(texCoord) / resolution;

  const vec3 albedo = imageLoad(gAlbedo, texCoord).rgb;

  const vec3 normal = normalize(imageLoad(gNormal, texCoord).xyz);
  const vec3 viewSpaceNormal = normalize((transpose(params.invView) * vec4(normal, 0.0)).xyz);

  const vec4 material = imageLoad(gMaterial, texCoord);
  const float occlusion = imageLoad(gOcclusion, texCoord).r;

  const float depth = texture(gDepth, uvTexCoord).x;

  const vec4 screenSpacePosition = vec4(uvTexCoord * 2.0 - 1.0, depth, 1.0);

  vec4 viewSpacePosition = params.invProj * screenSpacePosition;
  viewSpacePosition /= viewSpacePosition.w;

  vec4 worldSpacePosition = (params.invProjView * screenSpacePosition);
  worldSpacePosition /= worldSpacePosition.w;

  const vec3 cameraViewPosition = vec3(0);

  const vec3 viewDirection = (viewSpacePosition.xyz - cameraViewPosition);
  const vec3 reflection = texture(cubemap, reflect(viewDirection, viewSpaceNormal)).rgb;

  const bool calcLightInViewSpace = true;

  // change to IBL later
  vec3 color = vec3(albedo * 0.4 * (params.useOcclusion ? occlusion : 1.0));

  vec3 skyboxTexCoord = (params.cubemapTexCoordProj * vec4(screenSpacePosition.xy, 1.0, 1.0)).xyz;
  vec3 skyboxColor = texture(cubemap, normalize(skyboxTexCoord)).rgb;

  DirectionalLight shadowCastingDirLight = {
    shadowCastingDirLightDirection, shadowCastingDirLightIntensity, shadowCastingDirLightColor};

  vec3 point = shadowCastingDirLight.color *
    pow(clampedDot(
          normalize(-viewDirection),
          normalize(
            calcLightInViewSpace ? (params.view * vec4(shadowCastingDirLight.direction, 0)).xyz
                                 : shadowCastingDirLight.direction)),
        3500.0);
  skyboxColor += point;

  // if (depth >= 1.0)
  if (depth <= 0.0)
  {
    fragColor = vec4(skyboxColor, 1.0);
    return;
  }

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

  const float shadow = (params.usePCF) ? computeShadow(
                                           currentShadowTexCoordAndDepth.xy,
                                           currentShadowTexCoordAndDepth.z,
                                           shadowBias,
                                           currentCascade,
                                           texelSize)
                                       : getShadowFromTexture(
                                           currentShadowTexCoordAndDepth.xy,
                                           vec2(0, 0),
                                           currentShadowTexCoordAndDepth.z,
                                           shadowBias,
                                           currentCascade);

  const float nextShadow = (interpolator < 0.00001)
    ? 0.0
    : ((params.usePCF) ? computeShadow(
                           nextShadowTexCoordAndDepth.xy,
                           nextShadowTexCoordAndDepth.z,
                           shadowBias,
                           overlappingCascade,
                           texelSize)
                       : getShadowFromTexture(
                           nextShadowTexCoordAndDepth.xy,
                           vec2(0, 0),
                           nextShadowTexCoordAndDepth.z,
                           shadowBias,
                           overlappingCascade));

  const float finalShadow = mix(shadow, nextShadow, interpolator);

  const vec3 finalShadowColor = mix(shadowColor, nextShadowColor, interpolator);

  vec3 pbrColor = computeLightPBR(
    albedo,
    viewSpacePosition.xyz,
    shadowCastingDirLight,
    viewSpaceNormal,
    cameraViewPosition,
    reflection,
    material,
    calcLightInViewSpace);

  color += pbrColor * (1.0 - finalShadow) + finalShadowColor * finalShadow;

  for (uint i = 0; i < directionalLightsAmount; i++)
  {
    DirectionalLight currentLight = directionalLightsBuffer[i];

    vec3 pbrColor = computeLightPBR(
      albedo,
      viewSpacePosition.xyz,
      currentLight,
      viewSpaceNormal,
      cameraViewPosition,
      reflection,
      material,
      calcLightInViewSpace);
    skyboxColor += point;
    color += pbrColor;
  }

  for (uint i = 0; i < lightsAmount; i++)
  {
    Light currentLight = lightsBuffer[i];

    float dist = length(currentLight.pos - worldSpacePosition.xyz);
    if (dist > currentLight.radius)
    {
      continue;
    }

    vec3 pbrColor = computeLightPBR(
      albedo,
      viewSpacePosition.xyz,
      currentLight,
      viewSpaceNormal,
      cameraViewPosition,
      reflection,
      material,
      calcLightInViewSpace);
    color += pbrColor;
  }

  fragColor = vec4(color, 1);
}
