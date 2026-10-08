#version 460
// Native replacement for the Xenos pixel shader 6116B6CC7219148B: a search over
// the offsets c[20 + aL] (aL from loop constant 31) around the pixel, sampling
// texture fetch 0, that stops at the first tap meeting the edge condition and
// outputs the resulting weight in RGB (alpha 1).
//
// A line by line transliteration of the microcode with the translator's exact
// operation semantics (Xenos multiplication and max, NaN-safe saturation,
// previous scalar, predicated execution), so the result matches the
// translation (modification 0000400000000001) bit for bit. The per-draw
// texture parameters are decoded once instead of on every tap, and the plain
// unsigned texture case skips the per-component sign handling.

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
  layout(offset = 172) uint xe_textures_resolution_scaled;
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

vec4 C(int index) { return xe_float_constants[index]; }

// Xenos multiplication: a zero factor gives zero even against infinity or NaN.
precise float MulZ(float a, float b) {
  precise float product = a * b;
  return xe_nmin(abs(a), abs(b)) == 0.0 ? 0.0 : product;
}
// Shader Model 3 comparisons and max.
float Sge(float a, float b) { return a >= b ? 1.0 : 0.0; }
float Sne(float a, float b) { return a != b ? 1.0 : 0.0; }
float Sgt0(float a) { return a > 0.0 ? 1.0 : 0.0; }
float Sat(float a) { return xe_nclamp(a, 0.0, 1.0); }

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

// Per-draw state of texture fetch 0, decoded once.
vec2 g_coord_nudge;
float g_gradient_scale;
float g_exp_scale;
uint g_sign_modes;
bool g_sample_unsigned;
bool g_sample_signed;
bool g_plain;

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
  bool all_signed = true;
  bool any_signed = false;
  for (int i = 0; i < 4; ++i) {
    bool component_signed = bitfieldExtract(g_sign_modes, i * 2, 2) == 1u;
    all_signed = all_signed && component_signed;
    any_signed = any_signed || component_signed;
  }
  g_sample_unsigned = !all_signed;
  g_sample_signed = any_signed;
  g_plain = (g_sign_modes & 0xFFu) == 0u && g_exp_scale == 1.0;
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

// Samples texture fetch 0 at the nudged coordinate: level 0 in the level 0
// variant, otherwise with gradients taken at the coordinate like the
// translation does.
vec4 Sample0(texture2DArray image, vec2 nudged) {
#if XE_TEXTURES_LEVEL0
  return textureLod(sampler2DArray(image, xe_sampler0_fff), vec3(nudged, 0.0), 0.0);
#else
  vec2 gradient_x = dFdxCoarse(nudged) * g_gradient_scale;
  vec2 gradient_y = dFdyCoarse(nudged) * g_gradient_scale;
  return textureGrad(sampler2DArray(image, xe_sampler0_fff), vec3(nudged, 0.0), gradient_x,
                     gradient_y);
#endif
}

// tfetch2D from texture fetch 0 at coord (XYZW).
vec4 Fetch0(vec2 coord) {
  precise vec2 nudged = coord + g_coord_nudge;
#if XE_TEXTURES_PLAIN
  precise vec4 scaled = Sample0(xe_texture0_2d_u, nudged) * g_exp_scale;
  return scaled;
#endif
  if (g_plain) {
    return Sample0(xe_texture0_2d_u, nudged);
  }
  vec4 unsigned_value = vec4(0.0);
  if (g_sample_unsigned) {
    unsigned_value = Sample0(xe_texture0_2d_u, nudged);
  }
  vec4 signed_value = vec4(0.0);
  if (g_sample_signed) {
    signed_value = Sample0(xe_texture0_2d_s, nudged);
  }
  precise vec4 result;
  for (int i = 0; i < 4; ++i) {
    result[i] = ApplySignMode(bitfieldExtract(g_sign_modes, i * 2, 2), unsigned_value[i],
                              signed_value[i]) * g_exp_scale;
  }
  return result;
}

