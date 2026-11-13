#include <rex/cvar.h>
#include <rex/graphics/d3d11/buffer_cache.h>
#include <rex/graphics/d3d11/shared_memory.h>
#include <rex/graphics/util/draw_extent_estimator.h>
#include <rex/logging.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace rex::graphics::d3d11;

namespace {
void Require(bool condition, const std::string& message) {
  if (!condition)
    throw std::runtime_error(message);
}
void Check(HRESULT result, const char* message) {
  Require(SUCCEEDED(result), message);
}

struct Fixture {
  rex::ui::d3d11::D3D11Device& device;
  uint64_t compared_bytes = 0;
  uint32_t transitions = 0;
  uint32_t extent_checks = 0;
  uint32_t index_cache_checks = 0;
  uint32_t index_cached_reads = 0;
  uint32_t retained_output_checks = 0;

  void Compare(ID3D11Buffer* buffer, uint32_t first, const std::vector<uint8_t>& expected) {
    D3D11_BUFFER_DESC description = {};
    description.ByteWidth = uint32_t(expected.size());
    description.Usage = D3D11_USAGE_STAGING;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Buffer> staging;
    Check(device.device()->CreateBuffer(&description, nullptr, staging.GetAddressOf()),
          "Could not create native memory readback");
    D3D11_BOX box = {first, 0, 0, first + description.ByteWidth, 1, 1};
    device.context()->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, buffer, 0, &box);
    D3D11_MAPPED_SUBRESOURCE mapping = {};
    Check(device.context()->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapping),
          "Could not map native memory readback");
    const auto* actual = static_cast<const uint8_t*>(mapping.pData);
    size_t mismatch = 0;
    while (mismatch < expected.size() && actual[mismatch] == expected[mismatch])
      ++mismatch;
    std::string failure;
    if (mismatch != expected.size()) {
      failure = "Native memory mismatch after transition " + std::to_string(transitions) +
                " at byte " + std::to_string(mismatch) + ": " + std::to_string(actual[mismatch]) +
                " != " + std::to_string(expected[mismatch]);
    }
    device.context()->Unmap(staging.Get(), 0);
    Require(failure.empty(), failure);
    compared_bytes += expected.size();
  }

  void Write(ID3D11Buffer* buffer, uint32_t first, uint32_t size, uint8_t value) {
    std::vector<uint8_t> bytes(size, value);
    D3D11_BOX box = {first, 0, 0, first + size, 1, 1};
    device.context()->UpdateSubresource(buffer, 0, &box, bytes.data(), 0, 0);
  }

  void ExtentCase() {
    using namespace rex::graphics;
    constexpr uint32_t base = 48u << 20, page = 4096;
    rex::memory::Memory guest;
    Require(guest.Initialize(), "Could not initialize extent-test memory");
    Require(guest.LookupHeap(0xA0000000)
                ->AllocFixed(
                    0xA0000000 + base, 64u << 10, 64u << 10,
                    rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
                    rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite),
            "Could not allocate extent-test physical aliases");
    TraceWriter trace(guest.physical_membase());
    DrawContext draws(device);
    D3D11SharedMemory memory(device, draws, guest, trace);
    std::string error;
    Require(memory.Initialize(error), error);
    // Fetch r1, export r1 + zero as position. Only the first vertex is
    // declared, while all three lie in valid physical memory.
    const uint32_t code[] = {
        1u | (2u << 12) | (3u << 16),
        uint32_t(ucode::ControlFlowOpcode::kExecEnd) << 12,
        0,
        (1u << 12) | (1u << 19) | (1u << 20),
        0x688u | (uint32_t(xenos::VertexFormat::k_32_32_32_32_FLOAT) << 16),
        4,
        uint32_t(ucode::ExportRegister::kVSPosition) | (1u << 15) | (15u << 16) |
            (uint32_t(ucode::AluScalarOpcode::kRetainPrev) << 26),
        0,
        (1u << 16) | (uint32_t(ucode::AluVectorOpcode::kAdd) << 24) | (1u << 31)};
    Shader shader(xenos::ShaderType::kVertex, 0, code, std::size(code), std::endian::native);
    rex::string::StringBuffer disassembly;
    shader.AnalyzeUcode(disassembly);
    Require(shader.has_static_vertex_addresses(), "Extent shader has no input proof");
    RegisterFile regs;
    reg::VGT_DRAW_INITIATOR draw{};
    draw.source_select = xenos::SourceSelect::kAutoIndex;
    draw.prim_type = xenos::PrimitiveType::kTriangleList;
    draw.num_indices = 3;
    regs[XE_GPU_REG_VGT_DRAW_INITIATOR] = draw.value;
    regs[XE_GPU_REG_VGT_MAX_VTX_INDX] = 0xFFFFFF;
    xenos::xe_gpu_vertex_fetch_t fetch{};
    fetch.type = xenos::FetchConstantType::kVertex;
    fetch.address = base >> 2;
    fetch.size = 4;
    std::memcpy(regs.values + XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + 6, &fetch, sizeof(fetch));
    DrawExtentEstimator estimator(regs, guest, nullptr);
    estimator.SetSharedMemory(&memory);
    float positions[3][4] = {{8, 32, 0, 1}, {16, 64, 0, 1}, {24, 96, 0, 1}};
    std::memcpy(guest.TranslateVirtual(0xA0000000 + base), positions, sizeof(positions));
    float actual[3][4] = {};
    Require(estimator.GetAutoIndexedVertexPositions(shader, 3, actual) &&
                std::memcmp(actual, positions, sizeof(positions)) == 0 &&
                estimator.EstimateVertexMaxY(shader) == 96,
            "Extent estimation truncated a valid stream to its declared size");
    ++extent_checks;
    compared_bytes += sizeof(positions);

    auto owner = memory.PrepareNativeResolve(base, page, 2, error);
    Require(bool(owner), error);
    Write(owner->buffer.Get(), 0, page, 0x3F);
    memory.BeginNativeResolveWrite(owner);
    memory.RangeWrittenByGpu(base, page);
    Require(memory.EndNativeResolveWrite(), "Could not publish extent-test GPU vertices");
    Require(!estimator.GetAutoIndexedVertexPositions(shader, 3, actual) &&
                estimator.EstimateVertexMaxY(shader) == xenos::kTexture2DCubeMaxWidthHeight &&
                std::memcmp(guest.TranslatePhysical(base), positions, sizeof(positions)) == 0 &&
                !memory.buffer(),
            "Extent estimation consumed or materialized stale CPU vertices");
    ++extent_checks;

    positions[2][1] = 196;
    std::memcpy(guest.TranslateVirtual(0xA0000000 + base), positions, sizeof(positions));
    Require(estimator.GetAutoIndexedVertexPositions(shader, 3, actual) &&
                std::memcmp(actual, positions, sizeof(positions)) == 0 &&
                estimator.EstimateVertexMaxY(shader) == 196,
            "Extent estimation did not recover after CPU vertex ownership returned");
    ++extent_checks;
    compared_bytes += sizeof(positions);

    constexpr uint32_t index_base = base + page;
    uint32_t indices[] = {0, 1, 2};
    draw.source_select = xenos::SourceSelect::kDMA;
    draw.index_size = xenos::IndexFormat::kInt32;
    regs[XE_GPU_REG_VGT_DRAW_INITIATOR] = draw.value;
    regs[XE_GPU_REG_VGT_DMA_BASE] = index_base;
    reg::VGT_DMA_SIZE dma{};
    dma.num_words = 3;
    regs[XE_GPU_REG_VGT_DMA_SIZE] = dma.value;
    std::memset(guest.TranslateVirtual(0xA0000000 + index_base), 0, sizeof(indices));
    auto index_owner = memory.PrepareNativeResolve(index_base, page, 2, error);
    Require(bool(index_owner), error);
    D3D11_BOX box = {0, 0, 0, sizeof(indices), 1, 1};
    device.context()->UpdateSubresource(index_owner->buffer.Get(), 0, &box, indices, 0, 0);
    memory.BeginNativeResolveWrite(index_owner);
    memory.RangeWrittenByGpu(index_base, sizeof(indices));
    Require(memory.EndNativeResolveWrite(), "Could not publish extent-test GPU indices");
    Require(estimator.EstimateVertexMaxY(shader) == xenos::kTexture2DCubeMaxWidthHeight &&
                *guest.TranslatePhysical<const uint32_t*>(index_base + 8) == 0 && !memory.buffer(),
            "Extent estimation consumed or materialized stale CPU indices");
    ++extent_checks;
    std::memcpy(guest.TranslateVirtual(0xA0000000 + index_base), indices, sizeof(indices));
    Require(estimator.EstimateVertexMaxY(shader) == 196 && !memory.buffer(),
            "Extent estimation did not recover after CPU index ownership returned");
    ++extent_checks;
    memory.Shutdown();
  }

  void PressureCase() {
    constexpr uint32_t base = 32u << 20, page = 4096, size = 2u << 20;
    Require(rex::cvar::SetFlagByName("native_resolve_buffer_mb", "1"),
            "Could not set the native resource cache target");
    rex::memory::Memory guest;
    Require(guest.Initialize(), "Could not initialize pressure-test memory");
    Require(guest.LookupHeap(0xA0000000)
                ->AllocFixed(
                    0xA0000000 + base, 4u << 20, 64u << 10,
                    rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
                    rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite),
            "Could not allocate pressure-test physical aliases");
    rex::graphics::TraceWriter trace(guest.physical_membase());
    DrawContext draws(device);
    D3D11SharedMemory memory(device, draws, guest, trace);
    std::string error;
    Require(memory.Initialize(error), error);
    auto owner = memory.PrepareNativeResolve(base, size, 2, error);
    Require(bool(owner), error);
    Write(owner->buffer.Get(), 0, size, 0xB3);
    struct PublicationWrite {
      rex::memory::Memory* guest;
      bool armed = true;
    } publication{&guest};
    auto watch = memory.RegisterGlobalWatch(
        [](const std::unique_lock<std::recursive_mutex>&, void* context, uint32_t first,
           uint32_t last, bool gpu) {
          auto& publication = *static_cast<PublicationWrite*>(context);
          if (gpu && publication.armed && first <= base && last >= base) {
            publication.armed = false;
            *publication.guest->TranslateVirtual<uint8_t*>(0xA0000000 + base + 29) = 0xC1;
          }
        },
        &publication);
    memory.BeginNativeResolveWrite(owner);
    memory.RangeWrittenByGpu(base, size);
    *guest.TranslateVirtual<uint8_t*>(0xA0000000 + base + page + 31) = 0xC2;
    Require(memory.EndNativeResolveWrite(), "Could not publish the large GPU owner");
    memory.UnregisterGlobalWatch(watch);
    Require(!publication.armed && memory.HasNativeResolveRange(base, page) &&
                !memory.HasNativeResolveRange(base + page, page),
            "Native publication did not preserve the winning GPU and CPU page versions");
    owner.reset();
    std::memset(guest.TranslateVirtual(0xA0000000 + base + page), 0x71, size - 2 * page);
    Require(memory.HasNativeResolveRange(base, page) &&
                memory.HasNativeResolveRange(base + size - page, page),
            "CPU writes discarded untouched GPU pages");
    auto next = memory.PrepareNativeResolve(base + size, page, 2, error);
    Require(bool(next), error);
    auto first = memory.ReadNativeResolveRange(base, page, 2, error);
    Require(bool(first), error);
    ComPtr<ID3D11Resource> resource;
    first->GetResource(resource.GetAddressOf());
    ComPtr<ID3D11Buffer> buffer;
    Check(resource.As(&buffer), "The compacted native owner is not a buffer");
    D3D11_BUFFER_DESC description;
    buffer->GetDesc(&description);
    Require(description.ByteWidth == page, "The sparse GPU owner was not compacted");
    Compare(buffer.Get(), 0, std::vector<uint8_t>(page, 0xB3));
    auto combined = memory.ReadNativeResolveRange(base, size, 2, error);
    Require(bool(combined), error);
    combined->GetResource(resource.ReleaseAndGetAddressOf());
    Check(resource.As(&buffer), "The composed native input is not a buffer");
    std::vector<uint8_t> expected(size, 0x71);
    std::fill_n(expected.begin(), page, 0xB3);
    std::fill_n(expected.end() - page, page, 0xB3);
    Compare(buffer.Get(), 0, expected);
    bool downloaded = false;
    auto indices = memory.SnapshotNativeGpuIndices(base, size, downloaded);
    Require(indices && downloaded && *indices == expected,
            "Large native indices did not compose GPU owners and CPU gaps exactly");
    compared_bytes += size;
    auto cached_indices = memory.SnapshotNativeGpuIndices(base, size, downloaded);
    Require(cached_indices == indices && !downloaded,
            "Unchanged large native indices were downloaded again");
    // A smaller draw can use current bytes from a containing snapshot without
    // waiting for the GPU again. Include page boundaries and both edge pages.
    const std::array<std::pair<uint32_t, uint32_t>, 6> subranges = {
        std::pair{0u, 4u},         {4u, 4u},       {page - 4, 16u},
        {page + 12, 3 * page + 4}, {size - 4, 4u}, {size / 2, 4 * page}};
    for (auto [offset, length] : subranges) {
      auto part = memory.SnapshotNativeGpuIndices(base + offset, length, downloaded);
      Require(part && !downloaded && part->size() == length &&
                  std::equal(part->begin(), part->end(), expected.begin() + offset),
              "A contained index range required another download or selected the wrong bytes");
      compared_bytes += length;
      ++index_cache_checks;
      ++index_cached_reads;
    }
    // The unrelated GPU page supplies more misses than the entry budget. An
    // exact hit must keep the large hot version resident throughout the churn.
    Write(next->buffer.Get(), 0, page, 0xD7);
    memory.BeginNativeResolveWrite(next);
    memory.RangeWrittenByGpu(base + size, page);
    Require(memory.EndNativeResolveWrite(), "Could not publish cache-churn index data");
    Require(memory.HasNativeResolveRange(base + size, page),
            "A prepared native output was retired while its handle was still retained");
    ++retained_output_checks;
    for (uint32_t i = 0; i < 80; ++i) {
      auto other = memory.SnapshotNativeGpuIndices(base + size + i * 16, 4, downloaded);
      Require(other && downloaded && *other == std::vector<uint8_t>(4, 0xD7),
              "An unrelated index miss did not download its GPU owner");
      auto hot = memory.SnapshotNativeGpuIndices(base, size, downloaded);
      Require(hot == indices && !downloaded,
              "Cache churn evicted a repeatedly used immutable index version");
      compared_bytes += 4;
      index_cache_checks += 2;
      ++index_cached_reads;
    }
    constexpr uint32_t changed_offset = 2 * page + 32, changed_size = 64;
    auto sub_indices =
        memory.SnapshotNativeGpuIndices(base + changed_offset, changed_size, downloaded);
    Require(sub_indices && !downloaded, "A hot contained index range was downloaded again");
    ++index_cached_reads;
    ++index_cache_checks;
    *guest.TranslateVirtual<uint8_t*>(0xA0000000 + base + 2 * page + 41) = 0x42;
    expected[2 * page + 41] = 0x42;
    Require(!memory.IsRangeGpuWritten(base + changed_offset, changed_size),
            "A CPU write did not retire its GPU page ownership");
    auto rewritten_sub =
        memory.SnapshotNativeGpuIndices(base + changed_offset, changed_size, downloaded);
    // This page is CPU-only now: the caller must use CPU indices. The old
    // cached GPU snapshot must be refused, while its retained bytes stay fixed.
    Require(!rewritten_sub && !downloaded && (*sub_indices)[9] == 0x71,
            "CPU-owned indices reused a stale GPU snapshot or changed an older version");
    ++index_cache_checks;
    auto rewritten_indices = memory.SnapshotNativeGpuIndices(base, size, downloaded);
    Require(rewritten_indices && downloaded && rewritten_indices != indices &&
                *rewritten_indices == expected && (*indices)[2 * page + 41] == 0x71,
            "Large native indices lost CPU invalidation or immutable old versions");
    compared_bytes += size;
    auto rewritten_owner = memory.PrepareNativeResolve(base, page, 2, error);
    Require(bool(rewritten_owner), error);
    Write(rewritten_owner->buffer.Get(), 0, page, 0xA9);
    memory.BeginNativeResolveWrite(rewritten_owner);
    memory.RangeWrittenByGpu(base, page);
    Require(memory.EndNativeResolveWrite(), "Could not rewrite a cached GPU index source");
    auto gpu_rewritten = memory.SnapshotNativeGpuIndices(base, 4, downloaded);
    Require(gpu_rewritten && downloaded && *gpu_rewritten == std::vector<uint8_t>(4, 0xA9) &&
                (*indices)[0] == 0xB3 && (*rewritten_indices)[0] == 0xB3,
            "A GPU write reused stale indices or changed earlier immutable versions");
    std::fill_n(expected.begin(), page, 0xA9);
    compared_bytes += 4;
    ++index_cache_checks;
    uint64_t downloads = memory.native_index_download_count();
    for (auto [address, length] :
         {std::pair{base + 1, 4u}, std::pair{base, 3u}, std::pair{base, 0u},
          std::pair{D3D11SharedMemory::kBufferSize - 4, 8u}}) {
      Require(!memory.SnapshotNativeGpuIndices(address, length, downloaded) && !downloaded &&
                  memory.native_index_download_count() == downloads,
              "An invalid index range performed or reused a download");
      ++index_cache_checks;
    }
    auto delayed = memory.PrepareNativeResolve(base + size + page, page, 2, error);
    Require(bool(delayed), error);
    memory.ClearCache();
    Write(delayed->buffer.Get(), 0, page, 0xC6);
    memory.BeginNativeResolveWrite(delayed);
    memory.RangeWrittenByGpu(base + size + page, page);
    Require(
        memory.EndNativeResolveWrite() && memory.HasNativeResolveRange(base + size + page, page),
        "Cache clearing retired a prepared output while its handle was still retained");
    Compare(delayed->buffer.Get(), 0, std::vector<uint8_t>(page, 0xC6));
    ++retained_output_checks;
    Require(memory.HasNativeResolveRange(base, page) &&
                memory.HasNativeResolveRange(base + size - page, page),
            "Cache clear discarded authoritative GPU pages");
    combined = memory.ReadNativeResolveRange(base, size, 2, error);
    Require(bool(combined), error);
    combined->GetResource(resource.ReleaseAndGetAddressOf());
    Check(resource.As(&buffer), "The retained native input is not a buffer");
    Compare(buffer.Get(), 0, expected);
    Require(!memory.buffer(),
            "Native publication or storage pressure allocated the physical memory mirror");
    ++transitions;
  }

  void Run() {
    constexpr uint32_t base = 16u << 20, page = 4096, size = 6 * page;
    constexpr uint32_t offset = 512, written = size - 1024;
    Require(rex::cvar::SetFlagByName("gpu_stream_dynamic_pages", "false"),
            "Could not configure deterministic page ownership");
    ExtentCase();
    PressureCase();
    rex::memory::Memory guest;
    Require(guest.Initialize(), "Could not initialize physical memory");
    Require(guest.LookupHeap(0xA0000000)
                ->AllocFixed(
                    0xA0000000 + base, 64 * 1024, 64 * 1024,
                    rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
                    rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite),
            "Could not allocate a CPU-visible physical alias");
    rex::graphics::TraceWriter trace(guest.physical_membase());
    DrawContext draws(device);
    D3D11SharedMemory memory(device, draws, guest, trace);
    std::string error;
    Require(memory.Initialize(error), error);
    Require(memory.CanRetainNativeResolve(base + offset, written) &&
                !memory.CanRetainNativeResolve(base, (128u << 20) + page) &&
                !memory.CanRetainNativeResolve(D3D11SharedMemory::kBufferSize - 4, 8),
            "Unsupported native storage sizes did not retain the compatibility path");
    std::vector<uint8_t> original(size), expected(size);
    for (uint32_t i = 0; i < size; ++i)
      original[i] = uint8_t((i * 13 + i / 257) ^ 0xA7);
    expected = original;
    std::memcpy(guest.TranslatePhysical(base), original.data(), size);

    auto native = memory.PrepareNativeResolve(base + offset, written, 4, error);
    Require(bool(native), error);
    Require(!memory.HasNativeResolveRange(base, size), "Unwritten native data was published");
    Compare(native->buffer.Get(), 0, original);
    Require(!memory.buffer(), "CPU-owned native padding allocated the compatibility mirror");
    ++transitions;

    Write(native->buffer.Get(), offset, written, 0x31);
    std::fill(expected.begin() + offset, expected.begin() + offset + written, 0x31);
    memory.BeginNativeResolveWrite(native);
    memory.RangeWrittenByGpu(base + offset, written);
    Require(memory.EndNativeResolveWrite(), "Native resolve publication failed");
    Require(memory.HasNativeResolveRange(base, size),
            "Native data did not retain whole-page padding");
    Compare(native->buffer.Get(), 0, expected);
    Require(!memory.buffer(), "A native resolve eagerly allocated its original-resolution mirror");
    ++transitions;

    // Vertex snapshots combine distant GPU owners with CPU-only streams. They
    // must keep the version copied by this draw when a later draw rewrites an
    // owner, including words not present in CPU memory at all.
    {
      BufferCache buffers(device);
      auto source = memory.PrepareNativeResolve(base + 8 * page, page, 2, error);
      Require(bool(source), error);
      for (uint32_t test = 0; test < 16; ++test) {
        uint32_t first = test * 4, length = 32 + test * 12;
        std::vector<uint8_t> cpu(64 + 2 * length + 16, uint8_t(0x50 + test));
        std::vector<uint8_t> mixed = cpu;
        Write(source->buffer.Get(), 0, page, uint8_t(0x91 + test));
        memory.BeginNativeResolveWrite(source);
        memory.RangeWrittenByGpu(source->address, source->length);
        Require(memory.EndNativeResolveWrite(), "Could not publish a GPU vertex source");
        draws.Invalidate();
        std::array<BufferCache::GpuCopy, 2> copies = {
            BufferCache::GpuCopy{source->buffer.Get(), first, 64, length},
            BufferCache::GpuCopy{native->buffer.Get(), offset + first, 64 + length + 16, length}};
        std::fill_n(mixed.begin() + 64, length, uint8_t(0x91 + test));
        std::fill_n(mixed.begin() + 64 + length + 16, length, 0x31);
        auto snapshot = buffers.SnapshotMixedRanges(cpu, copies, error);
        Require(bool(snapshot), error);
        Compare(snapshot->buffer(), 0, mixed);
        Write(source->buffer.Get(), 0, page, uint8_t(0xC1 + test));
        memory.BeginNativeResolveWrite(source);
        memory.RangeWrittenByGpu(source->address, source->length);
        Require(memory.EndNativeResolveWrite(), "Could not publish a rewritten vertex source");
        Compare(snapshot->buffer(), 0, mixed);
        std::fill_n(mixed.begin() + 64, length, uint8_t(0xC1 + test));
        draws.Invalidate();
        auto rewritten = buffers.SnapshotMixedRanges(cpu, copies, error);
        Require(bool(rewritten), error);
        Compare(rewritten->buffer(), 0, mixed);
        Require(!memory.buffer(), "Mixed vertex snapshots allocated the physical mirror");
        ++transitions;
      }
      Require(memory.ReadbackCpuRange(source->address + 12, 4, error), error);
      Require(!memory.buffer() && memory.HasNativeResolveRange(source->address, page),
              "Direct CPU readback discarded its retained native GPU source");
      Require(std::all_of(guest.TranslatePhysical<const uint8_t*>(source->address),
                          guest.TranslatePhysical<const uint8_t*>(source->address) + page,
                          [](uint8_t value) { return value == 0xD0; }),
              "Direct CPU readback did not preserve all native GPU page bytes");
      compared_bytes += page;
      ++transitions;
    }

    auto view = memory.ReadNativeResolveRange(base + page, page, 3, error);
    Require(bool(view), error);
    D3D11_SHADER_RESOURCE_VIEW_DESC view_description = {};
    view->GetDesc(&view_description);
    Require(view_description.Format == DXGI_FORMAT_R32G32_UINT &&
                view_description.Buffer.FirstElement == page / 8,
            "Native texture input did not retain its resource-relative origin");
    ++transitions;

    // A temporary input watches its CPU gap without declaring that page current
    // in the mirror. A later compatibility request must still upload those bytes.
    std::vector<uint8_t> cpu_gap(page);
    for (uint32_t i = 0; i < page; ++i)
      cpu_gap[i] = uint8_t((i * 23 + i / 83) ^ 0xE9);
    std::memcpy(guest.TranslateVirtual(0xA0000000 + base + size), cpu_gap.data(), page);
    auto gap_view = memory.ReadNativeResolveRange(base + 5 * page, 2 * page, 2, error);
    Require(bool(gap_view), error);
    ComPtr<ID3D11Resource> gap_resource;
    gap_view->GetResource(gap_resource.GetAddressOf());
    ComPtr<ID3D11Buffer> gap_input;
    Check(gap_resource.As(&gap_input), "Native texture input with a CPU gap is not a buffer");
    std::vector<uint8_t> gap_expected(expected.begin() + 5 * page, expected.end());
    gap_expected.insert(gap_expected.end(), cpu_gap.begin(), cpu_gap.end());
    Compare(gap_input.Get(), 0, gap_expected);
    Require(!memory.buffer(), "A temporary native input eagerly allocated the mirror");
    ++transitions;

    // Disjoint native versions and untouched CPU pages must supply a larger
    // resolve's preimage directly, before any compatibility consumer exists.
    constexpr uint32_t composed_base = base + 10 * page, composed_size = 6 * page;
    std::vector<uint8_t> composed_expected(composed_size);
    for (uint32_t i = 0; i < composed_size; ++i)
      composed_expected[i] = uint8_t((i * 19 + i / 127) ^ 0x5B);
    std::memcpy(guest.TranslateVirtual(0xA0000000 + composed_base), composed_expected.data(),
                composed_size);
    for (uint32_t page_index : {0u, 2u, 5u}) {
      auto source = memory.PrepareNativeResolve(composed_base + page_index * page, page, 2, error);
      Require(bool(source), error);
      uint8_t value = uint8_t(0xA1 + page_index);
      Write(source->buffer.Get(), 0, page, value);
      std::fill_n(composed_expected.begin() + page_index * page, page, value);
      memory.BeginNativeResolveWrite(source);
      memory.RangeWrittenByGpu(composed_base + page_index * page, page);
      Require(memory.EndNativeResolveWrite(), "Composed source publication failed");
      Compare(source->buffer.Get(), 0,
              {composed_expected.begin() + page_index * page,
               composed_expected.begin() + (page_index + 1) * page});
      ++transitions;
    }
    Require(memory.CanReadNativeResolveRange(composed_base, composed_size),
            "Native owners and CPU gaps could not supply a compact texture input");
    auto composed_view = memory.ReadNativeResolveRange(composed_base, composed_size, 4, error);
    Require(bool(composed_view), error);
    ComPtr<ID3D11Resource> composed_resource;
    composed_view->GetResource(composed_resource.GetAddressOf());
    ComPtr<ID3D11Buffer> composed_input;
    Check(composed_resource.As(&composed_input), "Composed texture input is not a buffer");
    Compare(composed_input.Get(), 0, composed_expected);
    Require(!memory.buffer(), "Composing a native texture input allocated the mirror");
    ++transitions;

    auto composed = memory.PrepareNativeResolve(composed_base + 128, composed_size - 256, 4, error);
    Require(bool(composed), error);
    Compare(composed->buffer.Get(), 0, composed_expected);
    Require(!memory.buffer(), "Composing GPU owners and CPU gaps allocated the mirror");
    ++transitions;

    Write(composed->buffer.Get(), 128, composed_size - 256, 0x6D);
    std::fill(composed_expected.begin() + 128, composed_expected.end() - 128, 0x6D);
    memory.BeginNativeResolveWrite(composed);
    memory.RangeWrittenByGpu(composed_base + 128, composed_size - 256);
    Require(memory.EndNativeResolveWrite(), "Composed resolve publication failed");
    Compare(composed->buffer.Get(), 0, composed_expected);
    Require(!memory.buffer(), "Publishing composed native pages allocated the mirror");
    ++transitions;

    std::memset(guest.TranslateVirtual(0xA0000000 + composed_base + 4 * page), 0xD2, page);
    std::fill_n(composed_expected.begin() + 4 * page, page, 0xD2);
    Require(!memory.HasNativeResolveRange(composed_base + 4 * page, page) &&
                memory.HasNativeResolveRange(composed_base + 3 * page, page) &&
                memory.HasNativeResolveRange(composed_base + 5 * page, page),
            "A CPU overwrite invalidated the wrong composed pages");
    auto refreshed = memory.PrepareNativeResolve(composed_base, composed_size, 2, error);
    Require(bool(refreshed) && refreshed != composed, error);
    Compare(refreshed->buffer.Get(), 0, composed_expected);
    Require(!memory.buffer(), "Refreshing an invalidated composed page allocated the mirror");
    ++transitions;

    Require(memory.RequestRange(base + page + 128, 64), "Could not materialize one native page");
    std::vector<uint8_t> mirror(size, 0);
    std::copy_n(expected.begin() + page, page, mirror.begin() + page);
    Compare(memory.buffer(), base, mirror);
    Require(memory.HasNativeResolveRange(base, size), "A mirror read discarded native ownership");
    ++transitions;

    Require(memory.RequestRange(base + size, page), "Could not upload a temporary input's CPU gap");
    Compare(memory.buffer(), base + size, cpu_gap);
    ++transitions;

    auto reused = memory.PrepareNativeResolve(base + offset, written, 4, error);
    Require(reused == native, "Repeated native resolve did not reuse its live padding version");
    Write(native->buffer.Get(), offset, written, 0x72);
    std::fill(expected.begin() + offset, expected.begin() + offset + written, 0x72);
    memory.BeginNativeResolveWrite(native);
    memory.RangeWrittenByGpu(base + offset, written);
    Require(memory.EndNativeResolveWrite(), "Repeated native resolve publication failed");
    Compare(memory.buffer(), base, mirror);
    Compare(native->buffer.Get(), 0, expected);
    ++transitions;

    // A compatibility GPU write first requests the page, then invalidates its
    // native version. Neighboring pages must remain available natively.
    Require(memory.RequestRange(base + page + 128, 256), "Could not preserve a GPU write preimage");
    Write(memory.buffer(), base + page + 128, 256, 0xBC);
    std::fill(expected.begin() + page + 128, expected.begin() + page + 384, 0xBC);
    memory.RangeWrittenByGpu(base + page + 128, 256);
    Require(!memory.HasNativeResolveRange(base, size) && memory.HasNativeResolveRange(base, page) &&
                memory.HasNativeResolveRange(base + 2 * page, page),
            "A partial GPU write corrupted native ownership of neighboring pages");
    Require(!memory.CanReadNativeResolveRange(base, size),
            "An unretained GPU write was mistaken for a native texture source");
    Compare(memory.buffer(), base + page, {expected.begin() + page, expected.begin() + 2 * page});
    ++transitions;

    Require(memory.ReadbackCpuRange(base + 2 * page + 12, 4, error), error);
    Require(std::memcmp(guest.TranslatePhysical(base + 2 * page), expected.data() + 2 * page,
                        page) == 0,
            "CPU readback did not publish the whole preserved GPU page");
    compared_bytes += page;
    guest.TranslateVirtual<uint8_t*>(0xA0000000 + base + 2 * page)[29] = 0xE3;
    expected[2 * page + 29] = 0xE3;
    Require(!memory.IsRangeGpuWritten(base + 2 * page, page) &&
                !memory.HasNativeResolveRange(base + 2 * page, page) &&
                memory.HasNativeResolveRange(base + 3 * page, page),
            "A CPU write invalidated the wrong native pages");
    ++transitions;

    std::memset(guest.TranslateVirtual(0xA0000000 + base + 4 * page), 0x88, page);
    std::fill(expected.begin() + 4 * page, expected.begin() + 5 * page, 0x88);
    Require(!memory.HasNativeResolveRange(base + 4 * page, page) &&
                memory.HasNativeResolveRange(base + 5 * page, page),
            "A full CPU overwrite did not invalidate its native page independently");
    ++transitions;

    // A new overlapping version combines earlier native data, a compatibility
    // GPU write and a partial CPU write without changing their unwritten bytes.
    auto overlap = memory.PrepareNativeResolve(base + page + 128, page, 2, error);
    Require(bool(overlap) && overlap != native, error);
    Compare(overlap->buffer.Get(), 0, {expected.begin() + page, expected.begin() + 3 * page});
    Write(overlap->buffer.Get(), 128, page, 0x49);
    std::fill(expected.begin() + page + 128, expected.begin() + 2 * page + 128, 0x49);
    memory.BeginNativeResolveWrite(overlap);
    memory.RangeWrittenByGpu(base + page + 128, page);
    Require(memory.EndNativeResolveWrite(), "Overlapping native resolve publication failed");
    Compare(overlap->buffer.Get(), 0, {expected.begin() + page, expected.begin() + 3 * page});
    Require(memory.HasNativeResolveRange(base + page, 2 * page) &&
                memory.HasNativeResolveRange(base + 3 * page, page),
            "Overlapping native versions lost unrelated pages");
    ++transitions;

    Require(memory.InitializeTraceSubmitDownloads(), "Native trace download was not prepared");
    Require(memory.InitializeTraceCompleteDownloads(), "Native trace download failed");
    Require(memory.RequestRange(base, size), "Could not upload CPU-owned neighbors after tracing");
    Compare(memory.buffer(), base, expected);
    ++transitions;

    // Inject a real CPU fault between the GPU watch notification and the
    // common valid-page publication. The final GPU flags must still have a
    // valid compatibility source even though the native owner was invalidated.
    constexpr uint32_t race_address = base + 8 * page;
    auto race = memory.PrepareNativeResolve(race_address, page, 2, error);
    Require(bool(race), error);
    Write(race->buffer.Get(), 0, page, 0xFA);
    struct CpuWrite {
      rex::memory::Memory* guest;
      bool armed;
    } write{&guest, true};
    auto watch = memory.RegisterGlobalWatch(
        [](const std::unique_lock<std::recursive_mutex>&, void* context, uint32_t first,
           uint32_t last, bool gpu) {
          auto& write = *static_cast<CpuWrite*>(context);
          if (gpu && write.armed && first <= race_address && last >= race_address) {
            write.armed = false;
            *write.guest->TranslateVirtual<uint8_t*>(0xA0000000 + race_address + 29) = 0xC1;
          }
        },
        &write);
    memory.BeginNativeResolveWrite(race);
    memory.RangeWrittenByGpu(race_address, page);
    bool published = memory.EndNativeResolveWrite();
    memory.UnregisterGlobalWatch(watch);
    Require(published && !write.armed && memory.IsRangeGpuWritten(race_address, page) &&
                memory.HasNativeResolveRange(race_address, page),
            "The GPU-winning publication did not retain its native owner");
    Compare(race->buffer.Get(), 0, std::vector<uint8_t>(page, 0xFA));
    Require(memory.RequestRange(race_address, page), "Publication race left invalid GPU memory");
    Compare(memory.buffer(), race_address, std::vector<uint8_t>(page, 0xFA));
    ++transitions;

    memory.ClearCache();
    Require(memory.HasNativeResolveRange(base, page), "Cache clear discarded native ownership");
    Compare(memory.buffer(), base, expected);
    ++transitions;
    memory.Shutdown();
  }
};
}  // namespace

