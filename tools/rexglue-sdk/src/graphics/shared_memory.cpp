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
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <map>
#include <utility>

#include <rex/assert.h>
#include <rex/bit.h>
#include <rex/cvar.h>
#include <rex/dbg.h>
#include <rex/graphics/shared_memory.h>
#include <rex/graphics/util/bytes_equal.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/memory.h>
#include <rex/perf/counter.h>

REXCVAR_DEFINE_BOOL(gpu_stream_dynamic_pages, true, "GPU",
                    "Upload vertex and index data in pages the game rewrites every frame on "
                    "every use instead of write-protecting them after each upload (saves a "
                    "protection change and a write fault per page per frame)");

REXCVAR_DEFINE_BOOL(gpu_stream_skip_unchanged, true, "GPU",
                    "With gpu_stream_dynamic_pages, skip uploading a streamed page for a draw "
                    "when the bytes the draw reads from it are the same as when it was last "
                    "uploaded (every upload waits for the GPU to finish all earlier work)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(gpu_stream_after_one_fault, false, "GPU",
                    "With gpu_stream_dynamic_pages, stream a page after one guest write fault "
                    "instead of faults in two frames in a row - level streaming rewrites pages "
                    "once or a few times, and each time costs a fault and a protection change")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace rex::graphics {

namespace {
// Acquires the global critical region, adding the time spent waiting for
// another thread to release it to upload_lock_wait_us.
std::unique_lock<std::recursive_mutex> AcquireGlobalLockTimed(
    rex::thread::global_critical_region& region) {
  std::unique_lock<std::recursive_mutex> lock = region.TryAcquire();
  if (!lock.owns_lock()) {
    rex::perf::ScopedCounterTimer wait_timer(rex::perf::CounterId::kUploadLockWaitUs);
    lock.lock();
  }
  return lock;
}
}  // namespace

// Pages uploaded without being made valid (streamed), for the gpu_wait_stats
// log. Also, summed over frames: the most uploads of one streamed page in a
// frame, and how many pages were uploaded more than 8, 16, 32 and 63 times in
// a frame.
std::atomic<uint64_t> g_streamed_page_uploads{0};
// Watch arming (EnablePhysicalMemoryAccessCallbacks calls) by the kind of
// request: vertex / index data (allow_streamed) or anything else (textures,
// resolves), for the gpu_wait_stats log.
std::atomic<uint64_t> g_watch_arms_vertex{0};
std::atomic<uint64_t> g_watch_arms_other{0};
std::atomic<uint64_t> g_streamed_page_max_uploads{0};
std::atomic<uint64_t> g_streamed_pages_over[4] = {};
// Debugging (REX_CMD_STATS): requests that needed an upload, their requested
// and uploaded bytes, and a log2 histogram of the requested sizes.
uint64_t g_upload_request_stats[3 + 32] = {};
// Debugging (REX_CMD_STATS): streamed pages whose upload was skipped as
// unchanged, and requests that needed no upload because of that.
uint64_t g_streamed_skip_stats[2] = {};
// REX_CMD_STATS=2: requests that needed an upload by (start, length): count and uploaded bytes.
std::map<std::pair<uint32_t, uint32_t>, std::pair<uint64_t, uint64_t>> g_upload_request_ranges;

SharedMemory::SharedMemory(memory::Memory& memory) : memory_(memory) {
  page_size_log2_ = rex::log2_ceil(uint32_t(rex::memory::page_size()));
}

SharedMemory::~SharedMemory() {
  ShutdownCommon();
}

void SharedMemory::InitializeCommon() {
  num_system_page_flags_ = ((kBufferSize >> page_size_log2_) + 63) / 64;
  system_page_flags_valid_.assign(num_system_page_flags_, 0);
  system_page_flags_valid_and_gpu_written_.assign(num_system_page_flags_, 0);
  valid_flags_.store(system_page_flags_valid_.data(), std::memory_order_release);
  system_page_flags_streamed_.assign(num_system_page_flags_, 0);
  system_page_flags_streamed_blocked_.assign(num_system_page_flags_, 0);
  system_page_flags_faulted_.assign(num_system_page_flags_, 0);
  system_page_flags_faulted_previous_.assign(num_system_page_flags_, 0);
  streamed_page_uploads_this_frame_.assign(size_t(num_system_page_flags_) * 64, 0);

  memory_invalidation_callback_handle_ =
      memory_.RegisterPhysicalMemoryInvalidationCallback(MemoryInvalidationCallbackThunk, this);
}

void SharedMemory::InitializeSparseHostGpuMemory(uint32_t granularity_log2) {
  assert_true(granularity_log2 <= kBufferSizeLog2);
  assert_true(host_gpu_memory_sparse_granularity_log2_ == UINT32_MAX);
  host_gpu_memory_sparse_granularity_log2_ = granularity_log2;
  host_gpu_memory_sparse_allocated_.resize(
      size_t(1) << (std::max(kBufferSizeLog2 - granularity_log2, uint32_t(6)) - 6));
}

void SharedMemory::ShutdownCommon() {
  ReleaseTraceDownloadRanges();
  streamed_page_shadows_.clear();

  FireWatches(0, (kBufferSize - 1) >> page_size_log2_, false);
  assert_true(global_watches_.empty());
  // No watches now, so no references to the pools accessible by guest threads -
  // safe not to enter the global critical region.
  watch_node_first_free_ = nullptr;
  watch_node_current_pool_allocated_ = 0;
  for (WatchNode* pool : watch_node_pools_) {
    delete[] pool;
  }
  watch_node_pools_.clear();
  watch_range_first_free_ = nullptr;
  watch_range_current_pool_allocated_ = 0;
  for (WatchRange* pool : watch_range_pools_) {
    delete[] pool;
  }
  watch_range_pools_.clear();

  if (memory_invalidation_callback_handle_ != nullptr) {
    memory_.UnregisterPhysicalMemoryInvalidationCallback(memory_invalidation_callback_handle_);
    memory_invalidation_callback_handle_ = nullptr;
  }

  if (host_gpu_memory_sparse_used_bytes_) {
    host_gpu_memory_sparse_used_bytes_ = 0;
    COUNT_profile_set("gpu/shared_memory/host_gpu_memory_sparse_used_mb", 0);
  }
  if (host_gpu_memory_sparse_allocations_) {
    host_gpu_memory_sparse_allocations_ = 0;
    COUNT_profile_set("gpu/shared_memory/host_gpu_memory_sparse_allocations", 0);
  }
  host_gpu_memory_sparse_allocated_.clear();
  host_gpu_memory_sparse_allocated_.shrink_to_fit();
  host_gpu_memory_sparse_granularity_log2_ = UINT32_MAX;

  valid_flags_.store(nullptr, std::memory_order_relaxed);
  system_page_flags_valid_.clear();
  system_page_flags_valid_.shrink_to_fit();
  system_page_flags_valid_and_gpu_written_.clear();
  system_page_flags_valid_and_gpu_written_.shrink_to_fit();
  system_page_flags_streamed_.clear();
  system_page_flags_streamed_.shrink_to_fit();
  system_page_flags_streamed_blocked_.clear();
  system_page_flags_streamed_blocked_.shrink_to_fit();
  streamed_page_uploads_this_frame_.clear();
  streamed_page_uploads_this_frame_.shrink_to_fit();
  system_page_flags_faulted_.clear();
  system_page_flags_faulted_.shrink_to_fit();
  system_page_flags_faulted_previous_.clear();
  system_page_flags_faulted_previous_.shrink_to_fit();
  streamed_pages_active_ = false;
  num_system_page_flags_ = 0;
}

void SharedMemory::OnGuestFrameEnd() {
  bool enabled = REXCVAR_GET(gpu_stream_dynamic_pages);
  if (!enabled && !streamed_pages_active_) {
    return;
  }
  auto global_lock = global_critical_region_.Acquire();
  if (!num_system_page_flags_) {
    return;
  }
  if (!enabled) {
    // Switched off: streamed pages are watched normally from their next
    // upload.
    std::fill(system_page_flags_streamed_.begin(), system_page_flags_streamed_.end(), 0);
    std::fill(system_page_flags_streamed_blocked_.begin(),
              system_page_flags_streamed_blocked_.end(), 0);
    std::fill(system_page_flags_faulted_.begin(), system_page_flags_faulted_.end(), 0);
    std::fill(system_page_flags_faulted_previous_.begin(),
              system_page_flags_faulted_previous_.end(), 0);
    streamed_pages_active_ = false;
    streamed_page_shadows_.clear();
    return;
  }
  streamed_pages_active_ = true;
  // Streamed pages aren't protected, so they stop faulting; forget them now and
  // then so pages the game stopped rewriting go back to being watched.
  bool reset = ++streamed_pages_frames_ >= kStreamedPagesResetFrames;
  if (reset) {
    streamed_pages_frames_ = 0;
    streamed_page_shadows_.clear();
  }
  bool after_one_fault = REXCVAR_GET(gpu_stream_after_one_fault);
  for (uint32_t i = 0; i < num_system_page_flags_; ++i) {
    uint64_t faulted = system_page_flags_faulted_[i];
    if (reset) {
      system_page_flags_streamed_[i] = 0;
      system_page_flags_streamed_blocked_[i] = 0;
    } else {
      system_page_flags_streamed_[i] |=
          faulted & (after_one_fault ? UINT64_MAX : system_page_flags_faulted_previous_[i]) &
          ~system_page_flags_streamed_blocked_[i];
    }
    system_page_flags_faulted_previous_[i] = faulted;
    system_page_flags_faulted_[i] = 0;
  }
  uint32_t max_uploads = 0;
  uint32_t over[4] = {};
  for (uint8_t uploads : streamed_page_uploads_this_frame_) {
    if (uploads > 8) {
      max_uploads = std::max(max_uploads, uint32_t(uploads));
      ++over[0];
      over[1] += uploads > 16;
      over[2] += uploads > 32;
      over[3] += uploads >= kStreamedPageMaxUploadsPerFrame;
    }
  }
  g_streamed_page_max_uploads.fetch_add(max_uploads, std::memory_order_relaxed);
  for (uint32_t i = 0; i < 4; ++i) {
    g_streamed_pages_over[i].fetch_add(over[i], std::memory_order_relaxed);
  }
  std::fill(streamed_page_uploads_this_frame_.begin(), streamed_page_uploads_this_frame_.end(),
            uint8_t(0));
}

void SharedMemory::MarkRangeFaultedForStreaming(uint32_t start, uint32_t length) {
  if (!length || start >= kBufferSize) {
    return;
  }
  length = std::min(length, kBufferSize - start);
  auto global_lock = global_critical_region_.Acquire();
  if (!streamed_pages_active_) {
    return;
  }
  uint32_t page_last = (start + length - 1) >> page_size_log2_;
  for (uint32_t page = start >> page_size_log2_; page <= page_last; ++page) {
    system_page_flags_faulted_[page >> 6] |= uint64_t(1) << (page & 63);
  }
}

void SharedMemory::InvalidateAllPages() {
  auto global_lock = global_critical_region_.Acquire();

  if (num_system_page_flags_) {
    std::memset(system_page_flags_valid_.data(), 0,
                num_system_page_flags_ * sizeof(uint64_t));
    std::memset(system_page_flags_valid_and_gpu_written_.data(), 0,
                num_system_page_flags_ * sizeof(uint64_t));
  }
}

void SharedMemory::SetSystemPageBlocksValidWithGpuDataWritten() {
  // Drop the validity of everything the CPU uploaded, keeping only pages whose
  // contents the GPU itself produced (those can't be re-uploaded from guest
  // memory, which is not in sync with them). Runs once per frame on the
  // command processor thread, under the same lock guest-thread invalidations
  // take -- the flags must never be mutated outside it, or an invalidation
  // racing this can be lost and leave a rewritten page marked valid, which
  // makes RequestRanges skip its upload and the GPU read stale vertex data.
  auto global_lock = global_critical_region_.Acquire();

  for (uint32_t i = 0; i < num_system_page_flags_; ++i) {
    system_page_flags_valid_[i] = system_page_flags_valid_and_gpu_written_[i];
  }
}

void SharedMemory::ClearCache() {
  streamed_page_shadows_.clear();
  // Keeping GPU-written data, so "invalidated by GPU".
  FireWatches(0, (kBufferSize - 1) >> page_size_log2_, true);
  // No watches now, so no references to the pools accessible by guest threads -
  // safe not to enter the global critical region.
  watch_node_first_free_ = nullptr;
  watch_node_current_pool_allocated_ = 0;
  for (WatchNode* pool : watch_node_pools_) {
    delete[] pool;
  }
  watch_node_pools_.clear();
  watch_range_first_free_ = nullptr;
  watch_range_current_pool_allocated_ = 0;
  for (WatchRange* pool : watch_range_pools_) {
    delete[] pool;
  }
  watch_range_pools_.clear();
  SetSystemPageBlocksValidWithGpuDataWritten();
}

SharedMemory::GlobalWatchHandle SharedMemory::RegisterGlobalWatch(GlobalWatchCallback callback,
                                                                  void* callback_context) {
  GlobalWatch* watch = new GlobalWatch;
  watch->callback = callback;
  watch->callback_context = callback_context;

  auto global_lock = global_critical_region_.Acquire();
  global_watches_.push_back(watch);

  return reinterpret_cast<GlobalWatchHandle>(watch);
}

void SharedMemory::UnregisterGlobalWatch(GlobalWatchHandle handle) {
  auto watch = reinterpret_cast<GlobalWatch*>(handle);

  {
    auto global_lock = global_critical_region_.Acquire();
    auto it = std::find(global_watches_.begin(), global_watches_.end(), watch);
    assert_false(it == global_watches_.end());
    if (it != global_watches_.end()) {
      global_watches_.erase(it);
    }
  }

  delete watch;
}

SharedMemory::WatchHandle SharedMemory::WatchMemoryRange(uint32_t start, uint32_t length,
                                                         WatchCallback callback,
                                                         void* callback_context,
                                                         void* callback_data,
                                                         uint64_t callback_argument) {
  if (length == 0 || start >= kBufferSize) {
    return nullptr;
  }
  length = std::min(length, kBufferSize - start);
  uint32_t watch_page_first = start >> page_size_log2_;
  uint32_t watch_page_last = (start + length - 1) >> page_size_log2_;
  uint32_t bucket_first = watch_page_first << page_size_log2_ >> kWatchBucketSizeLog2;
  uint32_t bucket_last = watch_page_last << page_size_log2_ >> kWatchBucketSizeLog2;

  auto global_lock = global_critical_region_.Acquire();

  // Allocate the range.
  WatchRange* range = watch_range_first_free_;
  if (range != nullptr) {
    watch_range_first_free_ = range->next_free;
  } else {
    if (watch_range_pools_.empty() || watch_range_current_pool_allocated_ >= kWatchRangePoolSize) {
      watch_range_pools_.push_back(new WatchRange[kWatchRangePoolSize]);
      watch_range_current_pool_allocated_ = 0;
    }
    range = &(watch_range_pools_.back()[watch_range_current_pool_allocated_++]);
  }
  range->callback = callback;
  range->callback_context = callback_context;
  range->callback_data = callback_data;
  range->callback_argument = callback_argument;
  range->page_first = watch_page_first;
  range->page_last = watch_page_last;

  // Allocate and link the nodes.
  WatchNode* node_previous = nullptr;
  for (uint32_t i = bucket_first; i <= bucket_last; ++i) {
    WatchNode* node = watch_node_first_free_;
    if (node != nullptr) {
      watch_node_first_free_ = node->next_free;
    } else {
      if (watch_node_pools_.empty() || watch_node_current_pool_allocated_ >= kWatchNodePoolSize) {
        watch_node_pools_.push_back(new WatchNode[kWatchNodePoolSize]);
        watch_node_current_pool_allocated_ = 0;
      }
      node = &(watch_node_pools_.back()[watch_node_current_pool_allocated_++]);
    }
    node->range = range;
    node->range_node_next = nullptr;
    if (node_previous != nullptr) {
      node_previous->range_node_next = node;
    } else {
      range->node_first = node;
    }
    node_previous = node;
    node->bucket_node_previous = nullptr;
    node->bucket_node_next = watch_buckets_[i];
    if (watch_buckets_[i] != nullptr) {
      watch_buckets_[i]->bucket_node_previous = node;
    }
    watch_buckets_[i] = node;
  }

  return reinterpret_cast<WatchHandle>(range);
}

void SharedMemory::UnwatchMemoryRange(WatchHandle handle) {
  auto global_lock = global_critical_region_.Acquire();
  UnlinkWatchRange(reinterpret_cast<WatchRange*>(handle));
}

void SharedMemory::FireWatches(uint32_t page_first, uint32_t page_last, bool invalidated_by_gpu) {
  auto global_lock = global_critical_region_.Acquire();
  FireWatchesLocked(global_lock, page_first, page_last, invalidated_by_gpu);
}

void SharedMemory::FireWatchesLocked(const std::unique_lock<std::recursive_mutex>& global_lock,
                                     uint32_t page_first, uint32_t page_last,
                                     bool invalidated_by_gpu) {
  uint32_t address_first = page_first << page_size_log2_;
  uint32_t address_last = (page_last << page_size_log2_) + ((1 << page_size_log2_) - 1);
  uint32_t bucket_first = address_first >> kWatchBucketSizeLog2;
  uint32_t bucket_last = address_last >> kWatchBucketSizeLog2;

  // Fire global watches.
  for (const auto global_watch : global_watches_) {
    global_watch->callback(global_lock, global_watch->callback_context, address_first, address_last,
                           invalidated_by_gpu);
  }

  // Fire per-range watches.
  for (uint32_t i = bucket_first; i <= bucket_last; ++i) {
    WatchNode* node = watch_buckets_[i];
    while (node != nullptr) {
      WatchRange* range = node->range;
      // Store the next node now since when the callback is triggered, the links
      // will be broken.
      node = node->bucket_node_next;
      if (page_first <= range->page_last && page_last >= range->page_first) {
        range->callback(global_lock, range->callback_context, range->callback_data,
                        range->callback_argument, invalidated_by_gpu);
        UnlinkWatchRange(range);
      }
    }
  }
}

bool SharedMemory::WatchCpuMemoryRange(uint32_t start, uint32_t length) {
  if (!length || start >= kBufferSize || length > kBufferSize - start ||
      !memory_invalidation_callback_handle_) {
    return false;
  }
  memory().EnablePhysicalMemoryAccessCallbacks(start, length, true, false);
  return true;
}

void SharedMemory::InvalidateCpuMemoryRangeForTrace(uint32_t start, uint32_t length) {
  if (length && !IsRangeGpuWritten(start, length)) {
    FireWatches(start >> page_size_log2_, (start + length - 1) >> page_size_log2_, false);
  }
}

bool SharedMemory::IsRangeGpuWritten(uint32_t start, uint32_t length) {
  if (!length) {
    return false;
  }
  if (start >= kBufferSize || length > kBufferSize - start) {
    return true;
  }
  auto global_lock = global_critical_region_.Acquire();
  uint32_t page_first = start >> page_size_log2_;
  uint32_t page_last = (start + length - 1) >> page_size_log2_;
  for (uint32_t block = page_first >> 6; block <= (page_last >> 6); ++block) {
    uint64_t mask = UINT64_MAX;
    if (block == (page_first >> 6)) {
      mask &= UINT64_MAX << (page_first & 63);
    }
    if (block == (page_last >> 6)) {
      mask &= UINT64_MAX >> (63 - (page_last & 63));
    }
    if (system_page_flags_valid_and_gpu_written_[block] & mask) {
      return true;
    }
  }
  return false;
}

void SharedMemory::RangeWrittenByGpu(uint32_t start, uint32_t length) {
  if (length == 0 || start >= kBufferSize) {
    return;
  }
  length = std::min(length, kBufferSize - start);
  uint32_t end = start + length - 1;
  if (!FlushGpuWrittenRange(start, length, true)) {
    REXGPU_ERROR("Shared memory: failed to preserve native GPU data before a write");
    return;
  }
  uint32_t page_first = start >> page_size_log2_;
  uint32_t page_last = end >> page_size_log2_;

  // Trigger modification callbacks so, for instance, resolved data is loaded to
  // the texture.
  FireWatches(page_first, page_last, true);

  // Mark the range as valid (so pages are not reuploaded until modified by the
  // CPU) and watch it so the CPU can reuse it and this will be caught.
  MakeRangeValid(start, length, true);
}

bool SharedMemory::AllocateSparseHostGpuMemoryRange(uint32_t offset_allocations,
                                                    uint32_t length_allocations) {
  assert_always(
      "Sparse host GPU memory allocation has been initialized, but the "
      "implementation doesn't provide AllocateSparseHostGpuMemoryRange");
  return false;
}

void SharedMemory::MakeRangeValid(uint32_t start, uint32_t length, bool written_by_gpu) {
  if (length == 0 || start >= kBufferSize) {
    return;
  }
  length = std::min(length, kBufferSize - start);
  uint32_t last = start + length - 1;
  uint32_t valid_page_first = start >> page_size_log2_;
  uint32_t valid_page_last = last >> page_size_log2_;
  uint32_t valid_block_first = valid_page_first >> 6;
  uint32_t valid_block_last = valid_page_last >> 6;

  // The GPU copy of the pages no longer has what streamed pages were uploaded
  // with.
  if (written_by_gpu && !streamed_page_shadows_.empty()) {
    EraseStreamedPageShadows(valid_page_first, valid_page_last);
  }

  // In an upload for an allow_streamed request, streamed pages are neither
  // made valid nor protected, so their next request uploads them again. Pages
  // are only ever made valid together with being protected.
  const bool skip_streamed = upload_allow_streamed_ && !written_by_gpu;
  bool any_streamed = false;
  {
    auto global_lock = AcquireGlobalLockTimed(global_critical_region_);

    for (uint32_t i = valid_block_first; i <= valid_block_last; ++i) {
      uint64_t range_bits = UINT64_MAX;
      if (i == valid_block_first) {
        range_bits &= ~((uint64_t(1) << (valid_page_first & 63)) - 1);
      }
      if (i == valid_block_last && (valid_page_last & 63) != 63) {
        range_bits &= (uint64_t(1) << ((valid_page_last & 63) + 1)) - 1;
      }
      uint64_t valid_bits = range_bits;
      if (skip_streamed) {
        uint64_t streamed_bits = range_bits & system_page_flags_streamed_[i];
        // Stop streaming pages uploaded too many times this frame - they're
        // made valid and protected below like any other page.
        uint64_t streamed_bits_remaining = streamed_bits;
        uint32_t bit;
        while (rex::bit_scan_forward(streamed_bits_remaining, &bit)) {
          streamed_bits_remaining &= ~(uint64_t(1) << bit);
          uint8_t& uploads = streamed_page_uploads_this_frame_[(i << 6) + bit];
          if (uploads >= kStreamedPageMaxUploadsPerFrame) {
            streamed_bits &= ~(uint64_t(1) << bit);
            system_page_flags_streamed_[i] &= ~(uint64_t(1) << bit);
            system_page_flags_streamed_blocked_[i] |= uint64_t(1) << bit;
          } else {
            ++uploads;
          }
        }
        if (streamed_bits) {
          any_streamed = true;
          valid_bits &= ~streamed_bits;
          g_streamed_page_uploads.fetch_add(rex::bit_count(streamed_bits),
                                            std::memory_order_relaxed);
        }
      }
      system_page_flags_valid_[i] |= valid_bits;
      uint64_t& gpu_written = system_page_flags_valid_and_gpu_written_[i];
      gpu_written = written_by_gpu ? (gpu_written | valid_bits) : (gpu_written & ~range_bits);
    }
  }

  if (memory_invalidation_callback_handle_) {
    if (!any_streamed) {
      (upload_allow_streamed_ ? g_watch_arms_vertex : g_watch_arms_other)
          .fetch_add(1, std::memory_order_relaxed);
      memory().EnablePhysicalMemoryAccessCallbacks(
          valid_page_first << page_size_log2_,
          (valid_page_last - valid_page_first + 1) << page_size_log2_, true, false);
    } else {
      // Protect only the runs of pages made valid above. The streamed set is
      // only changed on this thread (OnGuestFrameEnd), so it's still the same.
      uint32_t run_first = UINT32_MAX;
      for (uint32_t page = valid_page_first; page <= valid_page_last + 1; ++page) {
        bool protect_page =
            page <= valid_page_last &&
            !((system_page_flags_streamed_[page >> 6] >> (page & 63)) & 1);
        if (protect_page) {
          if (run_first == UINT32_MAX) {
            run_first = page;
          }
        } else if (run_first != UINT32_MAX) {
          g_watch_arms_vertex.fetch_add(1, std::memory_order_relaxed);
          memory().EnablePhysicalMemoryAccessCallbacks(
              run_first << page_size_log2_, (page - run_first) << page_size_log2_, true, false);
          run_first = UINT32_MAX;
        }
      }
    }
  }
}

void SharedMemory::UnlinkWatchRange(WatchRange* range) {
  uint32_t bucket = range->page_first << page_size_log2_ >> kWatchBucketSizeLog2;
  WatchNode* node = range->node_first;
  while (node != nullptr) {
    WatchNode* node_next = node->range_node_next;
    if (node->bucket_node_previous != nullptr) {
      node->bucket_node_previous->bucket_node_next = node->bucket_node_next;
    } else {
      watch_buckets_[bucket] = node->bucket_node_next;
    }
    if (node->bucket_node_next != nullptr) {
      node->bucket_node_next->bucket_node_previous = node->bucket_node_previous;
    }
    node->next_free = watch_node_first_free_;
    watch_node_first_free_ = node;
    node = node_next;
    ++bucket;
  }
  range->next_free = watch_range_first_free_;
  watch_range_first_free_ = range;
}

bool SharedMemory::RequestRanges(const std::pair<uint32_t, uint32_t>* ranges, size_t count,
                                 bool allow_streamed) {
  rex::perf::ScopedCounterTimer upload_timer(rex::perf::CounterId::kCpUploadUs);
  if (ranges == nullptr || !count) {
    return true;
  }

  // A single range needs no merging, so it skips the heap-allocated, sorted
  // vector below and goes straight to the validity check on a stack slot.
  // This is the overwhelmingly common case -- every vertex buffer of every
  // draw arrives here -- and it matters now that the per-draw residency cache
  // is gone: that cache used to hide these calls, at the cost of feeding the
  // GPU stale vertex data.
  if (count == 1) {
    if (!ranges[0].second) {
      return true;
    }
    if (ranges[0].first > kBufferSize || (kBufferSize - ranges[0].first) < ranges[0].second) {
      return false;
    }
    if (!FlushGpuWrittenRange(ranges[0].first, ranges[0].second)) {
      return false;
    }
    SCOPE_profile_cpu_f("gpu");
    if (!EnsureHostGpuMemoryAllocated(ranges[0].first, ranges[0].second)) {
      return false;
    }
    return RequestValidatedRanges(ranges, 1, count, allow_streamed);
  }

  // Some texture or buffer is empty, for example - safe to draw in this case.
  std::vector<std::pair<uint32_t, uint32_t>> merged_ranges;
  merged_ranges.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    uint32_t start = ranges[i].first;
    uint32_t length = ranges[i].second;
    if (!length) {
      continue;
    }
    if (start > kBufferSize || (kBufferSize - start) < length) {
      return false;
    }
    merged_ranges.emplace_back(start, length);
  }
  if (merged_ranges.empty()) {
    return true;
  }

  SCOPE_profile_cpu_f("gpu");

  std::sort(merged_ranges.begin(), merged_ranges.end(),
            [](const std::pair<uint32_t, uint32_t>& a, const std::pair<uint32_t, uint32_t>& b) {
              return a.first < b.first;
            });
  size_t merged_write = 0;
  for (size_t i = 1; i < merged_ranges.size(); ++i) {
    std::pair<uint32_t, uint32_t>& range_previous = merged_ranges[merged_write];
    const std::pair<uint32_t, uint32_t>& range_current = merged_ranges[i];
    uint64_t previous_end = uint64_t(range_previous.first) + uint64_t(range_previous.second);
    uint64_t current_start = uint64_t(range_current.first);
    if (current_start <= previous_end) {
      uint64_t current_end = current_start + uint64_t(range_current.second);
      if (current_end > previous_end) {
        range_previous.second = uint32_t(current_end - uint64_t(range_previous.first));
      }
    } else {
      merged_ranges[++merged_write] = range_current;
    }
  }
  merged_ranges.resize(merged_write + 1);

  for (const std::pair<uint32_t, uint32_t>& range : merged_ranges) {
    if (!FlushGpuWrittenRange(range.first, range.second)) {
      return false;
    }
    if (!EnsureHostGpuMemoryAllocated(range.first, range.second)) {
      return false;
    }
  }

  return RequestValidatedRanges(merged_ranges.data(), merged_ranges.size(), count,
                                allow_streamed);
}

