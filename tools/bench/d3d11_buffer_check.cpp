#include <rex/graphics/d3d11/buffer_cache.h>
#include <rex/graphics/d3d11/scaled_resolve_memory.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>

using Microsoft::WRL::ComPtr;
using rex::graphics::d3d11::BufferCache;
using rex::graphics::d3d11::BufferKind;
using rex::graphics::d3d11::BufferVersion;
using rex::graphics::d3d11::DrawContext;
using rex::graphics::d3d11::ScaledResolveMemory;
using rex::ui::d3d11::D3D11Device;

namespace {
void Require(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}
void Check(HRESULT result, const char* operation) {
  if (FAILED(result)) {
    char text[160];
    std::snprintf(text, sizeof(text), "%s failed (0x%08X)", operation, unsigned(result));
    throw std::runtime_error(text);
  }
}
std::span<const uint8_t> Bytes(std::span<const uint32_t> words) {
  return {reinterpret_cast<const uint8_t*>(words.data()), words.size_bytes()};
}
std::shared_ptr<const BufferVersion> Acquire(BufferCache& cache, std::span<const uint32_t> words,
                                             BufferKind kind) {
  std::string error;
  auto version = cache.GetOrCreate(Bytes(words), kind, error);
  if (!version)
    throw std::runtime_error(error);
  return version;
}
ComPtr<ID3D11ComputeShader> Shader(D3D11Device& owner, bool constant) {
  std::string source = constant ? "cbuffer source_data : register(b1) {uint4 data[33];};\n"
                                : "ByteAddressBuffer source_data : register(t0);\n";
  source += R"(
RWByteAddressBuffer output_data : register(u0);
cbuffer parameters : register(b0) {uint count; uint offset;};
[numthreads(64,1,1)] void main(uint3 id : SV_DispatchThreadID) {
  if(id.x<count) {
)";
  source += constant ? "uint word = data[id.x >> 2][id.x & 3];\n"
                     : "uint word = source_data.Load(id.x * 4);\n";
  source += R"(
    output_data.Store((offset + id.x) * 4, word ^ (id.x * 0x9E3779B9u + offset));
  }
})";
  std::string error;
  ComPtr<ID3DBlob> code;
  HRESULT result = D3D11Device::CompileShader(source, "main", D3D11Device::ShaderStage::kCompute,
                                              code.GetAddressOf(), error);
  if (FAILED(result))
    throw std::runtime_error(error);
  ComPtr<ID3D11ComputeShader> shader;
  Check(owner.device()->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(),
                                            nullptr, shader.GetAddressOf()),
        "Buffer check shader creation");
  return shader;
}

struct Snapshot {
  std::shared_ptr<const BufferVersion> version;
  std::array<uint32_t, 129> words;
};

