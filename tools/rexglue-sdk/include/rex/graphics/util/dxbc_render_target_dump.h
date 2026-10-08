/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <string>
#include <vector>

#include <rex/graphics/util/dxbc_render_target_transfer.h>

namespace rex::graphics {

// Native GPU encoding of the render targets owning a guest EDRAM tile range.
class DxbcRenderTargetDumpShader {
 public:
  union Key {
    uint32_t key;
    struct {
      xenos::MsaaSamples msaa_samples : 2;
      uint32_t resource_format : 4;
      uint32_t is_depth : 1;
    };
    Key() : key(0) { static_assert(sizeof(*this) == 4); }
    xenos::ColorRenderTargetFormat GetColorFormat() const {
      return xenos::ColorRenderTargetFormat(resource_format);
    }
    xenos::DepthRenderTargetFormat GetDepthFormat() const {
      return xenos::DepthRenderTargetFormat(resource_format);
    }
  };
  union Offsets {
    uint32_t offsets;
    struct {
      uint32_t dispatch_first_tile : xenos::kEdramBaseTilesBits + 1;
      uint32_t source_base_tiles : xenos::kEdramBaseTilesBits;
    };
    Offsets() : offsets(0) { static_assert(sizeof(*this) == 4); }
  };
  union Pitches {
    uint32_t pitches;
    struct {
      uint32_t dest_pitch : xenos::kEdramPitchTilesBits;
      uint32_t source_pitch : xenos::kEdramPitchTilesBits;
    };
    Pitches() : pitches(0) { static_assert(sizeof(*this) == 4); }
  };
  enum DumpCbuffer : uint32_t { kDumpCbufferOffsets, kDumpCbufferPitches, kDumpCbufferCount };
  using Options = DxbcRenderTargetTransferShader::Options;
  static bool Create(Key key, const Options& options, std::vector<uint32_t>& output,
                     std::string& error);
};

}  // namespace rex::graphics