// Shared tail of RequestRanges: the input ranges are already validated,
// merged and backed by host GPU memory. Split out so a single-range request
// can reach it without building and sorting a vector first.
bool SharedMemory::RequestValidatedRanges(const std::pair<uint32_t, uint32_t>* merged_ranges,
                                          size_t merged_count, size_t original_count,
                                          bool allow_streamed) {
  // Fast path: if everything requested is already valid, nothing has to be
  // uploaded and the global lock can be skipped entirely. Reading the flags
  // without the lock is safe because the storage never moves and every writer
  // holds the lock -- the worst case is racing an invalidation that is being
  // applied right now, which the locked re-check below (taken whenever any
  // page reads invalid) resolves.
  uint64_t* valid_flags = valid_flags_.load(std::memory_order_acquire);
  if (valid_flags) {
    bool all_valid = true;
    for (size_t range_index = 0; range_index < merged_count; ++range_index) {
      const std::pair<uint32_t, uint32_t>& range = merged_ranges[range_index];
      if (!range.second) {
        continue;
      }
      uint32_t page_first = range.first >> page_size_log2_;
      uint32_t page_last = (range.first + range.second - 1) >> page_size_log2_;
      uint32_t block_first = page_first >> 6;
      uint32_t block_last = page_last >> 6;
      for (uint32_t i = block_first; i <= block_last; ++i) {
        uint64_t block_valid = valid_flags[i];
        if (i == block_first) {
          uint64_t block_before = (uint64_t(1) << (page_first & 63)) - 1;
          block_valid |= block_before;
        }
        if (i == block_last && (page_last & 63) != 63) {
          uint64_t block_inside = (uint64_t(1) << ((page_last & 63) + 1)) - 1;
          block_valid |= ~block_inside;
        }
        if (block_valid != UINT64_MAX) {
          all_valid = false;
          break;
        }
      }
      if (!all_valid) {
        break;
      }
    }
    if (all_valid) {
      COUNT_profile_set("gpu/shared_memory/request_ranges_count", uint32_t(original_count));
      COUNT_profile_set("gpu/shared_memory/request_ranges_merged_count",
                        uint32_t(merged_count));
      COUNT_profile_set("gpu/shared_memory/request_ranges_upload_count", 0);
      return true;
    }
  }

  rex::perf::ScopedCounterTimer scan_timer(rex::perf::CounterId::kUploadScanUs);
  upload_ranges_.clear();
  auto append_upload_range = [this](uint32_t page_start, uint32_t page_count) {
    if (!page_count) {
      return;
    }
    if (!upload_ranges_.empty()) {
      std::pair<uint32_t, uint32_t>& last_upload_range = upload_ranges_.back();
      if (last_upload_range.first + last_upload_range.second == page_start) {
        last_upload_range.second += page_count;
        return;
      }
    }
    upload_ranges_.emplace_back(page_start, page_count);
  };
  {
    auto global_lock = AcquireGlobalLockTimed(global_critical_region_);
    for (size_t range_index = 0; range_index < merged_count; ++range_index) {
      const std::pair<uint32_t, uint32_t>& range = merged_ranges[range_index];
      uint32_t page_first = range.first >> page_size_log2_;
      uint32_t page_last = (range.first + range.second - 1) >> page_size_log2_;
      uint32_t block_first = page_first >> 6;
      uint32_t block_last = page_last >> 6;
      uint32_t range_start = UINT32_MAX;
      for (uint32_t i = block_first; i <= block_last; ++i) {
        uint64_t block_valid = system_page_flags_valid_[i];
        // Consider pages in the block outside the requested range valid.
        if (i == block_first) {
          uint64_t block_before = (uint64_t(1) << (page_first & 63)) - 1;
          block_valid |= block_before;
        }
        if (i == block_last && (page_last & 63) != 63) {
          uint64_t block_inside = (uint64_t(1) << ((page_last & 63) + 1)) - 1;
          block_valid |= ~block_inside;
        }

        while (true) {
          uint32_t block_page;
          if (range_start == UINT32_MAX) {
            // Check if need to open a new range.
            if (!rex::bit_scan_forward(~block_valid, &block_page)) {
              break;
            }
            range_start = (i << 6) + block_page;
          } else {
            // Check if need to close the range.
            // Ignore the valid pages before the beginning of the range.
            uint64_t block_valid_from_start = block_valid;
            if (i == (range_start >> 6)) {
              block_valid_from_start &= ~((uint64_t(1) << (range_start & 63)) - 1);
            }
            if (!rex::bit_scan_forward(block_valid_from_start, &block_page)) {
              break;
            }
            append_upload_range(range_start, (i << 6) + block_page - range_start);
            // In the next iteration within this block, consider this range
            // valid since it has been queued for upload.
            block_valid |= (uint64_t(1) << block_page) - 1;
            range_start = UINT32_MAX;
          }
        }
      }
      if (range_start != UINT32_MAX) {
        append_upload_range(range_start, page_last + 1 - range_start);
      }
    }
  }

  COUNT_profile_set("gpu/shared_memory/request_ranges_count", uint32_t(original_count));
  COUNT_profile_set("gpu/shared_memory/request_ranges_merged_count",
                    uint32_t(merged_count));
  COUNT_profile_set("gpu/shared_memory/request_ranges_upload_count",
                    uint32_t(upload_ranges_.size()));
  scan_timer.Stop();

  if (upload_ranges_.empty()) {
    return true;
  }

  upload_allow_streamed_ = allow_streamed && streamed_pages_active_;
  if (upload_allow_streamed_ && streamed_page_shadows_supported_ &&
      REXCVAR_GET(gpu_stream_skip_unchanged) && !streamed_page_shadows_.empty()) {
    DropUnchangedStreamedPages(merged_ranges, merged_count);
    if (upload_ranges_.empty()) {
      upload_allow_streamed_ = false;
      ++g_streamed_skip_stats[1];
      return true;
    }
  }

  {
    static const bool upload_request_stats = std::getenv("REX_CMD_STATS") != nullptr;
    if (upload_request_stats) {
      uint64_t requested = 0, uploaded_pages = 0;
      for (size_t i = 0; i < merged_count; ++i) {
        requested += merged_ranges[i].second;
      }
      for (const auto& upload_range : upload_ranges_) {
        uploaded_pages += upload_range.second;
      }
      ++g_upload_request_stats[0];
      g_upload_request_stats[1] += requested;
      g_upload_request_stats[2] += uploaded_pages << page_size_log2_;
      ++g_upload_request_stats[3 + std::min(uint32_t(31), uint32_t(64 - rex::lzcnt(requested)))];
      static const bool upload_request_ranges = std::strcmp(std::getenv("REX_CMD_STATS"), "2") == 0;
      if (upload_request_ranges && merged_count == 1) {
        auto& entry = g_upload_request_ranges[merged_ranges[0]];
        ++entry.first;
        entry.second += uploaded_pages << page_size_log2_;
      }
    }
  }
  bool uploaded = UploadRanges(upload_ranges_);
  upload_allow_streamed_ = false;
  return uploaded;
}

