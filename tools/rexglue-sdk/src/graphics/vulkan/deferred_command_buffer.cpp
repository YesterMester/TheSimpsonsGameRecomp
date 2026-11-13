/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <vector>
#include <functional>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <utility>

#include <rex/assert.h>
#include <rex/dbg.h>
#include <rex/graphics/flags.h>
#include <rex/graphics/vulkan/command_processor.h>
#include <rex/graphics/vulkan/deferred_command_buffer.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/perf/counter.h>
#include <rex/platform/performance_hint.h>
#include <rex/thread.h>

namespace rex::graphics {
extern uint64_t g_upload_request_stats[3 + 32];
extern uint64_t g_streamed_skip_stats[2];
extern std::map<std::pair<uint32_t, uint32_t>, std::pair<uint64_t, uint64_t>>
    g_upload_request_ranges;
}  // namespace rex::graphics

namespace rex::graphics::vulkan {

namespace {
// Debugging aid: REX_CMD_STATS=1 logs the average mix of commands per executed command buffer.
struct CommandStats {
  bool enabled = std::getenv("REX_CMD_STATS") != nullptr;
  uint64_t executions = 0;
  uint64_t counts[64] = {};
  uint64_t buffer_barriers = 0;
  uint64_t image_barriers = 0;
  std::map<std::pair<uint32_t, uint32_t>, uint64_t> barrier_stages;
};
CommandStats& GetCommandStats() {
  static CommandStats stats;
  return stats;
}
constexpr const char* kCommandStatNames[] = {
    "BeginRenderPass", "BeginQuery", "BindDescriptorSets", "BindIndexBuffer", "BindPipeline",
    "BindVertexBuffers", "ClearAttachments", "ClearColorImage", "CopyBuffer", "FillBuffer",
    "CopyBufferToImage", "CopyImageToBuffer", "CopyQueryPoolResults", "Dispatch", "Draw",
    "DrawIndexed", "EndQuery", "EndRenderPass", "BeginRendering", "EndRendering",
    "PipelineBarrier", "PushConstants", "ResetQueryPool", "SetBlendConstants", "SetDepthBias",
    "SetScissor", "SetStencilCompareMask", "SetStencilReference", "SetStencilWriteMask",
    "SetViewport", "WriteTimestamp", "CopyImage"};
}  // namespace

DeferredCommandBuffer::DeferredCommandBuffer(const VulkanCommandProcessor& command_processor,
                                             size_t initial_size)
    : command_processor_(command_processor),
      chunk_capacities_(new size_t[kMaxChunks]()),
      chunk_ends_(new size_t[kMaxChunks]()) {
  chunks_.reserve(kMaxChunks);
  EnsureChunk(0, std::max(kChunkElements, initial_size / sizeof(uintmax_t)));
}

DeferredCommandBuffer::~DeferredCommandBuffer() {
  if (worker_.joinable()) {
    {
      std::lock_guard<std::mutex> lock(worker_mutex_);
      worker_stop_ = true;
    }
    worker_cond_.notify_one();
    finishing_.store(true, std::memory_order_release);
    wake_.fetch_add(1, std::memory_order_release);
    wake_.notify_one();
    worker_.join();
  }
}

void DeferredCommandBuffer::Reset() {
  assert_false(executing_);
  write_chunk_ = 0;
  write_offset_ = 0;
}

void DeferredCommandBuffer::EnsureChunk(size_t index, size_t size) {
  assert_true(index < kMaxChunks);
  if (index < chunks_.size() && chunk_capacities_[index] >= size) {
    return;
  }
  size_t capacity = std::max(kChunkElements, size);
  std::unique_ptr<uintmax_t[]> chunk(new uintmax_t[capacity]);
  if (index < chunks_.size()) {
    chunks_[index] = std::move(chunk);
  } else {
    chunks_.push_back(std::move(chunk));
  }
  chunk_capacities_[index] = capacity;
}

void DeferredCommandBuffer::Execute(VkCommandBuffer command_buffer) {
#if XE_GPU_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_GPU_FINE_GRAINED_DRAW_SCOPES
  assert_false(executing_);
  for (size_t i = 0; i <= write_chunk_; ++i) {
    const uintmax_t* chunk = chunks_[i].get();
    ExecuteRange(command_buffer, chunk, chunk + (i < write_chunk_ ? chunk_ends_[i] : write_offset_));
  }
  ReportCommandStats();
}

void DeferredCommandBuffer::BeginExecution(VkCommandBuffer command_buffer) {
  assert_false(executing_);
  executing_ = true;
  execution_command_buffer_ = command_buffer;
  commands_since_wake_ = 0;
  finishing_.store(false, std::memory_order_relaxed);
  finished_.store(false, std::memory_order_relaxed);
  // Everything written before is the caller's to record.
  uint64_t start = (uint64_t(write_chunk_) << 32) | write_offset_;
  written_.store(start, std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lock(worker_mutex_);
    // Not written_, which may be past it by the time the worker reads it.
    execution_start_ = start;
    worker_job_ = true;
    if (!worker_.joinable()) {
      worker_ = std::thread([this] { WorkerMain(); });
    }
  }
  worker_cond_.notify_one();
}

void DeferredCommandBuffer::FinishExecution() {
  if (!executing_) {
    return;
  }
  PublishWritten();
  finishing_.store(true, std::memory_order_release);
  wake_.fetch_add(1, std::memory_order_release);
  wake_.notify_one();
  while (!finished_.load(std::memory_order_acquire)) {
    finished_.wait(false, std::memory_order_acquire);
  }
  executing_ = false;
  execution_command_buffer_ = VK_NULL_HANDLE;
}

void DeferredCommandBuffer::PublishWritten() {
  written_.store((uint64_t(write_chunk_) << 32) | write_offset_, std::memory_order_release);
}

void DeferredCommandBuffer::WorkerMain() {
  rex::thread::set_current_thread_name("GPU Recording");
  // Submissions wait for this thread's recording.
  rex::platform::AddPerformanceHintThread();
  for (;;) {
    VkCommandBuffer command_buffer;
    uint64_t position;
    {
      std::unique_lock<std::mutex> lock(worker_mutex_);
      worker_cond_.wait(lock, [this] { return worker_job_ || worker_stop_; });
      if (worker_stop_) {
        return;
      }
      worker_job_ = false;
      command_buffer = execution_command_buffer_;
      position = execution_start_;
    }
    size_t chunk = size_t(position >> 32);
    size_t offset = size_t(uint32_t(position));
    for (;;) {
      // Read the wake counter before checking for work, so a wake after the
      // check isn't missed.
      uint32_t wake = wake_.load(std::memory_order_acquire);
      bool finishing = finishing_.load(std::memory_order_acquire);
      uint64_t written = written_.load(std::memory_order_acquire);
      size_t written_chunk = size_t(written >> 32);
      size_t written_offset = size_t(uint32_t(written));
      while (chunk < written_chunk || offset < written_offset) {
        size_t end = chunk < written_chunk ? chunk_ends_[chunk] : written_offset;
        const uintmax_t* data = chunks_[chunk].get();
        ExecuteRange(command_buffer, data + offset, data + end);
        if (chunk < written_chunk) {
          ++chunk;
          offset = 0;
        } else {
          offset = end;
        }
      }
      if (finishing) {
        // finishing_ is set after the last position is published.
        ReportCommandStats();
        finished_.store(true, std::memory_order_release);
        finished_.notify_one();
        break;
      }
      wake_.wait(wake, std::memory_order_acquire);
    }
  }
}

void DeferredCommandBuffer::ExecuteRange(VkCommandBuffer command_buffer, const uintmax_t* stream,
                                         const uintmax_t* stream_end) {
  const ui::vulkan::VulkanDevice::Functions& dfn =
      command_processor_.GetVulkanDevice()->functions();
  CommandStats& command_stats = GetCommandStats();
  while (stream < stream_end) {
    const CommandHeader& header = *reinterpret_cast<const CommandHeader*>(stream);
    stream += kCommandHeaderSizeElements;
    if (command_stats.enabled) {
      ++command_stats.counts[uint32_t(header.command) & 63];
    }

    switch (header.command) {
      case Command::kVkBeginRenderPass: {
        auto& args = *reinterpret_cast<const ArgsVkBeginRenderPass*>(stream);
        size_t offset_bytes = sizeof(ArgsVkBeginRenderPass);
        VkRenderPassBeginInfo render_pass_begin_info;
        render_pass_begin_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        render_pass_begin_info.pNext = nullptr;
        render_pass_begin_info.renderPass = args.render_pass;
        render_pass_begin_info.framebuffer = args.framebuffer;
        render_pass_begin_info.renderArea = args.render_area;
        render_pass_begin_info.clearValueCount = args.clear_value_count;
        if (render_pass_begin_info.clearValueCount) {
          offset_bytes = rex::align(offset_bytes, alignof(VkClearValue));
          render_pass_begin_info.pClearValues = reinterpret_cast<const VkClearValue*>(
              reinterpret_cast<const uint8_t*>(stream) + offset_bytes);
          offset_bytes += sizeof(VkClearValue) * render_pass_begin_info.clearValueCount;
        } else {
          render_pass_begin_info.pClearValues = nullptr;
        }
        dfn.vkCmdBeginRenderPass(command_buffer, &render_pass_begin_info, args.contents);
      } break;

      case Command::kVkBeginQuery: {
        auto& args = *reinterpret_cast<const ArgsVkBeginQuery*>(stream);
        dfn.vkCmdBeginQuery(command_buffer, args.query_pool, args.query, args.flags);
      } break;

      case Command::kVkBindDescriptorSets: {
        auto& args = *reinterpret_cast<const ArgsVkBindDescriptorSets*>(stream);
        size_t offset_bytes =
            rex::align(sizeof(ArgsVkBindDescriptorSets), alignof(VkDescriptorSet));
        const VkDescriptorSet* descriptor_sets = reinterpret_cast<const VkDescriptorSet*>(
            reinterpret_cast<const uint8_t*>(stream) + offset_bytes);
        offset_bytes += sizeof(VkDescriptorSet) * args.descriptor_set_count;
        const uint32_t* dynamic_offsets = nullptr;
        if (args.dynamic_offset_count) {
          offset_bytes = rex::align(offset_bytes, alignof(uint32_t));
          dynamic_offsets = reinterpret_cast<const uint32_t*>(
              reinterpret_cast<const uint8_t*>(stream) + offset_bytes);
          offset_bytes += sizeof(uint32_t) * args.dynamic_offset_count;
        }
        dfn.vkCmdBindDescriptorSets(command_buffer, args.pipeline_bind_point, args.layout,
                                    args.first_set, args.descriptor_set_count, descriptor_sets,
                                    args.dynamic_offset_count, dynamic_offsets);
      } break;

      case Command::kVkBindIndexBuffer: {
        auto& args = *reinterpret_cast<const ArgsVkBindIndexBuffer*>(stream);
        dfn.vkCmdBindIndexBuffer(command_buffer, args.buffer, args.offset, args.index_type);
      } break;

      case Command::kVkBindPipeline: {
        auto& args = *reinterpret_cast<const ArgsVkBindPipeline*>(stream);
        dfn.vkCmdBindPipeline(command_buffer, args.pipeline_bind_point, args.pipeline);
      } break;

      case Command::kVkBindVertexBuffers: {
        auto& args = *reinterpret_cast<const ArgsVkBindVertexBuffers*>(stream);
        size_t offset_bytes = rex::align(sizeof(ArgsVkBindVertexBuffers), alignof(VkBuffer));
        const VkBuffer* buffers = reinterpret_cast<const VkBuffer*>(
            reinterpret_cast<const uint8_t*>(stream) + offset_bytes);
        offset_bytes =
            rex::align(offset_bytes + sizeof(VkBuffer) * args.binding_count, alignof(VkDeviceSize));
        const VkDeviceSize* offsets = reinterpret_cast<const VkDeviceSize*>(
            reinterpret_cast<const uint8_t*>(stream) + offset_bytes);
        dfn.vkCmdBindVertexBuffers(command_buffer, args.first_binding, args.binding_count, buffers,
                                   offsets);
      } break;

      case Command::kVkClearAttachments: {
        auto& args = *reinterpret_cast<const ArgsVkClearAttachments*>(stream);
        size_t offset_bytes =
            rex::align(sizeof(ArgsVkClearAttachments), alignof(VkClearAttachment));
        const VkClearAttachment* attachments = reinterpret_cast<const VkClearAttachment*>(
            reinterpret_cast<const uint8_t*>(stream) + offset_bytes);
        offset_bytes = rex::align(offset_bytes + sizeof(VkClearAttachment) * args.attachment_count,
                                  alignof(VkClearRect));
        const VkClearRect* rects = reinterpret_cast<const VkClearRect*>(
            reinterpret_cast<const uint8_t*>(stream) + offset_bytes);
        dfn.vkCmdClearAttachments(command_buffer, args.attachment_count, attachments,
                                  args.rect_count, rects);
      } break;

      case Command::kVkClearColorImage: {
        auto& args = *reinterpret_cast<const ArgsVkClearColorImage*>(stream);
        dfn.vkCmdClearColorImage(
            command_buffer, args.image, args.image_layout, &args.color, args.range_count,
            reinterpret_cast<const VkImageSubresourceRange*>(
                reinterpret_cast<const uint8_t*>(stream) +
                rex::align(sizeof(ArgsVkClearColorImage), alignof(VkImageSubresourceRange))));
      } break;

      case Command::kVkFillBuffer: {  // HAND PATCH
        auto& args = *reinterpret_cast<const ArgsVkFillBuffer*>(stream);
        dfn.vkCmdFillBuffer(command_buffer, args.buffer, args.offset, args.size, args.data);
      } break;

      case Command::kVkCopyBuffer: {
        auto& args = *reinterpret_cast<const ArgsVkCopyBuffer*>(stream);
        dfn.vkCmdCopyBuffer(command_buffer, args.src_buffer, args.dst_buffer, args.region_count,
                            reinterpret_cast<const VkBufferCopy*>(
                                reinterpret_cast<const uint8_t*>(stream) +
                                rex::align(sizeof(ArgsVkCopyBuffer), alignof(VkBufferCopy))));
      } break;

      case Command::kVkCopyBufferToImage: {
        auto& args = *reinterpret_cast<const ArgsVkCopyBufferToImage*>(stream);
        dfn.vkCmdCopyBufferToImage(
            command_buffer, args.src_buffer, args.dst_image, args.dst_image_layout,
            args.region_count,
            reinterpret_cast<const VkBufferImageCopy*>(
                reinterpret_cast<const uint8_t*>(stream) +
                rex::align(sizeof(ArgsVkCopyBufferToImage), alignof(VkBufferImageCopy))));
      } break;

      case Command::kVkCopyImage: {
        auto& args = *reinterpret_cast<const ArgsVkCopyImage*>(stream);
        dfn.vkCmdCopyImage(command_buffer, args.src_image, args.src_image_layout, args.dst_image,
                           args.dst_image_layout, args.region_count,
                           reinterpret_cast<const VkImageCopy*>(
                               reinterpret_cast<const uint8_t*>(stream) +
                               rex::align(sizeof(ArgsVkCopyImage), alignof(VkImageCopy))));
      } break;

      case Command::kVkCopyImageToBuffer: {
        auto& args = *reinterpret_cast<const ArgsVkCopyImageToBuffer*>(stream);
        dfn.vkCmdCopyImageToBuffer(
            command_buffer, args.src_image, args.src_image_layout, args.dst_buffer,
            args.region_count,
            reinterpret_cast<const VkBufferImageCopy*>(
                reinterpret_cast<const uint8_t*>(stream) +
                rex::align(sizeof(ArgsVkCopyImageToBuffer), alignof(VkBufferImageCopy))));
      } break;

      case Command::kVkCopyQueryPoolResults: {
        auto& args = *reinterpret_cast<const ArgsVkCopyQueryPoolResults*>(stream);
        dfn.vkCmdCopyQueryPoolResults(command_buffer, args.query_pool, args.first_query,
                                      args.query_count, args.dst_buffer, args.dst_offset,
                                      args.stride, args.flags);
      } break;

      case Command::kVkDispatch: {
        auto& args = *reinterpret_cast<const ArgsVkDispatch*>(stream);
        dfn.vkCmdDispatch(command_buffer, args.group_count_x, args.group_count_y,
                          args.group_count_z);
      } break;

      case Command::kVkDraw: {
        auto& args = *reinterpret_cast<const ArgsVkDraw*>(stream);
        PROFILE_DRAW_CALL();
        PROFILE_VERTICES(int64_t(args.vertex_count) * args.instance_count);
        dfn.vkCmdDraw(command_buffer, args.vertex_count, args.instance_count, args.first_vertex,
                      args.first_instance);
      } break;

      case Command::kVkDrawIndexed: {
        auto& args = *reinterpret_cast<const ArgsVkDrawIndexed*>(stream);
        PROFILE_DRAW_CALL();
        PROFILE_VERTICES(int64_t(args.index_count) * args.instance_count);
        dfn.vkCmdDrawIndexed(command_buffer, args.index_count, args.instance_count,
                             args.first_index, args.vertex_offset, args.first_instance);
      } break;

      case Command::kVkEndQuery: {
        auto& args = *reinterpret_cast<const ArgsVkEndQuery*>(stream);
        dfn.vkCmdEndQuery(command_buffer, args.query_pool, args.query);
      } break;

      case Command::kVkEndRenderPass:
        dfn.vkCmdEndRenderPass(command_buffer);
        break;

      case Command::kVkBeginRendering: {
        auto& args = *reinterpret_cast<const ArgsVkBeginRendering*>(stream);
        size_t offset_bytes =
            rex::align(sizeof(ArgsVkBeginRendering), alignof(VkRenderingAttachmentInfo));
        const VkRenderingAttachmentInfo* color_attachments =
            args.color_attachment_count
                ? reinterpret_cast<const VkRenderingAttachmentInfo*>(
                      reinterpret_cast<const uint8_t*>(stream) + offset_bytes)
                : nullptr;
        offset_bytes += sizeof(VkRenderingAttachmentInfo) * args.color_attachment_count;
        const VkRenderingAttachmentInfo* depth_attachment =
            args.has_depth_attachment ? reinterpret_cast<const VkRenderingAttachmentInfo*>(
                                            reinterpret_cast<const uint8_t*>(stream) + offset_bytes)
                                      : nullptr;
        if (args.has_depth_attachment) {
          offset_bytes += sizeof(VkRenderingAttachmentInfo);
        }
        const VkRenderingAttachmentInfo* stencil_attachment =
            args.has_stencil_attachment
                ? reinterpret_cast<const VkRenderingAttachmentInfo*>(
                      reinterpret_cast<const uint8_t*>(stream) + offset_bytes)
                : nullptr;
        VkRenderingInfo rendering_info = {};
        rendering_info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        rendering_info.flags = args.flags;
        rendering_info.renderArea = args.render_area;
        rendering_info.layerCount = args.layer_count;
        rendering_info.viewMask = args.view_mask;
        rendering_info.colorAttachmentCount = args.color_attachment_count;
        rendering_info.pColorAttachments = color_attachments;
        rendering_info.pDepthAttachment = depth_attachment;
        rendering_info.pStencilAttachment = stencil_attachment;
        dfn.vkCmdBeginRendering(command_buffer, &rendering_info);
      } break;

      case Command::kVkEndRendering:
        dfn.vkCmdEndRendering(command_buffer);
        break;

      case Command::kVkPipelineBarrier: {
        auto& args = *reinterpret_cast<const ArgsVkPipelineBarrier*>(stream);
        size_t barrier_offset_bytes = sizeof(ArgsVkPipelineBarrier);
        const VkMemoryBarrier* memory_barriers = nullptr;
        if (args.memory_barrier_count) {
          barrier_offset_bytes = rex::align(barrier_offset_bytes, alignof(VkMemoryBarrier));
          memory_barriers = reinterpret_cast<const VkMemoryBarrier*>(
              reinterpret_cast<const uint8_t*>(stream) + barrier_offset_bytes);
          barrier_offset_bytes += sizeof(VkMemoryBarrier) * args.memory_barrier_count;
        }
        const VkBufferMemoryBarrier* buffer_memory_barriers = nullptr;
        if (args.buffer_memory_barrier_count) {
          barrier_offset_bytes = rex::align(barrier_offset_bytes, alignof(VkBufferMemoryBarrier));
          buffer_memory_barriers = reinterpret_cast<const VkBufferMemoryBarrier*>(
              reinterpret_cast<const uint8_t*>(stream) + barrier_offset_bytes);
          barrier_offset_bytes += sizeof(VkBufferMemoryBarrier) * args.buffer_memory_barrier_count;
        }
        const VkImageMemoryBarrier* image_memory_barriers = nullptr;
        if (args.image_memory_barrier_count) {
          barrier_offset_bytes = rex::align(barrier_offset_bytes, alignof(VkImageMemoryBarrier));
          image_memory_barriers = reinterpret_cast<const VkImageMemoryBarrier*>(
              reinterpret_cast<const uint8_t*>(stream) + barrier_offset_bytes);
          barrier_offset_bytes += sizeof(VkImageMemoryBarrier) * args.image_memory_barrier_count;
        }
        if (command_stats.enabled) {
          ++command_stats.barrier_stages[{uint32_t(args.src_stage_mask),
                                          uint32_t(args.dst_stage_mask)}];
          command_stats.buffer_barriers += args.buffer_memory_barrier_count;
          command_stats.image_barriers += args.image_memory_barrier_count;
        }
        dfn.vkCmdPipelineBarrier(command_buffer, args.src_stage_mask, args.dst_stage_mask,
                                 args.dependency_flags, args.memory_barrier_count, memory_barriers,
                                 args.buffer_memory_barrier_count, buffer_memory_barriers,
                                 args.image_memory_barrier_count, image_memory_barriers);
      } break;

      case Command::kVkPushConstants: {
        auto& args = *reinterpret_cast<const ArgsVkPushConstants*>(stream);
        dfn.vkCmdPushConstants(
            command_buffer, args.layout, args.stage_flags, args.offset, args.size,
            reinterpret_cast<const uint8_t*>(stream) + sizeof(ArgsVkPushConstants));
      } break;

      case Command::kVkWriteTimestamp: {
        auto& args = *reinterpret_cast<const ArgsVkWriteTimestamp*>(stream);
        if (cmd_write_timestamp_) {
          cmd_write_timestamp_(command_buffer, args.pipeline_stage, args.query_pool, args.query);
        }
      } break;

      case Command::kVkResetQueryPool: {
        auto& args = *reinterpret_cast<const ArgsVkResetQueryPool*>(stream);
        dfn.vkCmdResetQueryPool(command_buffer, args.query_pool, args.first_query,
                                args.query_count);
      } break;

      case Command::kVkSetBlendConstants: {
        auto& args = *reinterpret_cast<const ArgsVkSetBlendConstants*>(stream);
        dfn.vkCmdSetBlendConstants(command_buffer, args.blend_constants);
      } break;

      case Command::kVkSetDepthBias: {
        auto& args = *reinterpret_cast<const ArgsVkSetDepthBias*>(stream);
        dfn.vkCmdSetDepthBias(command_buffer, args.depth_bias_constant_factor,
                              args.depth_bias_clamp, args.depth_bias_slope_factor);
      } break;

      case Command::kVkSetScissor: {
        auto& args = *reinterpret_cast<const ArgsVkSetScissor*>(stream);
        dfn.vkCmdSetScissor(command_buffer, args.first_scissor, args.scissor_count,
                            reinterpret_cast<const VkRect2D*>(
                                reinterpret_cast<const uint8_t*>(stream) +
                                rex::align(sizeof(ArgsVkSetScissor), alignof(VkRect2D))));
      } break;

      case Command::kVkSetStencilCompareMask: {
        auto& args = *reinterpret_cast<const ArgsSetStencilMaskReference*>(stream);
        dfn.vkCmdSetStencilCompareMask(command_buffer, args.face_mask, args.mask_reference);
      } break;

      case Command::kVkSetStencilReference: {
        auto& args = *reinterpret_cast<const ArgsSetStencilMaskReference*>(stream);
        dfn.vkCmdSetStencilReference(command_buffer, args.face_mask, args.mask_reference);
      } break;

      case Command::kVkSetStencilWriteMask: {
        auto& args = *reinterpret_cast<const ArgsSetStencilMaskReference*>(stream);
        dfn.vkCmdSetStencilWriteMask(command_buffer, args.face_mask, args.mask_reference);
      } break;

      case Command::kVkSetViewport: {
        auto& args = *reinterpret_cast<const ArgsVkSetViewport*>(stream);
        dfn.vkCmdSetViewport(command_buffer, args.first_viewport, args.viewport_count,
                             reinterpret_cast<const VkViewport*>(
                                 reinterpret_cast<const uint8_t*>(stream) +
                                 rex::align(sizeof(ArgsVkSetViewport), alignof(VkViewport))));
      } break;

      default:
        assert_unhandled_case(header.command);
        break;
    }

    stream += header.arguments_size_elements;
  }
}

void DeferredCommandBuffer::ReportCommandStats() {
  CommandStats& command_stats = GetCommandStats();
  static const uint64_t stats_every = [] {
    const char* every = std::getenv("REX_CMD_STATS_EVERY");
    return every ? std::max(uint64_t(1), uint64_t(std::strtoull(every, nullptr, 10))) : uint64_t(20);
  }();
  if (command_stats.enabled && ++command_stats.executions % stats_every == 0) {
    std::string line;
    for (size_t i = 0; i < std::size(kCommandStatNames); ++i) {
      if (command_stats.counts[i]) {
        line += fmt::format(" {}={:.1f}", kCommandStatNames[i], command_stats.counts[i] / double(stats_every));
      }
    }
    REXGPU_INFO("[cmd-stats] per command buffer:{} | buffer barriers {:.1f}, image barriers {:.1f}",
                line, command_stats.buffer_barriers / double(stats_every), command_stats.image_barriers / double(stats_every));
    for (const auto& [stages, count] : command_stats.barrier_stages) {
      REXGPU_INFO("[cmd-stats]   barrier src={:08X} dst={:08X}: {:.1f}", stages.first,
                  stages.second, count / double(stats_every));
    }
    {
      std::string histogram;
      for (uint32_t i = 0; i < 32; ++i) {
        if (g_upload_request_stats[3 + i]) {
          histogram += fmt::format(" <{}B:{:.1f}", uint64_t(1) << i,
                                   g_upload_request_stats[3 + i] / double(stats_every));
        }
      }
      REXGPU_INFO("[cmd-stats] upload requests {:.1f}, requested {:.1f} KB, uploaded {:.1f} KB "
                  "(unchanged streamed pages skipped {:.1f}, requests skipped {:.1f}) |{}",
                  g_upload_request_stats[0] / double(stats_every), g_upload_request_stats[1] / (double(stats_every) * 1024.0),
                  g_upload_request_stats[2] / (double(stats_every) * 1024.0), g_streamed_skip_stats[0] / double(stats_every),
                  g_streamed_skip_stats[1] / double(stats_every), histogram);
      g_streamed_skip_stats[0] = 0;
      g_streamed_skip_stats[1] = 0;
      std::memset(g_upload_request_stats, 0, sizeof(g_upload_request_stats));
      if (!g_upload_request_ranges.empty()) {
        std::vector<std::pair<uint64_t, std::pair<uint32_t, uint32_t>>> ranked;
        for (const auto& [range, entry] : g_upload_request_ranges) {
          ranked.emplace_back(entry.second, range);
        }
        std::sort(ranked.begin(), ranked.end(), std::greater<>());
        for (size_t i = 0; i < std::min(ranked.size(), size_t(25)); ++i) {
          const auto& entry = g_upload_request_ranges[ranked[i].second];
          REXGPU_INFO("[cmd-stats]   range {:08X}+{:X}: {:.1f} requests, {:.1f} KB uploaded",
                      ranked[i].second.first, ranked[i].second.second, entry.first / double(stats_every),
                      entry.second / (double(stats_every) * 1024.0));
        }
        REXGPU_INFO("[cmd-stats]   {} distinct ranges", g_upload_request_ranges.size());
        g_upload_request_ranges.clear();
      }
    }
    std::memset(command_stats.counts, 0, sizeof(command_stats.counts));
    command_stats.buffer_barriers = 0;
    command_stats.image_barriers = 0;
    command_stats.barrier_stages.clear();
  }
}

void DeferredCommandBuffer::CmdVkPipelineBarrier(
    VkPipelineStageFlags src_stage_mask, VkPipelineStageFlags dst_stage_mask,
    VkDependencyFlags dependency_flags, uint32_t memory_barrier_count,
    const VkMemoryBarrier* memory_barriers, uint32_t buffer_memory_barrier_count,
    const VkBufferMemoryBarrier* buffer_memory_barriers, uint32_t image_memory_barrier_count,
    const VkImageMemoryBarrier* image_memory_barriers) {
  size_t arguments_size = sizeof(ArgsVkPipelineBarrier);
  size_t memory_barriers_offset = 0;
  if (memory_barrier_count) {
    arguments_size = rex::align(arguments_size, alignof(VkMemoryBarrier));
    memory_barriers_offset = arguments_size;
    arguments_size += sizeof(VkMemoryBarrier) * memory_barrier_count;
  }
  size_t buffer_memory_barriers_offset = 0;
  if (buffer_memory_barrier_count) {
    arguments_size = rex::align(arguments_size, alignof(VkBufferMemoryBarrier));
    buffer_memory_barriers_offset = arguments_size;
    arguments_size += sizeof(VkBufferMemoryBarrier) * buffer_memory_barrier_count;
  }
  size_t image_memory_barriers_offset = 0;
  if (image_memory_barrier_count) {
    arguments_size = rex::align(arguments_size, alignof(VkImageMemoryBarrier));
    image_memory_barriers_offset = arguments_size;
    arguments_size += sizeof(VkImageMemoryBarrier) * image_memory_barrier_count;
  }
  uint8_t* args_ptr =
      reinterpret_cast<uint8_t*>(WriteCommand(Command::kVkPipelineBarrier, arguments_size));
  auto& args = *reinterpret_cast<ArgsVkPipelineBarrier*>(args_ptr);
  args.src_stage_mask = src_stage_mask;
  args.dst_stage_mask = dst_stage_mask;
  args.dependency_flags = dependency_flags;
  args.memory_barrier_count = memory_barrier_count;
  args.buffer_memory_barrier_count = buffer_memory_barrier_count;
  args.image_memory_barrier_count = image_memory_barrier_count;
  if (memory_barrier_count) {
    std::memcpy(args_ptr + memory_barriers_offset, memory_barriers,
                sizeof(VkMemoryBarrier) * memory_barrier_count);
  }
  if (buffer_memory_barrier_count) {
    std::memcpy(args_ptr + buffer_memory_barriers_offset, buffer_memory_barriers,
                sizeof(VkBufferMemoryBarrier) * buffer_memory_barrier_count);
  }
  if (image_memory_barrier_count) {
    std::memcpy(args_ptr + image_memory_barriers_offset, image_memory_barriers,
                sizeof(VkImageMemoryBarrier) * image_memory_barrier_count);
  }
}

void DeferredCommandBuffer::CmdVkBeginRendering(const VkRenderingInfo* rendering_info) {
  assert_null(rendering_info->pNext);

  size_t arguments_size =
      rex::align(sizeof(ArgsVkBeginRendering), alignof(VkRenderingAttachmentInfo));
  size_t color_attachments_offset = arguments_size;
  arguments_size += sizeof(VkRenderingAttachmentInfo) * rendering_info->colorAttachmentCount;
  size_t depth_attachment_offset = arguments_size;
  if (rendering_info->pDepthAttachment) {
    arguments_size += sizeof(VkRenderingAttachmentInfo);
  }
  size_t stencil_attachment_offset = arguments_size;
  if (rendering_info->pStencilAttachment) {
    arguments_size += sizeof(VkRenderingAttachmentInfo);
  }

  uint8_t* args_ptr =
      reinterpret_cast<uint8_t*>(WriteCommand(Command::kVkBeginRendering, arguments_size));
  auto& args = *reinterpret_cast<ArgsVkBeginRendering*>(args_ptr);
  args.flags = rendering_info->flags;
  args.render_area = rendering_info->renderArea;
  args.layer_count = rendering_info->layerCount;
  args.view_mask = rendering_info->viewMask;
  args.color_attachment_count = rendering_info->colorAttachmentCount;
  args.has_depth_attachment = rendering_info->pDepthAttachment != nullptr;
  args.has_stencil_attachment = rendering_info->pStencilAttachment != nullptr;

  if (rendering_info->colorAttachmentCount) {
    std::memcpy(args_ptr + color_attachments_offset, rendering_info->pColorAttachments,
                sizeof(VkRenderingAttachmentInfo) * rendering_info->colorAttachmentCount);
  }
  if (rendering_info->pDepthAttachment) {
    std::memcpy(args_ptr + depth_attachment_offset, rendering_info->pDepthAttachment,
                sizeof(VkRenderingAttachmentInfo));
  }
  if (rendering_info->pStencilAttachment) {
    std::memcpy(args_ptr + stencil_attachment_offset, rendering_info->pStencilAttachment,
                sizeof(VkRenderingAttachmentInfo));
  }
}

void* DeferredCommandBuffer::WriteCommand(Command command, size_t arguments_size_bytes) {
  size_t arguments_size_elements =
      (arguments_size_bytes + sizeof(uintmax_t) - 1) / sizeof(uintmax_t);
  size_t size = kCommandHeaderSizeElements + arguments_size_elements;
  if (write_offset_ + size > chunk_capacities_[write_chunk_]) {
    chunk_ends_[write_chunk_] = write_offset_;
    EnsureChunk(write_chunk_ + 1, size);
    ++write_chunk_;
    write_offset_ = 0;
  }
  if (executing_) {
    // The caller has filled the previous command's arguments by now.
    PublishWritten();
    // Wake the worker in batches rather than for every command.
    if (++commands_since_wake_ >= 32) {
      commands_since_wake_ = 0;
      wake_.fetch_add(1, std::memory_order_release);
      wake_.notify_one();
    }
  }
  uintmax_t* data = chunks_[write_chunk_].get() + write_offset_;
  // Arguments start zeroed, as callers may leave padding unwritten.
  std::memset(data, 0, size * sizeof(uintmax_t));
  CommandHeader& header = *reinterpret_cast<CommandHeader*>(data);
  header.command = command;
  header.arguments_size_elements = uint32_t(arguments_size_elements);
  write_offset_ += size;
  return data + kCommandHeaderSizeElements;
}

}  // namespace rex::graphics::vulkan