uint64_t CheckScaledStorage(D3D11Device& device) {
  DrawContext draws(device);
  uint64_t compared_words = 0;
  for (uint32_t scale_area : {2u, 3u, 4u, 9u}) {
    ScaledResolveMemory storage(device, draws, scale_area == 9 ? 3 : scale_area,
                                scale_area == 9 ? 3 : 1);
    constexpr uint32_t base = 0x12000000;
    std::string error;
    auto first = storage.Write(base + 0x3100, 0x1000, 2, error);
    Require(bool(first), error.c_str());
    auto second = storage.Write(base + 0x7000, 0x2000, 2, error);
    Require(bool(second), error.c_str());
    const uint32_t a[4] = {0x1937FA21, 0x1937FA21, 0x1937FA21, 0x1937FA21};
    const uint32_t b[4] = {0xBF275163, 0xBF275163, 0xBF275163, 0xBF275163};
    device.context()->ClearUnorderedAccessViewUint(first.Get(), a);
    device.context()->ClearUnorderedAccessViewUint(second.Get(), b);
    ComPtr<ID3D11Resource> old_first;
    first->GetResource(old_first.GetAddressOf());
    // A third range bridges both original allocations and extends their ends.
    // No completion wait precedes the copies, so queued GPU writes must survive.
    Require(storage.Ensure(base + 0x2000, 0x7800, error), error.c_str());
    auto merged = storage.Read(base + 0x2000, 0x8000, 2, error);
    Require(bool(merged), error.c_str());
    Require(storage.resident_bytes() == 0x8000ull * scale_area,
            "Intersecting scaled ranges did not share preserved storage");
    auto inside = storage.Read(base + 0x3100, 0x1000, 4, error);
    Require(bool(inside), error.c_str());
    D3D11_SHADER_RESOURCE_VIEW_DESC inside_desc = {};
    inside->GetDesc(&inside_desc);
    Require(inside_desc.Buffer.FirstElement == (0x1100 * scale_area) / 16,
            "Merged scaled resolve view lost its relative offset");
    ComPtr<ID3D11Resource> merged_resource;
    merged->GetResource(merged_resource.GetAddressOf());
    ComPtr<ID3D11Buffer> merged_buffer;
    Check(merged_resource.As(&merged_buffer), "Merged scaled buffer");
    D3D11_BUFFER_DESC desc = {};
    merged_buffer->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = desc.MiscFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Buffer> staging;
    Check(device.device()->CreateBuffer(&desc, nullptr, staging.GetAddressOf()),
          "Scaled storage readback buffer");
    device.context()->CopyResource(staging.Get(), merged_buffer.Get());
    Check(device.WaitForCompletion(), "Scaled preservation completion");
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    Check(device.context()->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped),
          "Scaled storage readback");
    bool exact = true;
    for (uint32_t byte = 0; byte < desc.ByteWidth; byte += 4) {
      uint32_t address = 0x2000 + byte / scale_area;
      uint32_t expected = address >= 0x3100 && address < 0x5000   ? a[0]
                          : address >= 0x7000 && address < 0x9000 ? b[0]
                                                                  : 0;
      exact &= static_cast<const uint32_t*>(mapped.pData)[byte / 4] == expected;
      ++compared_words;
    }
    device.context()->Unmap(staging.Get(), 0);
    Require(exact, "A merged scaled resolve lost GPU writes or uninitialized padding");
    // The old view still owns its original bytes after replacement. Mutating
    // that retained resource must leave the new merged version independent.
    device.context()->ClearUnorderedAccessViewUint(first.Get(), b);
    device.context()->CopyResource(staging.Get(), merged_buffer.Get());
    Check(device.WaitForCompletion(), "Retained scaled view completion");
    Check(device.context()->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped),
          "Retained scaled view readback");
    bool retained = static_cast<const uint32_t*>(mapped.pData)[0x1100 * scale_area / 4] == a[0];
    device.context()->Unmap(staging.Get(), 0);
    Require(retained, "An old scaled resolve view changed the replacement buffer");
    Require(!storage.Ensure(0x1FFFF000, 0x1001, error),
            "A scaled range outside physical memory was accepted");
    Require(!storage.Read(base + 1, 16, 4, error), "A misaligned scaled typed view was accepted");
    Require(!storage.Write(base, 16, 1, error), "An unsupported scaled typed view was accepted");
  }
  return compared_words;
}