void SharedMemory::DropUnchangedStreamedPages(const std::pair<uint32_t, uint32_t>* ranges,
                                              size_t count) {
  const uint32_t page_size = uint32_t(1) << page_size_log2_;
  kept_upload_ranges_.clear();
  uint64_t dropped = 0;
  for (const std::pair<uint32_t, uint32_t>& upload_range : upload_ranges_) {
    for (uint32_t page = upload_range.first; page < upload_range.first + upload_range.second;
         ++page) {
      bool unchanged = false;
      if ((system_page_flags_streamed_[page >> 6] >> (page & 63)) & 1) {
        auto shadow_it = streamed_page_shadows_.find(page);
        if (shadow_it != streamed_page_shadows_.end()) {
          // Only the bytes the request covers matter.
          uint32_t page_start = page << page_size_log2_;
          uint32_t page_end = page_start + page_size;
          const uint8_t* guest = memory().TranslatePhysical(page_start);
          const uint8_t* shadow = shadow_it->second.get();
          unchanged = true;
          for (size_t i = 0; i < count && unchanged; ++i) {
            uint32_t compare_start = std::max(ranges[i].first, page_start);
            uint32_t compare_end = std::min(ranges[i].first + ranges[i].second, page_end);
            if (compare_start < compare_end &&
                !draw_util::BytesEqual(guest + (compare_start - page_start),
                                       shadow + (compare_start - page_start),
                                       compare_end - compare_start)) {
              unchanged = false;
            }
          }
        }
      }
      if (unchanged) {
        ++dropped;
        continue;
      }
      if (!kept_upload_ranges_.empty() &&
          kept_upload_ranges_.back().first + kept_upload_ranges_.back().second == page) {
        ++kept_upload_ranges_.back().second;
      } else {
        kept_upload_ranges_.emplace_back(page, 1);
      }
    }
  }
  g_streamed_skip_stats[0] += dropped;
  upload_ranges_.swap(kept_upload_ranges_);
}

