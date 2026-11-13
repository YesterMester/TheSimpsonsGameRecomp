#include <rex/cvar.h>
#include <rex/graphics/d3d11/texture_cache.h>
#include <rex/logging.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace rex::graphics;
using namespace rex::graphics::d3d11;

namespace {
void Require(bool condition, const std::string& message) {
  if (!condition)
    throw std::runtime_error(message);
}

std::vector<uint8_t> Pattern(uint32_t size, uint32_t seed) {
  std::vector<uint8_t> bytes(size);
  for (uint32_t i = 0; i < size; ++i)
    bytes[i] = uint8_t((i * 37 + i / 251 + seed * 53) ^ (i >> 7) ^ 0xB3);
  return bytes;
}

struct Fixture {
  rex::ui::d3d11::D3D11Device& device;
  uint64_t compared_bytes = 0;
  uint32_t cases = 0, decoded_images = 0;

  void Compare(ID3D11ShaderResourceView* view, const texture_util::TextureGuestLayout& layout,
               bool tiled, bool packed, uint32_t endian, const std::vector<uint8_t>& base,
               const std::vector<uint8_t>& mips) {
    ComPtr<ID3D11Resource> resource;
    view->GetResource(resource.GetAddressOf());
    ComPtr<ID3D11Texture2D> image;
    Require(SUCCEEDED(resource.As(&image)), "Decoded texture is not a native 2D image");
    D3D11_TEXTURE2D_DESC description;
    image->GetDesc(&description);
    Require(description.Width == 64 && description.Height == 64 && description.MipLevels == 7 &&
                description.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS,
            "Decoded texture has an unexpected layout");
    description.Usage = D3D11_USAGE_STAGING;
    description.BindFlags = 0;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    description.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> readback;
    Require(
        SUCCEEDED(device.device()->CreateTexture2D(&description, nullptr, readback.GetAddressOf())),
        "Could not allocate texture readback");
    device.context()->CopyResource(readback.Get(), image.Get());
    constexpr std::array<uint32_t, 4> byte_xor = {0, 1, 3, 2};
    for (uint32_t level = 0; level < description.MipLevels; ++level) {
      uint32_t stored_level = level ? (packed ? std::min(level, layout.packed_level) : level) : 0;
      const auto& stored = level ? layout.mips[stored_level] : layout.base;
      const auto& bytes = level ? mips : base;
      uint32_t origin = level ? layout.mip_offsets_bytes[stored_level] : 0;
      uint32_t offset_x = 0, offset_y = 0, offset_z = 0;
      if (packed && level >= layout.packed_level)
        texture_util::GetPackedMipOffset(64, 64, 1, xenos::TextureFormat::k_8_8_8_8, level,
                                         offset_x, offset_y, offset_z);
      uint32_t side = 64 >> level;
      D3D11_MAPPED_SUBRESOURCE mapping = {};
      Require(SUCCEEDED(device.context()->Map(readback.Get(), level, D3D11_MAP_READ, 0, &mapping)),
              "Could not map decoded texture");
      std::string failure;
      for (uint32_t y = 0; y < side && failure.empty(); ++y) {
        const auto* row = static_cast<const uint8_t*>(mapping.pData) + y * mapping.RowPitch;
        for (uint32_t x = 0; x < side && failure.empty(); ++x) {
          uint32_t input =
              origin + (tiled ? uint32_t(texture_util::GetTiledOffset2D(
                                    int32_t(x + offset_x), int32_t(y + offset_y),
                                    stored.row_pitch_bytes / 4, 2))
                              : (y + offset_y) * stored.row_pitch_bytes + (x + offset_x) * 4);
          for (uint32_t channel = 0; channel < 4; ++channel) {
            uint8_t expected = bytes.at(input + (channel ^ byte_xor[endian]));
            if (row[x * 4 + channel] != expected) {
              failure = "Decoded texture mismatch in case " + std::to_string(cases) + ", mip " +
                        std::to_string(level) + ", pixel " + std::to_string(x) + "," +
                        std::to_string(y) + ", channel " + std::to_string(channel) + ": " +
                        std::to_string(row[x * 4 + channel]) + " != " + std::to_string(expected);
              break;
            }
            ++compared_bytes;
          }
        }
      }
      device.context()->Unmap(readback.Get(), level);
      Require(failure.empty(), failure);
    }
    ++decoded_images;
  }

