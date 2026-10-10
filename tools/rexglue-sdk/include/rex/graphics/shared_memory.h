#pragma once
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

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <rex/memory.h>
#include <rex/thread/mutex.h>

namespace rex::graphics {

// Manages memory for unconverted textures, resolve targets, vertex and index
// buffers that can be accessed from shaders with Xenon physical addresses, with
// system page size granularity.
class SharedMemory {
 public:
  static constexpr uint32_t kBufferSizeLog2 = 29;
  static constexpr uint32_t kBufferSize = 1 << kBufferSizeLog2;

  virtual ~SharedMemory();
  // Call in the implementation-specific ClearCache.
  virtual void ClearCache();
  void SetSystemPageBlocksValidWithGpuDataWritten();
  void InvalidateAllPages();

  typedef void (*GlobalWatchCallback)(const std::unique_lock<std::recursive_mutex>& global_lock,
                                      void* context, uint32_t address_first, uint32_t address_last,
                                      bool invalidated_by_gpu);
  typedef void* GlobalWatchHandle;
  // Registers a callback invoked when something is invalidated in the GPU
  // memory copy by the CPU or (if triggered explicitly - such as by a resolve)
  // by the GPU. It will be fired for writes to pages previously requested, but
  // may also be fired regardless of whether it was used by GPU emulation - for
  // example, if the game changes protection level of a memory range containing
  // the watched range.
  //
  // The callback is called within the global critical region.
  GlobalWatchHandle RegisterGlobalWatch(GlobalWatchCallback callback, void* callback_context);
  void UnregisterGlobalWatch(GlobalWatchHandle handle);
  typedef void (*WatchCallback)(const std::unique_lock<std::recursive_mutex>& global_lock,
                                void* context, void* data, uint64_t argument,
                                bool invalidated_by_gpu);
  typedef void* WatchHandle;
  // Registers a callback invoked when the specified memory range is invalidated
  // in the GPU memory copy by the CPU or (if triggered explicitly - such as by
  // a resolve) by the GPU. It will be fired for writes to pages previously
  // requested, but may also be fired regardless of whether it was used by GPU
  // emulation - for example, if the game changes protection level of a memory
  // range containing the watched range.
  //
  // Generally the context is the subsystem pointer (for example, the texture
  // cache), the data is the object (such as a texture), and the argument is
  // additional subsystem/object-specific data (such as whether the range
  // belongs to the base mip level or to the rest of the mips).
  //
  // Called with the global critical region locked. Do NOT watch or unwatch
  // ranges from within it! The watch for the callback is cancelled after the
  // callback - the handle becomes invalid.
  WatchHandle WatchMemoryRange(uint32_t start, uint32_t length, WatchCallback callback,
                               void* callback_context, void* callback_data,
                               uint64_t callback_argument);
  // Unregisters previously registered watched memory range.
  void UnwatchMemoryRange(WatchHandle handle);

  // Checks if the range has been updated, uploads new data if needed and
  // ensures the host GPU memory backing the range are resident. Returns true if
  // the range has been fully updated and is usable. allow_streamed is for
  // vertex and index data that only the current draw reads: with
  // gpu_stream_dynamic_pages, pages the game rewrites every frame are then
  // uploaded without being made valid or write-protected (so they're uploaded
  // again on every such request, and never go stale).
  bool RequestRanges(const std::pair<uint32_t, uint32_t>* ranges, size_t count,
                     bool allow_streamed = false);
  bool RequestRange(uint32_t start, uint32_t length, bool allow_streamed = false);
  // Detect changes to a native upload without declaring the mirror valid.
  bool WatchCpuMemoryRange(uint32_t start, uint32_t length);
  // Force CPU-owned texture inputs to be captured again by a new trace.
  void InvalidateCpuMemoryRangeForTrace(uint32_t start, uint32_t length);

  // Call once per guest frame, on the command processor thread, to update
  // which pages count as streamed (see RequestRanges).
  void OnGuestFrameEnd();

  // Diagnostics (shared_memory_census): records a use of the mirror by the
  // code at the return address caller, with a kind (1 + the backend's usage,
  // or one of the kinds below for the scaled resolve buffer and copies of
  // native resolve results into either).
  static constexpr uint32_t kCensusKindScaledRead = 16;
  static constexpr uint32_t kCensusKindScaledWrite = 17;
  static constexpr uint32_t kCensusKindWriteBack = 18;
  static constexpr uint32_t kCensusKindScaledWriteBack = 19;
  static bool CensusEnabled();
  static void CensusRecord(const void* caller, uint32_t kind, uint64_t bytes);
  // The code a copy of native resolve results into a mirror is attributed to
  // (set around the call that makes the copy happen).
  static const void*& CensusTrigger();
  // Diagnostics: the function at a code address, with the offset into it.
  static std::string DescribeCodeAddress(const void* address);