void SharedMemory::EraseStreamedPageShadows(uint32_t page_first, uint32_t page_last) {
  if (streamed_page_shadows_.empty()) {
    return;
  }
  if (page_last - page_first >= streamed_page_shadows_.size()) {
    for (auto it = streamed_page_shadows_.begin(); it != streamed_page_shadows_.end();) {
      if (it->first >= page_first && it->first <= page_last) {
        it = streamed_page_shadows_.erase(it);
      } else {
        ++it;
      }
    }
  } else {
    for (uint32_t page = page_first; page <= page_last; ++page) {
      streamed_page_shadows_.erase(page);
    }
  }
}

void SharedMemory::CopyPagesForUpload(uint32_t page_first, uint32_t page_count, uint8_t* dest) {
  rex::perf::ScopedCounterTimer copy_timer(rex::perf::CounterId::kUploadCopyUs);
  PERF_counter_add(kUploadBytes, int64_t(page_count) << page_size_log2_);
  const uint8_t* source = memory().TranslatePhysical(page_first << page_size_log2_);
  if (!upload_allow_streamed_ || !REXCVAR_GET(gpu_stream_skip_unchanged)) {
    std::memcpy(dest, source, size_t(page_count) << page_size_log2_);
    if (page_count) {
      EraseStreamedPageShadows(page_first, page_first + page_count - 1);
    }
    return;
  }
  const size_t page_size = size_t(1) << page_size_log2_;
  for (uint32_t i = 0; i < page_count; ++i) {
    uint32_t page = page_first + i;
    const uint8_t* page_source = source + (size_t(i) << page_size_log2_);
    uint8_t* page_dest = dest + (size_t(i) << page_size_log2_);
    // MakeRangeValid already ran, so this is whether the page was left
    // streamed (not made valid) by this upload.
    if ((system_page_flags_streamed_[page >> 6] >> (page & 63)) & 1) {
      // Copy through the shadow so it has exactly the uploaded bytes even if a
      // guest thread is writing the page right now.
      std::unique_ptr<uint8_t[]>& shadow = streamed_page_shadows_[page];
      if (!shadow) {
        shadow = std::make_unique<uint8_t[]>(page_size);
      }
      std::memcpy(shadow.get(), page_source, page_size);
      std::memcpy(page_dest, shadow.get(), page_size);
    } else {
      std::memcpy(page_dest, page_source, page_size);
      if (!streamed_page_shadows_.empty()) {
        streamed_page_shadows_.erase(page);
      }
    }
  }
}

