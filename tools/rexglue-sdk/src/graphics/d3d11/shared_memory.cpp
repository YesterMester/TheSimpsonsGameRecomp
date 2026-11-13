#include <rex/graphics/d3d11/shared_memory.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include <rex/graphics/d3d11/profile.h>
#include <rex/graphics/flags.h>
#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(d3d11_lazy_memory_mirror, true, "GPU/D3D11",
                    "Allocate the guest-address compatibility buffer only when a draw or resolve "
                    "needs it; native resource uploads do not allocate it");

namespace rex::graphics::d3d11 {
namespace {
constexpr uint64_t kMaximumNativeResolveBytes = 128u << 20;
}

D3D11SharedMemory::~D3D11SharedMemory() {
  // Common watches are released by the base destructor after texture and
  // primitive caches have unregistered their callbacks.
  ResetTraceDownload();
  if (native_resolve_watch_)
    UnregisterGlobalWatch(native_resolve_watch_);
}

bool D3D11SharedMemory::Initialize(std::string& error) {
  error.clear();
  if (buffer_ || common_initialized_) {
    error = "DX11 shared memory is already initialized";
    return false;
  }
  InitializeCommon();
  common_initialized_ = true;
  native_resolve_watch_ = RegisterGlobalWatch(NativeResolveInvalidation, this);
  streamed_page_shadows_supported_ = true;
  upload_snapshot_.resize(std::max(4u * 1024 * 1024, 1u << page_size_log2()));
  if (!REXCVAR_GET(d3d11_lazy_memory_mirror) && !EnsureHostBuffer()) {
    error = "DX11 physical-address buffer creation failed";
    Shutdown();
    return false;
  }
  return true;
}

bool D3D11SharedMemory::EnsureHostBuffer() {
  if (buffer_)
    return true;
  if (diagnostics_) {
    diagnostics_->RecordDependency(NativeGpuDiagnostics::Dependency::kMemoryMirror, kBufferSize,
                                   "allocate the Direct3D 11 guest-address buffer");
  }
  D3D11_BUFFER_DESC desc = {};
  desc.ByteWidth = kBufferSize;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags =
      D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_INDEX_BUFFER;
  desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
  auto* device = device_.device();
  HRESULT result = device->CreateBuffer(&desc, nullptr, buffer_.GetAddressOf());
  static constexpr DXGI_FORMAT formats[] = {DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_R32_UINT,
                                            DXGI_FORMAT_R32G32_UINT, DXGI_FORMAT_R32G32B32A32_UINT};
  for (uint32_t i = 0; i < 4 && SUCCEEDED(result); ++i) {
    D3D11_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Format = formats[i];
    srv.ViewDimension = i ? D3D11_SRV_DIMENSION_BUFFER : D3D11_SRV_DIMENSION_BUFFEREX;
    uint32_t element_size = i ? 1u << (i + 1) : 4;
    srv.BufferEx.NumElements = kBufferSize / element_size;
    srv.BufferEx.Flags = i ? 0 : D3D11_BUFFEREX_SRV_FLAG_RAW;
    result = device->CreateShaderResourceView(buffer_.Get(), &srv, srvs_[i].GetAddressOf());
    if (FAILED(result))
      break;
    D3D11_UNORDERED_ACCESS_VIEW_DESC uav = {};
    uav.Format = formats[i];
    uav.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    uav.Buffer.NumElements = kBufferSize / element_size;
    uav.Buffer.Flags = i ? 0 : D3D11_BUFFER_UAV_FLAG_RAW;
    result = device->CreateUnorderedAccessView(buffer_.Get(), &uav, uavs_[i].GetAddressOf());
  }
  if (FAILED(result)) {
    char message[128];
    std::snprintf(message, sizeof(message), "DX11 physical-address buffer creation failed (0x%08X)",
                  unsigned(result));
    REXGPU_ERROR("{}", message);
    srvs_ = {};
    uavs_ = {};
    buffer_.Reset();
    return false;
  }
  draw_context_.Invalidate();
  const uint32_t zero[4] = {};
  device_.context()->ClearUnorderedAccessViewUint(uavs_[0].Get(), zero);
  if (diagnostics_) {
    diagnostics_->RecordAllocation(NativeGpuDiagnostics::Allocation::kMemoryMirror, kBufferSize,
                                   kBufferSize);
  }
  return true;
}

bool D3D11SharedMemory::CopyCpuRange(uint32_t address, std::span<uint8_t> destination) {
  if (destination.empty() || address >= kBufferSize || destination.size() > kBufferSize - address ||
      IsRangeGpuWritten(address, uint32_t(destination.size())))
    return false;
  std::memcpy(destination.data(), memory().TranslatePhysical<const void*>(address),
              destination.size());
  trace_writer_.WriteMemoryRead(address, uint32_t(destination.size()), destination.data());
  return true;
}

std::span<const uint8_t> D3D11SharedMemory::BorrowCpuRange(uint32_t address, uint32_t length) {
  if (!length || address >= kBufferSize || length > kBufferSize - address ||
      IsRangeGpuWritten(address, length))
    return {};
  return {memory().TranslatePhysical<const uint8_t*>(address), length};
}

void D3D11SharedMemory::TraceCpuRangeRead(uint32_t address, std::span<const uint8_t> snapshot) {
  trace_writer_.WriteMemoryRead(address, uint32_t(snapshot.size()), snapshot.data());
}

void D3D11SharedMemory::NativeResolveInvalidation(const std::unique_lock<std::recursive_mutex>&,
                                                  void* context, uint32_t first, uint32_t last,
                                                  bool gpu) {
  auto& self = *static_cast<D3D11SharedMemory*>(context);
  if (gpu && self.native_resolve_clearing_cache_)
    return;
  std::lock_guard lock(self.native_resolve_mutex_);
  for (const auto& buffer : self.native_resolve_buffers_) {
    if (gpu && buffer == self.native_resolve_pending_)
      continue;
    if (first >= buffer->address + buffer->length || last < buffer->address)
      continue;
    uint32_t begin = (std::max(first, buffer->address) - buffer->address) >> self.page_size_log2();
    uint32_t end = (std::min(last, buffer->address + buffer->length - 1) - buffer->address) >>
                   self.page_size_log2();
    std::fill(buffer->valid_pages.begin() + begin, buffer->valid_pages.begin() + end + 1, 0);
  }
}

D3D11SharedMemory::NativeResolveHandle D3D11SharedMemory::FindNativeResolveRange(
    uint32_t address, uint32_t length) const {
  // The caller holds native_resolve_mutex_. Do not acquire the common watch
  // lock here: memory invalidation enters this mutex from that lock.
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

bool D3D11SharedMemory::HasNativeResolveRange(uint32_t address, uint32_t length) const {
  std::lock_guard lock(native_resolve_mutex_);
  return bool(FindNativeResolveRange(address, length));
}

bool D3D11SharedMemory::CanReadNativeResolveRange(uint32_t address, uint32_t length) {
  if (!CanRetainNativeResolve(address, length))
    return false;
  auto global_lock = thread::global_critical_region::AcquireDirect();
  if (HasNativeResolveRange(address, length))
    return true;
  if (!IsRangeGpuWritten(address, length))
    return false;
  uint32_t page_size = 1u << page_size_log2();
  uint32_t first = address & ~(page_size - 1);
  bool native = false;
  for (uint32_t page = first; page < address + length; page += page_size) {
    if (HasNativeResolveRange(page, page_size)) {
      native = true;
    } else if (IsRangeGpuWritten(page, page_size)) {
      return false;
    }
  }
  return native;
}

bool D3D11SharedMemory::CanRetainNativeResolve(uint32_t address, uint32_t length) const {
  if (!length || address >= kBufferSize || length > kBufferSize - address)
    return false;
  uint32_t page_mask = (1u << page_size_log2()) - 1;
  uint32_t size = ((address + length + page_mask) & ~page_mask) - (address & ~page_mask);
  return size <= kMaximumNativeResolveBytes;
}

bool D3D11SharedMemory::PopulateNativeResolve(const NativeResolveHandle& buffer,
                                              std::string& error) {
  struct Source {
    NativeResolveHandle native;
    uint32_t address, length;
    bool gpu;
  };
  // Keep the common page flags and native owners from changing while selecting
  // and copying a preimage. The callback takes these locks in the same order.
  // No common memory call may enter while only native_resolve_mutex_ is held.
  auto global_lock = thread::global_critical_region::AcquireDirect();
  std::vector<Source> sources;
  NativeResolveHandle whole;
  {
    std::lock_guard lock(native_resolve_mutex_);
    whole = FindNativeResolveRange(buffer->address, buffer->length);
  }
  if (whole) {
    sources.push_back({std::move(whole), buffer->address, buffer->length, true});
  } else if (!IsRangeGpuWritten(buffer->address, buffer->length)) {
    sources.push_back({{}, buffer->address, buffer->length, false});
  } else {
    uint32_t page_size = 1u << page_size_log2();
    for (uint32_t address = buffer->address; address < buffer->address + buffer->length;
         address += page_size) {
      NativeResolveHandle native;
      {
        std::lock_guard lock(native_resolve_mutex_);
        native = FindNativeResolveRange(address, page_size);
      }
      bool gpu = native || IsRangeGpuWritten(address, page_size);
      if (!sources.empty() && sources.back().native == native && sources.back().gpu == gpu) {
        sources.back().length += page_size;
      } else {
        sources.push_back({std::move(native), address, page_size, gpu});
      }
    }
  }
  draw_context_.Invalidate();
  std::vector<uint8_t> snapshot;
  uint32_t native_parts = 0, cpu_parts = 0, compatibility_parts = 0;
  for (const auto& source : sources) {
    uint32_t destination = source.address - buffer->address;
    if (!source.gpu) {
      // Protect before taking the CPU snapshot without declaring the mirror
      // current: a texture input may never be published as a GPU-written page.
      if (!WatchCpuMemoryRange(source.address, source.length)) {
        error = "Unable to watch original-resolution native resolve padding";
        return false;
      }
      snapshot.resize(source.length);
      if (!CopyCpuRange(source.address, snapshot)) {
        error = "Original-resolution native resolve padding changed ownership";
        return false;
      }
      D3D11_BOX box = {destination, 0, 0, destination + source.length, 1, 1};
      device_.context()->UpdateSubresource(buffer->buffer.Get(), 0, &box, snapshot.data(), 0, 0);
      ++cpu_parts;
      continue;
    }
    ID3D11Buffer* input;
    uint32_t offset;
    if (source.native) {
      input = source.native->buffer.Get();
      offset = source.address - source.native->address;
      ++native_parts;
    } else {
      // A GPU writer without a retained native version still uses the proven
      // compatibility path, but unrelated native or CPU pages stay out of it.
      if (!RequestRange(source.address, source.length)) {
        error = "Unable to preserve original-resolution native resolve padding";
        return false;
      }
      input = buffer_.Get();
      offset = source.address;
      ++compatibility_parts;
    }
    D3D11_BOX box = {offset, 0, 0, offset + source.length, 1, 1};
    device_.context()->CopySubresourceRegion(buffer->buffer.Get(), 0, destination, 0, 0, input, 0,
                                             &box);
  }
  if (FAILED(device_.device()->GetDeviceRemovedReason())) {
    error = "The Direct3D 11 device was removed while preserving native resolve padding";
    return false;
  }
  if (native_parts && (++native_resolve_preimages_ <= 8 || !(native_resolve_preimages_ & 255))) {
    REXGPU_INFO(
        "[d3d11-native-preimage] {} bytes at {:08X}: {} native parts, {} CPU parts, {} "
        "compatibility parts ({})",
        buffer->length, buffer->address, native_parts, cpu_parts, compatibility_parts,
        native_resolve_preimages_);
  }
  return true;
}

void D3D11SharedMemory::CompactNativeResolves(uint64_t target_bytes) {
  auto global_lock = thread::global_critical_region::AcquireDirect();
  std::lock_guard lock(native_resolve_mutex_);
  uint32_t page_size = 1u << page_size_log2();
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
      part->valid_pages.assign(page - first, 1);
      part->materialized_pages.assign(old->materialized_pages.begin() + first,
                                      old->materialized_pages.begin() + page);
      D3D11_BUFFER_DESC description = {};
      description.ByteWidth = part->length;
      description.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
      description.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
      if (FAILED(
              device_.device()->CreateBuffer(&description, nullptr, part->buffer.GetAddressOf()))) {
        std::erase(native_resolve_buffers_, NativeResolveHandle());
        return;  // The original authoritative version remains intact.
      }
      parts.push_back(std::move(part));
    }
    draw_context_.Invalidate();
    for (const auto& part : parts) {
      uint32_t offset = part->address - old->address;
      D3D11_BOX box = {offset, 0, 0, offset + part->length, 1, 1};
      device_.context()->CopySubresourceRegion(part->buffer.Get(), 0, 0, 0, 0, old->buffer.Get(), 0,
                                               &box);
      native_resolve_buffers_.push_back(part);
      native_resolve_bytes_ += part->length;
    }
    std::fill(old->valid_pages.begin(), old->valid_pages.end(), 0);
    native_resolve_bytes_ -= old->length;
    // Immediate-context commands retain the copied resources through execution.
    native_resolve_buffers_[i].reset();
    if (++native_resolve_compactions_ <= 8 || !(native_resolve_compactions_ & 255))
      REXGPU_INFO("[d3d11-native-compact] {} bytes retain {} live bytes in {} native resources",
                  old->length, live * page_size, parts.size());
  }
  std::erase(native_resolve_buffers_, NativeResolveHandle());
}

