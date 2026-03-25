#version 460
#extension GL_ARB_separate_shader_objects : enable

layout(location = 0) in VS_OUT
{
  vec4 currentPos;
  vec4 previousPos;
  vec2 texCoord;
};

layout(set = 1, binding = 1) uniform render_params_t
{
  mat4 projView;
  mat4 previousProjView;
  vec2 currentJitter;
  vec2 previousJitter;
  vec3 cameraWorldPosition;
};

layout(location = 0) out vec4 gAlbedo;
layout(location = 1) out vec4 gNormal;
layout(location = 2) out vec4 gMaterial;
layout(location = 3) out vec2 gVelocity;

layout(set = 0, binding = 1) uniform sampler2D normalMap;

void main()
{
  gAlbedo = vec4(0.5, 0.5, 0.5, 1);
  gNormal = vec4(normalize(texture(normalMap, texCoord).xyz), 0.0);
  gMaterial = vec4(0, 1, 0.0, 1);

  const vec3 currentPosNDC = currentPos.xyz / currentPos.w;
  const vec3 previousPosNDC = previousPos.xyz / previousPos.w;

  gVelocity = (currentPosNDC.xy - currentJitter) - (previousPosNDC.xy - previousJitter);
}
