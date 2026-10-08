#include <rex/graphics/d3d11/texture_upload.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace rex::graphics::d3d11;
using rex::ui::d3d11::D3D11Device;

namespace {
void Require(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}
void Check(HRESULT result, const char* operation) {
  if (FAILED(result)) {
    char message[160];
    std::snprintf(message, sizeof(message), "%s failed (0x%08X)", operation, unsigned(result));
    throw std::runtime_error(message);
  }
}
struct Format {
  DXGI_FORMAT resource;
  DXGI_FORMAT view;
  uint32_t bytes;
};
constexpr std::array<Format, 10> formats = {
    {{DXGI_FORMAT_R8_TYPELESS, DXGI_FORMAT_R8_UINT, 1},
     {DXGI_FORMAT_R8G8_TYPELESS, DXGI_FORMAT_R8G8_UINT, 2},
     {DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UINT, 4},
     {DXGI_FORMAT_R10G10B10A2_TYPELESS, DXGI_FORMAT_R10G10B10A2_UINT, 4},
     {DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_R16_UINT, 2},
     {DXGI_FORMAT_R16G16_TYPELESS, DXGI_FORMAT_R16G16_UINT, 4},
     {DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R16G16B16A16_UINT, 8},
     {DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_R32_UINT, 4},
     {DXGI_FORMAT_R32G32_TYPELESS, DXGI_FORMAT_R32G32_UINT, 8},
     {DXGI_FORMAT_R32G32B32A32_TYPELESS, DXGI_FORMAT_R32G32B32A32_UINT, 16}}};

struct Result {
  uint64_t bytes = 0;
  uint32_t uploads = 0;
  uint32_t negatives = 0;
};

void CheckDebugMessages(D3D11Device& owner) {
  ComPtr<ID3D11InfoQueue> queue;
  if (FAILED(owner.device()->QueryInterface(IID_PPV_ARGS(queue.GetAddressOf()))))
    return;
  for (uint64_t i = 0; i < queue->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
    SIZE_T bytes = 0;
    Check(queue->GetMessage(i, nullptr, &bytes), "Texture debug message size");
    std::vector<uint8_t> storage(bytes);
    auto* message = reinterpret_cast<D3D11_MESSAGE*>(storage.data());
    Check(queue->GetMessage(i, message, &bytes), "Texture debug message read");
    if (message->Severity <= D3D11_MESSAGE_SEVERITY_WARNING)
      throw std::runtime_error(message->pDescription);
  }
}

void CheckFormat(D3D11Device& owner, DrawContext& draws, BufferCache& buffers,
                 TextureUpload& uploader, const Format& format, uint32_t dimension, Result& result,
                 uint32_t volume_first_slice = 1) {
  constexpr uint32_t width = 19, height = 13, mips = 2;
  ComPtr<ID3D11Resource> image, staging;
  bool volume = dimension == 3, array = dimension == 2;
  uint32_t layers = array ? 6 : 1;
  if (volume) {
    D3D11_TEXTURE3D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.Depth = 5;
    desc.MipLevels = mips;
    desc.Format = format.resource;
    desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture3D> texture;
    Check(owner.device()->CreateTexture3D(&desc, nullptr, texture.GetAddressOf()), "Native volume");
    image = texture;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    texture.Reset();
    Check(owner.device()->CreateTexture3D(&desc, nullptr, texture.GetAddressOf()),
          "Volume readback");
    staging = texture;
  } else {
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.ArraySize = layers;
    desc.MipLevels = mips;
    desc.Format = format.resource;
    desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> texture;
    Check(owner.device()->CreateTexture2D(&desc, nullptr, texture.GetAddressOf()),
          "Native texture");
    image = texture;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    texture.Reset();
    Check(owner.device()->CreateTexture2D(&desc, nullptr, texture.GetAddressOf()),
          "Texture readback");
    staging = texture;
  }
  std::vector<std::vector<uint8_t>> expected(layers * mips);
  draws.Invalidate();
  for (uint32_t layer = 0; layer < layers; ++layer) {
    for (uint32_t mip = 0; mip < mips; ++mip) {
      uint32_t w = width >> mip, h = height >> mip, d = volume ? 5u >> mip : 1;
      auto& data = expected[layer * mips + mip];
      data.assign(w * h * d * format.bytes, 0xCC);
      owner.context()->UpdateSubresource(image.Get(), layer * mips + mip, nullptr, data.data(),
                                         w * format.bytes, w * h * format.bytes);
    }
  }
  for (uint32_t mip = 0; mip < mips; ++mip) {
    uint32_t w = width >> mip, h = height >> mip;
    uint32_t first_slice = volume ? std::min(volume_first_slice, (5u >> mip) - 1) : 0;
    uint32_t planes = volume ? (5u >> mip) - first_slice : array ? 4 : 1;
    TextureUpload::Layout layout;
    layout.offset = 13;
    layout.width = w - 2;
    layout.height = h - 1;
    layout.depth_or_layers = planes;
    layout.row_pitch = layout.width * format.bytes + 5;
    layout.slice_pitch = layout.row_pitch * layout.height + 7;
    uint32_t size = (layout.offset + layout.slice_pitch * planes + 3) & ~3u;
    std::vector<uint8_t> source(size, 0xE7);
    for (uint32_t z = 0; z < planes; ++z) {
      for (uint32_t y = 0; y < layout.height; ++y) {
        for (uint32_t x = 0; x < layout.width * format.bytes; ++x) {
          // Dense arbitrary bit patterns exercise packed RGB10A2, half floats,
          // float NaNs and texels crossing unaligned raw-buffer words.
          uint8_t value = uint8_t(x * 37 + y * 67 + z * 101 + mip * 137 + format.bytes * 13);
          source[layout.offset + z * layout.slice_pitch + y * layout.row_pitch + x] = value;
          auto& target = expected[array ? (z + 1) * mips + mip : mip];
          uint32_t slice = volume ? z + first_slice : 0;
          target[(slice * h + y) * w * format.bytes + x] = value;
        }
      }
    }
    std::string error;
    auto version = buffers.GetOrCreate(source, BufferKind::kRaw, error);
    if (!version)
      throw std::runtime_error(error);
    D3D11_UNORDERED_ACCESS_VIEW_DESC desc = {};
    desc.Format = format.view;
    if (volume) {
      desc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE3D;
      desc.Texture3D.MipSlice = mip;
      desc.Texture3D.FirstWSlice = first_slice;
      desc.Texture3D.WSize = planes;
    } else if (array) {
      desc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
      desc.Texture2DArray.MipSlice = mip;
      desc.Texture2DArray.FirstArraySlice = 1;
      desc.Texture2DArray.ArraySize = planes;
    } else {
      desc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
      desc.Texture2D.MipSlice = mip;
    }
    ComPtr<ID3D11UnorderedAccessView> destination;
    Check(owner.device()->CreateUnorderedAccessView(image.Get(), &desc, destination.GetAddressOf()),
          "Native upload view");
    if (!uploader.Upload(version->raw_view(), destination.Get(), layout, error))
      throw std::runtime_error(error);
    ++result.uploads;
    auto malformed = layout;
    malformed.row_pitch = 1;
    Require(!uploader.Upload(version->raw_view(), destination.Get(), malformed, error),
            "Overlapping source rows accepted");
    malformed = layout;
    malformed.offset = UINT32_MAX;
    Require(!uploader.Upload(version->raw_view(), destination.Get(), malformed, error),
            "Upload source overflow accepted");
    malformed = layout;
    malformed.width = w + 1;
    malformed.row_pitch = (w + 1) * format.bytes;
    malformed.slice_pitch = malformed.row_pitch * h;
    Require(!uploader.Upload(version->raw_view(), destination.Get(), malformed, error),
            "Destination extent overflow accepted");
    result.negatives += 3;
  }
  draws.Invalidate();
  owner.context()->CopyResource(staging.Get(), image.Get());
  Check(owner.WaitForCompletion(), "Native texture upload completion");
  for (uint32_t layer = 0; layer < layers; ++layer) {
    for (uint32_t mip = 0; mip < mips; ++mip) {
      uint32_t w = width >> mip, h = height >> mip, d = volume ? 5u >> mip : 1;
      uint32_t subresource = layer * mips + mip;
      D3D11_MAPPED_SUBRESOURCE mapping;
      Check(owner.context()->Map(staging.Get(), subresource, D3D11_MAP_READ, 0, &mapping),
            "Native texture upload map");
      const auto& reference = expected[subresource];
      bool exact = true;
      for (uint32_t z = 0; z < d; ++z) {
        for (uint32_t y = 0; y < h; ++y) {
          exact &=
              !std::memcmp(static_cast<const uint8_t*>(mapping.pData) + z * mapping.DepthPitch +
                               y * mapping.RowPitch,
                           reference.data() + (z * h + y) * w * format.bytes, w * format.bytes);
          result.bytes += w * format.bytes;
        }
      }
      owner.context()->Unmap(staging.Get(), subresource);
      if (!exact) {
        char error[160];
        std::snprintf(error, sizeof(error),
                      "Native image bits or neighboring texels changed: format=%u dimension=%u "
                      "mip=%u layer=%u",
                      unsigned(format.view), dimension, mip, layer);
        throw std::runtime_error(error);
      }
    }
  }
}
void CheckPacked(D3D11Device& owner, DrawContext& draws, BufferCache& buffers,
                 TextureUpload& uploader, Result& result) {
  constexpr uint32_t width = 256, height = 256;
  std::vector<uint16_t> packed(width * height);
  for (uint32_t i = 0; i < packed.size(); ++i)
    packed[i] = uint16_t(i);
  std::string error;
  auto input =
      buffers.GetOrCreate({reinterpret_cast<const uint8_t*>(packed.data()), packed.size() * 2},
                          BufferKind::kRaw, error);
  Require(bool(input), error.c_str());
  D3D11_TEXTURE2D_DESC desc = {};
  desc.Width = width;
  desc.Height = height;
  desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
  desc.Format = DXGI_FORMAT_R32G32B32A32_TYPELESS;
  desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
  ComPtr<ID3D11Texture2D> image, staging;
  Check(owner.device()->CreateTexture2D(&desc, nullptr, image.GetAddressOf()),
        "Packed native image");
  D3D11_UNORDERED_ACCESS_VIEW_DESC view_desc = {};
  view_desc.Format = DXGI_FORMAT_R32G32B32A32_UINT;
  view_desc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
  ComPtr<ID3D11UnorderedAccessView> view;
  Check(owner.device()->CreateUnorderedAccessView(image.Get(), &view_desc, view.GetAddressOf()),
        "Packed native view");
  desc.BindFlags = 0;
  desc.Usage = D3D11_USAGE_STAGING;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  Check(owner.device()->CreateTexture2D(&desc, nullptr, staging.GetAddressOf()), "Packed readback");
  using Packing = TextureUpload::PackedSource;
  for (auto packing : {Packing::kB5G6R5, Packing::kB5G5R5A1, Packing::kB4G4R4A4}) {
    TextureUpload::Layout layout = {0, width * 2, width * height * 2, width, height, 1, packing};
    Require(uploader.Upload(input->raw_view(), view.Get(), layout, error), error.c_str());
    ++result.uploads;
    draws.Invalidate();
    owner.context()->CopyResource(staging.Get(), image.Get());
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    Check(owner.context()->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Packed map");
    bool exact = true;
    std::string mismatch;
    std::array<uint32_t, 4> shifts =
        packing == Packing::kB5G6R5     ? std::array<uint32_t, 4>{11, 5, 0, 0}
        : packing == Packing::kB5G5R5A1 ? std::array<uint32_t, 4>{10, 5, 0, 15}
                                        : std::array<uint32_t, 4>{8, 4, 0, 12};
    std::array<uint32_t, 4> masks =
        packing == Packing::kB5G6R5     ? std::array<uint32_t, 4>{31, 63, 31, 0}
        : packing == Packing::kB5G5R5A1 ? std::array<uint32_t, 4>{31, 31, 31, 1}
                                        : std::array<uint32_t, 4>{15, 15, 15, 15};
    for (uint32_t y = 0; y < height; ++y) {
      const auto* row = reinterpret_cast<const float*>(static_cast<const uint8_t*>(mapped.pData) +
                                                       y * mapped.RowPitch);
      for (uint32_t x = 0; x < width; ++x)
        for (uint32_t c = 0; c < 4; ++c) {
          double reference =
              masks[c] ? double(((y * width + x) >> shifts[c]) & masks[c]) / masks[c] : 1;
          if (row[x * 4 + c] != float(reference)) {
            if (exact) {
              char message[200];
              std::snprintf(
                  message, sizeof(message),
                  "Packed mismatch: packing=%u value=%u channel=%u actual=%.10g expected=%.10g",
                  unsigned(packing), y * width + x, c, double(row[x * 4 + c]), reference);
              mismatch = message;
            }
            exact = false;
          }
        }
    }
    owner.context()->Unmap(staging.Get(), 0);
    Require(exact, mismatch.c_str());
    result.bytes += uint64_t(width) * height * 16;
  }
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
        throw std::runtime_error(
            "usage: d3d11_texture_upload_check [--warp] [--output report.json]");
    }
    std::string error;
    auto owner = D3D11Device::Create(options, error);
    if (!owner)
      throw std::runtime_error(error);
    DrawContext draws(*owner);
    BufferCache buffers(*owner);
    TextureUpload uploader(*owner, draws);
    Result result;
    for (const auto& format : formats) {
      for (uint32_t dimension = 1; dimension <= 3; ++dimension) {
        CheckFormat(*owner, draws, buffers, uploader, format, dimension, result);
        if (dimension == 3) {
          CheckFormat(*owner, draws, buffers, uploader, format, dimension, result, 0);
          CheckFormat(*owner, draws, buffers, uploader, format, dimension, result, 2);
        }
      }
    }
    CheckPacked(*owner, draws, buffers, uploader, result);
    uploader.Clear();
    CheckDebugMessages(*owner);
    std::string report =
        "{\n  \"passed\":true,\n  \"scope\":\"native GPU buffer to image transfer\",\n";
    report += "  \"feature_level\":" + std::to_string(owner->features().level) + ",\n";
    report +=
        "  \"software\":" + std::string(owner->features().software ? "true" : "false") + ",\n";
    report += "  \"exact_bytes\":" + std::to_string(result.bytes) + ",\n";
    report += "  \"native_format_families\":10,\n  \"dimensions\":3,\n  \"mip_levels\":2,\n";
    report += "  \"volume_subview_origins\":3,\n";
    report += "  \"uploads\":" + std::to_string(result.uploads) + ",\n";
    report += "  \"rejected_layouts\":" + std::to_string(result.negatives) + "\n}\n";
    std::fputs(report.c_str(), stdout);
    if (!output.empty()) {
      std::ofstream file(output);
      file << report;
      Require(bool(file), "Could not write texture qualification");
    }
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    if (!output.empty()) {
      std::ofstream file(output);
      std::string message;
      for (char character : std::string(error.what())) {
        if (character == '\\' || character == '"')
          message += '\\';
        message += character == '\n' || character == '\r' ? ' ' : character;
      }
      file << "{\"passed\":false,\"error\":\"" << message << "\"}\n";
    }
    return 1;
  }
}