  // For the trace player, which reports the memory it writes with exact
  // invalidations (no write faults): counts the writes as write faults, so
  // pages the trace rewrites every frame become streamed like in the game.
  void MarkRangeFaultedForStreaming(uint32_t start, uint32_t length);

  // Marks the range and, if not exact_range, potentially its surroundings
  // (to up to the first GPU-written page, as an access violation exception
  // count optimization) as modified by the CPU, also invalidating GPU-written
  // pages directly in the range.
  std::pair<uint32_t, uint32_t> MemoryInvalidationCallback(uint32_t physical_address_start,
                                                           uint32_t length, bool exact_range);

  // Marks the range as containing GPU-generated data (such as resolves),
  // triggering modification callbacks, making it valid (so pages are not
  // copied from the main memory until they're modified by the CPU) and
  // protecting it. Before writing anything from the GPU side, RequestRange must
  // be called, to make sure, if the GPU writes don't overwrite *everything* in
  // the pages they touch, the CPU data is properly loaded to the unmodified
  // regions in those pages.
  void RangeWrittenByGpu(uint32_t start, uint32_t length);
  // A CPU snapshot cannot replace GPU-generated data that has not been read
  // back. Checks whole pages, so a neighboring GPU write also keeps the range
  // on the shared-memory path.
  bool IsRangeGpuWritten(uint32_t start, uint32_t length);
  // Whether the host GPU buffer has memory behind a guest physical address
  // (always with a non-sparse buffer).
  uint32_t host_gpu_memory_sparse_granularity_log2_public() const {
    return host_gpu_memory_sparse_granularity_log2_;
  }
  bool IsHostGpuMemoryAllocated(uint32_t address) const {
    if (host_gpu_memory_sparse_granularity_log2_ == UINT32_MAX) {
      return true;
    }
    uint32_t allocation = address >> host_gpu_memory_sparse_granularity_log2_;
    return (host_gpu_memory_sparse_allocated_[allocation >> 6] >> (allocation & 63)) & 1;
  }

 protected:
  // Creates the host GPU buffer if the implementation creates it only when
  // first needed. Called before anything uses the mirror.
  virtual bool EnsureHostBuffer() { return true; }
  // Native GPU resources may hold newer bytes than the compatibility mirror.
  // Called before requesting or overwriting a mirror range.
  virtual bool FlushGpuWrittenRange(uint32_t /*start*/, uint32_t /*length*/,
                                    bool /*writing*/ = false) {
    return true;
  }
  SharedMemory(memory::Memory& memory);
  // Call in implementation-specific initialization.
  void InitializeCommon();
  void InitializeSparseHostGpuMemory(uint32_t granularity_log2);
  // Call last in implementation-specific shutdown, also callable from the
  // destructor.
  void ShutdownCommon();

  // Sparse allocations are 4 MB, so not too many of them are allocated, but
  // also not to waste too much memory for padding (with 16 MB there's too
  // much).
  static constexpr uint32_t kHostGpuMemoryOptimalSparseAllocationLog2 = 22;
  static_assert(kHostGpuMemoryOptimalSparseAllocationLog2 <= kBufferSizeLog2);

  memory::Memory& memory() const { return memory_; }

  uint32_t page_size_log2() const { return page_size_log2_; }

  uint32_t host_gpu_memory_sparse_granularity_log2() const {
    return host_gpu_memory_sparse_granularity_log2_;
  }

  // Allocations in the host buffer are aligned the same way as in the guest
  // physical memory (for instance, if an allocation is 64 KB, it can represent
  // 0-64 KB, 64-128 KB, 128-192 KB in the guest memory, and so on, but not
  // something like 16-80 KB. This is assumed by the rules for texture data
  // access in the texture cache.
  virtual bool AllocateSparseHostGpuMemoryRange(uint32_t offset_allocations,
                                                uint32_t length_allocations);

  // Mark the memory range as updated and protect it.
  void MakeRangeValid(uint32_t start, uint32_t length, bool written_by_gpu);

  // Uploads a range of host pages - only called if host GPU sparse memory
  // allocation succeeded if needed. While uploading, MakeRangeValid must be
  // called for each successfully uploaded range as early as possible, before
  // the memcpy, to make sure invalidation that happened during the CPU -> GPU
  // memcpy isn't missed (upload_page_ranges is in pages because of this -
  // MakeRangeValid has page granularity). upload_page_ranges are sorted in
  // ascending address order, so front and back can be used to determine the
  // overall bounds of pages to be uploaded.
  virtual bool UploadRanges(
      const std::vector<std::pair<uint32_t, uint32_t>>& upload_page_ranges) = 0;
  static void CensusLog();
  // For UploadRanges, after MakeRangeValid: copies guest pages to the upload
  // buffer, remembering what streamed pages were uploaded with (see
  // streamed_page_shadows_).
  void CopyPagesForUpload(uint32_t page_first, uint32_t page_count, uint8_t* dest);
  // Set by implementations whose UploadRanges copies with CopyPagesForUpload.
  bool streamed_page_shadows_supported_ = false;

