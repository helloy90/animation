#version 460
#extension GL_ARB_separate_shader_objects : enable
#extension GL_GOOGLE_include_directive : require

#include "terrain_utils.glsl"

layout(location = 0) out uint instanceIndex;

void main()
{
  vec2 pos = gl_VertexIndex == 0
    ? vec2(0, 0)
    : (gl_VertexIndex == 1 ? vec2(0, 1)
                           : (gl_VertexIndex == 2 ? vec2(1, 0) : vec2(1, 1))); // assume quad
  gl_Position = vec4(pos, 0, 1);
  instanceIndex = gl_InstanceIndex; // assume baseInstance = 0
}
