#ifndef DEPTH_UTILS_GLSL_INCLUDED
#define DEPTH_UTILS_GLSL_INCLUDED

// NOTE - near -> 1, far -> 0
float linearize_depth(float depth, float near_plane, float far_plane)
{
  return near_plane * far_plane / (near_plane - depth * (near_plane - far_plane));
}

#endif
