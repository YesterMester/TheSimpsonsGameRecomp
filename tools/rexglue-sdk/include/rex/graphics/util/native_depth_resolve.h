/**
 ******************************************************************************
 * ReXGlue - Xbox 360 Static Recompilation Runtime                            *
 ******************************************************************************
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#pragma once

namespace rex::graphics::draw_util {

// Native single-sampled depth/stencil packing. The 20e4 conversion is the
// integer conversion used by the existing depth dump, including its rounding
// policy and the host depth's [0, 1) remap. No EDRAM buffer is involved.
// t0: depth SRV; t1: stencil SRV (green component); u0: R32_UINT destination.
// b0: width, height, pitch, base, scale_x, scale_y, float24, endian,
// round_float24, original_resolution, origin_x, origin_y, destination_height,
// destination_slice, destination_array, unused.
inline constexpr char kNativeDepthResolve[] = R"(
#ifdef NATIVE_RESOLVE_MSAA
Texture2DMS<float> source_depth : register(t0);
Texture2DMS<uint4> source_stencil : register(t1);
#else
Texture2D<float> source_depth : register(t0);
Texture2D<uint4> source_stencil : register(t1);
#endif
RWBuffer<uint> destination : register(u0);
cbuffer parameters : register(b0) {
  uint width; uint height; uint pitch; uint base;
  uint scale_x; uint scale_y; uint float24; uint endian;
  uint round_float24; uint original_resolution; uint origin_x; uint origin_y;
  uint destination_height; uint destination_slice; uint destination_array; uint padding;
  uint sample_select; uint sample_map; uint destination_packing; uint source_layout;
};
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
    uint tile_width = 80u >> uint(samples == 2u);
    uint tile_height = 16u >> uint(samples != 0u);
    uint2 scale = uint2(scale_x, scale_y);
    uint2 pixel = NativeScaleQuotient(texel, scale);
    uint tile = ((pixel.y / tile_height) * source_pitch + pixel.x / tile_width) & 2047u;
    result = (uint2(tile % source_pitch, tile / source_pitch) * uint2(tile_width, tile_height)
              + pixel % uint2(tile_width, tile_height)) * scale + (texel - pixel * scale);
  }
  return result;
}
uint TiledAddress(uint x, uint y) {
  uint result = 0;
  if (destination_array) {
    uint outer = ((y >> 4) + (destination_slice >> 2) * (destination_height >> 4)) * (pitch >> 5);
    uint macro = ((((x >> 5) + outer) << 8) & 0xFFFFFFFu) << 1;
    uint micro = ((x & 7) + ((y & 6) << 2)) << 2;
    uint bank = ((y >> 3) + (destination_slice >> 2)) & 1;
    uint pipe = bank + ((((x >> 3) + (bank << 1)) & 3) << 1);
    uint offset = ((macro + (micro & ~15u)) << 1) + (micro & 15) +
                  ((destination_slice & 3) << 8) + ((y & 1) << 4);
    uint address = (bank << 3) + ((offset >> 6) & 7);
    address = ((address << 3) + (pipe & ~1u)) << 2;
    result = ((address + (offset & ~511u)) << 3) + (offset & 63);
  } else {
    uint macro = ((x >> 5) + (y >> 5) * (pitch >> 5)) << 9;
    uint micro = ((x & 7) + ((y & 14) << 2)) << 2;
    uint offset = macro + ((micro & ~15u) << 1) + (micro & 15) + ((y & 1) << 4);
    result = ((offset & ~511u) << 3) + ((y & 16) << 7) + ((offset & 448) << 2)
      + (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 63);
  }
  return result;
}
uint Depth20e4(float value) {
  uint bits = asuint(value), biased;
  if (bits < 0x38000000u)
    biased = ((bits & 0x7FFFFFu) | 0x800000u) >> min(112u - (bits >> 23), 24u);
  else
    biased = bits + 0xC8800000u;
  if (round_float24) biased += 3u + ((biased >> 3) & 1u);
  return (biased >> 3) & 0xFFFFFFu;
}
[numthreads(8,8,1)] void main(uint3 id : SV_DispatchThreadID) {
  uint2 output_scale = original_resolution ? uint2(1,1) : uint2(scale_x,scale_y);
  if (id.x >= width * output_scale.x || id.y >= height * output_scale.y) return;
  uint2 texel = original_resolution
      ? id.xy * uint2(scale_x,scale_y) + uint2(scale_x,scale_y) / 2
      : id.xy;
  texel += uint2(origin_x,origin_y) * uint2(scale_x,scale_y);
  texel = NativeSourceCoord(texel);
#ifdef NATIVE_RESOLVE_MSAA
  uint host_sample = (sample_map >> (sample_select * 2u)) & 3u;
  float depth = source_depth.Load(texel, host_sample);
  uint stencil = source_stencil.Load(texel, host_sample).g;
#else
  float depth = source_depth.Load(int3(texel, 0));
  uint stencil = source_stencil.Load(int3(texel, 0)).g;
#endif
  precise float fixed_depth = depth * 16777215.0f;
  uint bits = ((float24 ? Depth20e4(depth) : uint(round(fixed_depth))) << 8) | stencil;
  if (endian == 1 || endian == 2)
    bits = ((bits & 0x00FF00FF) << 8) | ((bits & 0xFF00FF00) >> 8);
  if (endian == 2 || endian == 3) bits = (bits << 16) | (bits >> 16);
  uint2 group = uint2(id.x / 4, id.y);
  uint2 guest_group = NativeScaleQuotient(group, output_scale);
  uint2 subpixel = group - guest_group * output_scale;
  uint guest_x = guest_group.x * 4 + origin_x;
  uint guest_y = guest_group.y + origin_y;
  uint address = TiledAddress(guest_x, guest_y) * output_scale.x * output_scale.y
      + (subpixel.x * output_scale.y + subpixel.y) * 16
      + (id.x % 4) * 4 + base;
  destination[address >> 2] = bits;
})";

}  // namespace rex::graphics::draw_util
