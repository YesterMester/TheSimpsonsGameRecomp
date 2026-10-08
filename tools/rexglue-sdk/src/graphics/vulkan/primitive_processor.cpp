/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include <rex/assert.h>
#include <rex/graphics/flags.h>
#include <rex/graphics/vulkan/command_processor.h>
#include <rex/graphics/vulkan/deferred_command_buffer.h>
#include <rex/graphics/vulkan/primitive_processor.h>
#include <rex/graphics/util/native_buffer_watch.h>
#include <rex/graphics/util/bytes_equal.h>
#include <rex/logging.h>
#include <rex/ui/vulkan/util.h>

REXCVAR_DEFINE_BOOL(vulkan_force_expand_point_sprites_in_vs, false, "GPU/Vulkan",
                    "Force Vulkan point sprite expansion in the vertex shader, even when geometry "
                    "shaders are available")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(
    vulkan_force_expand_rectangle_lists_in_vs, false, "GPU/Vulkan",
    "Force Vulkan rectangle list expansion in the vertex shader, even when geometry "
    "shaders are available")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(vulkan_force_convert_quad_lists_to_triangle_lists, false, "GPU/Vulkan",
                    "Force Vulkan quad list conversion to triangle lists in primitive processing, "
                    "even when geometry shaders are available")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(vulkan_geometry_shader_primitives, true, "GPU/Vulkan",
                    "Draw quad lists, point sprites and rectangle lists with geometry shaders where "
                    "the device has them (slightly faster on the Steam Deck). Off: converted to "
                    "triangles and expanded in the vertex shader everywhere - the same image, and "
                    "the only way on devices without geometry shaders such as Mali GPUs")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(native_index_buffer_cache, false, "GPU/Vulkan",
                    "Retain immutable DMA index buffers, validating their bytes before reuse")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(native_index_buffer_cache_mb, 16, "GPU/Vulkan",
                     "Maximum MiB per retained index buffer generation")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(native_index_bounds_cache, false, "GPU/Vulkan",
                    "Reuse vertex bounds of verified immutable native index snapshots")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DECLARE(bool, native_buffer_write_watches);

namespace rex::graphics::vulkan {

struct VulkanPrimitiveProcessor::NativeIndexCache {
  struct Entry {
    std::vector<uint8_t> bytes;
    std::pair<VkBuffer, VkDeviceSize> buffer;
    std::unique_ptr<NativeBufferWatch> watch;
    std::array<uint32_t, 5> bounds_key{};
    draw_util::VertexIndexBounds bounds;
    bool bounds_valid = false;
  };
  explicit NativeIndexCache(const ui::vulkan::VulkanDevice* device)
      : pool(device, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, size_t(2) << 20) {}
  ui::vulkan::VulkanUploadBufferPool pool;
  std::unordered_map<uint64_t, Entry> entries;
  uint64_t bytes = 0;
};

VulkanPrimitiveProcessor::VulkanPrimitiveProcessor(const RegisterFile& register_file,
                                                   memory::Memory& memory,
                                                   TraceWriter& trace_writer,
                                                   SharedMemory& shared_memory,
                                                   VulkanCommandProcessor& command_processor)
    : PrimitiveProcessor(register_file, memory, trace_writer, shared_memory),
      command_processor_(command_processor) {}

VulkanPrimitiveProcessor::~VulkanPrimitiveProcessor() {
  Shutdown(true);
}

bool VulkanPrimitiveProcessor::Initialize() {
  const ui::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Properties& device_properties = vulkan_device->properties();
  // Keep triangle fan handling aligned with D3D12 by always converting fans to
  // lists in the shared primitive processor path.
  constexpr bool triangle_fans_supported_without_conversion = false;

  // Default to D3D12-like geometry-shader-based handling when available, but
  // allow opting into Vulkan fallback paths for debugging and downlevel
  // compatibility testing.
  bool geometry_shader_primitive_emulation_allowed =
      device_properties.geometryShader && REXCVAR_GET(vulkan_geometry_shader_primitives);
  bool quad_lists_supported_without_conversion =
      geometry_shader_primitive_emulation_allowed &&
      !REXCVAR_GET(vulkan_force_convert_quad_lists_to_triangle_lists);
  bool point_sprites_supported_without_vs_expansion =
      geometry_shader_primitive_emulation_allowed &&
      !REXCVAR_GET(vulkan_force_expand_point_sprites_in_vs);
  bool rectangle_lists_supported_without_vs_expansion =
      geometry_shader_primitive_emulation_allowed &&
      !REXCVAR_GET(vulkan_force_expand_rectangle_lists_in_vs);

  if (!InitializeCommon(
          device_properties.fullDrawIndexUint32, triangle_fans_supported_without_conversion, false,
          quad_lists_supported_without_conversion, point_sprites_supported_without_vs_expansion,
          rectangle_lists_supported_without_vs_expansion)) {
    Shutdown();
    return false;
  }
  frame_index_buffer_pool_ = std::make_unique<ui::vulkan::VulkanUploadBufferPool>(
      vulkan_device, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
      std::max(size_t(kMinRequiredConvertedIndexBufferSize),
               ui::GraphicsUploadBufferPool::kDefaultPageSize));
  return true;
}

void VulkanPrimitiveProcessor::Shutdown(bool from_destructor) {
  const ui::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  frame_index_buffers_.clear();
  native_index_cache_.reset();
  native_index_caches_retired_.clear();
  frame_index_buffer_pool_.reset();
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device, builtin_index_buffer_upload_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device,
                                         builtin_index_buffer_upload_memory_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device, builtin_index_buffer_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device, builtin_index_buffer_memory_);

