#version 460
// Native replacement for the Xenos pixel shader E170B95B0BC27F3C, the most
// expensive world shader of gameplay frames (Springfield: 61 draws, 2.4 ms per
// frame at 2x). Lighting from a texture, then two 3x3 filtered shadow map
// lookups (tf1 and tf0, texel offsets -1 to 1), under constant conditions.
//
// A line by line transliteration of the microcode with the translator's exact
// operation semantics (Xenos multiplication, Shader Model 3 max, min and
// comparisons, NaN-safe saturation, co-issued vector and scalar operations
// reading the registers before either writes, the previous scalar before
// saturation), so the result matches the translation (modification
// 000040000000003F) bit for bit. The per-draw texture parameters, including
// the normalized texel offsets, are decoded once instead of on every fetch,
// and the control flow is structured instead of going through the
// translator's dispatch loop.

// xe_modification 000040000000003F

#extension GL_EXT_spirv_intrinsics : require
spirv_instruction(set = "GLSL.std.450", id = 79) float xe_nmin(float a, float b);
spirv_instruction(set = "GLSL.std.450", id = 81) float xe_nclamp(float x, float lo, float hi);

// build_native_shaders.py adds the translator's float controls.
layout(early_fragment_tests) in;

// Built a second time with XE_TEXTURES_PLAIN=1 for the translator's plain
// texture variant (every component of every fetched texture unsigned), where
// fetches need no signedness handling or branching.
#ifndef XE_TEXTURES_PLAIN
#define XE_TEXTURES_PLAIN 0
#endif

// Built again with XE_TEXTURES_LEVEL0=1 for the translator's level 0 texture
// variant (every fetched texture has one mip level, no anisotropic filtering
// and the same magnification and minification filter), where fetches sample
// level 0 directly: gradients can't change the result.
#ifndef XE_TEXTURES_LEVEL0
#define XE_TEXTURES_LEVEL0 0
#endif

// The draw resolution scale the module is built for (build_native_shaders.py
// builds scale 2 into the set's scale2x2 directory).
#ifndef XE_RESOLUTION_SCALE
#define XE_RESOLUTION_SCALE 1
#endif

layout(set = 1, binding = 0, std140) uniform XeSystemConstants {
  layout(offset = 0) uint xe_flags;
#if XE_RESOLUTION_SCALE > 1
  // Bit per texture fetch constant: the texture is a resolution-scaled
  // resolve.
  layout(offset = 28) uint xe_textures_resolution_scaled;
#endif
  layout(offset = 176) uvec4 xe_texture_swizzled_signs[2];
  layout(offset = 288) vec4 xe_color_exp_bias;
};
// Without dynamic indexing, the translation packs the float constants the
// shader uses in register order.
layout(set = 1, binding = 2, std140) uniform XeFloatConstants {
  vec4 xe_float_constants[14];
};
#define c0 xe_float_constants[0]
#define c30 xe_float_constants[1]
#define c31 xe_float_constants[2]
#define c36 xe_float_constants[3]
#define c40 xe_float_constants[4]
#define c45 xe_float_constants[5]
#define c46 xe_float_constants[6]
#define c47 xe_float_constants[7]
#define c49 xe_float_constants[8]
#define c251 xe_float_constants[9]
#define c252 xe_float_constants[10]
#define c253 xe_float_constants[11]
#define c254 xe_float_constants[12]
#define c255 xe_float_constants[13]
layout(set = 1, binding = 4, std140) uniform XeFetchConstants {
  uvec4 xe_fetch_constants[48];
};
// The translator declares 2D textures as arrays and samples layer 0. Bindings
// follow the order in which the microcode first fetches each constant (tf2,
// tf3, tf1, tf0), images before samplers.
layout(set = 3, binding = 0) uniform texture2DArray xe_texture2_2d_u;
layout(set = 3, binding = 1) uniform texture2DArray xe_texture2_2d_s;
layout(set = 3, binding = 2) uniform texture2DArray xe_texture3_2d_u;
layout(set = 3, binding = 3) uniform texture2DArray xe_texture3_2d_s;
layout(set = 3, binding = 4) uniform texture2DArray xe_texture1_2d_u;
layout(set = 3, binding = 5) uniform texture2DArray xe_texture1_2d_s;
layout(set = 3, binding = 6) uniform texture2DArray xe_texture0_2d_u;
layout(set = 3, binding = 7) uniform texture2DArray xe_texture0_2d_s;
layout(set = 3, binding = 8) uniform sampler xe_sampler2_fff;
layout(set = 3, binding = 9) uniform sampler xe_sampler3_fff;
layout(set = 3, binding = 10) uniform sampler xe_sampler1_fff;
layout(set = 3, binding = 11) uniform sampler xe_sampler0_fff;

