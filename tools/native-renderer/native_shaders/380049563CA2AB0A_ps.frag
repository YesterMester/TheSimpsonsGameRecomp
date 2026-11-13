#version 460
// Native replacement for the Xenos pixel shader 380049563CA2AB0A: a box blur of
// texture fetch 0 over the offsets c[20 + aL] (aL from loop constant 31), in
// texels of 1 / (c50.x, c49.x), scaled by c255.x.
//
// Same interface and results as its translation (modification
// 0000400000000001): identical bindings, the translator's texture fetch rules
// (coordinate nudge, LOD bias, per-component sign modes including PWL gamma,
// exponent adjust), Xenos multiplication (0 * anything = 0), the color exponent
// bias and the gamma render target conversion, with the same float controls.
// What changes is that the per-draw fetch parameters are decoded once rather
// than on every tap, outside the translator's control flow dispatch loop.

// build_native_shaders.py adds the translator's float controls
// (DenormFlushToZero, SignedZeroInfNanPreserve and RoundingModeRTE for
// 32-bit floats).
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
layout(set = 1, binding = 2, std140) uniform XeFloatConstants {
  vec4 xe_float_constants[256];
};
layout(set = 1, binding = 3, std140) uniform XeBoolLoopConstants {
  uvec4 xe_bool_constants[2];
  uvec4 xe_loop_constants[8];
};
layout(set = 1, binding = 4, std140) uniform XeFetchConstants {
  uvec4 xe_fetch_constants[48];
};
// The translator declares 2D textures as arrays and samples layer 0.
layout(set = 3, binding = 0) uniform texture2DArray xe_texture0_2d_u;
layout(set = 3, binding = 1) uniform texture2DArray xe_texture0_2d_s;
layout(set = 3, binding = 2) uniform sampler xe_sampler0_fff;

layout(location = 0) in vec4 xe_in_interpolator_0;
layout(location = 0) invariant out vec4 xe_out_fragment_data_0;

const uint kSysFlagConvertColor0ToGamma = 1u << 19;

// Xenos multiplication: a zero factor gives zero even against infinity or NaN.
precise float MulZ(float a, float b) {
  precise float product = a * b;
  return min(abs(a), abs(b)) == 0.0 ? 0.0 : product;
}