int main(int argc, char** argv) {
  std::string output;
  try {
    rex::InitLogging("native-memory-runtime.log", spdlog::level::debug);
    rex::ui::d3d11::D3D11Device::Options options;
    options.debug = true;
    for (int i = 1; i < argc; ++i) {
      std::string arg(argv[i]);
      if (arg == "--warp")
        options.warp = true;
      else if (arg == "--output" && i + 1 < argc)
        output = argv[++i];
      else
        throw std::runtime_error(
            "usage: d3d11_native_memory_check [--warp] [--output report.json]");
    }
    std::string error;
    auto device = rex::ui::d3d11::D3D11Device::Create(options, error);
    Require(bool(device), error);
    Fixture fixture{*device};
    fixture.Run();
    std::string report =
        "{\n  \"passed\": true,\n  \"transitions\": " + std::to_string(fixture.transitions) +
        ",\n  \"extent_checks\": " + std::to_string(fixture.extent_checks) +
        ",\n  \"index_cache_checks\": " + std::to_string(fixture.index_cache_checks) +
        ",\n  \"index_cached_reads\": " + std::to_string(fixture.index_cached_reads) +
        ",\n  \"retained_output_checks\": " + std::to_string(fixture.retained_output_checks) +
        ",\n  \"exact_bytes\": " + std::to_string(fixture.compared_bytes) + "\n}\n";
    std::fputs(report.c_str(), stdout);
    if (!output.empty()) {
      std::ofstream file(output);
      file << report;
      Require(bool(file), "Could not write native memory report");
    }
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    if (!output.empty()) {
      std::ofstream(output) << "{\"passed\":false}\n";
      std::ofstream(output + ".error.txt") << error.what() << '\n';
    }
    return 1;
  }
}
