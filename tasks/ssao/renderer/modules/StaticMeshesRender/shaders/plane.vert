#version 460

layout(set = 0, binding = 0) uniform render_params_t
{
  mat4 projView;
  mat4 previousProjView;
  vec2 currentJitter;
  vec2 previousJitter;
  vec3 cameraWorldPosition;
};

layout(location = 0) out VS_OUT
{
  vec4 currentPos;
  vec4 previousPos;
  vec3 worldPos;
};

out gl_PerVertex
{
  vec4 gl_Position;
};

void main()
{
  float size = 100;
  float height = 0;

  vec3 pos[6] = {
    vec3(-size, height, -size),
    vec3(size, height, size),
    vec3(-size, height, size),
    vec3(-size, height, -size),
    vec3(size, height, -size),
    vec3(size, height, size)};

  worldPos = pos[gl_VertexIndex];

  currentPos = projView * vec4(worldPos, 1.0);
  previousPos = previousProjView * vec4(worldPos, 1.0);

  gl_Position = currentPos;
}