  const std::vector<std::pair<uint32_t, uint32_t>>& trace_download_ranges() {
    return trace_download_ranges_;
  }
  uint32_t trace_download_page_count() const { return trace_download_page_count_; }
  // Fills trace_download_ranges() and trace_download_page_count() with
  // GPU-written ranges that need to be downloaded, and also invalidates
  // non-GPU-written ranges so only the needed data - not the all the collected
  // data - will be written in the trace. trace_download_page_count() will be 0
  // if nothing to download.
  void PrepareForTraceDownload();
  // Release memory used for trace download ranges, to be called after
  // downloading or in cases when download is dropped.
  void ReleaseTraceDownloadRanges();

 private:
  memory::Memory& memory_;

  // Log2 of invalidation granularity (the system page size, but the dependency
  // on it is not hard - the access callback takes a range as an argument, and
  // touched pages of the buffer of this size will be invalidated).
  uint32_t page_size_log2_;

  bool EnsureHostGpuMemoryAllocated(uint32_t start, uint32_t length);

  // Shared tail of RequestRanges, taking ranges that are already bounds-checked,
  // merged and backed by host GPU memory. Lets a single-range request skip
  // building and sorting a vector. original_count is only for the profiler.
  bool RequestValidatedRanges(const std::pair<uint32_t, uint32_t>* merged_ranges,
                              size_t merged_count, size_t original_count, bool allow_streamed);
  uint32_t host_gpu_memory_sparse_granularity_log2_ = UINT32_MAX;
  std::vector<uint64_t> host_gpu_memory_sparse_allocated_;
  uint32_t host_gpu_memory_sparse_allocations_ = 0;
  uint32_t host_gpu_memory_sparse_used_bytes_ = 0;

  void* memory_invalidation_callback_handle_ = nullptr;
  void* memory_data_provider_handle_ = nullptr;

  // Ranges that need to be uploaded, generated by GetRangesToUpload (a
  // persistently allocated vector).
  std::vector<std::pair<uint32_t, uint32_t>> upload_ranges_;

  // GPU-written memory downloading for traces. <Start address, length>.
  std::vector<std::pair<uint32_t, uint32_t>> trace_download_ranges_;
  uint32_t trace_download_page_count_ = 0;

  // Mutex between the guest memory subsystem and the command processor, to be
  // locked when checking or updating validity of pages/ranges and when firing
  // watches.
  rex::thread::global_critical_region global_critical_region_;

  // ***************************************************************************
  // Things below should be fully protected by global_critical_region.
  // ***************************************************************************

  // Valid-page flags. Every mutation happens under global_critical_region;
  // the all-valid fast path in RequestRanges reads them without the lock,
  // which is why the storage is a stable allocation whose address never
  // changes for the lifetime of the buffer.
  //
  // This used to be double-buffered with an active/staging pointer swap at
  // frame end so the frame-end refresh could run lock-free. That was unsound:
  // the swap raced guest-thread invalidations (which load the active pointer
  // under the lock, then clear bits through it), so a clear could land in the
  // buffer being retired while the promoted buffer kept a stale VALID bit for
  // a page the guest had just rewritten. RequestRanges then took its
  // all-valid fast path and skipped the upload entirely, leaving the GPU
  // reading the previous frame's bytes for that page. Refreshing only "dirty"
  // 16 MB superblocks made it worse: untouched superblocks of the promoted
  // buffer were a two-frame-old snapshot.
  std::vector<uint64_t> system_page_flags_valid_;
  std::atomic<uint64_t*> valid_flags_{nullptr};
  // Subset of valid pages containing data written by the GPU.
  std::vector<uint64_t> system_page_flags_valid_and_gpu_written_;
  uint32_t num_system_page_flags_ = 0;