D3D11SharedMemory::NativeResolveHandle D3D11SharedMemory::PrepareNativeResolve(
    uint32_t address, uint32_t length, uint32_t element_size_log2, std::string& error) {
  error.clear();
  if (!length || address >= kBufferSize || length > kBufferSize - address ||
      element_size_log2 < 2 || element_size_log2 > 4) {
    error = "Invalid original-resolution native resolve range";
    return {};
  }
  const uint32_t page_mask = (1u << page_size_log2()) - 1;
  uint32_t first = address & ~page_mask;
  uint32_t size = ((address + length + page_mask) & ~page_mask) - first;
  if (size > kMaximumNativeResolveBytes) {
    error = "Original-resolution native resolve exceeds its storage limit";
    return {};
  }
  NativeResolveHandle buffer;
  {
    std::lock_guard lock(native_resolve_mutex_);
    buffer = FindNativeResolveRange(first, size);
    if (buffer && (buffer->address != first || buffer->length != size))
      buffer.reset();
    // A caller may have prepared a version for a later write, without any
    // valid pages yet. Keep its tracked ownership while that handle is held.
    // Context commands retain completed versions' resources independently.
    std::erase_if(native_resolve_buffers_, [&](const NativeResolveHandle& old) {
      if (old.use_count() != 1 || std::any_of(old->valid_pages.begin(), old->valid_pages.end(),
                                              [](uint8_t valid) { return valid != 0; }))
        return false;
      native_resolve_bytes_ -= old->length;
      return true;
    });
  }
  if (!buffer) {
    uint64_t target = uint64_t(std::clamp(REXCVAR_GET(native_resolve_buffer_mb), 1, 512)) << 20;
    if (native_resolve_bytes_ + size > target)
      CompactNativeResolves(size < target ? target - size : 0);
    // Live GPU versions are authoritative data, not disposable cache entries.
    // Retain them above the cache target instead of allocating a memory mirror.
    buffer = std::make_shared<NativeResolveBuffer>();
    buffer->address = first;
    buffer->length = size;
    buffer->valid_pages.resize(size >> page_size_log2());
    buffer->materialized_pages.resize(size >> page_size_log2(), 1);
    D3D11_BUFFER_DESC description = {};
    description.ByteWidth = size;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    description.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    if (FAILED(
            device_.device()->CreateBuffer(&description, nullptr, buffer->buffer.GetAddressOf()))) {
      error = "Unable to allocate original-resolution native resolve storage";
      return {};
    }
    if (!PopulateNativeResolve(buffer, error))
      return {};
    std::lock_guard lock(native_resolve_mutex_);
    native_resolve_buffers_.push_back(buffer);
    native_resolve_bytes_ += size;
  }
  auto& destination = buffer->writes[element_size_log2 - 2];
  if (!destination) {
    constexpr DXGI_FORMAT formats[] = {DXGI_FORMAT_R32_UINT, DXGI_FORMAT_R32G32_UINT,
                                       DXGI_FORMAT_R32G32B32A32_UINT};
    D3D11_UNORDERED_ACCESS_VIEW_DESC view = {};
    view.Format = formats[element_size_log2 - 2];
    view.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    view.Buffer.NumElements = buffer->length >> element_size_log2;
    if (FAILED(device_.device()->CreateUnorderedAccessView(buffer->buffer.Get(), &view,
                                                           destination.GetAddressOf()))) {
      error = "Unable to create original-resolution native resolve output";
      return {};
    }
  }
  return buffer;
}

