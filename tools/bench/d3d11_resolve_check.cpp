#include <rex/graphics/d3d11/resolve_copy.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace rex::graphics::d3d11;
namespace draw = rex::graphics::draw_util;
namespace xenos = rex::graphics::xenos;
using rex::ui::d3d11::D3D11Device;

namespace {
void Require(bool condition, const std::string& message) {
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

// CPU address reference for the guest's 32x32 texture tiles. The production
// path executes the SDK's compute bytecode and does not call this function.
uint32_t TextureAddress(uint32_t x, uint32_t y, uint32_t pitch, uint32_t size_log2) {
  uint32_t macro = ((x / 32) + (y / 32) * (pitch / 32)) << (size_log2 + 7);
  uint32_t micro = ((x % 8) + ((y & 14) * 4)) << size_log2;
  uint32_t offset = macro + (micro / 16) * 32 + (micro % 16) + (y % 2) * 16;
  return (offset / 512) * 4096 + (y & 16) * 128 + (offset & 448) * 4 +
         (((y / 4 & 2) + x / 8) % 4) * 64 + (offset % 64);
}
uint32_t SourceWord(uint32_t index) {
  return std::rotl(index * 0x9E3779B9u, 13) ^ 0xA7682F19u;
}
uint32_t EndianWord(uint32_t value, uint32_t endian) {
  std::array<uint8_t, 4> input, output;
  std::memcpy(input.data(), &value, 4);
  for (uint32_t i = 0; i < 4; ++i)
    output[i] = input[i ^ (endian == 1 ? 1 : endian == 2 ? 3 : endian == 3 ? 2 : 0)];
  std::memcpy(&value, output.data(), 4);
  return value;
}

struct Results {
  uint64_t compared_bytes = 0, written_bytes = 0;
  uint32_t dispatches = 0, rejected = 0, offset_dispatches = 0, mirrored_dispatches = 0;
};
struct Fixture {
  D3D11Device& device;
  DrawContext draws;
  ResolveCopy copy;
  Results results;
  explicit Fixture(D3D11Device& owner) : device(owner), draws(owner), copy(owner, draws) {}

  void Case(RenderTargetEncoder& encoder, uint32_t scale_x, uint32_t scale_y, bool wide, bool depth,
            uint32_t samples_log2, uint32_t sample, uint32_t endian, bool swap,
            int full_shader = -1, uint32_t reference_scale_x = 1, uint32_t reference_scale_y = 1) {
    constexpr uint32_t destination_bytes = 1024 * 1024, width = 64, height = 16, pitch = 128;
    uint32_t size_log2 = wide ? 3 : 2;
    if (full_shader >= 0)
      size_log2 = uint32_t(full_shader);
    const bool scaled = scale_x != 1 || scale_y != 1;
    const uint32_t view_offset = scaled ? 4096 : 0;
    draw::ResolveCopyShaderConstants constants = {};
    auto& edram = constants.dest_relative.edram_info;
    edram.pitch_tiles = 11;
    edram.base_tiles = 2044;
    edram.msaa_samples = xenos::MsaaSamples(samples_log2);
    edram.is_depth = depth;
    edram.format = wide ? 7 : 0;
    edram.format_is_64bpp = wide;
    auto& coordinate = constants.dest_relative.coordinate_info;
    coordinate.edram_offset_x_div_8 = 3;
    coordinate.edram_offset_y_div_8 = 1;
    coordinate.width_div_8 = width / 8;
    coordinate.draw_resolution_scale_x = scale_x;
    coordinate.draw_resolution_scale_y = scale_y;
    auto& target = constants.dest_relative.dest_coordinate_info;
    target.pitch_aligned_div_32 = pitch / 32;
    target.height_aligned_div_32 = 64 / 32;
    target.offset_x_div_8 = 2;
    target.offset_y_div_8 = 3;
    target.copy_sample_select = xenos::CopySampleSelect(sample);
    constants.dest_relative.dest_info.value = 0;
    constants.dest_relative.dest_info.copy_dest_endian = xenos::Endian128(endian);
    constants.dest_relative.dest_info.copy_dest_swap = swap;
    constexpr std::array<xenos::ColorFormat, 5> full_formats = {
        xenos::ColorFormat::k_8, xenos::ColorFormat::k_16, xenos::ColorFormat::k_8_8_8_8,
        xenos::ColorFormat::k_16_16_16_16, xenos::ColorFormat::k_32_32_32_32_FLOAT};
    constants.dest_relative.dest_info.copy_dest_format =
        full_shader >= 0 ? full_formats[size_log2]
        : wide           ? xenos::ColorFormat::k_16_16_16_16_FLOAT
                         : xenos::ColorFormat::k_8_8_8_8;
    constants.dest_base = 4096;
    auto shader = full_shader >= 0
                      ? draw::ResolveCopyShaderIndex(
                            uint32_t(draw::ResolveCopyShaderIndex::kFull8bpp) + size_log2)
                  : wide ? (samples_log2 == 2 ? draw::ResolveCopyShaderIndex::kFast64bpp4xMSAA
                                              : draw::ResolveCopyShaderIndex::kFast64bpp1x2xMSAA)
                         : (samples_log2 == 2 ? draw::ResolveCopyShaderIndex::kFast32bpp4xMSAA
                                              : draw::ResolveCopyShaderIndex::kFast32bpp1x2xMSAA);
    uint32_t group_size = full_shader == 0           ? 64
                          : full_shader == 4         ? 16
                          : full_shader >= 0 || wide ? 32
                                                     : 64;
    uint32_t groups_x = (width * scale_x + group_size - 1) / group_size;
    uint32_t groups_y = height * scale_y / 8;
    D3D11_BUFFER_DESC desc = {};
    desc.ByteWidth = destination_bytes;
    desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    std::vector<uint8_t> initial(destination_bytes, 0xCD), expected = initial;
    D3D11_SUBRESOURCE_DATA initial_data = {initial.data(), 0, 0};
    ComPtr<ID3D11Buffer> destination, staging;
    Check(device.device()->CreateBuffer(&desc, &initial_data, destination.GetAddressOf()),
          "Resolve destination buffer");
    D3D11_UNORDERED_ACCESS_VIEW_DESC view_desc = {};
    view_desc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    uint32_t view_size_log2 = full_shader == 0 || full_shader == 1 ? 3 : 4;
    view_desc.Format =
        view_size_log2 == 3 ? DXGI_FORMAT_R32G32_UINT : DXGI_FORMAT_R32G32B32A32_UINT;
    view_desc.Buffer.FirstElement = view_offset >> view_size_log2;
    view_desc.Buffer.NumElements = (destination_bytes - view_offset) >> view_size_log2;
    ComPtr<ID3D11UnorderedAccessView> view;
    Check(device.device()->CreateUnorderedAccessView(destination.Get(), &view_desc,
                                                     view.GetAddressOf()),
          "Resolve destination view");
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    Check(device.device()->CreateBuffer(&desc, nullptr, staging.GetAddressOf()),
          "Resolve destination staging");
    for (uint32_t y = 0; y < height * scale_y; ++y)
      for (uint32_t x = 0; x < width * scale_x; ++x) {
        uint32_t sx = (coordinate.edram_offset_x_div_8 * 8 * scale_x + x)
                      << uint32_t(samples_log2 == 2);
        uint32_t sy = (coordinate.edram_offset_y_div_8 * 8 * scale_y + y)
                      << uint32_t(samples_log2 != 0);
        sx += sample >> 1;
        sy += sample & 1;
        uint32_t tile_width = (wide ? 40 : 80) * scale_x, tile_height = 16 * scale_y;
        uint32_t tile =
            (edram.base_tiles + (sy / tile_height) * edram.pitch_tiles + sx / tile_width) % 2048;
        uint32_t sample_x = sx % tile_width;
        if (depth)
          sample_x = (sample_x + tile_width / 2) % tile_width;
        uint32_t source_y = sy % tile_height;
        if (reference_scale_x != 1 || reference_scale_y != 1) {
          // The mirror keeps each original MSAA sample and chooses the center
          // host pixel in its scaled footprint. Project after depth's tile swap.
          uint32_t sample_mask_x = uint32_t(samples_log2 == 2);
          uint32_t sample_mask_y = uint32_t(samples_log2 != 0);
          sample_x = (((sample_x >> sample_mask_x) * reference_scale_x + reference_scale_x / 2)
                      << sample_mask_x) +
                     (sample_x & sample_mask_x);
          source_y = (((source_y >> sample_mask_y) * reference_scale_y + reference_scale_y / 2)
                      << sample_mask_y) +
                     (source_y & sample_mask_y);
          tile_width *= reference_scale_x;
          tile_height *= reference_scale_y;
        }
        uint32_t word_index =
            (tile * tile_width * tile_height + source_y * tile_width + sample_x) * (wide ? 2 : 1);
        std::array<uint32_t, 4> value = {SourceWord(word_index), SourceWord(word_index + 1), 0, 0};
        if (swap) {
          if (wide) {
            uint32_t r = value[0] & 65535, b = value[1] & 65535;
            value[0] = (value[0] & 0xFFFF0000u) | b;
            value[1] = (value[1] & 0xFFFF0000u) | r;
          } else {
            value[0] =
                (value[0] & 0xFF00FF00u) | ((value[0] & 255) << 16) | ((value[0] >> 16) & 255);
          }
        }
        if (full_shader >= 0) {
          uint32_t rgba = value[0];
          if (size_log2 == 0)
            value[0] = rgba & 255;
          else if (size_log2 == 1)
            value[0] = (rgba & 255) * 257;
          else if (size_log2 == 3) {
            value[0] = (rgba & 255) * 257 | (((rgba >> 8) & 255) * 257 << 16);
            value[1] = ((rgba >> 16) & 255) * 257 | ((rgba >> 24) * 257 << 16);
          } else if (size_log2 == 4) {
            for (uint32_t c = 0; c < 4; ++c)
              value[c] = std::bit_cast<uint32_t>(float((rgba >> (c * 8)) & 255) * (1.0f / 255));
          }
        }
        for (auto& word : value)
          word = EndianWord(word, endian);
        // Scaled guest storage expands a typed buffer element, rather than
        // each individual texel. Its X subelement precedes Y in that block.
        uint32_t block_size = size_log2 == 0 ? 8 : 16;
        uint32_t pixels_per_block = block_size >> size_log2;
        uint32_t host_x = x + target.offset_x_div_8 * 8 * scale_x;
        uint32_t block_x = host_x / pixels_per_block;
        uint32_t guest_x = block_x / scale_x * pixels_per_block;
        uint32_t address =
            TextureAddress(guest_x, y / scale_y + target.offset_y_div_8 * 8, pitch, size_log2);
        address = address * scale_x * scale_y +
                  ((block_x % scale_x) * scale_y + y % scale_y) * block_size +
                  (host_x % pixels_per_block) * (1u << size_log2);
        if (!scaled)
          address += constants.dest_base;
        address += view_offset;
        Require(address + (1u << size_log2) <= expected.size(), "CPU resolve reference extent");
        std::memcpy(expected.data() + address, value.data(), 1u << size_log2);
        results.written_bytes += 1u << size_log2;
      }
    std::string error;
    bool copied =
        copy.Copy(encoder, shader, constants, view.Get(), groups_x, groups_y, scaled, error);
    Require(copied, "Resolve shader " + std::to_string(unsigned(shader)) + ": " + error);
    ++results.dispatches;
    results.offset_dispatches += scaled;
    auto invalid = constants;
    invalid.dest_relative.coordinate_info.draw_resolution_scale_x = 0;
    Require(!copy.Copy(encoder, shader, invalid, view.Get(), groups_x, groups_y, scaled, error),
            "Zero resolution scale was accepted");
    Require(!copy.Copy(encoder, draw::ResolveCopyShaderIndex::kUnknown, constants, view.Get(),
                       groups_x, groups_y, scaled, error),
            "Invalid resolve shader was accepted");
    Require(
        !copy.Copy(encoder, shader, constants, view.Get(), groups_x + 1, groups_y, scaled, error),
        "Invalid resolve dispatch width was accepted");
    results.rejected += 3;
    draws.Invalidate();
    device.context()->CopyResource(staging.Get(), destination.Get());
    Check(device.WaitForCompletion(), "Native resolve completion");
    D3D11_MAPPED_SUBRESOURCE mapping = {};
    Check(device.context()->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapping),
          "Native resolve map");
    const auto* actual = static_cast<const uint8_t*>(mapping.pData);
    size_t mismatch = expected.size();
    uint32_t actual_byte = 0;
    for (size_t i = 0; i < expected.size(); ++i)
      if (actual[i] != expected[i]) {
        mismatch = i;
        actual_byte = actual[i];
        break;
      }
    device.context()->Unmap(staging.Get(), 0);
    if (mismatch != expected.size()) {
      char message[256];
      std::snprintf(message, sizeof(message),
                    "Resolve mismatch shader=%u scale=%ux%u wide=%u depth=%u msaa=%u sample=%u "
                    "endian=%u swap=%u byte=%llu actual=%02X expected=%02X",
                    unsigned(shader), scale_x, scale_y, unsigned(wide), unsigned(depth),
                    samples_log2, sample, endian, unsigned(swap),
                    static_cast<unsigned long long>(mismatch), actual_byte, expected[mismatch]);
      throw std::runtime_error(message);
    }
    results.compared_bytes += expected.size();
    results.mirrored_dispatches += reference_scale_x != 1 || reference_scale_y != 1;
  }
};
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
        throw std::runtime_error("usage: d3d11_resolve_check [--warp] [--output report.json]");
    }
    std::string error;
    auto device = D3D11Device::Create(options, error);
    Require(bool(device), error);
    Fixture fixture(*device);
    for (const auto& scale :
         std::array<std::array<uint32_t, 2>, 5>{{{1, 1}, {2, 2}, {2, 1}, {1, 2}, {3, 1}}}) {
      RenderTargetEncoder encoder(*device, fixture.draws);
      rex::graphics::DxbcRenderTargetDumpShader::Options encoder_options;
      encoder_options.resolution_scale_x = scale[0];
      encoder_options.resolution_scale_y = scale[1];
      Require(encoder.Initialize(encoder_options, error), error);
      std::vector<uint32_t> input(encoder.byte_size() / 4);
      for (uint32_t i = 0; i < input.size(); ++i)
        input[i] = SourceWord(i);
      fixture.draws.Invalidate();
      device->context()->UpdateSubresource(encoder.buffer(), 0, nullptr, input.data(), 0, 0);
      for (uint32_t msaa = 0; msaa < 3; ++msaa)
        for (uint32_t sample = 0; sample < (1u << msaa); ++sample)
          for (uint32_t endian = 0; endian < 4; ++endian) {
            for (bool wide : {false, true})
              for (bool swap : {false, true})
                fixture.Case(encoder, scale[0], scale[1], wide, false, msaa, sample, endian, swap);
            fixture.Case(encoder, scale[0], scale[1], false, true, msaa, sample, endian, false);
          }
      for (int size = 0; size < 5; ++size)
        fixture.Case(encoder, scale[0], scale[1], false, false, 0, 0, 0, false, size);
      if (scale[0] != 1 || scale[1] != 1) {
        RenderTargetEncoder mirror(*device, fixture.draws);
        rex::graphics::DxbcRenderTargetDumpShader::Options mirror_options;
        Require(mirror.Initialize(mirror_options, error), error);
        for (uint32_t msaa = 0; msaa < 3; ++msaa)
          for (uint32_t sample = 0; sample < (1u << msaa); ++sample)
            for (uint32_t endian = 0; endian < 4; ++endian) {
              for (bool wide : {false, true}) {
                Require(mirror.DownsampleFrom(encoder, 2044, 11, 4, 11, wide,
                                              xenos::MsaaSamples(msaa), error),
                        error);
                for (bool swap : {false, true})
                  fixture.Case(mirror, 1, 1, wide, false, msaa, sample, endian, swap, -1, scale[0],
                               scale[1]);
              }
              Require(mirror.DownsampleFrom(encoder, 2044, 11, 4, 11, false,
                                            xenos::MsaaSamples(msaa), error),
                      error);
              fixture.Case(mirror, 1, 1, false, true, msaa, sample, endian, false, -1, scale[0],
                           scale[1]);
            }
        Require(
            mirror.DownsampleFrom(encoder, 2044, 11, 4, 11, false, xenos::MsaaSamples::k1X, error),
            error);
        for (int size = 0; size < 5; ++size)
          fixture.Case(mirror, 1, 1, false, false, 0, 0, 0, false, size, scale[0], scale[1]);
        Require(
            !mirror.DownsampleFrom(encoder, 2044, 0, 4, 11, false, xenos::MsaaSamples::k1X, error),
            "An empty mirror span was accepted");
        Require(
            !mirror.DownsampleFrom(mirror, 2044, 11, 4, 11, false, xenos::MsaaSamples::k1X, error),
            "An aliased mirror buffer was accepted");
        fixture.results.rejected += 2;
      }
    }
    fixture.draws.Invalidate();
    Check(device->WaitForCompletion(), "Resolve fixture completion");
    ComPtr<ID3D11InfoQueue> messages;
    if (SUCCEEDED(device->device()->QueryInterface(IID_PPV_ARGS(messages.GetAddressOf())))) {
      for (uint64_t i = 0; i < messages->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
        SIZE_T size = 0;
        Check(messages->GetMessage(i, nullptr, &size), "Resolve debug message size");
        std::vector<uint8_t> data(size);
        auto* message = reinterpret_cast<D3D11_MESSAGE*>(data.data());
        Check(messages->GetMessage(i, message, &size), "Resolve debug message");
        if (message->Severity <= D3D11_MESSAGE_SEVERITY_WARNING)
          throw std::runtime_error(message->pDescription);
      }
    }
    const auto& result = fixture.results;
    std::string report =
        "{\"passed\":true,\"scope\":\"native tiled resolve compute copies\",\"feature_level\":" +
        std::to_string(device->features().level) +
        ",\"software\":" + (device->features().software ? "true" : "false") +
        ",\"dispatches\":" + std::to_string(result.dispatches) +
        ",\"offset_view_dispatches\":" + std::to_string(result.offset_dispatches) +
        ",\"mirrored_dispatches\":" + std::to_string(result.mirrored_dispatches) +
        ",\"compared_bytes\":" + std::to_string(result.compared_bytes) +
        ",\"written_bytes\":" + std::to_string(result.written_bytes) +
        ",\"rejected_dispatches\":" + std::to_string(result.rejected) + "}\n";
    std::fputs(report.c_str(), stdout);
    if (!output.empty()) {
      std::ofstream file(output);
      file << report;
      Require(bool(file), "Could not write resolve qualification");
    }
    return 0;
  } catch (const std::exception& failure) {
    std::fprintf(stderr, "FAIL: %s\n", failure.what());
    if (!output.empty()) {
      std::string message;
      for (char c : std::string(failure.what())) {
        if (c == '\\' || c == '"')
          message += '\\';
        message += c == '\n' || c == '\r' ? ' ' : c;
      }
      std::ofstream file(output);
      file << "{\"passed\":false,\"error\":\"" << message << "\"}\n";
    }
    return 1;
  }
}
