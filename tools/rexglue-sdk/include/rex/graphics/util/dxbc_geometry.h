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

#include <cstdint>
#include <functional>
#include <vector>

namespace rex::graphics {

enum class DxbcGeometryShaderType : uint32_t {
  kNone,
  kPointList,
  kRectangleList,
  kQuadList,
};

union DxbcGeometryShaderKey {
  uint32_t key;
  struct {
    DxbcGeometryShaderType type : 2;
    uint32_t interpolator_count : 5;
    uint32_t user_clip_plane_count : 3;
    uint32_t user_clip_plane_cull : 1;
    uint32_t has_vertex_kill_and : 1;
    uint32_t has_point_size : 1;
    uint32_t has_point_coordinates : 1;
    // PA_CL_CLIP_CNTL::ps_ucp_mode for point primitives.
    uint32_t point_ps_ucp_mode : 2;
  };

  DxbcGeometryShaderKey() : key(0) { static_assert(sizeof(*this) == sizeof(key)); }

  struct Hasher {
    size_t operator()(const DxbcGeometryShaderKey& key) const {
      return std::hash<uint32_t>{}(key.key);
    }
  };
  bool operator==(const DxbcGeometryShaderKey& other_key) const { return key == other_key.key; }
  bool operator!=(const DxbcGeometryShaderKey& other_key) const { return !(*this == other_key); }
};

// Common primitive expansion bytecode used by the Direct3D backends.
// Direct3D 11 converts these exact SM 5.1 bindings to native SM 5.0.
void CreateDxbcGeometryShader(DxbcGeometryShaderKey key, std::vector<uint32_t>& shader_out);

}  // namespace rex::graphics