void main() {
  DecodeFetch0();
  precise vec4 r0 = xe_in_interpolator_0;
  precise vec4 r1 = vec4(0.0);
  precise vec4 r2 = vec4(0.0);
  precise vec4 r3 = vec4(0.0);
  precise vec4 r4 = vec4(0.0);
  float ps = 0.0;
  bool p0 = false;

  // 5: tfetch2D r1.wxyz, r0.xy, tf0
  vec4 f = Fetch0(r0.xy);
  r1 = vec4(f.w, f.x, f.y, f.z);
  // 6: sge r0.w, r1.x, c255.y
  r0.w = Sge(r1.x, C(255).y);
  // 7: sge r1.x, r1.w, c255.x
  r1.x = Sge(r1.w, C(255).x);
  // 8: mad r0.z, -r1.x, c254.z, r1.w
  r0.z = MulZ(-r1.x, C(254).z) + r1.w;
  // 9: sge r1.w, r0.z, c254.y + subsc r0.z, c255.z, r0.w
  {
    float v = Sge(r0.z, C(254).y);
    ps = C(255).z - r0.w;
    r1.w = v;
    r0.z = ps;
  }
  // 10: mul r1.w, r0.z, r1.w
  r1.w = MulZ(r0.z, r1.w);
  // 11: sne r2.x, r1.w, c254.x + sgts r1.w, -|r0.x|
  {
    float v = Sne(r1.w, C(254).x);
    ps = Sgt0(-abs(r0.x));
    r2.x = v;
    r1.w = ps;
  }
  // 12: add r1.z, r2.x, r1.z + subsc r1.x, c255.z, r1.x
  {
    precise float v = r2.x + r1.z;
    ps = C(255).z - r1.x;
    r1.z = v;
    r1.x = ps;
  }

  // loop i31
  uint loop_constant = xe_loop_constants[7].w;
  uint count = bitfieldExtract(loop_constant, 0, 8);
  int loop_address = int(bitfieldExtract(loop_constant, 8, 8));
  int loop_step = bitfieldExtract(int(loop_constant), 16, 8);
  uint i = 0u;
#if XE_TEXTURES_LEVEL0
  // The taps of 4 iterations are fetched together, before the search checks
  // any of them (their coordinates depend only on the constants and the
  // pixel), so their latencies overlap. Only pixels still searching fetch;
  // taps past the one that ends a pixel's search go unused. (Only without
  // gradients, which the other variants take at each fetch.)
  for (; i + 4u <= count; i += 4u) {
    vec4 taps[4];
    if (r1.w == 0.0) {
      for (int j = 0; j < 4; ++j) {
        // 14-17 for the tap's offset.
        precise float rcp_x = 1.0 / C(49).x;
        precise float rcp_y = 1.0 / C(48).x;
        vec4 offset = C(20 + loop_address + j * loop_step);
        precise float offset_x = MulZ(rcp_x, offset.x);
        precise float offset_y = MulZ(rcp_y, offset.y);
        precise float tap_y = MulZ(offset_y, C(50).x) + r0.y;
        precise float tap_x = MulZ(offset_x, C(50).x) + r0.x;
        // 18 with the coordinates swapped back.
        taps[j] = Fetch0(vec2(tap_x, tap_y));
      }
    }
    for (int j = 0; j < 4; ++j) {
      // 13: setp_eq r1.w
      p0 = r1.w == 0.0;
      ps = p0 ? 0.0 : 1.0;
      if (p0) {
        // 14, 15: rcp r2.x, c49.x; rcp r2.y, c48.x
        ps = 1.0 / C(49).x;
        r2.x = ps;
        ps = 1.0 / C(48).x;
        r2.y = ps;
        // 16: mul r2.xy, r2.xy, c[20+aL].xy
        vec4 offset = C(20 + loop_address);
        {
          precise float x = MulZ(r2.x, offset.x);
          precise float y = MulZ(r2.y, offset.y);
          r2.x = x;
          r2.y = y;
        }
        // 17: mad r2.xy, r2.yx, c50.xx, r0.yx
        {
          precise float x = MulZ(r2.y, C(50).x) + r0.y;
          precise float y = MulZ(r2.x, C(50).x) + r0.x;
          r2.x = x;
          r2.y = y;
        }
        // 18: tfetch2D r2, r2.yx, tf0
        r2 = taps[j];
        // 19: sge r4.yz, r2.zw, c255.xy + maxs r2.xx
        {
          float y = Sge(r2.z, C(255).x);
          float z = Sge(r2.w, C(255).y);
          ps = r2.x;
          r4.y = y;
          r4.z = z;
        }
        // 20: mad r1.w, -r4.y, c254.z, r2.z
        r1.w = MulZ(-r4.y, C(254).z) + r2.z;
        // 21: add_sat r4.w, r0.w, r4.z + adds_prev r3.x, -r1.y
        {
          precise float sum = r0.w + r4.z;
          float v = Sat(sum);
          precise float s = -r1.y + ps;
          ps = s;
          r4.w = v;
          r3.x = ps;
        }
        // 22: sge r4.x, |r3.x|, c255.w
        r4.x = Sge(abs(r3.x), C(255).w);
        // 23: add r3, -r4.xzyw, c255.zzzz
        {
          precise vec4 v = -vec4(r4.x, r4.z, r4.y, r4.w) + vec4(C(255).z);
          r3 = v;
        }
        // 24: sge r1.w, r1.w, c254.y + maxs r3.zz
        {
          float v = Sge(r1.w, C(254).y);
          ps = r3.z;
          r1.w = v;
        }
        // 25: mad r1.w, r3.y, r1.w, r2.y
        r1.w = MulZ(r3.y, r1.w) + r2.y;
        // 26: add r1.w, r1.w, -r1.z + muls_prev r1.x, r1.x
        {
          precise float v = r1.w + -r1.z;
          ps = MulZ(r1.x, ps);
          r1.w = v;
          r1.x = ps;
        }
        // 27: add_sat r2.x, r0.z, r1.x
        {
          precise float sum = r0.z + r1.x;
          r2.x = Sat(sum);
        }
        // 28: mul r1.w, r1.x, |r1.w| + maxs r2.xx
        {
          precise float v = MulZ(r1.x, abs(r1.w));
          ps = r2.x;
          r1.w = v;
        }
        // 29: sge r2.x, r1.w, c255.w + muls_prev r1.w, r4.x
        {
          float v = Sge(r1.w, C(255).w);
          ps = MulZ(r4.x, ps);
          r2.x = v;
          r1.w = ps;
        }
        // 30: add r2.x, -r1.w, r2.x + muls r2.y, r3.wx
        {
          precise float v = -r1.w + r2.x;
          ps = MulZ(r3.w, r3.x);
          r2.x = v;
          r2.y = ps;
        }
        // 31: mad r1.w, r2.y, r2.x, r1.w
        r1.w = MulZ(r2.y, r2.x) + r1.w;
      }
      loop_address += loop_step;
    }
  }
#endif
  for (; i < count; ++i) {
    // 13: setp_eq r1.w
    p0 = r1.w == 0.0;
    ps = p0 ? 0.0 : 1.0;
    if (p0) {
      // 14, 15: rcp r2.x, c49.x; rcp r2.y, c48.x
      ps = 1.0 / C(49).x;
      r2.x = ps;
      ps = 1.0 / C(48).x;
      r2.y = ps;
      // 16: mul r2.xy, r2.xy, c[20+aL].xy
      vec4 offset = C(20 + loop_address);
      {
        precise float x = MulZ(r2.x, offset.x);
        precise float y = MulZ(r2.y, offset.y);
        r2.x = x;
        r2.y = y;
      }
      // 17: mad r2.xy, r2.yx, c50.xx, r0.yx
      {
        precise float x = MulZ(r2.y, C(50).x) + r0.y;
        precise float y = MulZ(r2.x, C(50).x) + r0.x;
        r2.x = x;
        r2.y = y;
      }
      // 18: tfetch2D r2, r2.yx, tf0
      r2 = Fetch0(vec2(r2.y, r2.x));
      // 19: sge r4.yz, r2.zw, c255.xy + maxs r2.xx
      {
        float y = Sge(r2.z, C(255).x);
        float z = Sge(r2.w, C(255).y);
        ps = r2.x;
        r4.y = y;
        r4.z = z;
      }
      // 20: mad r1.w, -r4.y, c254.z, r2.z
      r1.w = MulZ(-r4.y, C(254).z) + r2.z;
      // 21: add_sat r4.w, r0.w, r4.z + adds_prev r3.x, -r1.y
      {
        precise float sum = r0.w + r4.z;
        float v = Sat(sum);
        precise float s = -r1.y + ps;
        ps = s;
        r4.w = v;
        r3.x = ps;
      }
      // 22: sge r4.x, |r3.x|, c255.w
      r4.x = Sge(abs(r3.x), C(255).w);
      // 23: add r3, -r4.xzyw, c255.zzzz
      {
        precise vec4 v = -vec4(r4.x, r4.z, r4.y, r4.w) + vec4(C(255).z);
        r3 = v;
      }
      // 24: sge r1.w, r1.w, c254.y + maxs r3.zz
      {
        float v = Sge(r1.w, C(254).y);
        ps = r3.z;
        r1.w = v;
      }
      // 25: mad r1.w, r3.y, r1.w, r2.y
      r1.w = MulZ(r3.y, r1.w) + r2.y;
      // 26: add r1.w, r1.w, -r1.z + muls_prev r1.x, r1.x
      {
        precise float v = r1.w + -r1.z;
        ps = MulZ(r1.x, ps);
        r1.w = v;
        r1.x = ps;
      }
      // 27: add_sat r2.x, r0.z, r1.x
      {
        precise float sum = r0.z + r1.x;
        r2.x = Sat(sum);
      }
      // 28: mul r1.w, r1.x, |r1.w| + maxs r2.xx
      {
        precise float v = MulZ(r1.x, abs(r1.w));
        ps = r2.x;
        r1.w = v;
      }
      // 29: sge r2.x, r1.w, c255.w + muls_prev r1.w, r4.x
      {
        float v = Sge(r1.w, C(255).w);
        ps = MulZ(r4.x, ps);
        r2.x = v;
        r1.w = ps;
      }
      // 30: add r2.x, -r1.w, r2.x + muls r2.y, r3.wx
      {
        precise float v = -r1.w + r2.x;
        ps = MulZ(r3.w, r3.x);
        r2.x = v;
        r2.y = ps;
      }
      // 31: mad r1.w, r2.y, r2.x, r1.w
      r1.w = MulZ(r2.y, r2.x) + r1.w;
    }
    loop_address += loop_step;
  }

  // 32: max oC0.xyz1, r1.w, r1.w
  precise vec4 color = vec4(r1.w, r1.w, r1.w, 1.0) * xe_color_exp_bias.x;
  vec3 rgb = color.rgb;
  if ((xe_flags & kSysFlagConvertColor0ToGamma) != 0u) {
    rgb = vec3(LinearToPwlGamma(rgb.x), LinearToPwlGamma(rgb.y), LinearToPwlGamma(rgb.z));
  }
  xe_out_fragment_data_0 = vec4(rgb, color.a);
}