layout(location = 0) in vec4 xe_in_interpolator_0;
layout(location = 1) in vec4 xe_in_interpolator_1;
layout(location = 2) in vec4 xe_in_interpolator_2;
layout(location = 3) in vec4 xe_in_interpolator_3;
layout(location = 4) in vec4 xe_in_interpolator_4;
layout(location = 5) in vec4 xe_in_interpolator_5;
layout(location = 0) invariant out vec4 xe_out_fragment_data_0;

const uint kSysFlagConvertColor0ToGamma = 1u << 19;

// Xenos multiplication: a zero factor gives zero even against infinity or NaN.
precise float MulZ(float a, float b) {
  precise float product = a * b;
  return xe_nmin(abs(a), abs(b)) == 0.0 ? 0.0 : product;
}
// Shader Model 3 comparisons, max and min.
float Sge(float a, float b) { return a >= b ? 1.0 : 0.0; }
float Sgt(float a, float b) { return a > b ? 1.0 : 0.0; }
float Max(float a, float b) { return a >= b ? a : b; }
float Min(float a, float b) { return a < b ? a : b; }
float Sat(float a) { return xe_nclamp(a, 0.0, 1.0); }
// Unordered, as the translator's not-equal comparisons.
bool NotZero(float a) { return !(a == 0.0); }

precise float PwlGammaToLinear(float gamma) {
  precise float x = xe_nclamp(gamma, 0.0, 1.0);
  bool ge_high = x >= 0.752941191;
  float scale_high = ge_high ? 0.0078125 : 0.00390625;
  float offset_high = ge_high ? -1024.0 : -256.0;
  bool ge_low = x >= 0.250980407;
  float scale_low = ge_low ? 0.001953125 : 0.0009765625;
  float offset_low = ge_low ? -64.0 : 0.0;
  bool ge_mid = x >= 0.376470596;
  float scale = ge_mid ? scale_high : scale_low;
  float offset = ge_mid ? offset_high : offset_low;
  precise float t = x * 261120.0 * scale + offset;
  precise float r = t + trunc(t * scale);
  return r * 0.000977517106;
}

precise float LinearToPwlGamma(float linear) {
  precise float x = xe_nclamp(linear, 0.0, 1.0);
  bool ge_high = x >= 0.500488758;
  float scale_high = ge_high ? 127.875 : 255.75;
  float offset_high = ge_high ? 0.501960814 : 0.250980407;
  bool ge_low = x >= 0.0625610948;
  float scale_low = ge_low ? 511.5 : 1023.0;
  float offset_low = ge_low ? 0.125490203 : 0.0;
  bool ge_mid = x >= 0.12512219;
  float scale = ge_mid ? scale_high : scale_low;
  float offset = ge_mid ? offset_high : offset_low;
  precise float r = trunc(x * scale) * 0.00392156886 + offset;
  return r;
}

// Per-draw state of a texture fetch constant, decoded once.
struct FetchState {
  // The texel offsets -1, 0 and 1 in normalized coordinates, including the
  // translator's rounding nudge of 1.5/1024 of a texel, for x and y.
  vec3 offset_x;
  vec3 offset_y;
  float gradient_scale;
  float exp_scale;
  uint sign_modes;
  bool sample_unsigned;
  bool sample_signed;
  bool plain;
};