void D3D11SharedMemory::BeginNativeResolveWrite(const NativeResolveHandle& buffer) {
  std::lock_guard lock(native_resolve_mutex_);
  native_resolve_pending_ = buffer;
  std::fill(buffer->valid_pages.begin(), buffer->valid_pages.end(), 1);
  std::fill(buffer->materialized_pages.begin(), buffer->materialized_pages.end(), 0);
  if (++native_resolve_writes_ <= 8 || !(native_resolve_writes_ & 255)) {
    REXGPU_INFO("[d3d11-native-resolve] {} writes, {} KiB retained without eager mirror copies",
                native_resolve_writes_, native_resolve_bytes_ >> 10);
  }
}

bool D3D11SharedMemory::EndNativeResolveWrite() {
  auto global_lock = thread::global_critical_region::AcquireDirect();
  std::lock_guard lock(native_resolve_mutex_);
  // The common tracker decides which publication won. Re-attach GPU-winning
  // pages to their native resource; a later CPU-winning page stays invalid.
  // CPU-only preimages are inputs to this draw, not authoritative future owners.
  if (native_resolve_pending_) {
    const auto& buffer = native_resolve_pending_;
    if (!GetGpuWrittenPages(buffer->address, buffer->valid_pages))
      return false;
  }
  native_resolve_pending_.reset();
  return SUCCEEDED(device_.device()->GetDeviceRemovedReason());
}

Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> D3D11SharedMemory::ReadNativeResolveRange(
    uint32_t address, uint32_t length, uint32_t element_size_log2, std::string& error) {
  error.clear();
  if (element_size_log2 < 2 || element_size_log2 > 4 ||
      (address & ((1u << element_size_log2) - 1))) {
    error = "Invalid native resolve input alignment";
    return {};
  }
  auto global_lock = thread::global_critical_region::AcquireDirect();
  NativeResolveHandle buffer;
  {
    std::lock_guard lock(native_resolve_mutex_);
    buffer = FindNativeResolveRange(address, length);
  }
  if (!buffer) {
    if (!CanReadNativeResolveRange(address, length)) {
      error = "Native resolve input no longer has proven GPU owners and CPU gaps";
      return {};
    }
    buffer = PrepareNativeResolve(address, length, element_size_log2, error);
    if (!buffer)
      return {};
  }
  std::lock_guard lock(native_resolve_mutex_);
  uint32_t offset = address - buffer->address;
  uint32_t key = offset | (element_size_log2 - 2);
  if (auto found = buffer->reads.find(key); found != buffer->reads.end())
    return found->second;
  constexpr DXGI_FORMAT formats[] = {DXGI_FORMAT_R32_UINT, DXGI_FORMAT_R32G32_UINT,
                                     DXGI_FORMAT_R32G32B32A32_UINT};
  D3D11_SHADER_RESOURCE_VIEW_DESC description = {};
  description.Format = formats[element_size_log2 - 2];
  description.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
  description.Buffer.FirstElement = offset >> element_size_log2;
  description.Buffer.NumElements = (buffer->length - offset) >> element_size_log2;
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;
  if (FAILED(device_.device()->CreateShaderResourceView(buffer->buffer.Get(), &description,
                                                        view.GetAddressOf()))) {
    error = "Unable to create native resolve input view";
    return {};
  }
  if (buffer->reads.size() >= 64)
    buffer->reads.clear();
  buffer->reads.emplace(key, view);
  return view;
}

