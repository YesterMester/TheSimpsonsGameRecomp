#version 460
// Native replacement for the Xenos pixel shader 346B23B40037EB24, the most
// expensive full-screen pass of gameplay frames. It reads texture fetches 0-4
// at the pixel, the maximum of four tf1 taps around it, and two runs of tf4
// taps (loop constant 16), under the microcode's nested predicated conditions.
//
// A line by line transliteration of the microcode with the translator's exact
// operation semantics (Xenos multiplication, Shader Model 3 max and
// comparisons, NaN-safe saturation, previous scalar, the setp push/inv/pop
// predicate stack and predicated loop breaks), so the result matches the
// translation (modification 0000400000000001) bit for bit. The per-draw
// texture parameters are decoded once instead of on every fetch, the plain
// unsigned texture case skips the per-component sign handling, and the control
// flow is structured instead of going through the translator's dispatch loop.

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
  vec4 xe_float_constants[16];
};
#define c20 xe_float_constants[0]
#define c21 xe_float_constants[1]
#define c22 xe_float_constants[2]
#define c23 xe_float_constants[3]
#define c24 xe_float_constants[4]
#define c25 xe_float_constants[5]
#define c26 xe_float_constants[6]
#define c27 xe_float_constants[7]
#define c48 xe_float_constants[8]
#define c49 xe_float_constants[9]
#define c50 xe_float_constants[10]
#define c251 xe_float_constants[11]
#define c252 xe_float_constants[12]
#define c253 xe_float_constants[13]
#define c254 xe_float_constants[14]
#define c255 xe_float_constants[15]
layout(set = 1, binding = 3, std140) uniform XeBoolLoopConstants {
  uvec4 xe_bool_constants[2];
  uvec4 xe_loop_constants[8];
};
layout(set = 1, binding = 4, std140) uniform XeFetchConstants {
  uvec4 xe_fetch_constants[48];
};
// The translator declares 2D textures as arrays and samples layer 0. Bindings
// follow the order in which the microcode first fetches each constant (tf3,
// tf1, tf4, tf2, tf0), images before samplers.
layout(set = 3, binding = 0) uniform texture2DArray xe_texture3_2d_u;
layout(set = 3, binding = 1) uniform texture2DArray xe_texture3_2d_s;
layout(set = 3, binding = 2) uniform texture2DArray xe_texture1_2d_u;
layout(set = 3, binding = 3) uniform texture2DArray xe_texture1_2d_s;
layout(set = 3, binding = 4) uniform texture2DArray xe_texture4_2d_u;
layout(set = 3, binding = 5) uniform texture2DArray xe_texture4_2d_s;
layout(set = 3, binding = 6) uniform texture2DArray xe_texture2_2d_u;
layout(set = 3, binding = 7) uniform texture2DArray xe_texture2_2d_s;
layout(set = 3, binding = 8) uniform texture2DArray xe_texture0_2d_u;
layout(set = 3, binding = 9) uniform texture2DArray xe_texture0_2d_s;
layout(set = 3, binding = 10) uniform sampler xe_sampler3_fff;
layout(set = 3, binding = 11) uniform sampler xe_sampler1_fff;
layout(set = 3, binding = 12) uniform sampler xe_sampler4_fff;
layout(set = 3, binding = 13) uniform sampler xe_sampler2_fff;
layout(set = 3, binding = 14) uniform sampler xe_sampler0_fff;

layout(location = 0) in vec4 xe_in_interpolator_0;
layout(location = 0) invariant out vec4 xe_out_fragment_data_0;

const uint kSysFlagConvertColor0ToGamma = 1u << 19;

// Xenos multiplication: a zero factor gives zero even against infinity or NaN.
precise float MulZ(float a, float b) {
  precise float product = a * b;
  return xe_nmin(abs(a), abs(b)) == 0.0 ? 0.0 : product;
}
// Shader Model 3 comparisons and max.
float Sge(float a, float b) { return a >= b ? 1.0 : 0.0; }
float Sgt(float a, float b) { return a > b ? 1.0 : 0.0; }
float Max(float a, float b) { return a >= b ? a : b; }
float Sat(float a) { return xe_nclamp(a, 0.0, 1.0); }

