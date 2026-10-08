#include <rex/graphics/d3d11/render_target_encoder.h>
#include <rex/graphics/util/dxbc_render_target_transfer.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <unordered_map>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace rex::graphics::d3d11;
using rex::graphics::DxbcRenderTargetTransferShader;
using rex::graphics::xenos::MsaaSamples;
using rex::ui::d3d11::D3D11Device;

namespace {
using Pixel = std::array<uint32_t, 4>;
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
struct Result {
  uint64_t pixels = 0, exact_integer_components = 0;
  uint64_t encoded_words = 0, preserved_encoding_words = 0;
  uint32_t surfaces = 0, clears = 0, transfers = 0, snapshots = 0, rejected = 0;
  uint32_t encodes = 0, stencil_reference_transfers = 0;
};
void CheckDebugMessages(D3D11Device& owner);
struct Fixture {
  D3D11Device& device;
  DrawContext draws;
  RenderTargetTransfer transfer;
  ShaderCache shaders;
  std::unordered_map<uint32_t, std::unique_ptr<RenderTargetEncoder>> encoders;
  Result result;
  explicit Fixture(D3D11Device& owner)
      : device(owner), draws(owner), transfer(owner, draws), shaders(owner) {}

  std::unique_ptr<RenderTargetSurface> Surface(uint32_t format, bool depth, MsaaSamples samples,
                                               bool native_2x, uint32_t width, uint32_t height) {
    RenderTargetSurface::Description description;
    description.width = width;
    description.height = height;
    description.guest_format = format;
    description.depth = depth;
    description.samples = samples;
    description.msaa_2x_supported = native_2x;
    std::string error;
    auto surface = RenderTargetSurface::Create(device, description, error);
    Require(bool(surface), error);
    ++result.surfaces;
    return surface;
  }