bool D3D11SharedMemory::FlushGpuWrittenRange(uint32_t start, uint32_t length, bool writing) {
  if (!length)
    return true;
  std::lock_guard lock(native_resolve_mutex_);
  if (writing && native_resolve_pending_ && start >= native_resolve_pending_->address &&
      uint64_t(start) + length <=
          uint64_t(native_resolve_pending_->address) + native_resolve_pending_->length) {
    // The pending output already preserves whole pages from their previous
    // native owners. Its publication invalidates those older owners; they do
    // not need a compatibility copy before that ownership change.
    return true;
  }
  uint64_t copied_bytes = 0;
  for (const auto& buffer : native_resolve_buffers_) {
    if ((writing && buffer == native_resolve_pending_) ||
        start >= buffer->address + buffer->length || uint64_t(start) + length <= buffer->address)
      continue;
    uint32_t first = (std::max(start, buffer->address) - buffer->address) >> page_size_log2();
    uint32_t end =
        uint32_t(std::min(uint64_t(start) + length, uint64_t(buffer->address) + buffer->length) -
                 1 - buffer->address) >>
        page_size_log2();
    for (uint32_t page = first; page <= end;) {
      if (!buffer->valid_pages[page] || buffer->materialized_pages[page]) {
        ++page;
        continue;
      }
      // All compatibility GPU writers must request their pages before writing.
      // Never overwrite a write already recorded with an older native version.
      if (writing) {
        REXGPU_ERROR("DX11 GPU write did not request its pending native resolve pages");
        return false;
      }
      uint32_t begin = page++;
      while (page <= end && buffer->valid_pages[page] && !buffer->materialized_pages[page])
        ++page;
      if (!EnsureHostBuffer())
        return false;
      uint32_t offset = begin << page_size_log2(), bytes = (page - begin) << page_size_log2();
      draw_context_.Invalidate();
      D3D11_BOX box = {offset, 0, 0, offset + bytes, 1, 1};
      device_.context()->CopySubresourceRegion(buffer_.Get(), 0, buffer->address + offset, 0, 0,
                                               buffer->buffer.Get(), 0, &box);
      std::fill(buffer->materialized_pages.begin() + begin,
                buffer->materialized_pages.begin() + page, 1);
      copied_bytes += bytes;
    }
  }
  if (copied_bytes && diagnostics_) {
    diagnostics_->RecordDependency(NativeGpuDiagnostics::Dependency::kMemoryMirror, copied_bytes,
                                   "materialize original-resolution Direct3D 11 native resolves");
  }
  return SUCCEEDED(device_.device()->GetDeviceRemovedReason());
}

