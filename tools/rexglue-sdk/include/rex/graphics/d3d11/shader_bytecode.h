#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace rex::graphics::d3d11 {

enum class ResourceKind : uint32_t {
  kSampler = 6,
  kShaderResource = 7,
  kConstantBuffer = 8,
  kUnorderedAccess = 30,
};

struct ResourceBinding {
  ResourceKind kind;
  uint32_t id;
  uint32_t source_register;
  uint32_t host_register;
};

// Guest and host-format channel mappings, composed by the texture cache.
// Four 3-bit selectors choose RGBA, zero or one (0 through 5 respectively).
struct TextureSwizzle {
  uint32_t srv_register;
  uint32_t swizzle;
  bool operator==(const TextureSwizzle&) const = default;
};
constexpr uint32_t kIdentityTextureSwizzle = 0 | (1 << 3) | (2 << 6) | (3 << 9);

struct ShaderBytecodeOptions {
  // Features queried from the owning D3D11 device, expressed as SFI0 bits.
  // The default requires only the base FL 11_0 shader features.
  uint64_t supported_feature_flags = 0;
  uint32_t uav_register_count = 8;
  // Graphics UAVs share the output-merger register namespace with the four guest
  // render targets. Keep them above the render targets even on FL 11_0.
  uint32_t graphics_uav_register_offset = 4;
  struct ReadOnlyUavAlias {
    uint32_t uav_register;
    uint32_t srv_register;
  };
  // The guest translator emits both SRV and UAV reads of shared memory, even
  // for shaders without memexport. When the pipeline cannot write that memory,
  // both reads may use the same SRV on FL 11_0. Aliases must refer to declared
  // raw SRVs; any UAV write makes conversion fail rather than losing a write.
  std::vector<ReadOnlyUavAlias> read_only_uav_aliases;
  // D3D11 SRVs have no channel mapping. Specialize texture fetch operands for
  // each immutable native shader variant. Permutations need no new instructions;
  // zero/one selectors add one masked MOV after the fetch. Size and LOD queries
  // remain unchanged. Raw shared-memory reads must never have a mapping.
  std::vector<TextureSwizzle> texture_swizzles;
};

struct ShaderBytecode {
  std::vector<uint8_t> data;
  std::vector<ResourceBinding> bindings;
  std::vector<ResourceBinding> aliased_uav_reads;
  uint32_t program_type = 0;
  uint32_t texture_swizzle_moves = 0;
};

// Converts the translator's single-resource, space-zero SM 5.1 bindings to
// fixed SM 5.0 registers, with optional native texture channel specialization.
// Bindless ranges and unsupported device features fail with an explanation.
// Input and output may not alias. Failure clears the output.
bool ConvertShaderBytecode(std::span<const uint8_t> source, const ShaderBytecodeOptions& options,
                           ShaderBytecode& output, std::string& error);

}  // namespace rex::graphics::d3d11
