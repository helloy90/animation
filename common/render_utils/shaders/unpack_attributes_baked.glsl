#ifndef UNPACK_ATTRIBUTES_BAKED_GLSL_INCLUDED
#define UNPACK_ATTRIBUTES_BAKED_GLSL_INCLUDED

// NOTE: .glsl extension is used for helper files with shader code

vec4 decode_normal(uint a_data)
{
  const uint a_enc_x = (a_data & 0x000000ff);
  const uint a_enc_y = ((a_data & 0x0000ff00) >> 8);
  const uint a_enc_z = ((a_data & 0x00ff0000) >> 16);
  const uint a_enc_w = ((a_data & 0xff000000) >> 24);

  ivec4 int_enc = ivec4(a_enc_x, a_enc_y, a_enc_z, a_enc_w);
  int_enc = ((int_enc + 128) % 256) - 128;
  vec4 true_enc = vec4(int_enc);

  return max(true_enc / 127.0, -1.0);
}


#endif // UNPACK_ATTRIBUTES_BAKED_GLSL_INCLUDED
