#version 460

layout(location = 0) out vec4 gAlbedo;
layout(location = 1) out vec3 gNormal;
layout(location = 2) out vec4 gMaterial;
layout(location = 3) out vec2 gVelocity;

layout(set = 0, binding = 0) uniform render_params_t
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
  vec3 worldPos;
};

void main()
{
  const float size = 1.0;
  const float edge = size / 32.0;
  const float faceTone = 0.5;
  const float edgeTone = 0.5;
  vec2 gridPos = mod(worldPos.xz, size);
  gAlbedo = vec4(vec3(min(gridPos.x, gridPos.y) < edge ? faceTone - edgeTone : faceTone), 1.0);

  gNormal = vec3(0.0, 1.0, 0.0);
  gMaterial = vec4(0.0, 1.0, 0.0, 1.0);

  const vec3 currentPosNDC = currentPos.xyz / currentPos.w;
  const vec3 previousPosNDC = previousPos.xyz / previousPos.w;

  gVelocity = (currentPosNDC.xy - currentJitter) - (previousPosNDC.xy - previousJitter);
}