  // gpu_stream_dynamic_pages: pages the game rewrites every frame. Requests
  // with allow_streamed upload them without making them valid or protecting
  // them, saving a protection change and a guest write fault per page per
  // frame. A page is streamed after write faults in two frames in a row, and
  // the set is re-learned every kStreamedPagesResetFrames frames. Only the
  // command processor thread changes the streamed set (under the global
  // critical region); guest threads set the faulted bits.
  static constexpr uint32_t kStreamedPagesResetFrames = 600;
  // A streamed page uploaded more times than this in one frame (used by very
  // many draws) is cheaper to protect: it goes back to being watched, and
  // can't be streamed again until the next reset. An extra 4 KB upload costs
  // well under a microsecond, a protection change plus the write fault that
  // undoes it about 25. A cap of 4 sent enough Springfield pages back to
  // being watched to cost about 1 FPS there.
  static constexpr uint8_t kStreamedPageMaxUploadsPerFrame = 64;
  std::vector<uint64_t> system_page_flags_streamed_;
  std::vector<uint64_t> system_page_flags_streamed_blocked_;
  std::vector<uint64_t> system_page_flags_faulted_;
  std::vector<uint64_t> system_page_flags_faulted_previous_;
  std::vector<uint8_t> streamed_page_uploads_this_frame_;
  uint32_t streamed_pages_frames_ = 0;
  bool streamed_pages_active_ = false;
  // Set while RequestValidatedRanges uploads for an allow_streamed request,
  // read by MakeRangeValid (both on the command processor thread).
  bool upload_allow_streamed_ = false;
  // gpu_stream_skip_unchanged: the bytes each streamed page was last uploaded
  // with, which is what the GPU copy of the page holds until anything else
  // changes it (another upload, or a GPU write, which erase the copy here). A
  // streamed request whose bytes in the page still match skips the page's
  // upload: a streamed page usually changes because of other data sharing it,
  // and draws reusing a buffer would otherwise upload it again each time (each
  // upload waits for the GPU to finish all earlier work). Only touched on the
  // command processor thread.
  std::unordered_map<uint32_t, std::unique_ptr<uint8_t[]>> streamed_page_shadows_;
  std::vector<std::pair<uint32_t, uint32_t>> kept_upload_ranges_;
  void DropUnchangedStreamedPages(const std::pair<uint32_t, uint32_t>* ranges, size_t count);
  void EraseStreamedPageShadows(uint32_t page_first, uint32_t page_last);

  static std::pair<uint32_t, uint32_t> MemoryInvalidationCallbackThunk(
      void* context_ptr, uint32_t physical_address_start, uint32_t length, bool exact_range);

  struct GlobalWatch {
    GlobalWatchCallback callback;
    void* callback_context;
  };
  std::vector<GlobalWatch*> global_watches_;
  struct WatchNode;
  // Watched range placed by other GPU subsystems.
  struct WatchRange {
    union {
      struct {
        WatchCallback callback;
        void* callback_context;
        void* callback_data;
        uint64_t callback_argument;
        WatchNode* node_first;
        uint32_t page_first;
        uint32_t page_last;
      };
      WatchRange* next_free;
    };
  };
  // Node for faster checking of watches when pages have been written to - all
  // 512 MB are split into smaller equally sized buckets, and then ranges are
  // linearly checked.
  struct WatchNode {
    union {
      struct {
        WatchRange* range;
        // Link to another node of this watched range in the next bucket.
        WatchNode* range_node_next;
        // Links to nodes belonging to other watched ranges in the bucket.
        WatchNode* bucket_node_previous;
        WatchNode* bucket_node_next;
      };
      WatchNode* next_free;
    };
  };
  static constexpr uint32_t kWatchBucketSizeLog2 = 22;
  static constexpr uint32_t kWatchBucketCount = 1 << (kBufferSizeLog2 - kWatchBucketSizeLog2);
  WatchNode* watch_buckets_[kWatchBucketCount] = {};
  // Allocation from pools - taking new WatchRanges and WatchNodes from the free
  // list, and if there are none, creating a pool if the current one is fully
  // used, and linearly allocating from the current pool.
  static constexpr uint32_t kWatchRangePoolSize = 8192;
  static constexpr uint32_t kWatchNodePoolSize = 8192;
  std::vector<WatchRange*> watch_range_pools_;
  std::vector<WatchNode*> watch_node_pools_;
  uint32_t watch_range_current_pool_allocated_ = 0;
  uint32_t watch_node_current_pool_allocated_ = 0;
  WatchRange* watch_range_first_free_ = nullptr;
  WatchNode* watch_node_first_free_ = nullptr;
  // Triggers the watches (global and per-range), removing triggered range
  // watches.
  void FireWatches(uint32_t page_first, uint32_t page_last, bool invalidated_by_gpu);
  // Same, for callers already holding the global critical region (pass the
  // held lock as proof) -- avoids a redundant recursive re-acquire on the
  // CPU-write-fault invalidation path, which fires tens of thousands of
  // times during level streaming (see mmio_handler.cpp's exception-handler
  // comment).
  void FireWatchesLocked(const std::unique_lock<std::recursive_mutex>& global_lock,
                         uint32_t page_first, uint32_t page_last, bool invalidated_by_gpu);
  // Unlinks and frees the range and its nodes. Call this in the global critical
  // region.
  void UnlinkWatchRange(WatchRange* range);
};

}  // namespace rex::graphics