// The predicate stack of nested predicated ifs, counted in a register:
// setp_*_push opens an if (the condition is src0 == 0 && src1 op 0), setp_inv
// switches to its else, setp_pop closes it.
precise float SetpPush(float counter, bool condition) {
  precise float result = (condition ? -1.0 : counter) + 1.0;
  return result;
}
float SetpInv(float counter, out bool predicate) {
  predicate = counter == 1.0;
  return predicate ? 0.0 : (counter == 0.0 ? 1.0 : counter);
}
precise float SetpPop(float counter, out bool predicate) {
  precise float counter_minus_1 = counter - 1.0;
  predicate = counter_minus_1 <= 0.0;
  return predicate ? 0.0 : counter_minus_1;
}

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

// The translator nudges fetch coordinates by 1.5/1024 of a texel; with draw
// resolution scaling, by 1/scale of that for resolution-scaled textures.
float CoordNudgeTexels(uint fetch_constant) {
#if XE_RESOLUTION_SCALE > 1
  if ((xe_textures_resolution_scaled & (1u << fetch_constant)) != 0u) {
    return 0.00146484375 * (1.0 / float(XE_RESOLUTION_SCALE));
  }
#endif
  return 0.00146484375;
}

// Per-draw state of a texture fetch constant, decoded once.
struct FetchState {
  vec2 coord_nudge;
  float gradient_scale;
  float exp_scale;
  uint sign_modes;
  bool sample_unsigned;
  bool sample_signed;
  bool plain;
};