void D3D11SharedMemory::ClearCache() {
  // Notify derived consumers without discarding authoritative GPU pages.
  auto global_lock = thread::global_critical_region::AcquireDirect();
  native_resolve_clearing_cache_ = true;
  SharedMemory::ClearCache();
  native_resolve_clearing_cache_ = false;
  std::lock_guard lock(native_resolve_mutex_);
  std::erase_if(native_resolve_buffers_, [&](const NativeResolveHandle& buffer) {
    if (buffer.use_count() != 1 ||
        std::any_of(buffer->valid_pages.begin(), buffer->valid_pages.end(),
                    [](uint8_t valid) { return valid != 0; }))
      return false;
    native_resolve_bytes_ -= buffer->length;
    return true;
  });
}

void D3D11SharedMemory::Shutdown() {
  draw_context_.Invalidate();
  ResetTraceDownload();
  srvs_ = {};
  uavs_ = {};
  buffer_.Reset();
  upload_snapshot_.clear();
  if (native_resolve_watch_) {
    UnregisterGlobalWatch(native_resolve_watch_);
    native_resolve_watch_ = nullptr;
  }
  native_resolve_pending_.reset();
  native_resolve_buffers_.clear();
  native_resolve_bytes_ = 0;
  if (common_initialized_) {
    ShutdownCommon();
    common_initialized_ = false;
  }
}