  if (!from_destructor) {
    ShutdownCommon();
  }
}

void VulkanPrimitiveProcessor::ClearCache() {
  // The command processor has waited for all queue operations.
  frame_index_buffer_pool_->ClearCache();
  native_index_cache_.reset();
  native_index_caches_retired_.clear();
}

void VulkanPrimitiveProcessor::CompletedSubmissionUpdated() {
  if (builtin_index_buffer_upload_ != VK_NULL_HANDLE &&
      command_processor_.GetCompletedSubmission() >= builtin_index_buffer_upload_submission_) {
    const ui::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
    const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
    const VkDevice device = vulkan_device->device();
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device,
                                           builtin_index_buffer_upload_);
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device,
                                           builtin_index_buffer_upload_memory_);
  }
}

void VulkanPrimitiveProcessor::BeginSubmission() {
  if (builtin_index_buffer_upload_ != VK_NULL_HANDLE &&
      builtin_index_buffer_upload_submission_ == UINT64_MAX) {
    // No need to submit deferred barriers - builtin_index_buffer_ has never
    // been used yet, and builtin_index_buffer_upload_ is written before
    // submitting commands reading it.

    command_processor_.EndRenderPass();

    DeferredCommandBuffer& command_buffer = command_processor_.deferred_command_buffer();

    VkBufferCopy* copy_region =
        command_buffer.CmdCopyBufferEmplace(builtin_index_buffer_upload_, builtin_index_buffer_, 1);
    copy_region->srcOffset = 0;
    copy_region->dstOffset = 0;
    copy_region->size = builtin_index_buffer_size_;

    command_processor_.PushBufferMemoryBarrier(
        builtin_index_buffer_, 0, VK_WHOLE_SIZE, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_INDEX_READ_BIT);

    builtin_index_buffer_upload_submission_ = command_processor_.GetCurrentSubmission();
  }
}

void VulkanPrimitiveProcessor::BeginFrame() {
  frame_index_buffer_pool_->Reclaim(command_processor_.GetCompletedFrame());
  while (!native_index_caches_retired_.empty() &&
         native_index_caches_retired_.front().first <= command_processor_.GetCompletedFrame()) {
    native_index_caches_retired_.pop_front();
  }
}

void VulkanPrimitiveProcessor::EndSubmission() {
  frame_index_buffer_pool_->FlushWrites();
  if (native_index_cache_) {
    native_index_cache_->pool.FlushWrites();
  }
}

void VulkanPrimitiveProcessor::EndFrame() {
  ClearPerFrameCache();
  frame_index_buffers_.clear();
}