bool SharedMemory::RequestRange(uint32_t start, uint32_t length, bool allow_streamed) {
  std::pair<uint32_t, uint32_t> range(start, length);
  return RequestRanges(&range, 1, allow_streamed);
}

std::pair<uint32_t, uint32_t> SharedMemory::MemoryInvalidationCallbackThunk(
    void* context_ptr, uint32_t physical_address_start, uint32_t length, bool exact_range) {
  return reinterpret_cast<SharedMemory*>(context_ptr)
      ->MemoryInvalidationCallback(physical_address_start, length, exact_range);
}

std::pair<uint32_t, uint32_t> SharedMemory::MemoryInvalidationCallback(
    uint32_t physical_address_start, uint32_t length, bool exact_range) {
  if (length == 0 || physical_address_start >= kBufferSize) {
    return std::make_pair(uint32_t(0), UINT32_MAX);
  }
  length = std::min(length, kBufferSize - physical_address_start);
  uint32_t physical_address_last = physical_address_start + (length - 1);

  uint32_t page_first = physical_address_start >> page_size_log2_;
  uint32_t page_last = physical_address_last >> page_size_log2_;
  uint32_t block_first = page_first >> 6;
  uint32_t block_last = page_last >> 6;

  auto global_lock = global_critical_region_.Acquire();

  if (!exact_range && streamed_pages_active_) {
    // A guest write fault - for picking the streamed pages.
    system_page_flags_faulted_[page_first >> 6] |= uint64_t(1) << (page_first & 63);
  }

  if (!exact_range) {
    // Check if a somewhat wider range (up to 256 KB with 4 KB pages) can be
    // invalidated - if no GPU-written data nearby that was not intended to be
    // invalidated since it's not in sync with CPU memory and can't be
    // reuploaded. It's a lot cheaper to upload some excess data than to catch
    // access violations - with 4 KB callbacks, 58410824 (being a
    // software-rendered game) runs at 4 FPS on Intel Core i7-3770, with 64 KB,
    // the CPU game code takes 3 ms to run per frame, but with 256 KB, it's
    // 0.7 ms.
    if (page_first & 63) {
      uint64_t gpu_written_start = system_page_flags_valid_and_gpu_written_[block_first];
      gpu_written_start &= (uint64_t(1) << (page_first & 63)) - 1;
      page_first = (page_first & ~uint32_t(63)) + (64 - rex::lzcnt(gpu_written_start));
    }
    if ((page_last & 63) != 63) {
      uint64_t gpu_written_end = system_page_flags_valid_and_gpu_written_[block_last];
      gpu_written_end &= ~((uint64_t(1) << ((page_last & 63) + 1)) - 1);
      page_last =
          (page_last & ~uint32_t(63)) + (std::max(rex::tzcnt(gpu_written_end), uint8_t(1)) - 1);
    }
  }

  for (uint32_t i = block_first; i <= block_last; ++i) {
    uint64_t invalidate_bits = UINT64_MAX;
    if (i == block_first) {
      invalidate_bits &= ~((uint64_t(1) << (page_first & 63)) - 1);
    }
    if (i == block_last && (page_last & 63) != 63) {
      invalidate_bits &= (uint64_t(1) << ((page_last & 63) + 1)) - 1;
    }
    system_page_flags_valid_[i] &= ~invalidate_bits;
    system_page_flags_valid_and_gpu_written_[i] &= ~invalidate_bits;
  }

  // HAND PATCH: was the public FireWatches, which re-acquires the global
  // critical region this function already holds (and whose sole caller,
  // PhysicalHeap::TriggerCallbacks, holds it a level above that -- its
  // signature takes the lock by value). This runs on every CPU write fault
  // against GPU-watched memory -- tens of thousands of times during level
  // streaming per mmio_handler.cpp's own comment -- so drop the innermost of
  // the three recursive acquires by passing the already-held lock through.
  FireWatchesLocked(global_lock, page_first, page_last, false);

  return std::make_pair(page_first << page_size_log2_, (page_last - page_first + 1)
                                                           << page_size_log2_);
}