FetchState DecodeFetch(uint fetch_constant, uint word_2, uint word_4, uint sign_modes) {
  FetchState f;
  precise float width = float(bitfieldExtract(word_2, 0, 13) + 1u);
  precise float height = float(bitfieldExtract(word_2, 13, 13) + 1u);
  // The translator adds the nudge to the offset (a constant), and with draw
  // resolution scaling multiplies it by 1 / scale for resolution-scaled
  // textures, before dividing by the size.
  vec3 offsets = vec3(-0.99853515625, 0.00146484375, 1.00146484375);
#if XE_RESOLUTION_SCALE > 1
  if ((xe_textures_resolution_scaled & (1u << fetch_constant)) != 0u) {
    offsets *= 1.0 / float(XE_RESOLUTION_SCALE);
  }
#endif
  f.offset_x = vec3(offsets.x / width, offsets.y / width, offsets.z / width);
  f.offset_y = vec3(offsets.x / height, offsets.y / height, offsets.z / height);
  int word_4_signed = int(word_4);
  precise float lod_bias = float(bitfieldExtract(word_4_signed, 12, 10)) * 0.03125;
  f.gradient_scale = exp2(lod_bias);
  f.exp_scale = ldexp(1.0, bitfieldExtract(word_4_signed, 13, 6));
  f.sign_modes = sign_modes;
  bool all_signed = true;
  bool any_signed = false;
  for (int i = 0; i < 4; ++i) {
    bool component_signed = bitfieldExtract(sign_modes, i * 2, 2) == 1u;
    all_signed = all_signed && component_signed;
    any_signed = any_signed || component_signed;
  }
  f.sample_unsigned = !all_signed;
  f.sample_signed = any_signed;
  f.plain = sign_modes == 0u && f.exp_scale == 1.0;
  return f;
}

// Samples level 0 in the level 0 variant, otherwise with gradients taken at the
// coordinate like the translation does.
vec4 XeSample(texture2DArray image, sampler image_sampler, vec2 nudged, float gradient_scale) {
#if XE_TEXTURES_LEVEL0
  return textureLod(sampler2DArray(image, image_sampler), vec3(nudged, 0.0), 0.0);
#else
  vec2 gradient_x = dFdxCoarse(nudged) * gradient_scale;
  vec2 gradient_y = dFdyCoarse(nudged) * gradient_scale;
  return textureGrad(sampler2DArray(image, image_sampler), vec3(nudged, 0.0), gradient_x,
                     gradient_y);
#endif
}

precise float ApplySignMode(uint mode, float unsigned_value, float signed_value) {
  if (mode == 1u) {
    return signed_value;
  }
  if (mode == 2u) {
    return unsigned_value * 2.0 + -1.0;
  }
  if (mode == 3u) {
    return PwlGammaToLinear(unsigned_value);
  }
  return unsigned_value;
}

// tfetch2D at coord with the texel offset (offset_index 0 to 2 for -1 to 1 on
// each axis), with gradients taken at the offset coordinate like the
// translation does.
#define DEFINE_FETCH(NAME, TEXTURE_U, TEXTURE_S, SAMPLER)                                         \
  vec4 NAME(vec2 coord, int offset_x_index, int offset_y_index, FetchState f) {                   \
    precise vec2 nudged = coord + vec2(f.offset_x[offset_x_index], f.offset_y[offset_y_index]);   \
    if (XE_TEXTURES_PLAIN != 0) {                                                                 \
      precise vec4 scaled = XeSample(TEXTURE_U, SAMPLER, nudged, f.gradient_scale) * f.exp_scale; \
      return scaled;                                                                              \
    }                                                                                             \
    if (f.plain) {                                                                                \
      return XeSample(TEXTURE_U, SAMPLER, nudged, f.gradient_scale);                              \
    }                                                                                             \
    vec4 unsigned_value = vec4(0.0);                                                              \
    if (f.sample_unsigned) {                                                                      \
      unsigned_value = XeSample(TEXTURE_U, SAMPLER, nudged, f.gradient_scale);                    \
    }                                                                                             \
    vec4 signed_value = vec4(0.0);                                                                \
    if (f.sample_signed) {                                                                        \
      signed_value = XeSample(TEXTURE_S, SAMPLER, nudged, f.gradient_scale);                      \
    }                                                                                             \
    precise vec4 result;                                                                          \
    for (int i = 0; i < 4; ++i) {                                                                 \
      result[i] = ApplySignMode(bitfieldExtract(f.sign_modes, i * 2, 2), unsigned_value[i],       \
                                signed_value[i]) * f.exp_scale;                                   \
    }                                                                                             \
    return result;                                                                                \
  }