uint64_t Run(D3D11Device& owner, BufferCache::Statistics& statistics) {
  constexpr uint32_t count = 129, mutations = 64, retained_checks = 16;
  constexpr uint32_t total = count * (mutations + retained_checks) * 2;
  BufferCache cache(owner, 2048);
  auto raw_shader = Shader(owner, false), constant_shader = Shader(owner, true);
  D3D11_BUFFER_DESC desc = {};
  desc.ByteWidth = total * 4;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
  desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
  ComPtr<ID3D11Buffer> output, staging;
  Check(owner.device()->CreateBuffer(&desc, nullptr, output.GetAddressOf()), "Output buffer");
  D3D11_UNORDERED_ACCESS_VIEW_DESC view = {};
  view.Format = DXGI_FORMAT_R32_TYPELESS;
  view.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
  view.Buffer.NumElements = total;
  view.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
  ComPtr<ID3D11UnorderedAccessView> uav;
  Check(owner.device()->CreateUnorderedAccessView(output.Get(), &view, uav.GetAddressOf()),
        "Output buffer view");
  auto* context = owner.context();
  auto* output_view = uav.Get();
  context->CSSetUnorderedAccessViews(0, 1, &output_view, nullptr);
  std::vector<uint32_t> expected;
  std::vector<Snapshot> snapshots;
  std::array<uint32_t, count> source;
  for (uint32_t i = 0; i < count; ++i)
    source[i] = i * 0x7219563u + 0x31A1057u;
  auto dispatch = [&](const Snapshot& snapshot) {
    uint32_t offset = uint32_t(expected.size());
    std::array<uint32_t, 4> parameters = {count, offset, 0, 0};
    auto parameter_version = Acquire(cache, parameters, BufferKind::kConstant);
    auto* parameter_buffer = parameter_version->buffer();
    context->CSSetConstantBuffers(0, 1, &parameter_buffer);
    bool constant = snapshot.version->kind() == BufferKind::kConstant;
    context->CSSetShader(constant ? constant_shader.Get() : raw_shader.Get(), nullptr, 0);
    auto* srv = snapshot.version->raw_view();
    context->CSSetShaderResources(0, 1, &srv);
    ID3D11Buffer* constants = constant ? snapshot.version->buffer() : nullptr;
    context->CSSetConstantBuffers(1, 1, &constants);
    context->Dispatch((count + 63) / 64, 1, 1);
    for (uint32_t i = 0; i < count; ++i) {
      expected.push_back(snapshot.words[i] ^ (i * 0x9E3779B9u + offset));
    }
  };
  constexpr std::array kinds = {BufferKind::kRaw, BufferKind::kIndex16, BufferKind::kIndex32,
                                BufferKind::kConstant};
  for (uint32_t iteration = 0; iteration < mutations; ++iteration) {
    source[(iteration * 17) % count] ^= 0xA83105Fu + iteration * 13;
    auto version = Acquire(cache, source, kinds[iteration % kinds.size()]);
    Require(Acquire(cache, source, version->kind()) == version,
            "Unchanged native bytes did not reuse their buffer");
    snapshots.push_back({version, source});
    dispatch(snapshots.back());
  }
  // Revisit old GPU objects after their cache entries have been evicted and
  // the original source array has changed many times, without waiting first.
  for (uint32_t i = 0; i < retained_checks; ++i)
    dispatch(snapshots[i]);
  // Guest DMA indices may already be owned by the GPU. Queue fresh GPU copies
  // before overwriting their source, and revisit retained copies after mutation.
  D3D11_BUFFER_DESC gpu_desc = {};
  gpu_desc.ByteWidth = (count + 2) * 4;
  ComPtr<ID3D11Buffer> gpu_source;
  Check(owner.device()->CreateBuffer(&gpu_desc, nullptr, gpu_source.GetAddressOf()),
        "Mutable GPU source");
  std::vector<Snapshot> gpu_snapshots;
  std::array<uint32_t, count + 2> gpu_words;
  gpu_words.front() = 0xDEAD1234;
  gpu_words.back() = 0xCAFE5678;
  for (uint32_t iteration = 0; iteration < mutations; ++iteration) {
    source[(iteration * 19) % count] ^= 0x7218A19u + iteration * 31;
    std::copy(source.begin(), source.end(), gpu_words.begin() + 1);
    context->UpdateSubresource(gpu_source.Get(), 0, nullptr, gpu_words.data(), 0, 0);
    std::string error;
    auto version =
        cache.SnapshotGpuRange(gpu_source.Get(), 4, count * 4, kinds[iteration % 3], error);
    if (!version)
      throw std::runtime_error(error);
    Require(version->size() == count * 4, "GPU snapshot lost its payload size");
    gpu_snapshots.push_back({std::move(version), source});
    dispatch(gpu_snapshots.back());
  }
  for (uint32_t i = 0; i < retained_checks; ++i)
    dispatch(gpu_snapshots[i]);
  statistics = cache.statistics();
  Require(statistics.evictions > 0 && statistics.retained_bytes <= 2048,
          "The forced native buffer cache budget was not exercised");
  Require(statistics.hits >= mutations, "Native buffer reuse was not exercised");
  cache.Clear();
  context->ClearState();
  desc.Usage = D3D11_USAGE_STAGING;
  desc.BindFlags = desc.MiscFlags = 0;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  Check(owner.device()->CreateBuffer(&desc, nullptr, staging.GetAddressOf()), "Buffer readback");
  context->CopyResource(staging.Get(), output.Get());
  Check(owner.WaitForCompletion(), "Queued buffer versions completion");
  D3D11_MAPPED_SUBRESOURCE mapped = {};
  Check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Buffer version readback");
  bool exact = std::memcmp(mapped.pData, expected.data(), expected.size() * 4) == 0;
  context->Unmap(staging.Get(), 0);
  Require(exact, "A queued or evicted native buffer changed its GPU data");
  std::string error;
  std::array<uint8_t, 3> malformed = {};
  Require(!cache.SnapshotGpuRange(gpu_source.Get(), 3, 4, BufferKind::kIndex16, error),
          "Misaligned GPU index snapshot accepted");
  Require(
      !cache.SnapshotGpuRange(gpu_source.Get(), gpu_desc.ByteWidth - 2, 4, BufferKind::kRaw, error),
      "Out-of-range GPU snapshot accepted");
  Require(!cache.SnapshotGpuRange(gpu_source.Get(), 0, 16, BufferKind::kConstant, error),
          "GPU constant snapshot accepted");
  Require(!cache.GetOrCreate(malformed, BufferKind::kIndex16, error),
          "Misaligned index16 accepted");
  Require(!cache.GetOrCreate(malformed, BufferKind::kIndex32, error),
          "Misaligned index32 accepted");
  Require(!cache.GetOrCreate({}, BufferKind::kRaw, error), "Empty native buffer accepted");
  return expected.size();
}
}  // namespace

