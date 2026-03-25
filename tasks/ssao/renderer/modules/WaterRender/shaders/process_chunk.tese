#version 460
#extension GL_ARB_separate_shader_objects : enable
#extension GL_GOOGLE_include_directive : require

#include "WaterParams.h"
#include "terrain_utils.glsl"

layout(quads, fractional_even_spacing, ccw) in;

layout(location = 0) in vec2 heightMapTextureCoord[];
layout(location = 1) in vec3 worldPosition[];

layout(location = 0) out VS_OUT
{
  vec4 currentPos;
  vec4 previousPos;
  vec3 worldPos;
  vec3 normal;
  vec2 texCoord;
};

layout(binding = 0) uniform params_t
{
  WaterParams params;
};

layout(binding = 2) uniform sampler2D heightMap;
layout(binding = 3) uniform sampler2D normalMap;

layout(binding = 7) uniform render_params_t
{
  mat4 projView;
  mat4 previousProjView;
  vec2 currentJitter;
  vec2 previousJitter;
  vec3 cameraWorldPosition;
};

void main()
{

  float u = gl_TessCoord.x;
  float v = gl_TessCoord.y;

  vec3 leftLower = worldPosition[0];
  vec3 leftUpper = worldPosition[1];
  vec3 rightLower = worldPosition[2];
  vec3 rightUpper = worldPosition[3];

  vec2 texLeftLower = heightMapTextureCoord[0];
  vec2 texLeftUpper = heightMapTextureCoord[1];
  vec2 texRightLower = heightMapTextureCoord[2];
  vec2 texRightUpper = heightMapTextureCoord[3];

  vec3 currentVertex = interpolate_in_plane(leftLower, leftUpper, rightLower, rightUpper, u, v);
  vec2 currentTexCoord =
    interpolate_in_plane(texLeftLower, texLeftUpper, texRightLower, texRightUpper, u, v);

  vec3 displacement = texture(heightMap, currentTexCoord).xyz;

  currentVertex += displacement;

  currentVertex.y += params.heightOffset;

  currentPos = projView * vec4(currentVertex, 1.0);
  previousPos = previousProjView * vec4(currentVertex, 1.0);

  worldPos = currentVertex;
  normal = texture(normalMap, currentTexCoord).xyz;
  texCoord = currentTexCoord;

  gl_Position = currentPos;
}
