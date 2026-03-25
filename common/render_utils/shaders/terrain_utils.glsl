#ifndef TERRAIN_UTILS_GLSL_INCLUDED
#define TERRAIN_UTILS_GLSL_INCLUDED


vec4 to_terrain_coords(vec4 vector)
{
  return vector.xzyw;
}

vec3 to_terrain_coords(vec3 vector)
{
  return vector.xzy;
}

vec2 get_horizontal_coords(vec4 vector)
{
  return vector.xz;
}

vec2 get_horizontal_coords(vec3 vector)
{
  return vector.xz;
}

vec3 interpolate_in_plane(
  vec3 leftLower, vec3 leftUpper, vec3 rightLower, vec3 rightUpper, float u, float v)
{
  return leftLower * (1.0 - u) * (1.0 - v) + leftUpper * (1.0 - u) * v +
    rightLower * u * (1.0 - v) + rightUpper * u * v;
}

vec2 interpolate_in_plane(
  vec2 leftLower, vec2 leftUpper, vec2 rightLower, vec2 rightUpper, float u, float v)
{
  return leftLower * (1.0 - u) * (1.0 - v) + leftUpper * (1.0 - u) * v +
    rightLower * u * (1.0 - v) + rightUpper * u * v;
}


#endif // TERRAIN_UTILS_GLSL_INCLUDED