FetchState DecodeFetch(uint fetch_constant, uint word_2, uint word_4,
                       uint sign_modes) {
  FetchState f;
  precise float width = float(bitfieldExtract(word_2, 0, 13) + 1u);
  precise float height = float(bitfieldExtract(word_2, 13, 13) + 1u);
  precise float nudge = CoordNudgeTexels(fetch_constant);
  f.coord_nudge = vec2(nudge / width, nudge / height);
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

// tfetch2D at coord (XYZW of the result), with gradients taken at the
// coordinate like the translation does.
#define DEFINE_FETCH(NAME, TEXTURE_U, TEXTURE_S, SAMPLER)                                         \
  vec4 NAME(vec2 coord, FetchState f) {                                                           \
    precise vec2 nudged = coord + f.coord_nudge;                                                  \
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
DEFINE_FETCH(Fetch4, xe_texture4_2d_u, xe_texture4_2d_s, xe_sampler4_fff)

// The taps fetched at once in the level 0 variant's loops (they run 10 times
// in this game).
#ifndef XE_TAP_BATCH
#define XE_TAP_BATCH 5u
#endif

// The rest of the first loop's body after its fetch into r4.
#define LOOP_1_BODY                                                  \
  /* 85: add r1.yzw, r4.yzw, r1.wzy + addsc r1.x, c254.w, r1.x */   \
  {                                                                  \
    precise vec3 v = r4.yzw + r1.wzy;                                \
    ps = c254.w + r1.x;                                              \
    r1.yzw = v;                                                      \
    r1.x = ps;                                                       \
  }                                                                  \
  /* 86: max r7.yzw, r1.wzy, r1.wzy + maxs r4.xx */                 \
  r7.yzw = r1.wzy;                                                   \
  ps = r4.x;                                                         \
  /* 87: max r1.yzw, r1.wzy, r1.wzy + adds_prev r7.x, r7.x */       \
  r1.yzw = r1.wzy;                                                   \
  ps = r7.x + ps;                                                    \
  r7.x = ps;

// The rest of the second loop's body after its fetch into r7.
#define LOOP_2_BODY                                                  \
  /* 96: max r6.zw, r4.zw, r4.zw + maxs r6.y, r8.yy */              \
  r6.zw = r4.zw;                                                     \
  ps = r8.y;                                                         \
  r6.y = ps;                                                         \
  /* 97: add r6, r7.xwyz, r6.xywz */                                 \
  r6 = r7.xwyz + r6.xywz;                                            \
  /* 98: max r8.xy, r6.yy, r6.yy */                                  \
  r8.xy = r6.yy;                                                     \
  /* 99: max r4, r6.wzwz, r6.wzwz + addsc r0.z, c254.w, r0.z */     \
  r4 = r6.wzwz;                                                      \
  ps = c254.w + r0.z;                                                \
  r0.z = ps;

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
  FetchState tf4 = DecodeFetch(4u, xe_fetch_constants[6].z, xe_fetch_constants[7].x,
                               bitfieldExtract(xe_texture_swizzled_signs[0].y, 0, 8));

  precise vec4 r0 = xe_in_interpolator_0;
  precise vec4 r1 = vec4(0.0);
  precise vec4 r2 = vec4(0.0);
  precise vec4 r3 = vec4(0.0);
  precise vec4 r4 = vec4(0.0);
  precise vec4 r5 = vec4(0.0);
  precise vec4 r6 = vec4(0.0);
  precise vec4 r7 = vec4(0.0);
  precise vec4 r8 = vec4(0.0);
  precise float ps = 0.0;
  bool p0 = false;
  vec4 f;

  // 12: tfetch2D r2, r0.xy, tf3
  r2 = Fetch3(r0.xy, tf3);
  // 13: add r0.w, c25.x, -c254.w
  r0.w = c25.x + -c254.w;
  // 14: sge r3.xyz, r2.zww, c255.xyz + sgts r5.x, -|r0.x|
  r3.x = Sge(r2.z, c255.x);
  r3.y = Sge(r2.w, c255.y);
  r3.z = Sge(r2.w, c255.z);
  ps = Sgt(-abs(r0.x), 0.0);
  r5.x = ps;
  // 15: sge r1.z, r2.w, c253.w + maxs c27.xx
  r1.z = Sge(r2.w, c253.w);
  ps = c27.x;
  // 16: mul r1.x, r3.z, r1.z + adds_prev r0.z, -c254.w
  r1.x = MulZ(r3.z, r1.z);
  ps = -c254.w + ps;
  r0.z = ps;
  // 17: mad r1.w, -r3.x, c252.w, r2.z
  r1.w = MulZ(-r3.x, c252.w) + r2.z;
  // 18: mad r0.z, r0.z, r3.y, c254.w
  r0.z = MulZ(r0.z, r3.y) + c254.w;
  // 19: sge r1.y, r1.w, c251.y + subsc r0.z, -c27.x, -r0.z
  r1.y = Sge(r1.w, c251.y);
  ps = -c27.x - -r0.z;
  r0.z = ps;
  // 20: mad r2.z, -r1.y, c253.y, r1.w
  r2.z = MulZ(-r1.y, c253.y) + r1.w;
  // 21: mad r0.z, r0.z, r3.z, c27.x
  r0.z = MulZ(r0.z, r3.z) + c27.x;
  // 22: add r1.w, r0.z, -c254.w + setp_gt |r1.x|
  r1.w = r0.z + -c254.w;
  p0 = abs(r1.x) > 0.0;
  ps = p0 ? 0.0 : 1.0;
  if (p0) {
    // 23: mad r2.w, r0.w, r1.y, c254.w
    r2.w = MulZ(r0.w, r1.y) + c254.w;
    // 24: mulsc r0.w, c254.x, r2.y
    ps = MulZ(c254.x, r2.y);
    r0.w = ps;
    // 25: frcs r0.z, r0.w
    ps = fract(r0.w);
    r0.z = ps;
    // 26: sge r1.y, r0.z, c252.y + subsc r2.x, c254.w, r0.z
    r1.y = Sge(r0.z, c252.y);
    ps = c254.w - r0.z;
    r2.x = ps;
    // 27: mad r2.x, r2.x, r1.y, r0.z
    r2.x = MulZ(r2.x, r1.y) + r0.z;
    // 28: add r0.z, -r2.x, r0.w
    r0.z = -r2.x + r0.w;
    // 29: mad r0.z, r0.z, c254.z, c251.z
    r0.z = MulZ(r0.z, c254.z) + c251.z;
    // 30: sge r0.w, r0.z, c252.y + subsc r1.y, c254.w, r0.z
    r0.w = Sge(r0.z, c252.y);
    ps = c254.w - r0.z;
    r1.y = ps;
    // 31: mad r2.y, r1.y, r0.w, r0.z
    r2.y = MulZ(r1.y, r0.w) + r0.z;
  } else {
    // 32: sge r0.z, r2.z, c253.z
    r0.z = Sge(r2.z, c253.z);
    // 33: mad r2.w, r0.w, r0.z, c254.w
    r2.w = MulZ(r0.w, r0.z) + c254.w;
    // 34: mad r0.w, -r0.z, c255.w, r2.z
    r0.w = MulZ(-r0.z, c255.w) + r2.z;
    // 35: sge r0.z, r0.w, c251.x
    r0.z = Sge(r0.w, c251.x);
    // 36: mulsc r5.x, c26.x, r0.z
    ps = MulZ(c26.x, r0.z);
    r5.x = ps;
    // 37: mad r2.z, -r0.z, c252.z, r0.w
    r2.z = MulZ(-r0.z, c252.z) + r0.w;
  }
  // 38: tfetch2D r1._x__, r0.xy, tf1
  f = Fetch1(r0.xy, tf1);
  r1.y = f.x;
  // 39: mul r8.yz, r2.yz, c254.xy
  r8.y = MulZ(r2.y, c254.x);
  r8.z = MulZ(r2.z, c254.y);
  // 40: mul r7.yz, r1.xx, r2.xy + frcs r0.z, r8.y
  r7.y = MulZ(r1.x, r2.x);
  r7.z = MulZ(r1.x, r2.y);
  ps = fract(r8.y);
  r0.z = ps;
  // 41: mul r8.x, r0.z, c254.x + mulsc_sat r8.w, c21.x, r1.y
  r8.x = MulZ(r0.z, c254.x);
  ps = MulZ(c21.x, r1.y);
  r8.w = Sat(ps);
  // 42: floor r0.zw, r8.xy
  r0.z = floor(r8.x);
  r0.w = floor(r8.y);
  // 43: mad r0.zw, r0.zw, c254.z, c251.z
  r0.z = MulZ(r0.z, c254.z) + c251.z;
  r0.w = MulZ(r0.w, c254.z) + c251.z;
  // 44: mul r0.zw, r0.wz, c252.w
  {
    precise float z = MulZ(r0.w, c252.w);
    precise float w = MulZ(r0.z, c252.w);
    r0.z = z;
    r0.w = w;
  }
  // 45: tfetch2D r6, r0.xy, tf4
  r6 = Fetch4(r0.xy, tf4);
  // 46: tfetch2D r3.x_yz, r0.wz, tf2
  f = Fetch2(r0.wz, tf2);
  r3.x = f.x;
  r3.z = f.y;
  r3.w = f.z;
  // 47: tfetch2D r4.wxyz, r0.xy, tf0
  f = Fetch0(r0.xy, tf0);
  r4 = vec4(f.w, f.x, f.y, f.z);
  // 48: sgt r0.z, r4.y, c253.w
  r0.z = Sgt(r4.y, c253.w);
  // 49: setp_ne_push r2.y, c252.x, r0.z
  p0 = c252.x == 0.0 && !(r0.z == 0.0);
  r2.y = SetpPush(c252.x, p0);
  // 50: mul r7.xw, r8.zw, r1.xz + subsc r0.z, c254.w, r1.x
  r7.x = MulZ(r8.z, r1.x);
  r7.w = MulZ(r8.w, r1.z);
  ps = c254.w - r1.x;
  r0.z = ps;
  // 51: mad r3.y, r7.w, r1.w, c254.w
  r3.y = MulZ(r7.w, r1.w) + c254.w;
  // 52: mad r3.xzw, r0.z, r3.xzw, r7.yzx
  r3.x = MulZ(r0.z, r3.x) + r7.y;
  r3.z = MulZ(r0.z, r3.z) + r7.z;
  r3.w = MulZ(r0.z, r3.w) + r7.x;
  if (p0) {
    // 53: max r2.x, r0.x, r0.x + rcp r0.z, c24.x
    r2.x = r0.x;
    ps = 1.0 / c24.x;
    r0.z = ps;
    // 54: max r1.w, r0.y, r0.y + rcp r0.w, c23.x
    r1.w = r0.y;
    ps = 1.0 / c23.x;
    r0.w = ps;
    // 55: mul r5.yz, r0.zw, c49.x
    r5.y = MulZ(r0.z, c49.x);
    r5.z = MulZ(r0.w, c49.x);
    // 56: add r2.z, -r5.y, r0.y
    r2.z = -r5.y + r0.y;
    // 57: add r1.z, -r5.z, r0.x + maxs r5.yy
    r1.z = -r5.z + r0.x;
    ps = r5.y;
    // 58: add r0.w, r5.z, r0.x + adds_prev r0.z, r0.y
    r0.w = r5.z + r0.x;
    ps = r0.y + ps;
    r0.z = ps;
    // 59-62: tfetch2D r1.x, r2.xz / r0.z, r0.xz / r1.z, r1.zw / r0.w, r0.wy, tf1
    r1.x = Fetch1(r2.xz, tf1).x;
    r0.z = Fetch1(r0.xz, tf1).x;
    r1.z = Fetch1(r1.zw, tf1).x;
    r0.w = Fetch1(r0.wy, tf1).x;
    // 63: max r0.w, r1.y, r0.w
    r0.w = Max(r1.y, r0.w);
    // 64: max r0.w, r0.w, r1.z
    r0.w = Max(r0.w, r1.z);
    // 65: max r0.z, r0.w, r0.z
    r0.z = Max(r0.w, r0.z);
    // 66: max r0.z, r0.z, r1.x
    r0.z = Max(r0.z, r1.x);
    // 67: mulsc_sat r2.z, c20.x, r0.z
    ps = MulZ(c20.x, r0.z);
    r2.z = Sat(ps);
    // 68: sgt r0.z, r2.z, c22.x
    r0.z = Sgt(r2.z, c22.x);
  }
  // 69: setp_eq_push r2.y, r2.y, r0.z
  p0 = r2.y == 0.0 && r0.z == 0.0;
  r2.y = SetpPush(r2.y, p0);
  if (p0) {
    // 70: mul r0, r4, c50.x
    r0 = vec4(MulZ(r4.x, c50.x), MulZ(r4.y, c50.x), MulZ(r4.z, c50.x), MulZ(r4.w, c50.x));
    // 71: mul r1, r0, c253.x
    r1 = vec4(MulZ(r0.x, c253.x), MulZ(r0.y, c253.x), MulZ(r0.z, c253.x),
              MulZ(r0.w, c253.x));
    // 72: mad r0.x, r2.z, r1.x, c254.w
    r0.x = MulZ(r2.z, r1.x) + c254.w;
    // 73: add r0.yzw, r1.yzw, c254.w
    r0.y = r1.y + c254.w;
    r0.z = r1.z + c254.w;
    r0.w = r1.w + c254.w;
    // 74: mad r0.yzw, r0.yzw, r3.xzw, -r3.xzw
    r0.y = MulZ(r0.y, r3.x) + -r3.x;
    r0.z = MulZ(r0.z, r3.z) + -r3.z;
    r0.w = MulZ(r0.w, r3.w) + -r3.w;
    // 75: mad r3.xzw, r2.z, r0.yzw, r3.xzw
    r3.x = MulZ(r2.z, r0.y) + r3.x;
    r3.z = MulZ(r2.z, r0.z) + r3.z;
    r3.w = MulZ(r2.z, r0.w) + r3.w;
  }
  // 76: setp_inv r2.y, r2.y
  ps = SetpInv(r2.y, p0);
  r2.y = ps;
  if (p0) {
    // 77: sgt r0.z, c254.w, r6.x
    r0.z = Sgt(c254.w, r6.x);
  }
  // 78: setp_ne_push r2.y, r2.y, r0.z
  p0 = r2.y == 0.0 && !(r0.z == 0.0);
  r2.y = SetpPush(r2.y, p0);
  if (p0) {
    // 79: sgt r7, -|r0.x|, c252.x + maxs c48.xx
    r7 = vec4(Sgt(-abs(r0.x), c252.x));
    ps = c48.x;
    // 80: sgt r1, -|r0.x|, c252.x + adds_prev r2.x, -c254.w
    r1 = vec4(Sgt(-abs(r0.x), c252.x));
    ps = -c254.w + ps;
    r2.x = ps;
    // 81: mul r0.z, r5.z, r2.x
    r0.z = MulZ(r5.z, r2.x);
    // 82: mad r0.z, -r0.z, c252.w, r0.x
    r0.z = MulZ(-r0.z, c252.w) + r0.x;
    // loop i16 with (!p0) break: p0 holds, so it runs its full count.
    uint loop_count = bitfieldExtract(xe_loop_constants[4].x, 0, 8);
    uint i = 0u;
#if XE_TEXTURES_LEVEL0
    // Taps in batches: the coordinates don't depend on what is fetched, so a
    // batch is fetched before the loop body uses any of it, and the latencies
    // of its fetches overlap. The body then runs in the original order. (Only
    // without gradients, which the other variants take at each fetch.)
    for (; i + XE_TAP_BATCH <= loop_count; i += XE_TAP_BATCH) {
      vec4 taps[XE_TAP_BATCH];
      precise float tap_r1_x = r1.x;
      for (uint j = 0u; j < XE_TAP_BATCH; ++j) {
        precise float tap_x = MulZ(r5.z, tap_r1_x) + r0.z;
        taps[j] = Fetch4(vec2(tap_x, r0.y), tf4);
        tap_r1_x = c254.w + tap_r1_x;
      }
      for (uint j = 0u; j < XE_TAP_BATCH; ++j) {
        // 83
        r0.w = MulZ(r5.z, r1.x) + r0.z;
        // 84
        r4 = taps[j];
        LOOP_1_BODY
      }
    }
#endif
    for (; i < loop_count; ++i) {
      // 83: mad r0.w, r5.z, r1.x, r0.z
      r0.w = MulZ(r5.z, r1.x) + r0.z;
      // 84: tfetch2D r4, r0.wy, tf4
      r4 = Fetch4(r0.wy, tf4);
      LOOP_1_BODY
    }
    // 88: sgt r1, -|r0.x|, c252.x
    r1 = vec4(Sgt(-abs(r0.x), c252.x));
    // 89: mul r0.w, r5.y, r2.x + maxs r6.x, r1.ww
    r0.w = MulZ(r5.y, r2.x);
    ps = r1.w;
    r6.x = ps;
    // 90: sgt r4, -|r0.x|, c252.x + rcp r2.x, c48.x
    r4 = vec4(Sgt(-abs(r0.x), c252.x));
    ps = 1.0 / c48.x;
    r2.x = ps;
    // 91: max r8.y, r1.x, r1.x + maxs r8.x, r1.yy
    r8.y = r1.x;
    ps = r1.y;
    r8.x = ps;
    // 92: mul r1, r2.x, r7.xwzy + maxs r0.z, r1.zz
    {
      precise vec4 v = vec4(MulZ(r2.x, r7.x), MulZ(r2.x, r7.w), MulZ(r2.x, r7.z),
                            MulZ(r2.x, r7.y));
      ps = r1.z;
      r1 = v;
      r0.z = ps;
    }
    // 93: mad r0.y, -r0.w, c252.w, r0.y
    r0.y = MulZ(-r0.w, c252.w) + r0.y;
    // loop i16 with (!p0) break, as above.
    loop_count = bitfieldExtract(xe_loop_constants[4].x, 0, 8);
    i = 0u;
#if XE_TEXTURES_LEVEL0
    // In batches like the first loop.
    for (; i + XE_TAP_BATCH <= loop_count; i += XE_TAP_BATCH) {
      vec4 taps[XE_TAP_BATCH];
      precise float tap_r0_z = r0.z;
      for (uint j = 0u; j < XE_TAP_BATCH; ++j) {
        precise float tap_y = MulZ(r5.y, tap_r0_z) + r0.y;
        taps[j] = Fetch4(vec2(r0.x, tap_y), tf4);
        tap_r0_z = c254.w + tap_r0_z;
      }
      for (uint j = 0u; j < XE_TAP_BATCH; ++j) {
        // 94
        r0.w = MulZ(r5.y, r0.z) + r0.y;
        // 95
        r7 = taps[j];
        LOOP_2_BODY
      }
    }
#endif
    for (; i < loop_count; ++i) {
      // 94: mad r0.w, r5.y, r0.z, r0.y
      r0.w = MulZ(r5.y, r0.z) + r0.y;
      // 95: tfetch2D r7, r0.xw, tf4
      r7 = Fetch4(r0.xw, tf4);
      LOOP_2_BODY
    }
    // 100: max r8.yz, r4.xy, r4.xy + maxs r8.w, r6.xx
    r8.yz = r4.xy;
    ps = r6.x;
    r8.w = ps;
    // 101: mad r0, r2.x, r8.wzyx, r1
    r0 = vec4(MulZ(r2.x, r8.w) + r1.x, MulZ(r2.x, r8.z) + r1.y, MulZ(r2.x, r8.y) + r1.z,
              MulZ(r2.x, r8.x) + r1.w);
    // 102: mul r0, r0.wzxy, c50.x
    r0 = vec4(MulZ(r0.w, c50.x), MulZ(r0.z, c50.x), MulZ(r0.x, c50.x), MulZ(r0.y, c50.x));
    // 103: mul r1, r0.ywxz, c253.x
    r1 = vec4(MulZ(r0.y, c253.x), MulZ(r0.w, c253.x), MulZ(r0.x, c253.x),
              MulZ(r0.z, c253.x));
    // 104: mad r0.x, r2.z, r1.z, c254.w
    r0.x = MulZ(r2.z, r1.z) + c254.w;
    // 105: add r0.yzw, r1.yxw, c254.w
    r0.y = r1.y + c254.w;
    r0.z = r1.x + c254.w;
    r0.w = r1.w + c254.w;
    // 106: mad r0.yzw, r0.yzw, r3.zwx, -r3.zwx
    r0.y = MulZ(r0.y, r3.z) + -r3.z;
    r0.z = MulZ(r0.z, r3.w) + -r3.w;
    r0.w = MulZ(r0.w, r3.x) + -r3.x;
    // 107: mad r3.xzw, r2.z, r0.wyz, r3.xzw
    r3.x = MulZ(r2.z, r0.w) + r3.x;
    r3.z = MulZ(r2.z, r0.y) + r3.z;
    r3.w = MulZ(r2.z, r0.z) + r3.w;
  }
  // 108: setp_inv r2.y, r2.y
  ps = SetpInv(r2.y, p0);
  r2.y = ps;
  if (p0) {
    // 109: mad r0.x, r2.z, -r6.w, c254.w
    r0.x = MulZ(r2.z, -r6.w) + c254.w;
    // 110: add r0.yzw, -r6.xyz, c254.w
    r0.y = -r6.x + c254.w;
    r0.z = -r6.y + c254.w;
    r0.w = -r6.z + c254.w;
    // 111: add r0.yzw, r0.yzw, -r3.xzw
    r0.y = r0.y + -r3.x;
    r0.z = r0.z + -r3.z;
    r0.w = r0.w + -r3.w;
    // 112: mad r3.xzw, r2.z, r0.yzw, r3.xzw
    r3.x = MulZ(r2.z, r0.y) + r3.x;
    r3.z = MulZ(r2.z, r0.z) + r3.z;
    r3.w = MulZ(r2.z, r0.w) + r3.w;
  }
  // 113, 114: setp_pop r2.y, r2.y
  ps = SetpPop(r2.y, p0);
  r2.y = ps;
  ps = SetpPop(r2.y, p0);
  r2.y = ps;
  // 115: setp_inv r2.y, r2.y
  ps = SetpInv(r2.y, p0);
  r2.y = ps;
  if (p0) {
    // 116: maxs r0.x, c254.ww
    ps = c254.w;
    r0.x = ps;
  }
  // 117: mul r0.yz, r3.xy, r2.w
  r0.y = MulZ(r3.x, r2.w);
  r0.z = MulZ(r3.y, r2.w);
  // 118: mul r0.x, r0.z, r0.x
  r0.x = MulZ(r0.z, r0.x);
  // 119: mul r0.yzw, r0.yzz, r3.yzw
  {
    precise float y = MulZ(r0.y, r3.y);
    precise float z = MulZ(r0.z, r3.z);
    precise float w = MulZ(r0.z, r3.w);
    r0.y = y;
    r0.z = z;
    r0.w = w;
  }
  // 120: add oC0, r0.yzwx, r5.x
  precise vec4 color = r0.yzwx + vec4(r5.x);

  precise vec4 biased = color * xe_color_exp_bias.x;
  vec3 rgb = biased.rgb;
  if ((xe_flags & kSysFlagConvertColor0ToGamma) != 0u) {
    rgb = vec3(LinearToPwlGamma(rgb.x), LinearToPwlGamma(rgb.y), LinearToPwlGamma(rgb.z));
  }
  xe_out_fragment_data_0 = vec4(rgb, biased.a);
}