DEFINE_FETCH(Fetch0, xe_texture0_2d_u, xe_texture0_2d_s, xe_sampler0_fff)
DEFINE_FETCH(Fetch1, xe_texture1_2d_u, xe_texture1_2d_s, xe_sampler1_fff)
DEFINE_FETCH(Fetch2, xe_texture2_2d_u, xe_texture2_2d_s, xe_sampler2_fff)
DEFINE_FETCH(Fetch3, xe_texture3_2d_u, xe_texture3_2d_s, xe_sampler3_fff)

void main() {
  uint sign_modes_0_3 = xe_texture_swizzled_signs[0].x;
  FetchState tf0 = DecodeFetch(0u, xe_fetch_constants[0].z, xe_fetch_constants[1].x,
                               bitfieldExtract(sign_modes_0_3, 0, 8));
  FetchState tf1 = DecodeFetch(1u, xe_fetch_constants[2].x, xe_fetch_constants[2].z,
                               bitfieldExtract(sign_modes_0_3, 8, 8));
  FetchState tf2 = DecodeFetch(2u, xe_fetch_constants[3].z, xe_fetch_constants[4].x,
                               bitfieldExtract(sign_modes_0_3, 16, 8));
  FetchState tf3 = DecodeFetch(3u, xe_fetch_constants[5].x, xe_fetch_constants[5].z,
                               bitfieldExtract(sign_modes_0_3, 24, 8));

  precise vec4 r0 = xe_in_interpolator_0;
  precise vec4 r1 = xe_in_interpolator_1;
  precise vec4 r2 = xe_in_interpolator_2;
  precise vec4 r3 = xe_in_interpolator_3;
  precise vec4 r4 = xe_in_interpolator_4;
  precise vec4 r5 = xe_in_interpolator_5;
  precise vec4 r6 = vec4(0.0);
  precise vec4 r7 = vec4(0.0);
  precise float ps = 0.0;
  bool p0 = false;
  vec4 f;

  // 13: setp_gt r0._, c49.w
  p0 = c49.w > 0.0;
  ps = p0 ? 0.0 : 1.0;
  // 14: tfetch2D r5.yx_z, r0.xy, tf2
  f = Fetch2(r0.xy, 1, 1, tf2);
  r5.x = f.y;
  r5.y = f.x;
  r5.w = f.z;
  // 15: max r0._, c0, c0 writes nothing.
  // (!p0) jmp L4
  if (p0) {
    // 16: mul r4.xy__, r4.yxxx, c47.yxxx
    {
      precise float x = MulZ(r4.y, c47.y);
      precise float y = MulZ(r4.x, c47.x);
      r4.x = x;
      r4.y = y;
    }
    // 17: tfetch2D r4.xyz_, r4.yx, tf3
    f = Fetch3(r4.yx, 1, 1, tf3);
    r4.xyz = f.xyz;
    // 18: add r4.xyz_, r4.xyzz, -c45.xyzz
    r4.x = r4.x + -c45.x;
    r4.y = r4.y + -c45.y;
    r4.z = r4.z + -c45.z;
    // 19: mul_sat r6.xyz_, r_abs[4].xyzz, c46.xyzz
    r6.x = Sat(MulZ(abs(r4.x), c46.x));
    r6.y = Sat(MulZ(abs(r4.y), c46.y));
    r6.z = Sat(MulZ(abs(r4.z), c46.z));
    // 20: sgt r7.xyz_, r4.xyzz, -r4.xyzz
    r7.x = Sgt(r4.x, -r4.x);
    r7.y = Sgt(r4.y, -r4.y);
    r7.z = Sgt(r4.z, -r4.z);
    // 21: sgt r4.xyz_, -r4.xyzz, r4.xyzz
    r4.x = Sgt(-r4.x, r4.x);
    r4.y = Sgt(-r4.y, r4.y);
    r4.z = Sgt(-r4.z, r4.z);
    // 22: add r4.xyz_, r7.xyzz, -r4.xyzz
    r4.x = r7.x + -r4.x;
    r4.y = r7.y + -r4.y;
    r4.z = r7.z + -r4.z;
    // 23: mad r4.xyz_, r6.xyzz, r4.xyzz, c253.yyyy
    r4.x = MulZ(r6.x, r4.x) + c253.y;
    r4.y = MulZ(r6.y, r4.y) + c253.y;
    r4.z = MulZ(r6.z, r4.z) + c253.y;
    // 24: mul r4.xyz_, r4.xyzz, c49.wwww
    r4.x = MulZ(r4.x, c49.w);
    r4.y = MulZ(r4.y, c49.w);
    r4.z = MulZ(r4.z, c49.w);
    // 25: mul_sat r5.xy_w, r4.yxzz, r5.xyww
    r5.x = Sat(MulZ(r4.y, r5.x));
    r5.y = Sat(MulZ(r4.x, r5.y));
    r5.w = Sat(MulZ(r4.z, r5.w));
  }
  // L4
  // 26: sgt r4.x___, c47.zzzz, c255.yyyy + snes r0.x___, c49.z
  r4.x = Sgt(c47.z, c255.y);
  ps = NotZero(c49.z) ? 1.0 : 0.0;
  r0.x = ps;
  // 27: sge r3.___w, c47.wwww, c255.yyyy + maxs r0._, c49.zz
  r3.w = Sge(c47.w, c255.y);
  ps = c49.z;
  // 28: sge r7._y__, r5.zzzz, c251.yyyy + adds_prev r0._y__, -c40.x
  r7.y = Sge(r5.z, c251.y);
  ps = -c40.x + ps;
  r0.y = ps;
  // 29: mad r4._y__, r0.yyyy, r0.xxxx, c40.xxxx
  r4.y = MulZ(r0.y, r0.x) + c40.x;
  // 30: dp3 r4.__z_, r3.zxyy, r3.zxyy (identical operands: plain products)
  {
    precise float zz = r3.z * r3.z;
    precise float xx = r3.x * r3.x;
    precise float yy = r3.y * r3.y;
    precise float d = zz + xx;
    r4.z = d + yy;
  }
  // 31: max r5.xy__, r5.xyyy, c251.xxxx + frcs r0.x___, r0.w
  r5.x = Max(r5.x, c251.x);
  r5.y = Max(r5.y, c251.x);
  ps = fract(r0.w);
  r0.x = ps;
  // 32: min r0.___w, r5.xxxx, c252.yyyy + adds r0.x___, r0.xx
  r0.w = Min(r5.x, c252.y);
  ps = r0.x + r0.x;
  r0.x = ps;
  // 33: sge r0._y__, r0.xxxx, c253.yyyy + rsq r4.__z_, r_abs[4].z
  r0.y = Sge(r0.x, c253.y);
  ps = inversesqrt(abs(r4.z));
  r4.z = ps;
  // 34: mul r3.xyz_, r4.zzzz, r3.xyzz
  r3.x = MulZ(r4.z, r3.x);
  r3.y = MulZ(r4.z, r3.y);
  r3.z = MulZ(r4.z, r3.z);
  // 35: mul r4.__zw, r0.yyyw, c254.zzzw + maxs r0.___w, c253.yy
  {
    precise float z = MulZ(r0.y, c254.z);
    precise float w = MulZ(r0.w, c254.w);
    ps = c253.y;
    r4.z = z;
    r4.w = w;
    r0.w = ps;
  }
  // 36: min r5.__z_, r5.yyyy, c252.yyyy + floors r5._y__, r4.w
  r5.z = Min(r5.y, c252.y);
  ps = floor(r4.w);
  r5.y = ps;
  // 37: dp3_sat r0._y__, r3.zxyy, c36.zxyy + maxs r0._, r0.xx
  {
    precise float z = MulZ(r3.z, c36.z);
    precise float x = MulZ(r3.x, c36.x);
    precise float y = MulZ(r3.y, c36.y);
    precise float d = z + x;
    precise float dot = d + y;
    ps = r0.x;
    r0.y = Sat(dot);
  }
  // 38: sge r7.x___, c253.wwww, r0.yyyy + adds_prev r5.x___, r4.z
  r7.x = Sge(c253.w, r0.y);
  ps = r4.z + ps;
  r5.x = ps;
  // 39: mul r6._yzw, r5.xxyz, c255.zzzz + frcs r7.___w, r0.z
  r6.y = MulZ(r5.x, c255.z);
  r6.z = MulZ(r5.y, c255.z);
  r6.w = MulZ(r5.z, c255.z);
  ps = fract(r0.z);
  r7.w = ps;
  // 40: floor r7.__z_, r6.yyyy + maxs_sat r0.x___, r3.yy
  r7.z = floor(r6.y);
  ps = r3.y;
  r0.x = Sat(ps);
  // 41: mul r7, r7, c255 + floors r3._y__, r6.w
  {
    precise vec4 v = vec4(MulZ(r7.x, c255.x), MulZ(r7.y, c255.y), MulZ(r7.z, c255.z),
                          MulZ(r7.w, c255.w));
    ps = floor(r6.w);
    r7 = v;
    r3.y = ps;
  }
  // 42: add r6.x___, r4.yyyy, r7.zzzz + floors r3.x___, r7.w
  r6.x = r4.y + r7.z;
  ps = floor(r7.w);
  r3.x = ps;
  // 43: mad r0.__z_, r7.xxxx, r3.wwww, r7.yyyy
  r0.z = MulZ(r7.x, r3.w) + r7.y;
  // 44: mad r3.__z_, r5.wwww, c251.zzzz, r0.zzzz
  r3.z = MulZ(r5.w, c251.z) + r0.z;
  // 45: add r3.xy__, r6.xzzz, r3.xyyy + subsc r3.___w, c253.y, r0.x
  {
    precise float x = r6.x + r3.x;
    precise float y = r6.z + r3.y;
    ps = c253.y - r0.x;
    r3.x = x;
    r3.y = y;
    r3.w = ps;
  }
  // 46: mul r3.xy__, r3.xyyy, c254.xyyy + setp_ne r0._, r4.x
  r3.x = MulZ(r3.x, c254.x);
  r3.y = MulZ(r3.y, c254.y);
  p0 = NotZero(r4.x);
  ps = p0 ? 0.0 : 1.0;
  // (!p0) jmp L23
  if (p0) {
    // 47: max r0.__z_, c253.yyyy, c253.yyyy + rcp r0.___w, r1.w
    r0.z = c253.y;
    ps = 1.0 / r1.w;
    r0.w = ps;
    // 48: mul_sat r0.x___, r0.wwww, r1.zzzz
    r0.x = Sat(MulZ(r0.w, r1.z));
    // 49: mul r1.xy__, r0.wwww, r1.xyyy + setp_gt r0._, c31.x
    r1.x = MulZ(r0.w, r1.x);
    r1.y = MulZ(r0.w, r1.y);
    p0 = c31.x > 0.0;
    ps = p0 ? 0.0 : 1.0;
    // 50: mad r1.xy__, r1.xyyy, c253.xzzz, c255.yyyy
    r1.x = MulZ(r1.x, c253.x) + c255.y;
    r1.y = MulZ(r1.y, c253.z) + c255.y;
    // (!p0) jmp L15
    if (p0) {
      // 51-59: tfetch2D from tf1 at r1.xy with offsets -1 to 1.
      r0.z = Fetch1(r1.xy, 1, 1, tf1).y;
      r0.w = Fetch1(r1.xy, 2, 1, tf1).z;
      r5.x = Fetch1(r1.xy, 2, 0, tf1).z;
      r5.y = Fetch1(r1.xy, 0, 0, tf1).x;
      r5.z = Fetch1(r1.xy, 1, 0, tf1).y;
      r4.x = Fetch1(r1.xy, 0, 1, tf1).x;
      r4.y = Fetch1(r1.xy, 2, 2, tf1).z;
      r4.z = Fetch1(r1.xy, 0, 2, tf1).x;
      r4.w = Fetch1(r1.xy, 1, 2, tf1).y;
      // 60: mul r6.xy__, r1.xyyy, c252.wwww + subsc r0.x___, c253.y, r0.x
      r6.x = MulZ(r1.x, c252.w);
      r6.y = MulZ(r1.y, c252.w);
      ps = c253.y - r0.x;
      r0.x = ps;
      // 61: sge r4, r0.xxxx, r4
      r4 = vec4(Sge(r0.x, r4.x), Sge(r0.x, r4.y), Sge(r0.x, r4.z), Sge(r0.x, r4.w));
      // 62: sge r1._yzw, r0.xxxx, r5.xxyz + frcs r5.x___, r6.y
      {
        float y = Sge(r0.x, r5.x);
        float z = Sge(r0.x, r5.y);
        float w = Sge(r0.x, r5.z);
        ps = fract(r6.y);
        r1.y = y;
        r1.z = z;
        r1.w = w;
        r5.x = ps;
      }
      // 63: add r5._yzw, r4.yyzw, -r1.yyzw + frcs r1.x___, r6.x
      r5.y = r4.y + -r1.y;
      r5.z = r4.z + -r1.z;
      r5.w = r4.w + -r1.w;
      ps = fract(r6.x);
      r1.x = ps;
      // 64: mad r1._yzw, r5.yyzw, r5.xxxx, r1.yyzw
      r1.y = MulZ(r5.y, r5.x) + r1.y;
      r1.z = MulZ(r5.z, r5.x) + r1.z;
      r1.w = MulZ(r5.w, r5.x) + r1.w;
      // 65: sge r0.x_z_, r0.xxxx, r0.zwww + maxs r0._, r1.zz
      {
        float x = Sge(r0.x, r0.z);
        float z = Sge(r0.x, r0.w);
        ps = r1.z;
        r0.x = x;
        r0.z = z;
      }
      // 66: add r0.__zw, r1.yyyw, r0.zzzx + adds_prev r0.x___, r4.x
      {
        precise float z = r1.y + r0.z;
        precise float w = r1.w + r0.x;
        ps = r4.x + ps;
        r0.z = z;
        r0.w = w;
        r0.x = ps;
      }
      // 67: add r1._y__, r0.zzzz, -r0.xxxx
      r1.y = r0.z + -r0.x;
      // 68: mad r0.x___, r1.yyyy, r1.xxxx, r0.xxxx
      r0.x = MulZ(r1.y, r1.x) + r0.x;
      // 69: add r0.x___, r0.xxxx, r0.wwww
      r0.x = r0.x + r0.w;
      // 70: mulsc r0.__z_, c255.y, r0.x
      ps = MulZ(c255.y, r0.x);
      r0.z = ps;
    }
    // L15
    // 71: floor r1.__z_, -r0.yyyy + maxs r0._y__, c253.yy
    r1.z = floor(-r0.y);
    ps = c253.y;
    r0.y = ps;
    // 72: add r1._y__, -c30.yyyy, c253.yyyy + rcp r1.x___, r2.w
    r1.y = -c30.y + c253.y;
    ps = 1.0 / r2.w;
    r1.x = ps;
    // 73: mul_sat r0.x___, r1.xxxx, r2.zzzz + floors r0.___w, r3.w
    r0.x = Sat(MulZ(r1.x, r2.z));
    ps = floor(r3.w);
    r0.w = ps;
    // 74: mad_sat r0.__z_, r1.yyyy, r0.zzzz, c30.yyyy
    r0.z = Sat(MulZ(r1.y, r0.z) + c30.y);
    // 75: mul r1.xy__, r1.xxxx, r2.xyyy + maxs r0._, r0.zz
    {
      precise float x = MulZ(r1.x, r2.x);
      precise float y = MulZ(r1.x, r2.y);
      ps = r0.z;
      r1.x = x;
      r1.y = y;
    }
    // 76: mul r1.xy__, r1.xyyy, c253.xzzz + adds_prev_sat r0.__z_, r0.w
    r1.x = MulZ(r1.x, c253.x);
    r1.y = MulZ(r1.y, c253.z);
    ps = r0.w + ps;
    r0.z = Sat(ps);
    // 77: add r1.xyz_, r1.xyzz, c253.xxyy + setp_gt r0._, c31.x
    r1.x = r1.x + c253.x;
    r1.y = r1.y + c253.x;
    r1.z = r1.z + c253.y;
    p0 = c31.x > 0.0;
    ps = p0 ? 0.0 : 1.0;
    // (!p0) jmp L22
    if (p0) {
      // 78-86: tfetch2D from tf0 at r1.xy with offsets -1 to 1.
      r0.y = Fetch0(r1.xy, 1, 1, tf0).x;
      r0.w = Fetch0(r1.xy, 2, 1, tf0).z;
      r4.x = Fetch0(r1.xy, 2, 0, tf0).z;
      r4.y = Fetch0(r1.xy, 0, 0, tf0).x;
      r4.z = Fetch0(r1.xy, 1, 0, tf0).y;
      r2.x = Fetch0(r1.xy, 0, 1, tf0).x;
      r2.y = Fetch0(r1.xy, 2, 2, tf0).z;
      r2.z = Fetch0(r1.xy, 0, 2, tf0).x;
      r2.w = Fetch0(r1.xy, 1, 2, tf0).y;
      // 87: mul r6.xy__, r1.xyyy, c252.wwww + subsc r0.x___, c253.y, r0.x
      r6.x = MulZ(r1.x, c252.w);
      r6.y = MulZ(r1.y, c252.w);
      ps = c253.y - r0.x;
      r0.x = ps;
      // 88: sge r2, r0.xxxx, r2
      r2 = vec4(Sge(r0.x, r2.x), Sge(r0.x, r2.y), Sge(r0.x, r2.z), Sge(r0.x, r2.w));
      // 89: sge r4.xyz_, r0.xxxx, r4.xyzz + frcs r3.___w, r6.y
      r4.x = Sge(r0.x, r4.x);
      r4.y = Sge(r0.x, r4.y);
      r4.z = Sge(r0.x, r4.z);
      ps = fract(r6.y);
      r3.w = ps;
      // 90: add r5.xyz_, r2.yzww, -r4.xyzz + frcs r1.___w, r6.x
      r5.x = r2.y + -r4.x;
      r5.y = r2.z + -r4.y;
      r5.z = r2.w + -r4.z;
      ps = fract(r6.x);
      r1.w = ps;
      // 91: mad r4.xyz_, r5.xyzz, r3.wwww, r4.xyzz
      r4.x = MulZ(r5.x, r3.w) + r4.x;
      r4.y = MulZ(r5.y, r3.w) + r4.y;
      r4.z = MulZ(r5.z, r3.w) + r4.z;
      // 92: sge r0.xy__, r0.xxxx, r0.ywww + maxs r0._, r4.yy
      {
        float x = Sge(r0.x, r0.y);
        float y = Sge(r0.x, r0.w);
        ps = r4.y;
        r0.x = x;
        r0.y = y;
      }
      // 93: add r0.xy__, r4.xzzz, r0.yxxx + adds_prev r0.___w, r2.x
      {
        precise float x = r4.x + r0.y;
        precise float y = r4.z + r0.x;
        ps = r2.x + ps;
        r0.x = x;
        r0.y = y;
        r0.w = ps;
      }
      // 94: add r2.x___, r0.xxxx, -r0.wwww
      r2.x = r0.x + -r0.w;
      // 95: mad r0.___w, r2.xxxx, r1.wwww, r0.wwww
      r0.w = MulZ(r2.x, r1.w) + r0.w;
      // 96: add r0.x___, r0.wwww, r0.yyyy
      r0.x = r0.w + r0.y;
      // 97: mulsc r0._y__, c255.y, r0.x
      ps = MulZ(c255.y, r0.x);
      r0.y = ps;
    }
    // L22
    // 98: add_sat r0.x___, r1.zzzz, r0.yyyy
    r0.x = Sat(r1.z + r0.y);
    // 99: min r0.___w, r0.zzzz, r0.xxxx
    r0.w = Min(r0.z, r0.x);
  }
  // L23
  // 100: floors r0.x___, r0.w
  ps = floor(r0.w);
  r0.x = ps;
  // 101: subsc r0.x___, c253.y, r0.x
  ps = c253.y - r0.x;
  r0.x = ps;
  // 102: mad r3.___w, r0.xxxx, c252.zzzz, c252.xxxx
  r3.w = MulZ(r0.x, c252.z) + c252.x;
  // 103: max oC0, r3, r3
  precise vec4 color = r3;

  precise vec4 biased = color * xe_color_exp_bias.x;
  vec3 rgb = biased.rgb;
  if ((xe_flags & kSysFlagConvertColor0ToGamma) != 0u) {
    rgb = vec3(LinearToPwlGamma(rgb.x), LinearToPwlGamma(rgb.y), LinearToPwlGamma(rgb.z));
  }
  xe_out_fragment_data_0 = vec4(rgb, biased.a);
}
