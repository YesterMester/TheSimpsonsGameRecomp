/**
 ******************************************************************************
 * ReXGlue - Xbox 360 Static Recompilation Runtime                            *
 ******************************************************************************
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#pragma once

#include <rex/graphics/xenos.h>

namespace rex::graphics::draw_util {

enum class NativeColorResolvePacking : uint32_t {
  k8888,
  k2101010,
  kUint16x2,
  kUint16x4,
  kUint32,
  kUint32x2,
  kSint16x2,
  kSint16x4,
  kFloat7e3,
  kGamma8888,
  kUnsupported = UINT32_MAX,
};

constexpr NativeColorResolvePacking GetNativeColorResolvePacking(
    xenos::ColorRenderTargetFormat format) {
  switch (format) {
    case xenos::ColorRenderTargetFormat::k_8_8_8_8:
      return NativeColorResolvePacking::k8888;
    case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA:
      return NativeColorResolvePacking::kGamma8888;
    case xenos::ColorRenderTargetFormat::k_2_10_10_10:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10:
      return NativeColorResolvePacking::k2101010;
    case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
      return NativeColorResolvePacking::kUint16x2;
    case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
      return NativeColorResolvePacking::kUint16x4;
    case xenos::ColorRenderTargetFormat::k_32_FLOAT:
      return NativeColorResolvePacking::kUint32;
    case xenos::ColorRenderTargetFormat::k_32_32_FLOAT:
      return NativeColorResolvePacking::kUint32x2;
    case xenos::ColorRenderTargetFormat::k_16_16:
      return NativeColorResolvePacking::kSint16x2;
    case xenos::ColorRenderTargetFormat::k_16_16_16_16:
      return NativeColorResolvePacking::kSint16x4;
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16:
      return NativeColorResolvePacking::kFloat7e3;
    default:
      return NativeColorResolvePacking::kUnsupported;
  }
}

constexpr bool NativeColorResolveUsesIntegerSource(NativeColorResolvePacking packing) {
  return packing >= NativeColorResolvePacking::kUint16x2 &&
         packing <= NativeColorResolvePacking::kSint16x4;
}

enum class NativeColorResolveDestination : uint32_t {
  k8888,
  k2101010,
  kFloat16x2,
  kFloat16x4,
  kFloat32,
  kFloat32x2,
  k111110,
  k101111,
  kUnorm16x2,
  kUnorm16x4,
  kUnorm8,
  kUnorm8AliasA,
  kUnorm8AliasB,
  kUnorm8x2,
  k1555,
  k565,
  k655,
  k4444,
  kUnorm16,
  kFloat16,
  kFloat32x4,
  kUnsupported = UINT32_MAX,
};

constexpr NativeColorResolveDestination GetNativeColorResolvePacking(xenos::ColorFormat format) {
  switch (format) {
    case xenos::ColorFormat::k_8_8_8_8:
    case xenos::ColorFormat::k_8_8_8_8_A:
    case xenos::ColorFormat::k_8_8_8_8_AS_16_16_16_16:
      return NativeColorResolveDestination::k8888;
    case xenos::ColorFormat::k_2_10_10_10:
    case xenos::ColorFormat::k_2_10_10_10_AS_16_16_16_16:
      return NativeColorResolveDestination::k2101010;
    case xenos::ColorFormat::k_16_16_FLOAT:
      return NativeColorResolveDestination::kFloat16x2;
    case xenos::ColorFormat::k_16_16_16_16_FLOAT:
      return NativeColorResolveDestination::kFloat16x4;
    case xenos::ColorFormat::k_32_FLOAT:
      return NativeColorResolveDestination::kFloat32;
    case xenos::ColorFormat::k_32_32_FLOAT:
      return NativeColorResolveDestination::kFloat32x2;
    case xenos::ColorFormat::k_10_11_11:
    case xenos::ColorFormat::k_10_11_11_AS_16_16_16_16:
      return NativeColorResolveDestination::k111110;
    case xenos::ColorFormat::k_11_11_10:
    case xenos::ColorFormat::k_11_11_10_AS_16_16_16_16:
      return NativeColorResolveDestination::k101111;
    case xenos::ColorFormat::k_16_16:
      return NativeColorResolveDestination::kUnorm16x2;
    case xenos::ColorFormat::k_16_16_16_16:
      return NativeColorResolveDestination::kUnorm16x4;
    case xenos::ColorFormat::k_8:
      return NativeColorResolveDestination::kUnorm8;
    case xenos::ColorFormat::k_8_A:
      return NativeColorResolveDestination::kUnorm8AliasA;
    case xenos::ColorFormat::k_8_B:
      return NativeColorResolveDestination::kUnorm8AliasB;
    case xenos::ColorFormat::k_8_8:
      return NativeColorResolveDestination::kUnorm8x2;
    case xenos::ColorFormat::k_1_5_5_5:
      return NativeColorResolveDestination::k1555;
    case xenos::ColorFormat::k_5_6_5:
      return NativeColorResolveDestination::k565;
    case xenos::ColorFormat::k_6_5_5:
      return NativeColorResolveDestination::k655;
    case xenos::ColorFormat::k_4_4_4_4:
      return NativeColorResolveDestination::k4444;
    case xenos::ColorFormat::k_16:
      return NativeColorResolveDestination::kUnorm16;
    case xenos::ColorFormat::k_16_FLOAT:
      return NativeColorResolveDestination::kFloat16;
    case xenos::ColorFormat::k_32_32_32_32_FLOAT:
      return NativeColorResolveDestination::kFloat32x4;
    default:
      return NativeColorResolveDestination::kUnsupported;
  }
}

constexpr uint32_t GetNativeColorResolveSizeLog2(NativeColorResolveDestination format) {
  uint32_t value = uint32_t(format);
  if (value <= 9u)
    return value == 3u || value == 5u || value == 9u ? 3u : 2u;
  if (value <= 12u)
    return 0u;
  if (value <= 19u)
    return 1u;
  return value == 20u ? 4u : UINT32_MAX;
}

constexpr bool NativeResolveSampleSelectValid(xenos::MsaaSamples samples,
                                              xenos::CopySampleSelect selection, bool depth) {
  uint32_t value = uint32_t(selection);
  if (samples > xenos::MsaaSamples::k4X)
    return false;
  if (value < 4u)
    return value < (1u << uint32_t(samples));
  return !depth && ((samples == xenos::MsaaSamples::k2X && value == 4u) ||
                    (samples == xenos::MsaaSamples::k4X && value <= 6u));
}

// Native attachment packing, including floating-point payloads through their
// integer ownership views. NATIVE_COLOR_UINT selects that source declaration.
// t0: native image; u0: R32_UINT tiled destination.
// b0: width, height, pitch, base, scale_x, scale_y, packing, endian,
// swap_red_blue, original_resolution, origin_x, origin_y, destination_height,
// destination_slice, destination_array, exp_bias.
// Base is relative to the raw surface origin, including its displacement from
// the bound resource. Scaled addresses retain each 16-byte guest group.
inline constexpr char kNativeColorResolve[] = R"(
#if defined(NATIVE_RESOLVE_MSAA) && defined(NATIVE_COLOR_UINT)
Texture2DMS<uint4> source : register(t0);
#elif defined(NATIVE_RESOLVE_MSAA)
Texture2DMS<float4> source : register(t0);
#elif defined(NATIVE_COLOR_UINT)
Texture2D<uint4> source : register(t0);
#else
Texture2D<float4> source : register(t0);
#endif
RWBuffer<uint> destination : register(u0);
cbuffer parameters : register(b0) {
  uint width; uint height; uint pitch; uint base;
  uint scale_x; uint scale_y; uint packing; uint endian;
  uint swap_red_blue; uint original_resolution; uint origin_x; uint origin_y;
  uint destination_height; uint destination_slice; uint destination_array; float exp_bias;
  uint sample_select; uint sample_map; uint destination_packing; uint source_layout;
};
// Scale is uniform across the dispatch. Keep the common 1x/2x/3x divisions
// constant so the compiler can avoid a general integer divide per host pixel.
uint NativeScaleQuotient(uint value, uint scale) {
  uint result = value;
  [branch] if (scale == 2u) {
    result = value >> 1u;
  } else if (scale == 3u) {
    result = value / 3u;
  } else if (scale != 1u) {
    result = value / scale;
  }
  return result;
}
uint2 NativeScaleQuotient(uint2 value, uint2 scale) {
  return uint2(NativeScaleQuotient(value.x, scale.x), NativeScaleQuotient(value.y, scale.y));
}
uint2 NativeSourceCoord(uint2 texel) {
  uint2 result = texel;
  if (source_layout & 0x80000000u) {
    uint source_pitch = (source_layout >> 8u) & 65535u;
    uint samples = (source_layout >> 4u) & 3u;
    uint tile_width = 80u >> (((source_layout >> 6u) & 1u) + uint(samples == 2u));
    uint tile_height = 16u >> uint(samples != 0u);
    uint2 scale = uint2(scale_x, scale_y);
    uint2 pixel = NativeScaleQuotient(texel, scale);
    uint tile = ((pixel.y / tile_height) * source_pitch + pixel.x / tile_width) & 2047u;
    result = (uint2(tile % source_pitch, tile / source_pitch) * uint2(tile_width, tile_height)
              + pixel % uint2(tile_width, tile_height)) * scale + (texel - pixel * scale);
  }
  return result;
}
uint Encode7e3(float value) {
  uint f = asuint(clamp(value, 0.0f, 31.875f));
  uint biased = f < 0x3E800000u
      ? ((f & 0x7FFFFFu) | 0x800000u) >> min(125u - (f >> 23), 24u)
      : f + 0xC2000000u;
  return ((biased + 0x7FFFu + ((biased >> 16) & 1u)) >> 16) & 1023u;
}
float Decode7e3(uint value) {
  uint mantissa = value & 127u, exponent = value >> 7;
  float result = 0.0f;
  if (value) {
    if (!exponent) {
      uint msb = firstbithigh(mantissa);
      result = asfloat(((msb + 118u) << 23) | ((mantissa << (23u - msb)) & 0x7FFFFFu));
    } else {
      result = asfloat(((exponent + 124u) << 23) | (mantissa << 16));
    }
  }
  return result;
}
uint GammaByte(float value) {
  float scale = value >= 128.0f/1023.0f
      ? (value >= 512.0f/1023.0f ? 1023.0f/8.0f : 1023.0f/4.0f)
      : (value >= 64.0f/1023.0f ? 1023.0f/2.0f : 1023.0f);
  uint offset = value >= 128.0f/1023.0f
      ? (value >= 512.0f/1023.0f ? 128u : 64u)
      : (value >= 64.0f/1023.0f ? 32u : 0u);
  return uint(value * scale) + offset;
}
uint4 LoadBits(uint2 texel, uint sample) {
  texel = NativeSourceCoord(texel);
#ifdef NATIVE_RESOLVE_MSAA
  uint host_sample = (sample_map >> (sample * 2u)) & 3u;
  #ifdef NATIVE_COLOR_UINT
    return source.Load(texel, host_sample);
  #else
    float4 color = source.Load(texel, host_sample);
  #endif
#else
  #ifdef NATIVE_COLOR_UINT
    return source.Load(int3(texel, 0));
  #else
    float4 color = source.Load(int3(texel, 0));
  #endif
#endif
#ifndef NATIVE_COLOR_UINT
  uint4 result = 0;
  if (packing == 8u)
    result = uint4(Encode7e3(color.r), Encode7e3(color.g), Encode7e3(color.b), uint(saturate(color.a) * 3.0f + 0.5f));
  else if (packing == 9u)
    result = uint4(GammaByte(color.r), GammaByte(color.g), GammaByte(color.b), uint(color.a * 255.0f + 0.5f));
  else
    result = uint4(color * (packing == 1u ? float4(1023,1023,1023,3) : 255.0f) + 0.5f);
  return result;
#endif
}
float4 Decode(uint4 bits) {
  precise float4 color = 0;
#ifdef NATIVE_COLOR_UINT
  if (packing >= 6u) {
    precise float4 scaled = float4(int4(bits << 16) >> 16) * (32.0f/32767.0f);
    color = max(-1.0f, scaled);
    if (packing == 6u) color.zw = 0;
  } else {
    color = packing < 4u ? f16tof32(bits) : asfloat(bits);
    if (packing != 3u) color.zw = 0;
    if (packing == 4u) color.y = 0;
  }
#else
  if (packing == 8u)
    color = float4(Decode7e3(bits.r), Decode7e3(bits.g), Decode7e3(bits.b), float(bits.a) * (1.0f/3.0f));
  else
    color = float4(bits) * (packing == 1u
        ? float4(1.0f/1023.0f,1.0f/1023.0f,1.0f/1023.0f,1.0f/3.0f) : 1.0f/255.0f);
#endif
  return color;
}
uint4 Pack(uint4 bits, uint format) {
  uint4 result = 0;
  if (format == 0u)
    result = uint4(bits.r | (bits.g << 8) | (bits.b << 16) | (bits.a << 24), 0, 0, 0);
  else if (format == 1u)
    result = uint4(bits.r | (bits.g << 10) | (bits.b << 20) | (bits.a << 30), 0, 0, 0);
  else
    result = format < 4u ? uint4(bits.r | (bits.g << 16), bits.b | (bits.a << 16), 0, 0) : uint4(bits.rg, 0, 0);
  return result;
}
uint4 PackFloat(float4 color) {
  uint format = destination_packing;
  uint4 result = 0;
  if (format < 6u) {
    uint4 bits = format < 2u
      ? uint4(saturate(color) * (destination_packing == 1u ? float4(1023,1023,1023,3) : 255.0f) + 0.5f)
      : format < 4u ? f32tof16(color) : asuint(color);
    result = Pack(bits, format);
  } else if (format <= 7u) {
    uint4 bits = uint4(saturate(color) * (format == 6u ? float4(2047,2047,1023,0) : float4(1023,2047,2047,0)) + 0.5f);
    result = uint4(bits.r | (bits.g << (format == 6u ? 11u : 10u)) | (bits.b << (format == 6u ? 22u : 21u)), 0, 0, 0);
  } else if (format == 19u) {
    result = uint4(f32tof16(color.r), 0, 0, 0);
  } else if (format == 20u) {
    result = asuint(color);
  } else {
    float4 maximum = format <= 9u || format == 18u ? 65535.0f
      : format <= 13u ? 255.0f : format == 14u ? float4(31,31,31,1)
      : format == 15u ? float4(31,63,31,0) : format == 16u ? float4(31,31,63,0) : 15.0f;
    uint4 bits = uint4(saturate(color) * maximum + 0.5f);
    if (format <= 9u) result = uint4(bits.r | (bits.g << 16), bits.b | (bits.a << 16), 0, 0);
    else if (format <= 12u) result = uint4(bits.r, 0, 0, 0);
    else if (format == 13u) result = uint4(bits.r | (bits.g << 8), 0, 0, 0);
    else if (format == 14u) result = uint4(bits.r | (bits.g << 5) | (bits.b << 10) | (bits.a << 15), 0, 0, 0);
    else if (format == 15u) result = uint4(bits.r | (bits.g << 5) | (bits.b << 11), 0, 0, 0);
    else if (format == 16u) result = uint4(bits.r | (bits.g << 5) | (bits.b << 10), 0, 0, 0);
    else if (format == 17u) result = uint4(bits.r | (bits.g << 4) | (bits.b << 8) | (bits.a << 12), 0, 0, 0);
    else result = uint4(bits.r, 0, 0, 0);
  }
  return result;
}
uint DestinationSizeLog2() {
  uint f = destination_packing;
  return f <= 9u ? (f == 3u || f == 5u || f == 9u ? 3u : 2u)
                : f <= 12u ? 0u : f <= 19u ? 1u : 4u;
}
uint TiledAddress(uint x, uint y, uint size_log2) {
  uint result = 0;
  if (destination_array) {
    uint outer = ((y >> 4) + (destination_slice >> 2) * (destination_height >> 4)) * (pitch >> 5);
    uint macro = ((((x >> 5) + outer) << (size_log2 + 6)) & 0xFFFFFFFu) << 1;
    uint micro = ((x & 7) + ((y & 6) << 2)) << size_log2;
    uint bank = ((y >> 3) + (destination_slice >> 2)) & 1;
    uint pipe = bank + ((((x >> 3) + (bank << 1)) & 3) << 1);
    uint offset = ((macro + (micro & ~15u)) << 1) + (micro & 15) +
                  ((destination_slice & 3) << (size_log2 + 6)) + ((y & 1) << 4);
    uint address = (bank << 3) + ((offset >> 6) & 7);
    address = ((address << 3) + (pipe & ~1u)) << 2;
    result = ((address + (offset & ~511u)) << 3) + (offset & 63);
  } else {
    uint macro = ((x >> 5) + (y >> 5) * (pitch >> 5)) << (size_log2 + 7);
    uint micro = ((x & 7) + ((y & 14) << 2)) << size_log2;
    uint offset = macro + ((micro & ~15u) << 1) + (micro & 15) + ((y & 1) << 4);
    result = ((offset & ~511u) << 3) + ((y & 16) << 7) + ((offset & 448) << 2)
      + (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 63);
  }
  return result;
}
[numthreads(8,8,1)] void main(uint3 id : SV_DispatchThreadID) {
  uint2 output_scale = original_resolution ? uint2(1,1) : uint2(scale_x,scale_y);
  if (id.x >= width * output_scale.x || id.y >= height * output_scale.y) return;
  uint2 texel = original_resolution
      ? id.xy * uint2(scale_x,scale_y) + uint2(scale_x,scale_y) / 2
      : id.xy;
  texel += uint2(origin_x,origin_y) * uint2(scale_x,scale_y);
  uint4 bits = 0;
  uint first_sample = sample_select < 4u ? sample_select : sample_select == 5u ? 2u : 0u;
  uint4 raw = LoadBits(texel, first_sample);
  if (packing < 6u && sample_select < 4u && exp_bias == 1.0f && packing == destination_packing) {
    if (swap_red_blue && (packing < 2u || packing == 3u)) raw = raw.bgra;
    bits = Pack(raw, packing);
  } else {
    precise float4 color = Decode(raw);
    precise float factor = exp_bias;
    if (sample_select >= 4u) {
      color += Decode(LoadBits(texel, first_sample + 1u));
      factor *= 0.5f;
      if (sample_select == 6u) {
        color += Decode(LoadBits(texel, 2u));
        color += Decode(LoadBits(texel, 3u));
        factor *= 0.5f;
      }
    }
    color *= factor;
    if (swap_red_blue) {
      if (destination_packing >= 10u && destination_packing <= 12u) {
        if (packing == 5u) color.r = color.g;
        else if (packing != 2u && packing != 4u && packing != 6u) color.r = color.b;
      } else {
        color = color.bgra;
      }
    }
    bits = PackFloat(color);
  }
  uint size_log2 = DestinationSizeLog2();
  uint pixel_endian = endian < 4u ? endian
      : (size_log2 >= 3u && endian == 4u) || (size_log2 == 4u && endian == 5u) ? 2u : 0u;
  if (size_log2 >= 2u && (pixel_endian == 1 || pixel_endian == 2))
    bits = ((bits & 0x00FF00FF) << 8) | ((bits & 0xFF00FF00) >> 8);
  if (size_log2 >= 2u && (pixel_endian == 2 || pixel_endian == 3)) bits = (bits << 16) | (bits >> 16);
  if (size_log2 == 1u) {
    bits.x = endian == 1u ? ((bits.x & 255u) << 8) | ((bits.x >> 8) & 255u) : bits.x;
  }
  uint group_bytes_log2 = size_log2 == 0u ? 3u : 4u;
  uint group_bytes = 1u << group_bytes_log2;
  uint lane_shift = group_bytes_log2 - size_log2;
  uint lanes = 1u << lane_shift;
  uint2 position = id.xy + uint2(origin_x,origin_y) * output_scale;
  uint2 group = uint2(position.x >> lane_shift, position.y);
  uint2 guest_group = NativeScaleQuotient(group, output_scale);
  uint2 subpixel = group - guest_group * output_scale;
  uint guest_x = guest_group.x * lanes;
  uint guest_y = guest_group.y;
  uint address = TiledAddress(guest_x, guest_y, size_log2)
      * output_scale.x * output_scale.y
      + (subpixel.x * output_scale.y + subpixel.y) * group_bytes
      + (position.x & (lanes - 1u)) * (1u << size_log2) + base;
  if (output_scale.x == 1u && output_scale.y == 1u)
    address = TiledAddress(position.x, position.y, size_log2) + base;
  if (size_log2 < 2u) {
    uint shift = (address & 3u) * 8u;
    uint mask = size_log2 == 1u ? 65535u : 255u;
    InterlockedAnd(destination[address >> 2], ~(mask << shift));
    InterlockedOr(destination[address >> 2], (bits.x & mask) << shift);
  } else {
    uint word_xor = size_log2 >= 3u && endian == 4u ? 1u : size_log2 == 4u && endian == 5u ? 3u : 0u;
    for (uint word = 0; word < (1u << (size_log2 - 2u)); ++word)
      destination[(address >> 2) + (word ^ word_xor)] = bits[word];
  }
})";

}  // namespace rex::graphics::draw_util