int main(int argc, char** argv) {
  std::string output;
  try {
    D3D11Device::Options options;
    options.debug = true;
    options.maximum_feature_level = D3D_FEATURE_LEVEL_11_0;
    for (int i = 1; i < argc; ++i) {
      std::string arg(argv[i]);
      if (arg == "--warp")
        options.warp = true;
      else if (arg == "--output" && i + 1 < argc)
        output = argv[++i];
      else
        throw std::runtime_error("usage: d3d11_buffer_check [--warp] [--output report.json]");
    }
    std::string error;
    auto owner = D3D11Device::Create(options, error);
    if (!owner)
      throw std::runtime_error(error);
    BufferCache::Statistics statistics;
    uint64_t words = Run(*owner, statistics);
    uint64_t scaled_words = CheckScaledStorage(*owner);
    std::string report =
        "{\n  \"passed\": true,\n  \"scope\": \"immutable native buffer versions\",\n";
    report += "  \"feature_level\": " + std::to_string(owner->features().level) + ",\n";
    report +=
        "  \"software\": " + std::string(owner->features().software ? "true" : "false") + ",\n";
    report += "  \"exact_words\": " + std::to_string(words) + ",\n";
    report += "  \"scaled_preservation_words\": " + std::to_string(scaled_words) + ",\n";
    report += "  \"source_mutations\": 64,\n  \"retained_versions_revisited\": 16,\n";
    report += "  \"gpu_source_mutations\": 64,\n  \"gpu_versions_revisited\": 16,\n";
    report += "  \"cache_hits\": " + std::to_string(statistics.hits) + ",\n";
    report += "  \"cache_evictions\": " + std::to_string(statistics.evictions) + "\n}\n";
    std::fputs(report.c_str(), stdout);
    if (!output.empty()) {
      std::ofstream file(output);
      file << report;
      Require(bool(file), "Could not write buffer qualification");
    }
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
  }
}