void SharedMemory::PrepareForTraceDownload() {
  ReleaseTraceDownloadRanges();
  assert_true(trace_download_ranges_.empty());
  assert_zero(trace_download_page_count_);

  // Invalidate the entire memory CPU->GPU memory copy so all the history
  // doesn't have to be written into every frame trace, and collect the list of
  // ranges with data modified on the GPU.

  uint32_t fire_watches_range_start = UINT32_MAX;
  uint32_t gpu_written_range_start = UINT32_MAX;
  auto global_lock = global_critical_region_.Acquire();
  for (uint32_t i = 0; i < num_system_page_flags_; ++i) {
    uint64_t previously_valid_block = system_page_flags_valid_[i];
    uint64_t gpu_written_block = system_page_flags_valid_and_gpu_written_[i];
    system_page_flags_valid_[i] = gpu_written_block;

    // Fire watches on the invalidated pages.
    uint64_t fire_watches_block = previously_valid_block & ~gpu_written_block;
    uint64_t fire_watches_break_block = ~fire_watches_block;
    while (true) {
      uint32_t fire_watches_block_page;
      if (!rex::bit_scan_forward(fire_watches_range_start == UINT32_MAX ? fire_watches_block
                                                                        : fire_watches_break_block,
                                 &fire_watches_block_page)) {
        break;
      }
      uint32_t fire_watches_page = (i << 6) + fire_watches_block_page;
      if (fire_watches_range_start == UINT32_MAX) {
        fire_watches_range_start = fire_watches_page;
      } else {
        FireWatches(fire_watches_range_start, fire_watches_page - 1, false);
        fire_watches_range_start = UINT32_MAX;
      }
      uint64_t fire_watches_block_mask = ~((uint64_t(1) << fire_watches_block_page) - 1);
      fire_watches_block &= fire_watches_block_mask;
      fire_watches_break_block &= fire_watches_block_mask;
    }

    // Add to the GPU-written ranges.
    uint64_t gpu_written_break_block = ~gpu_written_block;
    while (true) {
      uint32_t gpu_written_block_page;
      if (!rex::bit_scan_forward(
              gpu_written_range_start == UINT32_MAX ? gpu_written_block : gpu_written_break_block,
              &gpu_written_block_page)) {
        break;
      }
      uint32_t gpu_written_page = (i << 6) + gpu_written_block_page;
      if (gpu_written_range_start == UINT32_MAX) {
        gpu_written_range_start = gpu_written_page;
      } else {
        uint32_t gpu_written_range_length = gpu_written_page - gpu_written_range_start;
        // Call EnsureHostGpuMemoryAllocated in case the page was marked as
        // GPU-written not as a result to an actual write to the shared memory
        // buffer, but, for instance, by resolving with resolution scaling (to a
        // separate buffer).
        if (EnsureHostGpuMemoryAllocated(gpu_written_range_start << page_size_log2_,
                                         gpu_written_range_length << page_size_log2_)) {
          trace_download_ranges_.push_back(
              std::make_pair(gpu_written_range_start << page_size_log2_,
                             gpu_written_range_length << page_size_log2_));
          trace_download_page_count_ += gpu_written_range_length;
        }
        gpu_written_range_start = UINT32_MAX;
      }
      uint64_t gpu_written_block_mask = ~((uint64_t(1) << gpu_written_block_page) - 1);
      gpu_written_block &= gpu_written_block_mask;
      gpu_written_break_block &= gpu_written_block_mask;
    }
  }
  uint32_t page_count = kBufferSize >> page_size_log2_;
  if (fire_watches_range_start != UINT32_MAX) {
    FireWatches(fire_watches_range_start, page_count - 1, false);
  }
  if (gpu_written_range_start != UINT32_MAX) {
    uint32_t gpu_written_range_length = page_count - gpu_written_range_start;
    if (EnsureHostGpuMemoryAllocated(gpu_written_range_start << page_size_log2_,
                                     gpu_written_range_length << page_size_log2_)) {
      trace_download_ranges_.push_back(std::make_pair(gpu_written_range_start << page_size_log2_,
                                                      gpu_written_range_length << page_size_log2_));
      trace_download_page_count_ += gpu_written_range_length;
    }
  }
}