ID3D11ShaderResourceView* D3D11SharedMemory::typed_srv(uint32_t element_size_log2) const {
  return element_size_log2 >= 2 && element_size_log2 <= 4 ? srvs_[element_size_log2 - 1].Get()
                                                          : nullptr;
}
ID3D11UnorderedAccessView* D3D11SharedMemory::typed_uav(uint32_t element_size_log2) const {
  return element_size_log2 >= 2 && element_size_log2 <= 4 ? uavs_[element_size_log2 - 1].Get()
                                                          : nullptr;
}

bool D3D11SharedMemory::UploadRanges(
    const std::vector<std::pair<uint32_t, uint32_t>>& page_ranges) {
  if (page_ranges.empty())
    return true;
  if (!buffer_ || upload_snapshot_.empty())
    return false;
  if (diagnostics_) {
    uint64_t bytes = 0;
    for (auto [first, count] : page_ranges)
      bytes += uint64_t(count) << page_size_log2();
    diagnostics_->RecordDependency(NativeGpuDiagnostics::Dependency::kMemoryMirror, bytes,
                                   "upload guest pages into the Direct3D 11 address buffer");
  }
  // Updates are ordered with the bound draws and do not change any binding.
  for (auto [first, count] : page_ranges) {
    trace_writer_.WriteMemoryRead(first << page_size_log2(), count << page_size_log2());
    while (count) {
      uint32_t pages = std::min(count, uint32_t(upload_snapshot_.size() >> page_size_log2()));
      uint32_t address = first << page_size_log2(), length = pages << page_size_log2();
      // Protect before copying, so a concurrent CPU write invalidates this
      // upload rather than falsely leaving the page marked current.
      MakeRangeValid(address, length, false);
      CopyPagesForUpload(first, pages, upload_snapshot_.data());
      D3D11_BOX box = {address, 0, 0, address + length, 1, 1};
      device_.context()->UpdateSubresource(buffer_.Get(), 0, &box, upload_snapshot_.data(), 0, 0);
      g_profile.memory_uploads.fetch_add(1, std::memory_order_relaxed);
      g_profile.memory_upload_bytes.fetch_add(length, std::memory_order_relaxed);
      first += pages;
      count -= pages;
    }
  }
  return SUCCEEDED(device_.device()->GetDeviceRemovedReason());
}