  void Run() {
    constexpr uint32_t first_address = 16u << 20;
    rex::memory::Memory guest;
    Require(guest.Initialize(), "Could not initialize physical memory");
    Require(guest.LookupHeap(0xA0000000)
                ->AllocFixed(
                    0xA0000000 + first_address, 32u << 20, 65536,
                    rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
                    rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite),
            "Could not allocate CPU-visible texture inputs");
    TraceWriter trace(guest.physical_membase());
    DrawContext draws(device);
    D3D11SharedMemory memory(device, draws, guest, trace);
    NativeGpuDiagnostics diagnostics;
    diagnostics.Initialize("d3d11");
    memory.SetNativeDiagnostics(&diagnostics);
    std::string error;
    Require(memory.Initialize(error), error);
    RegisterFile registers;
    D3D11TextureCache cache(registers, memory, device, draws, 1, 1);
    for (bool tiled : {false, true}) {
      for (bool packed : {false, true}) {
        auto layout = texture_util::GetGuestTextureLayout(
            xenos::DataDimension::k2DOrStacked, 2, 64, 64, 1, tiled,
            xenos::TextureFormat::k_8_8_8_8, packed, true, 6);
        for (uint32_t endian = 0; endian < 4; ++endian) {
          for (bool gpu_base : {false, true}) {
            uint32_t base_address = first_address + cases * (1u << 20);
            uint32_t mip_address = base_address + (512u << 10);
            auto base = Pattern(rex::align(layout.base.level_data_extent_bytes, uint32_t(4096)), 1);
            auto mips = Pattern(rex::align(layout.mips_total_extent_bytes, uint32_t(4096)), 2);
            auto write_cpu = [&](uint32_t address, const std::vector<uint8_t>& data) {
              std::memcpy(guest.TranslateVirtual(0xA0000000 + address), data.data(), data.size());
            };
            write_cpu(base_address, base);
            write_cpu(mip_address, mips);
            auto& gpu_bytes = gpu_base ? base : mips;
            auto& cpu_bytes = gpu_base ? mips : base;
            uint32_t gpu_address = gpu_base ? base_address : mip_address;
            auto write_gpu = [&](uint32_t seed) {
              auto native =
                  memory.PrepareNativeResolve(gpu_address, uint32_t(gpu_bytes.size()), 2, error);
              Require(bool(native), error);
              gpu_bytes = Pattern(uint32_t(gpu_bytes.size()), seed);
              device.context()->UpdateSubresource(native->buffer.Get(), 0, nullptr,
                                                  gpu_bytes.data(), 0, 0);
              memory.BeginNativeResolveWrite(native);
              memory.RangeWrittenByGpu(gpu_address, uint32_t(gpu_bytes.size()));
              Require(memory.EndNativeResolveWrite(), "Could not publish native texture input");
            };
            write_gpu(3);
            xenos::xe_gpu_texture_fetch_t fetch = {};
            fetch.type = xenos::FetchConstantType::kTexture;
            fetch.pitch = 2;
            fetch.tiled = tiled;
            fetch.format = xenos::TextureFormat::k_8_8_8_8;
            fetch.endianness = xenos::Endian(endian);
            fetch.base_address = base_address >> 12;
            fetch.mip_address = mip_address >> 12;
            fetch.size_2d.width = 63;
            fetch.size_2d.height = 63;
            fetch.swizzle = xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA;
            fetch.dimension = xenos::DataDimension::k2DOrStacked;
            fetch.mip_max_level = 6;
            fetch.packed_mips = packed;
            std::memcpy(&registers[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0], &fetch, sizeof(fetch));
            auto decode = [&] {
              xenos::TextureFormat format;
              uint32_t swizzle, width, height;
              bool scaled;
              auto view = cache.RequestSwapTexture(format, swizzle, width, height, scaled);
              Require(bool(view), cache.last_error());
              Require(!scaled && width == 64 && height == 64,
                      "Native mixed inputs changed texture dimensions");
              Compare(view.Get(), layout, tiled, packed, endian, base, mips);
              Require(!memory.buffer(), "Mixed texture inputs allocated the compatibility mirror");
            };
            decode();
            cpu_bytes = Pattern(uint32_t(cpu_bytes.size()), 4);
            write_cpu(gpu_base ? mip_address : base_address, cpu_bytes);
            decode();
            write_gpu(5);
            decode();
            ++cases;
          }
        }
      }
    }
    diagnostics.WriteReport(true);
  }
};
}  // namespace

int main(int argc, char** argv) {
  std::string output;
  try {
    rex::ui::d3d11::D3D11Device::Options options;
    for (int i = 1; i < argc; ++i) {
      std::string arg(argv[i]);
      if (arg == "--warp")
        options.warp = true;
      else if (arg == "--output" && i + 1 < argc)
        output = argv[++i];
      else
        throw std::runtime_error(
            "usage: d3d11_native_texture_check [--warp] [--output report.json]");
    }
    rex::InitLogging("native-texture-runtime.log", spdlog::level::info);
    Require(rex::cvar::SetFlagByName("gpu_stream_dynamic_pages", "false") &&
                rex::cvar::SetFlagByName("d3d11_native_texture_uploads", "true") &&
                rex::cvar::SetFlagByName("native_gpu_diagnostics", "true") &&
                rex::cvar::SetFlagByName("native_gpu_strict", "true") &&
                rex::cvar::SetFlagByName("native_gpu_report_path", "native-texture-coverage.json"),
            "Could not configure native texture qualification");
    std::string error;
    auto device = rex::ui::d3d11::D3D11Device::Create(options, error);
    Require(bool(device), error);
    Fixture fixture{*device};
    fixture.Run();
    std::string report = "{\n  \"passed\": true,\n  \"cases\": " + std::to_string(fixture.cases) +
                         ",\n  \"decoded_images\": " + std::to_string(fixture.decoded_images) +
                         ",\n  \"exact_bytes\": " + std::to_string(fixture.compared_bytes) +
                         "\n}\n";
    std::fputs(report.c_str(), stdout);
    if (!output.empty()) {
      std::ofstream file(output);
      file << report;
      Require(bool(file), "Could not write native texture report");
    }
    return 0;
  } catch (const std::exception& exception) {
    std::fprintf(stderr, "DX11 native texture check failed: %s\n", exception.what());
    return 1;
  }
}
