#version 460
#extension GL_ARB_separate_shader_objects : enable
#extension GL_GOOGLE_include_directive : require

#include "unpack_attributes_baked.glsl"

layout(location = 0) in vec4 vPosNorm;
layout(location = 1) in vec4 vTexCoordAndTang;

layout(std140, binding = 0) readonly buffer instance_matrices_t
{
  mat4 instanceMatrices[];
};

layout(binding = 1) readonly buffer draw_instance_indices_t
{
  uint drawInstanceIndices[];
};

layout(binding = 2) readonly buffer light_info_t
{
  mat4 lightProjView;
};

// layout(push_constant) uniform push_constant_t
// {
//   mat4 translation;
// };

out gl_PerVertex
{
  vec4 gl_Position;
};

void main(void)
{
  mat4 currentModelMatrix = instanceMatrices[drawInstanceIndices[gl_InstanceIndex]];

  gl_Position = lightProjView * currentModelMatrix * vec4(vPosNorm.xyz, 1.0);
}