precise float PwlGammaToLinear(float gamma) {
  precise float x = clamp(gamma, 0.0, 1.0);
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
  precise float x = clamp(linear, 0.0, 1.0);
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

// Per-draw state of texture fetch 0, decoded once.
vec2 g_coord_nudge;
float g_gradient_scale;
float g_exp_scale;
uint g_sign_modes;
bool g_sample_unsigned;
bool g_sample_signed;

void DecodeFetch0() {
  uint size = xe_fetch_constants[0].z;
  precise float width = float(bitfieldExtract(size, 0, 13) + 1u);
  precise float height = float(bitfieldExtract(size, 13, 13) + 1u);
  precise float nudge = CoordNudgeTexels(0u);
  g_coord_nudge = vec2(nudge / width, nudge / height);
  int word4 = int(xe_fetch_constants[1].x);
  precise float lod_bias = float(bitfieldExtract(word4, 12, 10)) * 0.03125;
  g_gradient_scale = exp2(lod_bias);
  g_exp_scale = ldexp(1.0, bitfieldExtract(word4, 13, 6));
  g_sign_modes = xe_texture_swizzled_signs[0].x;
  bool signed_x = bitfieldExtract(g_sign_modes, 0, 2) == 1u;
  bool signed_y = bitfieldExtract(g_sign_modes, 2, 2) == 1u;
  bool signed_z = bitfieldExtract(g_sign_modes, 4, 2) == 1u;
  g_sample_unsigned = !(signed_x && signed_y && signed_z);
  g_sample_signed = signed_x || signed_y || signed_z;
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

// Every tap of a draw has the same derivatives (the offsets are constants), so
// the gradients are taken once at the draw's own coordinates.
vec2 g_gradient_x;
vec2 g_gradient_y;

// Samples texture fetch 0 at the nudged coordinate: level 0 in the level 0
// variant, otherwise with the draw's gradients.
vec4 Sample0(texture2DArray image, vec2 nudged) {
#if XE_TEXTURES_LEVEL0
  return textureLod(sampler2DArray(image, xe_sampler0_fff), vec3(nudged, 0.0), 0.0);
#else
  return textureGrad(sampler2DArray(image, xe_sampler0_fff), vec3(nudged, 0.0), g_gradient_x,
                     g_gradient_y);
#endif
}

vec3 Fetch0(vec2 coord) {
  precise vec2 nudged = coord + g_coord_nudge;
  vec4 unsigned_value = vec4(0.0);
  if (g_sample_unsigned) {
    unsigned_value = Sample0(xe_texture0_2d_u, nudged);
  }
  vec4 signed_value = vec4(0.0);
  if (g_sample_signed) {
    signed_value = Sample0(xe_texture0_2d_s, nudged);
  }
  precise vec3 result;
  result.x = ApplySignMode(bitfieldExtract(g_sign_modes, 0, 2), unsigned_value.x,
                           signed_value.x) * g_exp_scale;
  result.y = ApplySignMode(bitfieldExtract(g_sign_modes, 2, 2), unsigned_value.y,
                           signed_value.y) * g_exp_scale;
  result.z = ApplySignMode(bitfieldExtract(g_sign_modes, 4, 2), unsigned_value.z,
                           signed_value.z) * g_exp_scale;
  return result;
}

// The usual state of this pass: an unsigned texture, where a fetch is just the
// sample (times the exponent bias, which is 1 here outside the plain variant).
vec3 Fetch0Plain(vec2 coord) {
  precise vec2 nudged = coord + g_coord_nudge;
  vec3 value = Sample0(xe_texture0_2d_u, nudged).xyz;
#if XE_TEXTURES_PLAIN
  precise vec3 scaled = value * g_exp_scale;
  return scaled;
#else
  return value;
#endif
}

void main() {
  DecodeFetch0();
  vec2 coord = xe_in_interpolator_0.xy;
#if !XE_TEXTURES_LEVEL0
  precise vec2 gradient_base = coord + g_coord_nudge;
  g_gradient_x = dFdxCoarse(gradient_base) * g_gradient_scale;
  g_gradient_y = dFdyCoarse(gradient_base) * g_gradient_scale;
#endif
  // Uniform for the draw, so only one of the loops below runs per draw.
  bool plain = XE_TEXTURES_PLAIN != 0 || ((g_sign_modes & 0x3Fu) == 0u && g_exp_scale == 1.0);

  uint loop_constant = xe_loop_constants[7].w;
  uint count = bitfieldExtract(loop_constant, 0, 8);
  int loop_address = int(bitfieldExtract(loop_constant, 8, 8));
  int loop_step = bitfieldExtract(int(loop_constant), 16, 8);
  precise float texel_x = 1.0 / xe_float_constants[50].x;
  precise float texel_y = 1.0 / xe_float_constants[49].x;
  precise vec3 sum;
  if (plain) {
    sum = Fetch0Plain(coord);
    // Taps in groups of 4 (the count in this game), all fetched before any of
    // them is added, so their latencies overlap instead of adding up. The sum
    // keeps the same order.
    uint i = 0u;
    for (; i + 4u <= count; i += 4u) {
      vec3 taps[4];
      for (int j = 0; j < 4; ++j) {
        vec4 offset = xe_float_constants[20 + loop_address + j * loop_step];
        precise vec2 tap = vec2(MulZ(texel_x, offset.x) + coord.x,
                                MulZ(texel_y, offset.y) + coord.y);
        taps[j] = Fetch0Plain(tap);
      }
      for (int j = 0; j < 4; ++j) {
        sum += taps[j];
      }
      loop_address += 4 * loop_step;
    }
    for (; i < count; ++i) {
      vec4 offset = xe_float_constants[20 + loop_address];
      precise vec2 tap = vec2(MulZ(texel_x, offset.x) + coord.x,
                              MulZ(texel_y, offset.y) + coord.y);
      sum += Fetch0Plain(tap);
      loop_address += loop_step;
    }
  } else {
    sum = Fetch0(coord);
    for (uint i = 0u; i < count; ++i) {
      vec4 offset = xe_float_constants[20 + loop_address];
      precise vec2 tap = vec2(MulZ(texel_x, offset.x) + coord.x,
                              MulZ(texel_y, offset.y) + coord.y);
      sum += Fetch0(tap);
      loop_address += loop_step;
    }
  }

  float scale = xe_float_constants[255].x;
  precise vec3 color = vec3(MulZ(sum.x, scale), MulZ(sum.y, scale), MulZ(sum.z, scale));
  precise vec4 biased = vec4(color, 1.0) * xe_color_exp_bias.x;
  vec3 rgb = biased.rgb;
  if ((xe_flags & kSysFlagConvertColor0ToGamma) != 0u) {
    rgb = vec3(LinearToPwlGamma(rgb.x), LinearToPwlGamma(rgb.y), LinearToPwlGamma(rgb.z));
  }
  xe_out_fragment_data_0 = vec4(rgb, biased.a);
}