void SharedMemory::ReleaseTraceDownloadRanges() {
  trace_download_ranges_.clear();
  trace_download_ranges_.shrink_to_fit();
  trace_download_page_count_ = 0;
}

bool SharedMemory::EnsureHostGpuMemoryAllocated(uint32_t start, uint32_t length) {
  if (host_gpu_memory_sparse_granularity_log2_ == UINT32_MAX) {
    return true;
  }
  if (!length) {
    return true;
  }
  if (start > kBufferSize || (kBufferSize - start) < length) {
    return false;
  }
  uint32_t page_first = start >> page_size_log2_;
  uint32_t page_last = (start + length - 1) >> page_size_log2_;
  uint32_t allocation_first =
      page_first << page_size_log2_ >> host_gpu_memory_sparse_granularity_log2_;
  uint32_t allocation_last =
      page_last << page_size_log2_ >> host_gpu_memory_sparse_granularity_log2_;
  while (true) {
    std::pair<size_t, size_t> allocation_range =
        rex::bit::GetNextRangeUnset(host_gpu_memory_sparse_allocated_.data(), allocation_first,
                                    allocation_last - allocation_first + 1);
    if (!allocation_range.second) {
      break;
    }
    if (!AllocateSparseHostGpuMemoryRange(uint32_t(allocation_range.first),
                                          uint32_t(allocation_range.second))) {
      return false;
    }
    rex::bit::SetRange(host_gpu_memory_sparse_allocated_.data(), allocation_range.first,
                       allocation_range.second);
    ++host_gpu_memory_sparse_allocations_;
    COUNT_profile_set("gpu/shared_memory/host_gpu_memory_sparse_allocations",
                      host_gpu_memory_sparse_allocations_);
    host_gpu_memory_sparse_used_bytes_ += uint32_t(allocation_range.second)
                                          << host_gpu_memory_sparse_granularity_log2_;
    COUNT_profile_set("gpu/shared_memory/host_gpu_memory_sparse_used_mb",
                      (host_gpu_memory_sparse_used_bytes_ + ((1 << 20) - 1)) >> 20);
    allocation_first = uint32_t(allocation_range.first + allocation_range.second);
  }
  return true;
}

}  // namespace rex::graphics
