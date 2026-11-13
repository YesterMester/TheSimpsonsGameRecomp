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

#include <cstring>
#include <utility>
#include <stdexcept>
#include <vector>

#include <rex/assert.h>
#include <rex/cvar.h>
#include <rex/graphics/d3d12/command_processor.h>
#include <rex/graphics/d3d12/shared_memory.h>
#include <rex/graphics/flags.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/ui/d3d12/d3d12_util.h>

REXCVAR_DEFINE_BOOL(d3d12_tiled_shared_memory, true, "GPU/D3D12",
                    "Use tiled shared memory on D3D12")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace rex::graphics::d3d12 {
namespace {
constexpr uint64_t kMaximumNativeResolveBytes = 128u << 20;
}

D3D12SharedMemory::D3D12SharedMemory(D3D12CommandProcessor& command_processor,
                                     memory::Memory& memory, TraceWriter& trace_writer)
    : SharedMemory(memory), command_processor_(command_processor), trace_writer_(trace_writer) {}

D3D12SharedMemory::~D3D12SharedMemory() {
  Shutdown(true);
}

bool D3D12SharedMemory::DownloadNativeGpuIndices(uint32_t start, std::span<uint8_t> bytes) {
  return command_processor_.DownloadNativeIndexRange(start, bytes);
}

bool D3D12SharedMemory::SnapshotCpuRange(uint32_t address, uint32_t length, void* destination) {
  if (!length || address >= kBufferSize || length > kBufferSize - address ||
      IsRangeGpuWritten(address, length)) {
    return false;
  }
  std::memcpy(destination, memory().TranslatePhysical<const void*>(address), length);
  trace_writer_.WriteMemoryRead(address, length, destination);
  return true;
}

bool D3D12SharedMemory::Initialize() {
  InitializeCommon();
  native_resolve_watch_ = RegisterGlobalWatch(NativeResolveInvalidation, this);

  const ui::d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();

  tiled_buffer_ = REXCVAR_GET(d3d12_tiled_shared_memory) &&
                  provider.GetTiledResourcesTier() != D3D12_TILED_RESOURCES_TIER_NOT_SUPPORTED &&
                  !provider.GetGraphicsAnalysis();
  if (tiled_buffer_) {
    static_assert(D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES == (1 << 16));
    InitializeSparseHostGpuMemory(
        std::max(kHostGpuMemoryOptimalSparseAllocationLog2, uint32_t(16)));
  }

  D3D12_DESCRIPTOR_HEAP_DESC buffer_descriptor_heap_desc;
  buffer_descriptor_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  buffer_descriptor_heap_desc.NumDescriptors = uint32_t(BufferDescriptorIndex::kCount);
  buffer_descriptor_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
  buffer_descriptor_heap_desc.NodeMask = 0;
  if (FAILED(device->CreateDescriptorHeap(&buffer_descriptor_heap_desc,
                                          IID_PPV_ARGS(&buffer_descriptor_heap_)))) {
    REXGPU_ERROR("Shared memory: Failed to create the descriptor heap for buffer views");
    Shutdown();
    return false;
  }
  buffer_descriptor_heap_start_ = buffer_descriptor_heap_->GetCPUDescriptorHandleForHeapStart();
  UpdateBufferDescriptors();

  upload_buffer_pool_ = std::make_unique<ui::d3d12::D3D12UploadBufferPool>(
      provider, rex::align(ui::d3d12::D3D12UploadBufferPool::kDefaultPageSize,
                           size_t(1) << page_size_log2()));

  return true;
}

bool D3D12SharedMemory::EnsureBuffer() {
  if (buffer_) {
    return true;
  }
  const auto& provider = command_processor_.GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  D3D12_RESOURCE_DESC buffer_desc;
  ui::d3d12::util::FillBufferResourceDesc(buffer_desc, kBufferSize,
                                          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  buffer_state_ = D3D12_RESOURCE_STATE_COPY_DEST;
  if (tiled_buffer_) {
    if (FAILED(device->CreateReservedResource(&buffer_desc, buffer_state_, nullptr,
                                              IID_PPV_ARGS(&buffer_)))) {
      REXGPU_ERROR("Shared memory: Failed to create the {} MB tiled buffer", kBufferSize >> 20);
      return false;
    }
    command_processor_.native_gpu_diagnostics().RecordAllocation(
        NativeGpuDiagnostics::Allocation::kMemoryMirror, kBufferSize, 0);
  } else {
    REXGPU_INFO(
        "Direct3D 12 tiled resources are not used for shared memory "
        "emulation - video memory usage may increase significantly "
        "because a full {} MB buffer will be created",
        kBufferSize >> 20);
    if (provider.GetGraphicsAnalysis()) {
      // As of October 8th, 2018, PIX doesn't support tiled buffers.
      // FIXME(Triang3l): Re-enable tiled resources with PIX once fixed.
      REXGPU_INFO(
          "This is caused by PIX being attached, which doesn't support tiled "
          "resources yet.");
    }
    if (FAILED(device->CreateCommittedResource(&ui::d3d12::util::kHeapPropertiesDefault,
                                               provider.GetHeapFlagCreateNotZeroed(), &buffer_desc,
                                               buffer_state_, nullptr, IID_PPV_ARGS(&buffer_)))) {
      REXGPU_ERROR("Shared memory: Failed to create the {} MB buffer", kBufferSize >> 20);
      return false;
    }
    command_processor_.native_gpu_diagnostics().RecordAllocation(
        NativeGpuDiagnostics::Allocation::kMemoryMirror, kBufferSize, kBufferSize);
  }
  buffer_gpu_address_ = buffer_->GetGPUVirtualAddress();
  buffer_uav_writes_commit_needed_ = false;

  UpdateBufferDescriptors();
  command_processor_.InvalidateSharedMemoryBindings();
  return true;
}

void D3D12SharedMemory::UpdateBufferDescriptors() {
  const auto& provider = command_processor_.GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  ui::d3d12::util::CreateBufferRawSRV(
      device,
      provider.OffsetViewDescriptor(buffer_descriptor_heap_start_,
                                    uint32_t(BufferDescriptorIndex::kRawSRV)),
      buffer_, kBufferSize);
  ui::d3d12::util::CreateBufferTypedSRV(
      device,
      provider.OffsetViewDescriptor(buffer_descriptor_heap_start_,
                                    uint32_t(BufferDescriptorIndex::kR32UintSRV)),
      buffer_, DXGI_FORMAT_R32_UINT, kBufferSize >> 2);
  ui::d3d12::util::CreateBufferTypedSRV(
      device,
      provider.OffsetViewDescriptor(buffer_descriptor_heap_start_,
                                    uint32_t(BufferDescriptorIndex::kR32G32UintSRV)),
      buffer_, DXGI_FORMAT_R32G32_UINT, kBufferSize >> 3);
  ui::d3d12::util::CreateBufferTypedSRV(
      device,
      provider.OffsetViewDescriptor(buffer_descriptor_heap_start_,
                                    uint32_t(BufferDescriptorIndex::kR32G32B32A32UintSRV)),
      buffer_, DXGI_FORMAT_R32G32B32A32_UINT, kBufferSize >> 4);
  ui::d3d12::util::CreateBufferRawUAV(
      device,
      provider.OffsetViewDescriptor(buffer_descriptor_heap_start_,
                                    uint32_t(BufferDescriptorIndex::kRawUAV)),
      buffer_, kBufferSize);
  ui::d3d12::util::CreateBufferTypedUAV(
      device,
      provider.OffsetViewDescriptor(buffer_descriptor_heap_start_,
                                    uint32_t(BufferDescriptorIndex::kR32UintUAV)),
      buffer_, DXGI_FORMAT_R32_UINT, kBufferSize >> 2);
  ui::d3d12::util::CreateBufferTypedUAV(
      device,
      provider.OffsetViewDescriptor(buffer_descriptor_heap_start_,
                                    uint32_t(BufferDescriptorIndex::kR32G32UintUAV)),
      buffer_, DXGI_FORMAT_R32G32_UINT, kBufferSize >> 3);
  ui::d3d12::util::CreateBufferTypedUAV(
      device,
      provider.OffsetViewDescriptor(buffer_descriptor_heap_start_,
                                    uint32_t(BufferDescriptorIndex::kR32G32B32A32UintUAV)),
      buffer_, DXGI_FORMAT_R32G32B32A32_UINT, kBufferSize >> 4);
}

void D3D12SharedMemory::Shutdown(bool from_destructor) {
  if (native_resolve_watch_) {
    UnregisterGlobalWatch(native_resolve_watch_);
    native_resolve_watch_ = nullptr;
  }
  native_resolve_pending_.reset();
  native_resolve_buffers_.clear();
  native_resolve_bytes_ = 0;
  ResetTraceDownload();

  upload_buffer_pool_.reset();

  ui::d3d12::util::ReleaseAndNull(buffer_descriptor_heap_);

  // First free the buffer to detach it from the heaps.
  ui::d3d12::util::ReleaseAndNull(buffer_);

  for (ID3D12Heap* heap : buffer_tiled_heaps_) {
    heap->Release();
  }
  buffer_tiled_heaps_.clear();

  // If calling from the destructor, the SharedMemory destructor will call
  // ShutdownCommon.
  if (!from_destructor) {
    ShutdownCommon();
  }
}

void D3D12SharedMemory::ClearCache() {
  // These are authoritative GPU pages, not disposable derived cache entries.
  // The common clear notifies consumers while preserving GPU-written flags.
  auto global_lock = native_resolve_region_.Acquire();
  native_resolve_clearing_cache_ = true;
  SharedMemory::ClearCache();
  native_resolve_clearing_cache_ = false;
  ReclaimNativeResolves();

  upload_buffer_pool_->ClearCache();
}

void D3D12SharedMemory::CompletedSubmissionUpdated() {
  upload_buffer_pool_->Reclaim(command_processor_.GetCompletedSubmission());
  ReclaimNativeResolves();
}

void D3D12SharedMemory::BeginSubmission() {
  // ExecuteCommandLists is a full UAV barrier.
  buffer_uav_writes_commit_needed_ = false;
}

void D3D12SharedMemory::NativeResolveInvalidation(const std::unique_lock<std::recursive_mutex>&,
                                                  void* context, uint32_t first, uint32_t last,
                                                  bool gpu) {
  auto& self = *static_cast<D3D12SharedMemory*>(context);
  if (gpu && self.native_resolve_clearing_cache_)
    return;
  for (const auto& buffer : self.native_resolve_buffers_) {
    if ((gpu && buffer == self.native_resolve_pending_) ||
        first >= buffer->address + buffer->length || last < buffer->address)
      continue;
    uint32_t begin = (std::max(first, buffer->address) - buffer->address) >> self.page_size_log2();
    uint32_t end = (std::min(last, buffer->address + buffer->length - 1) - buffer->address) >>
                   self.page_size_log2();
    std::fill(buffer->valid_pages.begin() + begin, buffer->valid_pages.begin() + end + 1, 0);
  }
}

void D3D12SharedMemory::InvalidateNativeResolves() {
  auto lock = native_resolve_region_.Acquire();
  for (const auto& buffer : native_resolve_buffers_)
    std::fill(buffer->valid_pages.begin(), buffer->valid_pages.end(), 0);
}

void D3D12SharedMemory::ReclaimNativeResolves() {
  auto lock = native_resolve_region_.Acquire();
  uint64_t completed = command_processor_.GetCompletedSubmission();
  std::erase_if(native_resolve_buffers_, [&](const NativeResolveHandle& buffer) {
    // Prepared outputs may be held for a later submission before their first
    // GPU publication. An external handle must keep their ownership tracked.
    if (buffer.use_count() != 1 || buffer == native_resolve_pending_ ||
        buffer->last_submission > completed ||
        std::any_of(buffer->valid_pages.begin(), buffer->valid_pages.end(),
                    [](uint8_t valid) { return valid != 0; }))
      return false;
    native_resolve_bytes_ -= buffer->length;
    return true;
  });
}

D3D12SharedMemory::NativeResolveHandle D3D12SharedMemory::FindNativeResolveRange(
    uint32_t address, uint32_t length) const {
  // The caller holds the common global critical region.
  if (!length || address >= kBufferSize || length > kBufferSize - address)
    return {};
  for (auto i = native_resolve_buffers_.rbegin(); i != native_resolve_buffers_.rend(); ++i) {
    const auto& buffer = *i;
    if (address < buffer->address || address + length > buffer->address + buffer->length)
      continue;
    uint32_t first = (address - buffer->address) >> page_size_log2();
    uint32_t last = (address + length - 1 - buffer->address) >> page_size_log2();
    if (std::all_of(buffer->valid_pages.begin() + first, buffer->valid_pages.begin() + last + 1,
                    [](uint8_t valid) { return valid != 0; }))
      return buffer;
  }
  return {};
}

bool D3D12SharedMemory::CanRetainNativeResolve(uint32_t address, uint32_t length) const {
  if (!length || address >= kBufferSize || length > kBufferSize - address)
    return false;
  uint32_t mask = (1u << page_size_log2()) - 1;
  return ((address + length + mask) & ~mask) - (address & ~mask) <= kMaximumNativeResolveBytes;
}

bool D3D12SharedMemory::CanReadNativeResolveRange(uint32_t address, uint32_t length) {
  if (!CanRetainNativeResolve(address, length))
    return false;
  auto lock = native_resolve_region_.Acquire();
  if (FindNativeResolveRange(address, length) || !IsRangeGpuWritten(address, length))
    return true;
  uint32_t page_size = 1u << page_size_log2();
  for (uint32_t page = address & ~(page_size - 1); page < address + length; page += page_size) {
    if (!FindNativeResolveRange(page, page_size) && IsRangeGpuWritten(page, page_size))
      return false;
  }
  return true;
}

void D3D12SharedMemory::TransitionNativeResolve(const NativeResolveHandle& buffer,
                                                D3D12_RESOURCE_STATES state) {
  buffer->last_submission = command_processor_.GetCurrentSubmission();
  if (command_processor_.PushTransitionBarrier(buffer->resource.Get(), buffer->state, state)) {
    buffer->uav_writes = false;
    buffer->state = state;
  } else if (buffer->uav_writes) {
    command_processor_.PushUAVBarrier(buffer->resource.Get());
    buffer->uav_writes = false;
  }
}

bool D3D12SharedMemory::PopulateNativeResolve(const NativeResolveHandle& buffer) {
  struct Source {
    NativeResolveHandle native;
    uint32_t address, length;
    bool gpu;
  };
  auto lock = native_resolve_region_.Acquire();
  std::vector<Source> sources;
  auto whole = FindNativeResolveRange(buffer->address, buffer->length);
  if (whole) {
    sources.push_back({std::move(whole), buffer->address, buffer->length, true});
  } else if (!IsRangeGpuWritten(buffer->address, buffer->length)) {
    sources.push_back({{}, buffer->address, buffer->length, false});
  } else {
    uint32_t page_size = 1u << page_size_log2();
    for (uint32_t address = buffer->address; address < buffer->address + buffer->length;
         address += page_size) {
      auto native = FindNativeResolveRange(address, page_size);
      bool gpu = native || IsRangeGpuWritten(address, page_size);
      if (!sources.empty() && sources.back().native == native && sources.back().gpu == gpu)
        sources.back().length += page_size;
      else
        sources.push_back({std::move(native), address, page_size, gpu});
    }
  }
  TransitionNativeResolve(buffer, D3D12_RESOURCE_STATE_COPY_DEST);
  auto& commands = command_processor_.GetDeferredCommandList();
  for (const auto& source : sources) {
    if (!source.gpu) {
      if (!WatchCpuMemoryRange(source.address, source.length))
        return false;
      uint32_t copied = 0;
      while (copied < source.length) {
        ID3D12Resource* upload;
        size_t offset, size;
        uint8_t* mapping = upload_buffer_pool_->RequestPartial(
            command_processor_.GetCurrentSubmission(), source.length - copied,
            size_t(1) << page_size_log2(), &upload, &offset, &size, nullptr);
        if (!mapping || !SnapshotCpuRange(source.address + copied, uint32_t(size), mapping))
          return false;
        command_processor_.SubmitBarriers();
        commands.D3DCopyBufferRegion(buffer->resource.Get(),
                                     source.address + copied - buffer->address, upload, offset,
                                     size);
        copied += uint32_t(size);
      }
      continue;
    }
    ID3D12Resource* input;
    uint32_t offset;
    if (source.native) {
      TransitionNativeResolve(source.native, D3D12_RESOURCE_STATE_COPY_SOURCE);
      input = source.native->resource.Get();
      offset = source.address - source.native->address;
    } else {
      if (!RequestRange(source.address, source.length))
        return false;
      CommitUAVWritesAndTransitionBuffer(D3D12_RESOURCE_STATE_COPY_SOURCE);
      input = buffer_;
      offset = source.address;
    }
    command_processor_.SubmitBarriers();
    commands.D3DCopyBufferRegion(buffer->resource.Get(), source.address - buffer->address, input,
                                 offset, source.length);
  }
  return true;
}

void D3D12SharedMemory::CompactNativeResolves(uint64_t target_bytes) {
  auto lock = native_resolve_region_.Acquire();
  uint32_t page_size = 1u << page_size_log2();
  const auto& provider = command_processor_.GetD3D12Provider();
  size_t count = native_resolve_buffers_.size();
  for (size_t i = 0; i < count && native_resolve_bytes_ > target_bytes; ++i) {
    if (native_resolve_buffers_[i].use_count() != 1 ||
        native_resolve_buffers_[i] == native_resolve_pending_)
      continue;
    auto old = native_resolve_buffers_[i];
    uint32_t live = uint32_t(std::count_if(old->valid_pages.begin(), old->valid_pages.end(),
                                           [](uint8_t valid) { return valid != 0; }));
    if (!live || live >= old->valid_pages.size() / 2)
      continue;
    std::vector<NativeResolveHandle> parts;
    for (uint32_t page = 0; page < old->valid_pages.size();) {
      if (!old->valid_pages[page]) {
        ++page;
        continue;
      }
      uint32_t first = page++;
      while (page < old->valid_pages.size() && old->valid_pages[page])
        ++page;
      auto part = std::make_shared<NativeResolveBuffer>();
      part->address = old->address + first * page_size;
      part->length = (page - first) * page_size;
      part->last_submission = command_processor_.GetCurrentSubmission();
      part->valid_pages.assign(page - first, 0);
      part->materialized_pages.assign(old->materialized_pages.begin() + first,
                                      old->materialized_pages.begin() + page);
      D3D12_RESOURCE_DESC description;
      ui::d3d12::util::FillBufferResourceDesc(description, part->length,
                                              D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
      if (FAILED(provider.GetDevice()->CreateCommittedResource(
              &ui::d3d12::util::kHeapPropertiesDefault, provider.GetHeapFlagCreateNotZeroed(),
              &description, part->state, nullptr, IID_PPV_ARGS(&part->resource))))
        return;  // No ownership changes until every copy has a destination.
      parts.push_back(std::move(part));
    }
    TransitionNativeResolve(old, D3D12_RESOURCE_STATE_COPY_SOURCE);
    command_processor_.SubmitBarriers();
    for (const auto& part : parts) {
      // Retain both sides until the copying submission completes, even if a
      // subsequent caller cannot allocate its new resource.
      native_resolve_buffers_.push_back(part);
      native_resolve_bytes_ += part->length;
      command_processor_.GetDeferredCommandList().D3DCopyBufferRegion(
          part->resource.Get(), 0, old->resource.Get(), part->address - old->address, part->length);
      std::fill(part->valid_pages.begin(), part->valid_pages.end(), 1);
    }
    std::fill(old->valid_pages.begin(), old->valid_pages.end(), 0);
    if (++native_resolve_compactions_ <= 8 || !(native_resolve_compactions_ & 255))
      REXGPU_INFO("[d3d12-native-compact] {} bytes retain {} live bytes in {} native resources",
                  old->length, live * page_size, parts.size());
  }
  ReclaimNativeResolves();
}

D3D12SharedMemory::NativeResolveHandle D3D12SharedMemory::PrepareNativeResolve(uint32_t address,
                                                                               uint32_t length) {
  if (!CanRetainNativeResolve(address, length))
    return {};
  uint32_t mask = (1u << page_size_log2()) - 1;
  uint32_t first = address & ~mask;
  uint32_t size = ((address + length + mask) & ~mask) - first;
  auto lock = native_resolve_region_.Acquire();
  auto buffer = FindNativeResolveRange(first, size);
  if (buffer && buffer->address == first && buffer->length == size)
    return buffer;
  ReclaimNativeResolves();
  uint64_t target = uint64_t(std::clamp(REXCVAR_GET(native_resolve_buffer_mb), 1, 512)) << 20;
  if (native_resolve_bytes_ + size > target)
    CompactNativeResolves(size < target ? target - size : 0);
  // Preserve live authoritative resources above the cache target. Fence-bound
  // old copies are reclaimed after submission completion, never via a mirror.
  buffer = std::make_shared<NativeResolveBuffer>();
  buffer->address = first;
  buffer->length = size;
  buffer->last_submission = command_processor_.GetCurrentSubmission();
  buffer->valid_pages.resize(size >> page_size_log2());
  buffer->materialized_pages.resize(size >> page_size_log2(), 1);
  D3D12_RESOURCE_DESC desc;
  ui::d3d12::util::FillBufferResourceDesc(desc, size, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  const auto& provider = command_processor_.GetD3D12Provider();
  if (FAILED(provider.GetDevice()->CreateCommittedResource(
          &ui::d3d12::util::kHeapPropertiesDefault, provider.GetHeapFlagCreateNotZeroed(), &desc,
          buffer->state, nullptr, IID_PPV_ARGS(&buffer->resource))))
    return {};
  // Register before recording copies: even a later allocation failure must
  // retain resources referenced by this submission until its fence completes.
  native_resolve_buffers_.push_back(buffer);
  native_resolve_bytes_ += size;
  if (!PopulateNativeResolve(buffer))
    return {};
  return buffer;
}

void D3D12SharedMemory::WriteNativeResolveUAV(const NativeResolveHandle& buffer,
                                              D3D12_CPU_DESCRIPTOR_HANDLE descriptor) {
  TransitionNativeResolve(buffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  ui::d3d12::util::CreateBufferTypedUAV(command_processor_.GetD3D12Provider().GetDevice(),
                                        descriptor, buffer->resource.Get(), DXGI_FORMAT_R32_UINT,
                                        buffer->length >> 2);
}

void D3D12SharedMemory::MarkNativeResolveUAVWrites(const NativeResolveHandle& buffer) {
  buffer->uav_writes = true;
}

void D3D12SharedMemory::WriteNativeResolveRawUAV(const NativeResolveHandle& buffer,
                                                 D3D12_CPU_DESCRIPTOR_HANDLE descriptor) {
  TransitionNativeResolve(buffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  ui::d3d12::util::CreateBufferRawUAV(command_processor_.GetD3D12Provider().GetDevice(), descriptor,
                                      buffer->resource.Get(), buffer->length);
}

void D3D12SharedMemory::BeginNativeResolveWrite(const NativeResolveHandle& buffer) {
  auto lock = native_resolve_region_.Acquire();
  native_resolve_pending_ = buffer;
  std::fill(buffer->valid_pages.begin(), buffer->valid_pages.end(), 1);
  std::fill(buffer->materialized_pages.begin(), buffer->materialized_pages.end(), 0);
  if (++native_resolve_writes_ <= 8 || !(native_resolve_writes_ & 4095)) {
    REXGPU_INFO("[d3d12-native-memory] {} resolves retained in {} KiB of resource-local storage",
                native_resolve_writes_, native_resolve_bytes_ >> 10);
  }
}

bool D3D12SharedMemory::EndNativeResolveWrite() {
  auto lock = native_resolve_region_.Acquire();
  // The common tracker orders CPU and GPU publication. Keep GPU-winning pages
  // in the native resource and retire CPU-winning pages and CPU-only preimages.
  if (native_resolve_pending_) {
    const auto& buffer = native_resolve_pending_;
    if (!GetGpuWrittenPages(buffer->address, buffer->valid_pages))
      return false;
  }
  native_resolve_pending_.reset();
  return true;
}

bool D3D12SharedMemory::FlushGpuWrittenRange(uint32_t start, uint32_t length, bool writing) {
  if (!length)
    return true;
  auto lock = native_resolve_region_.Acquire();
  if (writing && native_resolve_pending_ && start >= native_resolve_pending_->address &&
      uint64_t(start) + length <=
          uint64_t(native_resolve_pending_->address) + native_resolve_pending_->length)
    return true;
  for (const auto& buffer : native_resolve_buffers_) {
    if (start >= buffer->address + buffer->length || uint64_t(start) + length <= buffer->address)
      continue;
    uint32_t first = (std::max(start, buffer->address) - buffer->address) >> page_size_log2();
    uint32_t end =
        uint32_t(std::min(uint64_t(start) + length, uint64_t(buffer->address) + buffer->length) -
                 buffer->address) >>
        page_size_log2();
    // Include a partially requested final page too.
    if ((std::min(uint64_t(start) + length, uint64_t(buffer->address) + buffer->length) -
         buffer->address) &
        ((1u << page_size_log2()) - 1))
      ++end;
    for (uint32_t page = first; page < end;) {
      if (!buffer->valid_pages[page] || buffer->materialized_pages[page]) {
        ++page;
        continue;
      }
      if (writing) {
        REXGPU_ERROR("D3D12 GPU write did not request pending native resolve pages");
        return false;
      }
      uint32_t begin = page++;
      while (page < end && buffer->valid_pages[page] && !buffer->materialized_pages[page])
        ++page;
      uint32_t offset = begin << page_size_log2(), size = (page - begin) << page_size_log2();
      if (!EnsureHostGpuMemoryAllocated(buffer->address + offset, size))
        return false;
      TransitionNativeResolve(buffer, D3D12_RESOURCE_STATE_COPY_SOURCE);
      CommitUAVWritesAndTransitionBuffer(D3D12_RESOURCE_STATE_COPY_DEST);
      command_processor_.SubmitBarriers();
      command_processor_.GetDeferredCommandList().D3DCopyBufferRegion(
          buffer_, buffer->address + offset, buffer->resource.Get(), offset, size);
      std::fill(buffer->materialized_pages.begin() + begin,
                buffer->materialized_pages.begin() + page, 1);
    }
  }
  return true;
}

void D3D12SharedMemory::UseAsCopySource() {
  if (!FlushGpuWrittenRange(0, kBufferSize)) {
    REXGPU_ERROR("D3D12 native resolves could not be made available for readback");
    return;
  }
  CommitUAVWritesAndTransitionBuffer(D3D12_RESOURCE_STATE_COPY_SOURCE);
}

D3D12SharedMemory::NativeResolveHandle D3D12SharedMemory::GetNativeResolveCopySource(
    uint32_t address, uint32_t length) {
  auto lock = native_resolve_region_.Acquire();
  auto buffer = FindNativeResolveRange(address, length);
  if (buffer) {
    TransitionNativeResolve(buffer, D3D12_RESOURCE_STATE_COPY_SOURCE);
  }
  return buffer;
}

D3D12SharedMemory::NativeResolveHandle D3D12SharedMemory::ComposeNativeResolveCopySource(
    uint32_t address, uint32_t length) {
  auto lock = native_resolve_region_.Acquire();
  if (!CanReadNativeResolveRange(address, length))
    return {};
  auto buffer = FindNativeResolveRange(address, length);
  if (!buffer)
    buffer = PrepareNativeResolve(address, length);
  if (buffer)
    TransitionNativeResolve(buffer, D3D12_RESOURCE_STATE_COPY_SOURCE);
  return buffer;
}

bool D3D12SharedMemory::WriteNativeResolveSRV(uint32_t address, uint32_t length,
                                              uint32_t element_size_log2,
                                              D3D12_CPU_DESCRIPTOR_HANDLE descriptor) {
  if (element_size_log2 < 2 || element_size_log2 > 4 || (address & ((1u << element_size_log2) - 1)))
    return false;
  auto lock = native_resolve_region_.Acquire();
  auto buffer = FindNativeResolveRange(address, length);
  if (!buffer)
    buffer = PrepareNativeResolve(address, length);
  const auto& provider = command_processor_.GetD3D12Provider();
  DXGI_FORMAT format = ui::d3d12::util::GetUintPow2DXGIFormat(element_size_log2);
  if (!buffer) {
    // Allocation pressure must preserve the image. Bind a relative view of the
    // requested compatibility range and report that dependency accurately.
    if (!RequestRange(address, length))
      return false;
    UseForReading();
    ui::d3d12::util::CreateBufferTypedSRV(provider.GetDevice(), descriptor, buffer_, format,
                                          length >> element_size_log2,
                                          address >> element_size_log2);
    command_processor_.native_gpu_diagnostics().RecordDependency(
        NativeGpuDiagnostics::Dependency::kTextureFallback, length,
        "D3D12 native texture input exceeded its storage budget");
    return true;
  }
  TransitionNativeResolve(buffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  uint32_t offset = address - buffer->address;
  ui::d3d12::util::CreateBufferTypedSRV(provider.GetDevice(), descriptor, buffer->resource.Get(),
                                        format, (buffer->length - offset) >> element_size_log2,
                                        offset >> element_size_log2);
  command_processor_.native_gpu_diagnostics().RecordOperation(
      NativeGpuDiagnostics::Operation::kGpuTextureUpload);
  return true;
}

void D3D12SharedMemory::CommitUAVWritesAndTransitionBuffer(D3D12_RESOURCE_STATES new_state) {
  if (!EnsureBuffer()) {
    throw std::runtime_error("Could not allocate D3D12 compatibility memory");
  }
  command_processor_.native_gpu_diagnostics().RecordDependency(
      NativeGpuDiagnostics::Dependency::kMemoryMirror, 0, "use D3D12 physical-address mirror");
  if (buffer_state_ == new_state) {
    if (new_state == D3D12_RESOURCE_STATE_UNORDERED_ACCESS && buffer_uav_writes_commit_needed_) {
      command_processor_.PushUAVBarrier(buffer_);
      buffer_uav_writes_commit_needed_ = false;
    }
    return;
  }
  command_processor_.PushTransitionBarrier(buffer_, buffer_state_, new_state);
  buffer_state_ = new_state;
  // "UAV -> anything" transition commits the writes implicitly.
  buffer_uav_writes_commit_needed_ = false;
}

void D3D12SharedMemory::WriteRawSRVDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE handle) {
  const ui::d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  device->CopyDescriptorsSimple(
      1, handle,
      provider.OffsetViewDescriptor(buffer_descriptor_heap_start_,
                                    uint32_t(BufferDescriptorIndex::kRawSRV)),
      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

void D3D12SharedMemory::WriteRawUAVDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE handle) {
  const ui::d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  device->CopyDescriptorsSimple(
      1, handle,
      provider.OffsetViewDescriptor(buffer_descriptor_heap_start_,
                                    uint32_t(BufferDescriptorIndex::kRawUAV)),
      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

void D3D12SharedMemory::WriteUintPow2SRVDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE handle,
                                                   uint32_t element_size_bytes_pow2) {
  BufferDescriptorIndex descriptor_index;
  switch (element_size_bytes_pow2) {
    case 2:
      descriptor_index = BufferDescriptorIndex::kR32UintSRV;
      break;
    case 3:
      descriptor_index = BufferDescriptorIndex::kR32G32UintSRV;
      break;
    case 4:
      descriptor_index = BufferDescriptorIndex::kR32G32B32A32UintSRV;
      break;
    default:
      assert_unhandled_case(element_size_bytes_pow2);
      return;
  }
  const ui::d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  device->CopyDescriptorsSimple(
      1, handle,
      provider.OffsetViewDescriptor(buffer_descriptor_heap_start_, uint32_t(descriptor_index)),
      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

void D3D12SharedMemory::WriteUintPow2UAVDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE handle,
                                                   uint32_t element_size_bytes_pow2) {
  BufferDescriptorIndex descriptor_index;
  switch (element_size_bytes_pow2) {
    case 2:
      descriptor_index = BufferDescriptorIndex::kR32UintUAV;
      break;
    case 3:
      descriptor_index = BufferDescriptorIndex::kR32G32UintUAV;
      break;
    case 4:
      descriptor_index = BufferDescriptorIndex::kR32G32B32A32UintUAV;
      break;
    default:
      assert_unhandled_case(element_size_bytes_pow2);
      return;
  }
  const ui::d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  device->CopyDescriptorsSimple(
      1, handle,
      provider.OffsetViewDescriptor(buffer_descriptor_heap_start_, uint32_t(descriptor_index)),
      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

bool D3D12SharedMemory::InitializeTraceSubmitDownloads() {
  ResetTraceDownload();
  PrepareForTraceDownload();
  uint32_t download_page_count = trace_download_page_count();
  if (!download_page_count) {
    return false;
  }
  D3D12_RESOURCE_DESC download_buffer_desc;
  ui::d3d12::util::FillBufferResourceDesc(
      download_buffer_desc, download_page_count << page_size_log2(), D3D12_RESOURCE_FLAG_NONE);
  const ui::d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  if (FAILED(device->CreateCommittedResource(&ui::d3d12::util::kHeapPropertiesReadback,
                                             provider.GetHeapFlagCreateNotZeroed(),
                                             &download_buffer_desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                             nullptr, IID_PPV_ARGS(&trace_download_buffer_)))) {
    REXGPU_ERROR(
        "Shared memory: Failed to create a {} KB GPU-written memory download "
        "buffer for frame tracing",
        download_page_count << page_size_log2() >> 10);
    ResetTraceDownload();
    return false;
  }
  auto& command_list = command_processor_.GetDeferredCommandList();
  UseAsCopySource();
  command_processor_.SubmitBarriers();
  uint32_t download_buffer_offset = 0;
  for (const auto& download_range : trace_download_ranges()) {
    command_list.D3DCopyBufferRegion(trace_download_buffer_, download_buffer_offset, buffer_,
                                     download_range.first, download_range.second);
    download_buffer_offset += download_range.second;
  }
  return true;
}

void D3D12SharedMemory::InitializeTraceCompleteDownloads() {
  if (!trace_download_buffer_) {
    return;
  }
  void* download_mapping;
  if (SUCCEEDED(trace_download_buffer_->Map(0, nullptr, &download_mapping))) {
    uint32_t download_buffer_offset = 0;
    for (const auto& download_range : trace_download_ranges()) {
      trace_writer_.WriteMemoryRead(
          download_range.first, download_range.second,
          reinterpret_cast<const uint8_t*>(download_mapping) + download_buffer_offset);
    }
    D3D12_RANGE download_write_range = {};
    trace_download_buffer_->Unmap(0, &download_write_range);
  } else {
    REXGPU_ERROR(
        "Shared memory: Failed to map the GPU-written memory download buffer "
        "for frame tracing");
  }
  ResetTraceDownload();
}

void D3D12SharedMemory::ResetTraceDownload() {
  ui::d3d12::util::ReleaseAndNull(trace_download_buffer_);
  ReleaseTraceDownloadRanges();
}

bool D3D12SharedMemory::AllocateSparseHostGpuMemoryRange(uint32_t offset_allocations,
                                                         uint32_t length_allocations) {
  if (!EnsureBuffer()) {
    return false;
  }
  if (!length_allocations) {
    return true;
  }

  uint32_t offset_bytes = offset_allocations << host_gpu_memory_sparse_granularity_log2();
  uint32_t length_bytes = length_allocations << host_gpu_memory_sparse_granularity_log2();

  const ui::d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  ID3D12CommandQueue* direct_queue = provider.GetDirectQueue();

  D3D12_HEAP_DESC heap_desc = {};
  heap_desc.SizeInBytes = length_bytes;
  heap_desc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
  heap_desc.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS | provider.GetHeapFlagCreateNotZeroed();
  ID3D12Heap* heap;
  if (FAILED(device->CreateHeap(&heap_desc, IID_PPV_ARGS(&heap)))) {
    REXGPU_ERROR("Shared memory: Failed to create a tile heap");
    return false;
  }
  buffer_tiled_heaps_.push_back(heap);
  command_processor_.native_gpu_diagnostics().RecordAllocation(
      NativeGpuDiagnostics::Allocation::kMemoryMirror, 0, length_bytes);

  D3D12_TILED_RESOURCE_COORDINATE region_start_coordinates;
  region_start_coordinates.X = offset_bytes / D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES;
  region_start_coordinates.Y = 0;
  region_start_coordinates.Z = 0;
  region_start_coordinates.Subresource = 0;
  D3D12_TILE_REGION_SIZE region_size;
  region_size.NumTiles = length_bytes / D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES;
  region_size.UseBox = FALSE;
  D3D12_TILE_RANGE_FLAGS range_flags = D3D12_TILE_RANGE_FLAG_NONE;
  UINT heap_range_start_offset = 0;
  direct_queue->UpdateTileMappings(buffer_, 1, &region_start_coordinates, &region_size, heap, 1,
                                   &range_flags, &heap_range_start_offset, &region_size.NumTiles,
                                   D3D12_TILE_MAPPING_FLAG_NONE);
  command_processor_.NotifyQueueOperationsDoneDirectly();
  return true;
}

bool D3D12SharedMemory::UploadRanges(
    const std::vector<std::pair<uint32_t, uint32_t>>& upload_page_ranges) {
  if (upload_page_ranges.empty()) {
    return true;
  }
  CommitUAVWritesAndTransitionBuffer(D3D12_RESOURCE_STATE_COPY_DEST);
  command_processor_.SubmitBarriers();
  auto& command_list = command_processor_.GetDeferredCommandList();
  for (auto upload_range : upload_page_ranges) {
    uint32_t upload_range_start = upload_range.first;
    uint32_t upload_range_length = upload_range.second;
    trace_writer_.WriteMemoryRead(upload_range_start << page_size_log2(),
                                  upload_range_length << page_size_log2());
    while (upload_range_length != 0) {
      ID3D12Resource* upload_buffer;
      size_t upload_buffer_offset, upload_buffer_size;
      uint8_t* upload_buffer_mapping = upload_buffer_pool_->RequestPartial(
          command_processor_.GetCurrentSubmission(), upload_range_length << page_size_log2(),
          size_t(1) << page_size_log2(), &upload_buffer, &upload_buffer_offset, &upload_buffer_size,
          nullptr);
      if (upload_buffer_mapping == nullptr) {
        REXGPU_ERROR("Shared memory: Failed to get an upload buffer");
        return false;
      }
      MakeRangeValid(upload_range_start << page_size_log2(), uint32_t(upload_buffer_size), false);
      std::memcpy(upload_buffer_mapping,
                  memory().TranslatePhysical(upload_range_start << page_size_log2()),
                  upload_buffer_size);
      command_list.D3DCopyBufferRegion(buffer_, upload_range_start << page_size_log2(),
                                       upload_buffer, UINT64(upload_buffer_offset),
                                       UINT64(upload_buffer_size));
      uint32_t upload_buffer_pages = uint32_t(upload_buffer_size >> page_size_log2());
      upload_range_start += upload_buffer_pages;
      upload_range_length -= upload_buffer_pages;
    }
  }
  return true;
}

}  // namespace rex::graphics::d3d12