bool D3D11SharedMemory::InitializeTraceSubmitDownloads() {
  ResetTraceDownload();
  if (!FlushGpuWrittenRange(0, kBufferSize))
    return false;
  PrepareForTraceDownload();
  uint32_t pages = trace_download_page_count();
  if (!pages)
    return false;
  D3D11_BUFFER_DESC desc = {};
  desc.ByteWidth = pages << page_size_log2();
  desc.Usage = D3D11_USAGE_STAGING;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  HRESULT result = device_.device()->CreateBuffer(&desc, nullptr, trace_download_.GetAddressOf());
  if (FAILED(result)) {
    REXGPU_ERROR("DX11 shared memory: Failed to create GPU-written trace readback storage");
    ResetTraceDownload();
    return false;
  }
  draw_context_.Invalidate();
  uint32_t offset = 0;
  for (auto [address, length] : trace_download_ranges()) {
    D3D11_BOX box = {address, 0, 0, address + length, 1, 1};
    device_.context()->CopySubresourceRegion(trace_download_.Get(), 0, offset, 0, 0, buffer_.Get(),
                                             0, &box);
    offset += length;
  }
  return true;
}

bool D3D11SharedMemory::InitializeTraceCompleteDownloads() {
  if (!trace_download_)
    return false;
  HRESULT result = device_.WaitForCompletion();
  D3D11_MAPPED_SUBRESOURCE mapping = {};
  if (SUCCEEDED(result))
    result = device_.context()->Map(trace_download_.Get(), 0, D3D11_MAP_READ, 0, &mapping);
  if (FAILED(result)) {
    REXGPU_ERROR("DX11 shared memory: Failed to read GPU-written trace bytes");
    ResetTraceDownload();
    return false;
  }
  uint32_t offset = 0;
  for (auto [address, length] : trace_download_ranges()) {
    trace_writer_.WriteMemoryRead(address, length,
                                  static_cast<const uint8_t*>(mapping.pData) + offset);
    offset += length;
  }
  device_.context()->Unmap(trace_download_.Get(), 0);
  ResetTraceDownload();
  return true;
}