bool VulkanPrimitiveProcessor::InitializeBuiltinIndexBuffer(
    size_t size_bytes, std::function<void(void*)> fill_callback) {
  assert_not_zero(size_bytes);
  assert_true(builtin_index_buffer_ == VK_NULL_HANDLE);
  assert_true(builtin_index_buffer_memory_ == VK_NULL_HANDLE);
  assert_true(builtin_index_buffer_upload_ == VK_NULL_HANDLE);
  assert_true(builtin_index_buffer_upload_memory_ == VK_NULL_HANDLE);

  const ui::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  builtin_index_buffer_size_ = VkDeviceSize(size_bytes);
  if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(
          vulkan_device, builtin_index_buffer_size_,
          VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
          ui::vulkan::util::MemoryPurpose::kDeviceLocal, builtin_index_buffer_,
          builtin_index_buffer_memory_)) {
    REXGPU_ERROR(
        "Vulkan primitive processor: Failed to create the built-in index "
        "buffer GPU resource with {} bytes",
        size_bytes);
    return false;
  }
  uint32_t upload_memory_type;
  if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(
          vulkan_device, builtin_index_buffer_size_, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
          ui::vulkan::util::MemoryPurpose::kUpload, builtin_index_buffer_upload_,
          builtin_index_buffer_upload_memory_, &upload_memory_type)) {
    REXGPU_ERROR(
        "Vulkan primitive processor: Failed to create the built-in index "
        "buffer upload resource with {} bytes",
        size_bytes);
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device, builtin_index_buffer_);
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device, builtin_index_buffer_memory_);
    return false;
  }

  void* mapping;
  if (dfn.vkMapMemory(device, builtin_index_buffer_upload_memory_, 0, VK_WHOLE_SIZE, 0, &mapping) !=
      VK_SUCCESS) {
    REXGPU_ERROR(
        "Vulkan primitive processor: Failed to map the built-in index buffer "
        "upload resource with {} bytes",
        size_bytes);
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device,
                                           builtin_index_buffer_upload_);
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device,
                                           builtin_index_buffer_upload_memory_);
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device, builtin_index_buffer_);
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device, builtin_index_buffer_memory_);
    return false;
  }
  fill_callback(mapping);
  ui::vulkan::util::FlushMappedMemoryRange(vulkan_device, builtin_index_buffer_memory_,
                                           upload_memory_type);
  dfn.vkUnmapMemory(device, builtin_index_buffer_upload_memory_);

  // Schedule uploading in the first submission.
  builtin_index_buffer_upload_submission_ = UINT64_MAX;
  return true;
}

void* VulkanPrimitiveProcessor::RequestHostConvertedIndexBufferForCurrentFrame(
    xenos::IndexFormat format, uint32_t index_count, bool coalign_for_simd,
    uint32_t coalignment_original_address, size_t& backend_handle_out) {
  size_t index_size = format == xenos::IndexFormat::kInt16 ? sizeof(uint16_t) : sizeof(uint32_t);
  VkBuffer buffer;
  VkDeviceSize offset;
  uint8_t* mapping = frame_index_buffer_pool_->Request(
      command_processor_.GetCurrentFrame(),
      index_size * index_count + (coalign_for_simd ? XE_GPU_PRIMITIVE_PROCESSOR_SIMD_SIZE : 0),
      index_size, buffer, offset);
  if (!mapping) {
    return nullptr;
  }
  if (coalign_for_simd) {
    ptrdiff_t coalignment_offset = GetSimdCoalignmentOffset(mapping, coalignment_original_address);
    mapping += coalignment_offset;
    offset = VkDeviceSize(offset + coalignment_offset);
  }
  backend_handle_out = frame_index_buffers_.size();
  frame_index_buffers_.emplace_back(buffer, offset);
  return mapping;
}