  std::vector<Pixel> Read(const RenderTargetSurface& surface) {
    const auto& desc = surface.description();
    uint32_t samples = surface.sample_count();
    std::string image_type = samples > 1 ? "Texture2DMS" : "Texture2D";
    std::string coordinates = samples > 1 ? "int2(id.xy),s" : "int3(id.xy,0)";
    std::string source =
        image_type + (surface.integer_transfer() ? "<uint4> image:t0;" : "<float4> image:t0;");
    if (desc.depth)
      source += image_type + "<uint4> stencil:t1;";
    source +=
        "RWStructuredBuffer<uint4> result:u0;"
        "[numthreads(8,8,1)] void main(uint3 id:SV_DispatchThreadID) {"
        "if(id.x>=" +
        std::to_string(desc.width) + "||id.y>=" + std::to_string(desc.height) +
        ") return; for(uint s=0;s<" + std::to_string(samples) +
        ";++s) {"
        "uint4 value=" +
        (surface.integer_transfer() ? "image.Load(" : "asuint(image.Load(") + coordinates +
        (surface.integer_transfer() ? ");" : "));");
    if (desc.depth)
      source += "value=uint4(value.x,stencil.Load(" + coordinates + ").y,0,0);";
    source += "result[(id.y*" + std::to_string(desc.width) + "+id.x)*" + std::to_string(samples) +
              "+s]=value; }}";
    ComPtr<ID3DBlob> code, diagnostics;
    HRESULT compilation = D3DCompile(source.data(), source.size(), "target_readback", nullptr,
                                     nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                                     code.GetAddressOf(), diagnostics.GetAddressOf());
    if (FAILED(compilation) && diagnostics)
      throw std::runtime_error(static_cast<const char*>(diagnostics->GetBufferPointer()));
    Check(compilation, "Render target readback compilation");
    ComPtr<ID3D11ComputeShader> shader;
    Check(device.device()->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(),
                                               nullptr, shader.GetAddressOf()),
          "Render target readback program");
    std::vector<Pixel> result(desc.width * desc.height * samples);
    D3D11_BUFFER_DESC buffer_desc = {};
    buffer_desc.ByteWidth = uint32_t(result.size() * sizeof(Pixel));
    buffer_desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    buffer_desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    buffer_desc.StructureByteStride = sizeof(Pixel);
    ComPtr<ID3D11Buffer> buffer, staging;
    Check(device.device()->CreateBuffer(&buffer_desc, nullptr, buffer.GetAddressOf()),
          "Render target readback output");
    D3D11_UNORDERED_ACCESS_VIEW_DESC view_desc = {};
    view_desc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    view_desc.Buffer.NumElements = uint32_t(result.size());
    ComPtr<ID3D11UnorderedAccessView> view;
    Check(device.device()->CreateUnorderedAccessView(buffer.Get(), &view_desc, view.GetAddressOf()),
          "Render target readback view");
    buffer_desc.Usage = D3D11_USAGE_STAGING;
    buffer_desc.BindFlags = buffer_desc.MiscFlags = buffer_desc.StructureByteStride = 0;
    buffer_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    Check(device.device()->CreateBuffer(&buffer_desc, nullptr, staging.GetAddressOf()),
          "Render target staging buffer");
    draws.Invalidate();
    device.context()->CSSetShader(shader.Get(), nullptr, 0);
    std::array<ID3D11ShaderResourceView*, 2> reads = {surface.read_view(), surface.stencil_view()};
    device.context()->CSSetShaderResources(0, desc.depth ? 2 : 1, reads.data());
    ID3D11UnorderedAccessView* output = view.Get();
    device.context()->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
    device.context()->Dispatch((desc.width + 7) / 8, (desc.height + 7) / 8, 1);
    draws.Invalidate();
    device.context()->CopyResource(staging.Get(), buffer.Get());
    Check(device.WaitForCompletion(), "Render target readback completion");
    D3D11_MAPPED_SUBRESOURCE mapping;
    Check(device.context()->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapping),
          "Render target readback map");
    std::memcpy(result.data(), mapping.pData, result.size() * sizeof(Pixel));
    device.context()->Unmap(staging.Get(), 0);
    return result;
  }

  void Clear(RenderTargetSurface& surface, uint32_t pattern, std::span<const D3D11_RECT> rects) {
    std::string error;
    bool passed;
    if (surface.description().depth) {
      passed = transfer.ClearDepthStencil(surface, true, (pattern + 1) / 16.0f, true,
                                          uint8_t(0x25 + pattern * 37), rects, error);
    } else if (surface.integer_transfer()) {
      // These are raw native UINT stores, including negative SNORM endpoints,
      // NaNs, infinities, denormals and sign bits in floating point storage.
      constexpr std::array<Pixel, 4> bits = {{{0x80008000, 0x7FC17D31, 0x00000001, 0xFF80FC01},
                                              {0x7F800000, 0xFF800000, 0x7E017C01, 0x80000000},
                                              {0xFFFFFFFF, 0x00008000, 0x00007FFF, 0xFFFF7E01},
                                              {0x00000000, 0x80000001, 0x7FC12345, 0x12345678}}};
      passed = transfer.ClearColorInteger(surface, bits[pattern & 3], rects, error);
    } else {
      std::array<float, 4> value = {(pattern + 1) / 16.0f, (pattern + 2) / 16.0f,
                                    (pattern + 3) / 16.0f, 1.0f};
      passed = transfer.ClearColor(surface, value, rects, error);
    }
    Require(passed, error);
    ++result.clears;
  }

  Pixel ExpectedClear(const RenderTargetSurface& surface, uint32_t pattern) {
    const auto& desc = surface.description();
    if (desc.depth) {
      float depth = (pattern + 1) / 16.0f;
      if (desc.guest_format == 0)
        depth = float(std::round(double(depth) * 16777215.0) / 16777215.0);
      return {std::bit_cast<uint32_t>(depth), uint8_t(0x25 + pattern * 37), 0, 0};
    }
    if (surface.integer_transfer()) {
      constexpr std::array<Pixel, 4> bits = {{{0x80008000, 0x7FC17D31, 0x00000001, 0xFF80FC01},
                                              {0x7F800000, 0xFF800000, 0x7E017C01, 0x80000000},
                                              {0xFFFFFFFF, 0x00008000, 0x00007FFF, 0xFFFF7E01},
                                              {0x00000000, 0x80000001, 0x7FC12345, 0x12345678}}};
      Pixel expected = bits[pattern & 3];
      if (desc.guest_format == 14 || desc.guest_format == 15) {
        expected[2] = 0;
        expected[3] = 1;
        if (desc.guest_format == 14)
          expected[1] = 0;
      } else {
        for (auto& value : expected)
          value &= 0xFFFF;
        if (desc.guest_format == 4 || desc.guest_format == 6) {
          expected[2] = 0;
          expected[3] = 1;
        }
      }
      return expected;
    }
    Pixel expected;
    for (uint32_t c = 0; c < 4; ++c) {
      float value = c == 3 ? 1.0f : (pattern + c + 1) / 16.0f;
      uint32_t maximum = desc.guest_format == 0                              ? 255
                         : desc.guest_format == 1                            ? 65535
                         : desc.guest_format == 2 || desc.guest_format == 10 ? (c == 3 ? 3 : 1023)
                                                                             : 0;
      if (maximum)
        value = float(std::round(double(value) * maximum) / maximum);
      expected[c] = std::bit_cast<uint32_t>(value);
    }
    return expected;
  }

  void Compare(const RenderTargetSurface& surface, const std::vector<Pixel>& actual,
               const std::vector<Pixel>& expected, const char* operation) {
    Require(actual.size() == expected.size(), "Render target reference extent mismatch");
    for (size_t i = 0; i < actual.size(); ++i) {
      if (surface.description().samples == MsaaSamples::k2X &&
          !surface.description().msaa_2x_supported && (i % 4 == 1 || i % 4 == 2))
        continue;
      for (uint32_t c = 0; c < 4; ++c) {
        bool exact = actual[i][c] == expected[i][c];
        // Native UNORM readback may choose the adjacent float32 representation.
        // Integer views, stencil and raw floating point payloads stay bit exact.
        if (!surface.integer_transfer() && (!surface.description().depth || c == 0))
          exact = std::abs(int64_t(actual[i][c]) - int64_t(expected[i][c])) <= 1;
        if (!exact) {
          char message[240];
          std::snprintf(message, sizeof(message),
                        "%s mismatch format=%u depth=%u samples=%u index=%llu component=%u "
                        "actual=%08X expected=%08X",
                        operation, surface.description().guest_format,
                        unsigned(surface.description().depth), surface.sample_count(),
                        static_cast<unsigned long long>(i), c, actual[i][c], expected[i][c]);
          throw std::runtime_error(message);
        }
        if (surface.integer_transfer() || (surface.description().depth && c == 1))
          ++result.exact_integer_components;
      }
      ++result.pixels;
    }
  }

  const ShaderProgram* Program(uint32_t format, bool depth, MsaaSamples destination,
                               MsaaSamples source, bool native_2x, uint32_t scale,
                               bool stencil = false, bool stencil_reference = false) {
    using Generator = DxbcRenderTargetTransferShader;
    Generator::TransferShaderKey key;
    key.mode = depth ? (stencil ? Generator::TransferMode::kDepthToStencilBit
                                : Generator::TransferMode::kDepthToDepth)
                     : Generator::TransferMode::kColorToColor;
    key.dest_resource_format = key.source_resource_format = format;
    key.dest_msaa_samples = destination;
    key.source_msaa_samples = source;
    Generator::Options options;
    options.resolution_scale_x = options.resolution_scale_y = scale;
    options.msaa_2x_supported = native_2x;
    options.stencil_reference_output = stencil_reference;
    options.portable_integer_division = true;
    std::vector<uint32_t> words;
    std::string error;
    Require(Generator::Create(key, options, words, error), error);
    auto program = shaders.GetOrCreate(
        {reinterpret_cast<const uint8_t*>(words.data()), words.size() * 4}, false, error);
    Require(program != nullptr, error);
    return program;
  }

  static uint32_t SampleY(MsaaSamples samples, bool native_2x, uint32_t sample) {
    if (samples == MsaaSamples::k4X)
      return sample >> 1;
    if (samples == MsaaSamples::k2X)
      return native_2x ? sample ^ 1 : sample == 3;
    return 0;
  }
  static uint32_t NativeSample(MsaaSamples samples, bool native_2x, uint32_t x, uint32_t y) {
    if (samples == MsaaSamples::k4X)
      return x | (y << 1);
    if (samples == MsaaSamples::k2X)
      return native_2x ? y ^ 1 : y * 3;
    return 0;
  }

  static uint32_t Float10(float value) {
    if (value < 0.25f)
      return uint32_t(std::round(value * 512));
    int exponent;
    float fraction = std::frexp(value, &exponent) * 2;
    return (uint32_t(exponent + 2) << 7) | uint32_t(std::round((fraction - 1) * 128));
  }
  uint64_t GuestBits(const RenderTargetSurface& source, uint32_t pattern) {
    auto pixel = ExpectedClear(source, pattern);
    auto desc = source.description();
    if (desc.depth) {
      float depth = (pattern + 1) / 16.0f;
      uint32_t bits = desc.guest_format == 0
                          ? uint32_t(std::round(double(depth) * 16777215))
                          : ((std::bit_cast<uint32_t>(depth * 2) + 0xC8000000u) >> 3) & 0xFFFFFF;
      return (uint64_t(bits) << 8) | pixel[1];
    }
    if (desc.guest_format == 14)
      return pixel[0];
    if (desc.guest_format == 15)
      return pixel[0] | (uint64_t(pixel[1]) << 32);
    if (source.integer_transfer()) {
      uint64_t value = 0;
      for (uint32_t c = 0; c < 4; ++c)
        value |= uint64_t(pixel[c] & 0xFFFF) << (c * 16);
      return value;
    }
    uint32_t bits = 0;
    for (uint32_t c = 0; c < 4; ++c) {
      float value = std::bit_cast<float>(pixel[c]);
      uint32_t component;
      if (desc.guest_format == 0 || desc.guest_format == 1) {
        if (desc.guest_format == 1 && c < 3) {
          float scale = value >= 512.0f / 1023   ? 1023.0f / 8
                        : value >= 128.0f / 1023 ? 1023.0f / 4
                        : value >= 64.0f / 1023  ? 1023.0f / 2
                                                 : 1023.0f;
          uint32_t offset = value >= 512.0f / 1023   ? 128
                            : value >= 128.0f / 1023 ? 64
                            : value >= 64.0f / 1023  ? 32
                                                     : 0;
          component = uint32_t(std::trunc(value * scale)) + offset;
        } else {
          component = uint32_t(std::round(value * 255));
        }
        bits |= component << (c * 8);
      } else {
        component = c == 3 ? uint32_t(std::round(value * 3))
                    : desc.guest_format == 3 || desc.guest_format == 12
                        ? Float10(value)
                        : uint32_t(std::round(value * 1023));
        bits |= component << (c == 3 ? 30 : c * 10);
      }
    }
    return bits;
  }

  void EncodeSource(const RenderTargetSurface& source, uint32_t scale, bool native_2x) {
    uint32_t cache_key = scale * 2 + native_2x;
    auto& encoder = encoders[cache_key];
    std::string error;
    if (!encoder) {
      encoder = std::make_unique<RenderTargetEncoder>(device, draws);
      rex::graphics::DxbcRenderTargetDumpShader::Options options;
      options.resolution_scale_x = options.resolution_scale_y = scale;
      options.msaa_2x_supported = native_2x;
      Require(encoder->Initialize(options, error), error);
    } else {
      encoder->Clear();
    }
    bool wide =
        !source.description().depth &&
        rex::graphics::xenos::IsColorRenderTargetFormat64bpp(
            rex::graphics::xenos::ColorRenderTargetFormat(source.description().guest_format));
    uint32_t source_base = 2044, first_tile = 2047, destination_pitch = 4;
    uint32_t source_pitch = 3 << uint32_t(wide);
    Require(encoder->Encode(source, first_tile, source_base, destination_pitch, source_pitch, 2, 2,
                            error),
            error);
    ++result.encodes;
    uint32_t tile_width = (wide ? 40 : 80) * scale, tile_height = 16 * scale;
    uint32_t words_per_pixel = wide ? 2 : 1;
    uint32_t tile_words = tile_width * tile_height * words_per_pixel;
    std::vector<uint32_t> expected(encoder->byte_size() / 4, 0);
    for (uint32_t row = 0; row < 2; ++row)
      for (uint32_t column = 0; column < 2; ++column) {
        uint32_t tile = first_tile + row * destination_pitch + column;
        uint32_t relative = tile - source_base;
        for (uint32_t y = 0; y < tile_height; ++y)
          for (uint32_t x = 0; x < tile_width; ++x) {
            uint32_t sx = (relative % source_pitch) * tile_width + x;
            uint32_t sy = (relative / source_pitch) * tile_height + y;
            if (source.description().samples == MsaaSamples::k4X)
              sx >>= 1;
            if (source.description().samples != MsaaSamples::k1X)
              sy >>= 1;
            Require(sx < source.description().width && sy < source.description().height,
                    "Independent resolve encoding address outside the source");
            uint32_t pattern = sx / (120 * scale) + (sy / (32 * scale)) * 2;
            uint64_t bits = GuestBits(source, pattern);
            uint32_t destination_x = x;
            if (source.description().depth)
              destination_x = x < tile_width / 2 ? x + tile_width / 2 : x - tile_width / 2;
            size_t offset = size_t(tile & 2047) * tile_words +
                            (y * tile_width + destination_x) * words_per_pixel;
            expected[offset] = uint32_t(bits);
            if (wide)
              expected[offset + 1] = uint32_t(bits >> 32);
          }
      }
    ComPtr<ID3D11Buffer> staging;
    D3D11_BUFFER_DESC description = {};
    description.ByteWidth = encoder->byte_size();
    description.Usage = D3D11_USAGE_STAGING;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    Check(device.device()->CreateBuffer(&description, nullptr, staging.GetAddressOf()),
          "Encoding staging buffer");
    draws.Invalidate();
    device.context()->CopyResource(staging.Get(), encoder->buffer());
    Check(device.WaitForCompletion(), "Resolve encoding completion");
    D3D11_MAPPED_SUBRESOURCE mapping;
    Check(device.context()->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapping),
          "Resolve encoding map");
    const auto* actual = static_cast<const uint32_t*>(mapping.pData);
    size_t mismatch = expected.size();
    uint32_t value = 0;
    for (size_t i = 0; i < expected.size(); ++i)
      if (actual[i] != expected[i]) {
        mismatch = i;
        value = actual[i];
        break;
      }
    device.context()->Unmap(staging.Get(), 0);
    if (mismatch != expected.size()) {
      char message[240];
      std::snprintf(message, sizeof(message),
                    "Resolve encoding mismatch format=%u depth=%u msaa=%u scale=%u native2=%u "
                    "word=%llu actual=%08X expected=%08X",
                    source.description().guest_format, unsigned(source.description().depth),
                    unsigned(source.description().samples), scale, unsigned(native_2x),
                    static_cast<unsigned long long>(mismatch), value, expected[mismatch]);
      try {
        CheckDebugMessages(device);
      } catch (const std::exception& debug_error) {
        throw std::runtime_error(std::string(message) + ": " + debug_error.what());
      }
      throw std::runtime_error(message);
    }
    result.encoded_words += 4 * tile_words;
    result.preserved_encoding_words += expected.size() - 4 * tile_words;
  }

  // With stencil_reference, depth transfers write stencil from the pixel
  // shader in one pass, as the game does where the device supports it.
  void Case(uint32_t format, bool depth, MsaaSamples source_samples,
            MsaaSamples destination_samples, bool native_2x, uint32_t scale,
            bool stencil_reference = false) {
    constexpr uint32_t destination_pitch = 2, source_pitch = 3;
    auto source = Surface(format, depth, source_samples, native_2x, 240 * scale, 64 * scale);
    auto destination =
        Surface(format, depth, destination_samples, native_2x, 160 * scale, 32 * scale);
    D3D11_RECT source_full = {0, 0, LONG(240 * scale), LONG(64 * scale)};
    D3D11_RECT full = {0, 0, LONG(160 * scale), LONG(32 * scale)};
    Clear(*source, 0, {&source_full, 1});
    Clear(*destination, 3, {&full, 1});
    // A different value in every source quadrant catches stale bindings, base
    // changes, pitch changes and source coordinates rather than just copies.
    for (uint32_t y = 0; y < 2; ++y)
      for (uint32_t x = 0; x < 2; ++x) {
        D3D11_RECT rect = {LONG(x * 120 * scale), LONG(y * 32 * scale), LONG((x + 1) * 120 * scale),
                           LONG((y + 1) * 32 * scale)};
        Clear(*source, x + y * 2, {&rect, 1});
      }
    auto source_pixels = Read(*source);
    std::vector<Pixel> source_reference(source_pixels.size());
    for (uint32_t y = 0; y < source->description().height; ++y)
      for (uint32_t x = 0; x < source->description().width; ++x)
        for (uint32_t s = 0; s < source->sample_count(); ++s) {
          uint32_t pattern = (x / (120 * scale)) + (y / (32 * scale)) * 2;
          bool unused_sample = source_samples == MsaaSamples::k2X && !native_2x && s != 0 && s != 3;
          // Initialize unused samples explicitly; the guest never addresses them.
          if (unused_sample)
            continue;
          source_reference[(y * source->description().width + x) * source->sample_count() + s] =
              ExpectedClear(*source, pattern);
        }
    // The 2x-as-4x guest mask intentionally leaves the other two samples alone.
    for (size_t i = 0; i < source_pixels.size(); ++i)
      if (source_samples == MsaaSamples::k2X && !native_2x && (i % 4 == 1 || i % 4 == 2))
        source_reference[i] = source_pixels[i];
    Compare(*source, source_pixels, source_reference, "Native clear");
    std::string error;
    auto snapshot = source->Snapshot(device, draws, error);
    Require(bool(snapshot), error);
    ++result.snapshots;
    Clear(*source, 3, {&source_full, 1});
    Compare(*snapshot, Read(*snapshot), source_pixels, "Retained source");
    if (destination_samples == MsaaSamples::k1X)
      EncodeSource(*snapshot, scale, native_2x);

    auto expected = Read(*destination);
    D3D11_RECT rect = {LONG(7 * scale), LONG(3 * scale), LONG(153 * scale), LONG(29 * scale)};
    DxbcRenderTargetTransferShader::TransferAddressConstant address;
    bool wide = !depth && rex::graphics::xenos::IsColorRenderTargetFormat64bpp(
                              rex::graphics::xenos::ColorRenderTargetFormat(format));
    address.dest_pitch = destination_pitch << uint32_t(wide);
    address.source_pitch = source_pitch << uint32_t(wide);
    address.source_to_dest = 1 << uint32_t(wide);
    RenderTargetTransfer::Constants constants;
    constants.address = address.constant;
    constants.output =
        depth ? RenderTargetTransfer::Output::kDepth : RenderTargetTransfer::Output::kColor;
    constants.write_stencil_reference = depth && stencil_reference;
    if (depth && !stencil_reference)
      Require(transfer.ClearDepthStencil(*destination, false, 0, true, 0, {&rect, 1}, error),
              error);
    const auto* program = Program(format, depth, destination_samples, source_samples, native_2x,
                                  scale, false, stencil_reference);
    Require(
        transfer.Transfer(*destination, *snapshot, nullptr, *program, constants, {&rect, 1}, error),
        error);
    ++result.transfers;
    result.stencil_reference_transfers += constants.write_stencil_reference;
    if (result.transfers == 1) {
      ComPtr<ID3D11ShaderResourceView> bound;
      device.context()->PSGetShaderResources(0, 1, bound.GetAddressOf());
      ComPtr<ID3D11Buffer> cb, staging;
      device.context()->PSGetConstantBuffers(1, 1, cb.GetAddressOf());
      Require(bound.Get() == snapshot->read_view(), "Transfer source not bound at t0");
      Require(bool(cb), "Transfer address not bound at b1");
      D3D11_BUFFER_DESC desc;
      cb->GetDesc(&desc);
      desc.Usage = D3D11_USAGE_STAGING;
      desc.BindFlags = desc.MiscFlags = 0;
      desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      Check(device.device()->CreateBuffer(&desc, nullptr, staging.GetAddressOf()), "Address check");
      device.context()->CopyResource(staging.Get(), cb.Get());
      D3D11_MAPPED_SUBRESOURCE map;
      Check(device.context()->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map), "Address readback");
      uint32_t actual_address;
      std::memcpy(&actual_address, map.pData, 4);
      device.context()->Unmap(staging.Get(), 0);
      Require(actual_address == address.constant, "Transfer address buffer differs from the draw");
    }
    if (depth && !stencil_reference) {
      program = Program(format, true, destination_samples, source_samples, native_2x, scale, true);
      constants.output = RenderTargetTransfer::Output::kStencilBit;
      for (uint32_t bit = 1; bit < 256; bit <<= 1) {
        constants.stencil_bit = uint8_t(bit);
        Require(transfer.Transfer(*destination, *snapshot, nullptr, *program, constants, {&rect, 1},
                                  error),
                error);
        ++result.transfers;
      }
    }
    uint32_t destination_x_samples = destination_samples == MsaaSamples::k4X ? 2 : 1;
    uint32_t destination_y_samples = destination_samples == MsaaSamples::k1X ? 1 : 2;
    uint32_t source_x_samples = source_samples == MsaaSamples::k4X ? 2 : 1;
    uint32_t source_y_samples = source_samples == MsaaSamples::k1X ? 1 : 2;
    uint32_t words_per_sample = wide ? 2 : 1;
    uint32_t tile_width = 80 * scale, tile_height = 16 * scale;
    for (uint32_t y = rect.top; y < uint32_t(rect.bottom); ++y)
      for (uint32_t x = rect.left; x < uint32_t(rect.right); ++x)
        for (uint32_t s = 0; s < destination->sample_count(); ++s) {
          if (destination_samples == MsaaSamples::k2X && !native_2x && s != 0 && s != 3)
            continue;
          uint32_t word_x =
              (x * destination_x_samples + (destination_samples == MsaaSamples::k4X ? s & 1 : 0)) *
              words_per_sample;
          uint32_t word_y = y * destination_y_samples + SampleY(destination_samples, native_2x, s);
          uint32_t tile = (word_y / tile_height) * address.dest_pitch + word_x / tile_width +
                          address.source_to_dest;
          uint32_t source_word_x = (tile % address.source_pitch) * tile_width + word_x % tile_width;
          uint32_t source_word_y =
              (tile / address.source_pitch) * tile_height + word_y % tile_height;
          uint32_t sx = source_word_x / words_per_sample / source_x_samples;
          uint32_t sy = source_word_y / source_y_samples;
          uint32_t ss = NativeSample(source_samples, native_2x,
                                     (source_word_x / words_per_sample) % source_x_samples,
                                     source_word_y % source_y_samples);
          Require(sx < snapshot->description().width && sy < snapshot->description().height,
                  "Independent transfer address outside the source");
          expected[(y * destination->description().width + x) * destination->sample_count() + s] =
              source_pixels[(sy * snapshot->description().width + sx) * snapshot->sample_count() +
                            ss];
        }
    Compare(*destination, Read(*destination), expected, "Ownership transfer");
    // A later invalid rectangle must reject the whole batch before its first,
    // valid rectangle changes any pixels.
    std::array<D3D11_RECT, 2> invalid = {full, D3D11_RECT{-1, 0, 2, 2}};
    Require(!transfer.Transfer(*destination, *snapshot, nullptr,
                               *Program(format, depth, destination_samples, source_samples,
                                        native_2x, scale, false, stencil_reference),
                               constants, invalid, error),
            "Out-of-range transfer rectangle accepted");
    ++result.rejected;
    Compare(*destination, Read(*destination), expected, "Rejected batch preservation");
    if (depth) {
      Require(transfer.ClearDepthStencil(*destination, false, 0, true, 0xA7, {&rect, 1}, error),
              error);
      for (uint32_t y = rect.top; y < uint32_t(rect.bottom); ++y)
        for (uint32_t x = rect.left; x < uint32_t(rect.right); ++x)
          for (uint32_t s = 0; s < destination->sample_count(); ++s)
            if (destination_samples != MsaaSamples::k2X || native_2x || s == 0 || s == 3)
              expected[(y * destination->description().width + x) * destination->sample_count() + s]
                      [1] = 0xA7;
      Compare(*destination, Read(*destination), expected, "Stencil-only clear preserves depth");
    }
  }
};
void CheckDebugMessages(D3D11Device& owner) {
  ComPtr<ID3D11InfoQueue> queue;
  if (FAILED(owner.device()->QueryInterface(IID_PPV_ARGS(queue.GetAddressOf()))))
    return;
  for (uint64_t i = 0; i < queue->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
    SIZE_T bytes = 0;
    Check(queue->GetMessage(i, nullptr, &bytes), "Target debug message size");
    std::vector<uint8_t> data(bytes);
    auto* message = reinterpret_cast<D3D11_MESSAGE*>(data.data());
    Check(queue->GetMessage(i, message, &bytes), "Target debug message read");
    if (message->Severity <= D3D11_MESSAGE_SEVERITY_WARNING)
      throw std::runtime_error(message->pDescription);
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
      else if (arg == "--fl11-1")
        options.maximum_feature_level = D3D_FEATURE_LEVEL_11_1;
      else if (arg == "--output" && i + 1 < argc)
        output = argv[++i];
      else
        throw std::runtime_error(
            "usage: d3d11_render_target_check [--warp] [--fl11-1] [--output report.json]");
    }
    std::string error;
    auto device = D3D11Device::Create(options, error);
    Require(bool(device), error);
    Fixture fixture(*device);
    constexpr std::array<uint32_t, 12> colors = {0, 1, 2, 3, 4, 5, 6, 7, 10, 12, 14, 15};
    for (uint32_t scale : {1u, 2u})
      for (bool native_2x : {true, false})
        for (MsaaSamples source : {MsaaSamples::k1X, MsaaSamples::k2X, MsaaSamples::k4X})
          for (MsaaSamples destination : {MsaaSamples::k1X, MsaaSamples::k2X, MsaaSamples::k4X}) {
            for (uint32_t format : colors)
              fixture.Case(format, false, source, destination, native_2x, scale);
            for (uint32_t format : {0u, 1u}) {
              fixture.Case(format, true, source, destination, native_2x, scale);
              if (device->features().pixel_shader_stencil_reference)
                fixture.Case(format, true, source, destination, native_2x, scale, true);
            }
          }
    fixture.draws.Invalidate();
    Check(device->WaitForCompletion(), "Native target fixture completion");
    CheckDebugMessages(*device);
    auto& result = fixture.result;
    std::string report =
        "{\n  \"passed\":true,\n  \"scope\":\"native image ownership transfers and partial "
        "clears\",\n";
    report += "  \"feature_level\":" + std::to_string(device->features().level) + ",\n";
    report +=
        "  \"software\":" + std::string(device->features().software ? "true" : "false") + ",\n";
    report += "  \"pixels\":" + std::to_string(result.pixels) + ",\n";
    report +=
        "  \"exact_integer_components\":" + std::to_string(result.exact_integer_components) + ",\n";
    report += "  \"resolve_encoding_dispatches\":" + std::to_string(result.encodes) + ",\n";
    report += "  \"exact_encoded_words\":" + std::to_string(result.encoded_words) + ",\n";
    report +=
        "  \"preserved_encoding_words\":" + std::to_string(result.preserved_encoding_words) + ",\n";
    report += "  \"surfaces\":" + std::to_string(result.surfaces) + ",\n";
    report += "  \"partial_clears\":" + std::to_string(result.clears) + ",\n";
    report += "  \"ownership_transfers\":" + std::to_string(result.transfers) + ",\n";
    report += "  \"retained_snapshots\":" + std::to_string(result.snapshots) + ",\n";
    report += "  \"pixel_shader_stencil_reference\":" +
              std::string(device->features().pixel_shader_stencil_reference ? "true" : "false") +
              ",\n";
    report +=
        "  \"stencil_reference_transfers\":" + std::to_string(result.stencil_reference_transfers) +
        ",\n";
    report += "  \"rejected_rectangle_batches\":" + std::to_string(result.rejected) + "\n}\n";
    std::fputs(report.c_str(), stdout);
    if (!output.empty()) {
      std::ofstream file(output);
      file << report;
      Require(bool(file), "Could not write render target qualification");
    }
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    if (!output.empty()) {
      std::ofstream file(output);
      std::string message;
      for (char c : std::string(error.what())) {
        if (c == '\\' || c == '"')
          message += '\\';
        message += c == '\n' || c == '\r' ? ' ' : c;
      }
      file << "{\"passed\":false,\"error\":\"" << message << "\"}\n";
    }
    return 1;
  }
}