bool D3D11SharedMemory::DownloadNativeGpuIndices(uint32_t start, std::span<uint8_t> bytes) {
  const uint32_t length = uint32_t(bytes.size());
  if (!CanReadNativeResolveRange(start, length))
    return false;
  std::string error;
  auto view = ReadNativeResolveRange(start, length, 2, error);
  if (!view)
    return false;
  D3D11_SHADER_RESOURCE_VIEW_DESC source_desc;
  view->GetDesc(&source_desc);
  Microsoft::WRL::ComPtr<ID3D11Resource> source;
  view->GetResource(source.GetAddressOf());
  D3D11_BUFFER_DESC desc = {};
  desc.ByteWidth = length;
  desc.Usage = D3D11_USAGE_STAGING;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  Microsoft::WRL::ComPtr<ID3D11Buffer> staging;
  if (FAILED(device_.device()->CreateBuffer(&desc, nullptr, staging.GetAddressOf())))
    return false;
  draw_context_.Invalidate();
  uint32_t offset = source_desc.Buffer.FirstElement * 4;
  D3D11_BOX box = {offset, 0, 0, offset + length, 1, 1};
  device_.context()->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, source.Get(), 0, &box);
  D3D11_MAPPED_SUBRESOURCE mapped = {};
  if (FAILED(device_.context()->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
    return false;
  std::memcpy(bytes.data(), mapped.pData, length);
  device_.context()->Unmap(staging.Get(), 0);
  return true;
}

bool D3D11SharedMemory::ReadbackCpuRange(uint32_t address, uint32_t length, std::string& error) {
  error.clear();
  if (!length)
    return true;
  if (address >= kBufferSize || length > kBufferSize - address) {
    error = "Invalid native CPU readback range";
    return false;
  }
  // A CPU write invalidates an entire tracked page. Publish its preserved
  // neighbors too, so subsequent partial writes cannot discard GPU data.
  uint32_t first = address & ~((1u << page_size_log2()) - 1);
  uint32_t end =
      (address + length + (1u << page_size_log2()) - 1) & ~((1u << page_size_log2()) - 1);
  auto global_lock = thread::global_critical_region::AcquireDirect();
  NativeResolveHandle native;
  if (CanReadNativeResolveRange(first, end - first)) {
    // Re-publication must cover only the downloaded pages. A larger owner may
    // also contain invalid pages changed by other GPU or CPU writers; reviving
    // that whole version would replace their newer bytes with its old ones.
    native = PrepareNativeResolve(first, end - first, 2, error);
    if (!native)
      return false;
  } else if (!RequestRange(first, end - first)) {
    error = "Unable to materialize CPU readback pages";
    return false;
  }
  D3D11_BUFFER_DESC desc = {};
  desc.ByteWidth = end - first;
  desc.Usage = D3D11_USAGE_STAGING;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  Microsoft::WRL::ComPtr<ID3D11Buffer> staging;
  HRESULT result = device_.device()->CreateBuffer(&desc, nullptr, staging.GetAddressOf());
  if (SUCCEEDED(result)) {
    draw_context_.Invalidate();
    uint32_t offset = native ? first - native->address : first;
    D3D11_BOX box = {offset, 0, 0, offset + end - first, 1, 1};
    device_.context()->CopySubresourceRegion(
        staging.Get(), 0, 0, 0, 0, native ? native->buffer.Get() : buffer_.Get(), 0, &box);
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    result = device_.context()->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (SUCCEEDED(result)) {
      std::memcpy(memory().TranslatePhysical(first), mapped.pData, end - first);
      device_.context()->Unmap(staging.Get(), 0);
      // Readback is an exact publication of the same GPU version. Re-arm
      // ownership and watches after the physical-memory write fault callback.
      if (native)
        BeginNativeResolveWrite(native);
      RangeWrittenByGpu(first, end - first);
      if (native && !EndNativeResolveWrite()) {
        error = "Unable to retain native ownership after CPU readback";
        return false;
      }
    }
  }
  if (FAILED(result)) {
    error = "Native CPU readback failed";
    return false;
  }
  return true;
}

void D3D11SharedMemory::ResetTraceDownload() {
  trace_download_.Reset();
  ReleaseTraceDownloadRanges();
}

}  // namespace rex::graphics::d3d11
