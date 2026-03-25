#version 460
#extension GL_ARB_separate_shader_objects : enable
#extension GL_EXT_nonuniform_qualifier : enable

struct RenderElement
{
  uint vertexOffset;
  uint indexOffset;
  uint indexCount;
  uint material;
};

struct Material
{
  vec4 baseColorFactor;
  float roughnessFactor;
  float metallicFactor;
  uint baseColorTexture;
  uint metallicRoughnessTexture;
  uint normalTexture;
  uint _padding0;
  uint _padding1;
  uint _padding2;
};

layout(location = 0) out vec4 gAlbedo;
layout(location = 1) out vec3 gNormal;
layout(location = 2) out vec4 gMaterial;
layout(location = 3) out vec2 gVelocity;

layout(set = 0, binding = 0) readonly buffer materials_t
{
  Material materials[];
};

layout(set = 0, binding = 1) uniform sampler2D textures[];

layout(set = 1, binding = 0) readonly buffer relems_t
{
  RenderElement relems[];
};

layout(set = 1, binding = 3) uniform render_params_t
{
  mat4 projView;
  mat4 previousProjView;
  vec2 currentJitter;
  vec2 previousJitter;
  vec3 cameraWorldPosition;
};

layout(location = 0) in VS_OUT
{
  vec4 currentPos;
  vec4 previousPos;
  // vec3 wNorm;
  // vec4 wTangent;
  // vec3 wBitangent;
  vec3 wNormOut;
  vec2 texCoord;
  flat uint relemIdx;
};


void main()
{
  Material currentMaterial = materials[relems[relemIdx].material];
  float currentLod =
    textureQueryLod(textures[nonuniformEXT(currentMaterial.baseColorTexture)], texCoord).x;
  gAlbedo =
    textureLod(textures[nonuniformEXT(currentMaterial.baseColorTexture)], texCoord, currentLod) *
    currentMaterial.baseColorFactor;
  gNormal = wNormOut;
  gMaterial = texture(textures[nonuniformEXT(currentMaterial.metallicRoughnessTexture)], texCoord);
  gMaterial.g *= currentMaterial.roughnessFactor;
  gMaterial.b *= currentMaterial.metallicFactor;

  const vec3 currentPosNDC = currentPos.xyz / currentPos.w;
  const vec3 previousPosNDC = previousPos.xyz / previousPos.w;

  gVelocity = (currentPosNDC.xy - currentJitter) - (previousPosNDC.xy - previousJitter);
}
