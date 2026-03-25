#version 460
#extension GL_ARB_separate_shader_objects : enable
#extension GL_GOOGLE_include_directive : require

#include "TerrainParams.h"
#include "terrain_utils.glsl"

layout(quads, fractional_even_spacing, ccw) in;

layout(location = 0) in vec2 heightMapTextureCoord[];
layout(location = 1) in vec3 worldPosition[];

layout(location = 0) out VS_OUT
{
  vec4 currentPos;
  vec4 previousPos;
  vec2 texCoord;
};

layout(set = 0, binding = 0) uniform sampler2D heightMap;

layout(set = 1, binding = 0) uniform params_t
{
  TerrainParams params;
};

layout(set = 1, binding = 1) uniform render_params_t
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

  currentVertex.y = texture(heightMap, currentTexCoord).x;

  currentPos = projView * vec4(currentVertex, 1.0);
  previousPos = previousProjView * vec4(currentVertex, 1.0);

  texCoord = currentTexCoord;

  gl_Position = currentPos;
}