draw_util::VertexIndexBounds VulkanPrimitiveProcessor::GetNativeVertexIndexBounds(
    const ProcessingResult& primitives, xenos::Endian endian, uint32_t base, uint32_t clamp_min,
    uint32_t clamp_max) {
  bool index_16 = primitives.host_index_format == xenos::IndexFormat::kInt16;
  bool restart = primitives.host_primitive_reset_enabled;
  NativeIndexCache::Entry* entry = nullptr;
  std::array<uint32_t, 5> key = {
      primitives.host_draw_vertex_count, base, clamp_min, clamp_max,
      uint32_t(endian) | (uint32_t(index_16) << 8) | (uint32_t(restart) << 9)};
  if (REXCVAR_GET(native_index_bounds_cache) && native_index_cache_) {
    uint64_t length = uint64_t(primitives.host_draw_vertex_count) * (index_16 ? 2 : 4);
    if (length <= UINT32_MAX) {
      uint64_t buffer_key = (uint64_t(primitives.guest_index_base) << 32) | length;
      auto found = native_index_cache_->entries.find(buffer_key);
      // Process validated the bytes before returning this immutable shadow.
      // Its identity excludes the fresh scratch shadow rewritten each draw.
      if (found != native_index_cache_->entries.end() &&
          primitives.native_index_snapshot == found->second.bytes.data()) {
        entry = &found->second;
        if (entry->bounds_valid && entry->bounds_key == key) {
          static thread_local uint64_t reused = 0;
          if (++reused <= 8 || !(reused & 4095)) {
            REXGPU_INFO("[native-index-bounds-reuse] {} verified scans reused", reused);
          }
          return entry->bounds;
        }
      }
    }
  }
  auto bounds = draw_util::GetVertexIndexBounds(primitives.native_index_snapshot,
                                                primitives.host_draw_vertex_count, index_16, endian,
                                                restart, base, clamp_min, clamp_max);
  if (entry) {
    entry->bounds_key = key;
    entry->bounds = bounds;
    entry->bounds_valid = true;
  }
  return bounds;
}

bool VulkanPrimitiveProcessor::TryRetainedNativeIndexBuffer(uint32_t address, uint32_t length,
                                                            const void* source,
                                                            size_t& backend_handle_out,
                                                            const void*& cpu_snapshot_out) {
  if (!REXCVAR_GET(native_index_buffer_cache) || !length || length > (1u << 20)) {
    return false;
  }
  uint64_t key = (uint64_t(address) << 32) | length;
  if (native_index_cache_) {
    auto found = native_index_cache_->entries.find(key);
    if (found != native_index_cache_->entries.end()) {
      const NativeIndexCache::Entry& entry = found->second;
      if ((!entry.watch || !entry.watch->IsCurrent()) &&
          !draw_util::BytesEqual(entry.bytes.data(), source, length)) {
        // Keep the old GPU version immutable while earlier draws use it.
        return false;
      }
      cpu_snapshot_out = entry.bytes.data();
      backend_handle_out = frame_index_buffers_.size();
      frame_index_buffers_.push_back(entry.buffer);
      static thread_local uint64_t reused = 0;
      if (++reused <= 8 || !(reused & 4095)) {
        REXGPU_INFO("[native-index-reuse] {} unchanged snapshots reused", reused);
      }
      return true;
    }
    size_t byte_limit = size_t(std::clamp(REXCVAR_GET(native_index_buffer_cache_mb), 1, 64)) << 20;
    if (native_index_cache_->bytes + length > byte_limit ||
        native_index_cache_->entries.size() >= 4096) {
      native_index_cache_->pool.FlushWrites();
      native_index_caches_retired_.emplace_back(command_processor_.GetCurrentFrame(),
                                                std::move(native_index_cache_));
    }
  }
  if (!native_index_cache_) {
    native_index_cache_ = std::make_unique<NativeIndexCache>(command_processor_.GetVulkanDevice());
  }
  NativeIndexCache::Entry entry;
  entry.bytes.resize(length);
  std::memcpy(entry.bytes.data(), source, length);
  uint8_t* mapping =
      native_index_cache_->pool.Request(command_processor_.GetCurrentFrame(), length,
                                        sizeof(uint32_t), entry.buffer.first, entry.buffer.second);
  if (!mapping) {
    return false;
  }
  std::memcpy(mapping, entry.bytes.data(), length);
  backend_handle_out = frame_index_buffers_.size();
  frame_index_buffers_.push_back(entry.buffer);
  native_index_cache_->bytes += length;
  if (REXCVAR_GET(native_buffer_write_watches) && length >= 1024) {
    entry.watch = std::make_unique<NativeBufferWatch>(shared_memory());
    entry.watch->AddRange(address, length, entry.bytes.data(), source);
  }
  cpu_snapshot_out =
      native_index_cache_->entries.emplace(key, std::move(entry)).first->second.bytes.data();
  return true;
}

}  // namespace rex::graphics::vulkan
