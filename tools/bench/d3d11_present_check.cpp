#include <rex/ui/d3d11/d3d11_image_renderer.h>
#include <rex/graphics/d3d11/buffer_cache.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>

using Microsoft::WRL::ComPtr;
using rex::graphics::d3d11::BufferCache;
using rex::graphics::d3d11::BufferKind;
using rex::ui::d3d11::D3D11Device;
using rex::ui::d3d11::D3D11ImageRenderer;
namespace {
namespace shaders {
#include "../rexglue-sdk/src/ui/shaders/bytecode/d3d12_5_1/guest_output_bilinear_ps.h"
#include "../rexglue-sdk/src/ui/shaders/bytecode/d3d12_5_1/guest_output_bilinear_dither_ps.h"
#include "../rexglue-sdk/src/ui/shaders/bytecode/d3d12_5_1/guest_output_ffx_cas_sharpen_ps.h"
#include "../rexglue-sdk/src/ui/shaders/bytecode/d3d12_5_1/guest_output_ffx_cas_sharpen_dither_ps.h"
#include "../rexglue-sdk/src/ui/shaders/bytecode/d3d12_5_1/guest_output_ffx_cas_resample_ps.h"
#include "../rexglue-sdk/src/ui/shaders/bytecode/d3d12_5_1/guest_output_ffx_cas_resample_dither_ps.h"
#include "../rexglue-sdk/src/ui/shaders/bytecode/d3d12_5_1/guest_output_ffx_fsr_easu_ps.h"
#include "../rexglue-sdk/src/ui/shaders/bytecode/d3d12_5_1/guest_output_ffx_fsr_rcas_ps.h"
#include "../rexglue-sdk/src/ui/shaders/bytecode/d3d12_5_1/guest_output_ffx_fsr_rcas_dither_ps.h"
}  // namespace shaders
void Require(bool value, const char* message) {
  if (!value)
    throw std::runtime_error(message);
}
void Check(HRESULT result, const char* message) {
  Require(SUCCEEDED(result), message);
}
struct Image {
  ComPtr<ID3D11Texture2D> texture;
  ComPtr<ID3D11ShaderResourceView> view;
  ComPtr<ID3D11RenderTargetView> target;
  ComPtr<ID3D11UnorderedAccessView> write;
};
Image MakeImage(D3D11Device& device, uint32_t width, uint32_t height, DXGI_FORMAT format,
                bool compute = false) {
  D3D11_TEXTURE2D_DESC desc = {};
  desc.Width = width;
  desc.Height = height;
  desc.ArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
  desc.Format = format;
  desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
  if (compute)
    desc.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
  Image image;
  Check(device.device()->CreateTexture2D(&desc, nullptr, image.texture.GetAddressOf()),
        "Image creation");
  Check(device.device()->CreateShaderResourceView(image.texture.Get(), nullptr,
                                                  image.view.GetAddressOf()),
        "Image view");
  Check(device.device()->CreateRenderTargetView(image.texture.Get(), nullptr,
                                                image.target.GetAddressOf()),
        "Image target");
  if (compute)
    Check(device.device()->CreateUnorderedAccessView(image.texture.Get(), nullptr,
                                                     image.write.GetAddressOf()),
          "Image compute writes");
  return image;
}
std::vector<uint32_t> Read(D3D11Device& device, ID3D11Texture2D* texture) {
  D3D11_TEXTURE2D_DESC desc;
  texture->GetDesc(&desc);
  desc.Usage = D3D11_USAGE_STAGING;
  desc.BindFlags = 0;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ComPtr<ID3D11Texture2D> readback;
  Check(device.device()->CreateTexture2D(&desc, nullptr, readback.GetAddressOf()),
        "Image readback");
  device.context()->ClearState();
  device.context()->CopyResource(readback.Get(), texture);
  D3D11_MAPPED_SUBRESOURCE mapped = {};
  Check(device.context()->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Readback map");
  std::vector<uint32_t> data(desc.Width * desc.Height);
  for (uint32_t y = 0; y < desc.Height; ++y)
    std::memcpy(data.data() + y * desc.Width,
                static_cast<uint8_t*>(mapped.pData) + y * mapped.RowPitch, desc.Width * 4);
  device.context()->Unmap(readback.Get(), 0);
  return data;
}
void Draw(D3D11ImageRenderer& renderer, Image& source, Image& target, uint32_t width,
          uint32_t height, uint32_t swizzle = 0x688, uint32_t mode = 0,
          ID3D11ShaderResourceView* gamma = nullptr) {
  std::string error;
  if (!renderer.Draw(source.view.Get(), target.target.Get(), width, height, {0, 0, width, height},
                     swizzle, mode, gamma, error))
    throw std::runtime_error(error);
}
uint64_t Run(D3D11Device& device, uint32_t& draws) {
  D3D11ImageRenderer renderer(device);
  std::string error;
  Require(renderer.Initialize(error), error.c_str());
  BufferCache buffers(device);
  constexpr uint32_t width = 32, height = 24;
  auto source = MakeImage(device, width, height, DXGI_FORMAT_R8G8B8A8_UNORM);
  std::array<Image, 3> mailbox;
  for (auto& image : mailbox)
    image = MakeImage(device, width, height, DXGI_FORMAT_R8G8B8A8_UNORM);
  auto output = MakeImage(device, width, height, DXGI_FORMAT_R8G8B8A8_UNORM);
  std::vector<uint32_t> expected(width * height);
  uint64_t pixels = 0;
  for (uint32_t frame = 0; frame < 64; ++frame) {
    for (uint32_t i = 0; i < expected.size(); ++i)
      expected[i] = ((i * 13 + frame * 37) & 255) | (((i * 7 + frame * 29) & 255) << 8) |
                    (((i * 31 + frame * 19) & 255) << 16) | 0xFF000000;
    device.context()->ClearState();
    device.context()->UpdateSubresource(source.texture.Get(), 0, nullptr, expected.data(),
                                        width * 4, 0);
    Draw(renderer, source, mailbox[frame % 3], width, height);
    ++draws;
    Draw(renderer, mailbox[frame % 3], output, width, height);
    ++draws;
    Require(Read(device, output.texture.Get()) == expected,
            "Mailbox composition displayed stale pixels");
    pixels += expected.size();
  }
  // Permutations and constant channels must reach the presented image too.
  Draw(renderer, source, output, width, height, 0x60A);
  ++draws;  // B,G,R,A.
  auto data = Read(device, output.texture.Get());
  for (uint32_t i = 0; i < data.size(); ++i)
    Require(data[i] == ((expected[i] & 0xFF00FF00) | ((expected[i] & 255) << 16) |
                        ((expected[i] >> 16) & 255)),
            "Presented swizzle mismatch");
  pixels += data.size();
  // Apply actual guest table and PWL ramps, preserving their 10-bit precision.
  auto gamma_output = MakeImage(device, width, height, DXGI_FORMAT_R10G10B10A2_UNORM);
  std::array<uint32_t, 256> table;
  for (uint32_t i = 0; i < table.size(); ++i)
    table[i] = ((1023 - i * 4) & 1023) << 20 | ((i * 3) & 1023) << 10 | ((i * 2) & 1023);
  auto table_buffer = buffers.GetOrCreate(
      {reinterpret_cast<const uint8_t*>(table.data()), sizeof(table)}, BufferKind::kRaw, error);
  Require(bool(table_buffer), error.c_str());
  Draw(renderer, source, gamma_output, width, height, 0x688, 1, table_buffer->raw_view());
  ++draws;
  data = Read(device, gamma_output.texture.Get());
  for (uint32_t i = 0; i < data.size(); ++i) {
    uint32_t r = expected[i] & 255, g = (expected[i] >> 8) & 255, b = (expected[i] >> 16) & 255;
    uint32_t packed = ((table[r] >> 20) & 1023) | (((table[g] >> 10) & 1023) << 10) |
                      ((table[b] & 1023) << 20) | 0xC0000000;
    Require(data[i] == packed, "Guest table gamma pixel mismatch");
  }
  pixels += data.size();
  std::array<uint32_t, 128 * 3> pwl;
  for (uint32_t i = 0; i < 128; ++i)
    for (uint32_t c = 0; c < 3; ++c)
      pwl[i * 3 + c] = uint32_t((i * (433 + c * 7)) & 65535) | ((uint32_t(257 + c * 128)) << 16);
  auto pwl_buffer = buffers.GetOrCreate({reinterpret_cast<const uint8_t*>(pwl.data()), sizeof(pwl)},
                                        BufferKind::kRaw, error);
  Require(bool(pwl_buffer), error.c_str());
  Draw(renderer, source, gamma_output, width, height, 0x688, 2, pwl_buffer->raw_view());
  ++draws;
  data = Read(device, gamma_output.texture.Get());
  for (uint32_t i = 0; i < data.size(); ++i) {
    for (uint32_t c = 0; c < 3; ++c) {
      uint32_t index =
          uint32_t(std::floor(double((expected[i] >> (c * 8)) & 255) * 1023 / 255 + 0.5));
      uint32_t packed = pwl[(index >> 3) * 3 + c];
      double value = std::min(
          1.0, (double(packed & 65535) + double((packed >> 16) * (index & 7)) / 8) / 65535);
      int reference = int(std::floor(value * 1023 + 0.5));
      Require(std::abs(int((data[i] >> (c * 10)) & 1023) - reference) <= 1,
              "Guest PWL gamma pixel mismatch");
    }
    Require((data[i] >> 30) == 3, "Gamma output alpha mismatch");
  }
  pixels += data.size();
  // Draw UI through the production vertex-pulling shader with retained native
  // index data, alpha blending, coordinate scaling, and partial scissoring.
  struct Vertex {
    float x, y, u, v;
    uint32_t color;
  };
  static_assert(sizeof(Vertex) == 20);
  std::array<Vertex, 3> vertices = {Vertex{0, 0, 0, 0, 0x80FFFFFF},
                                    Vertex{float(width), 0, 2, 0, 0x80FFFFFF},
                                    Vertex{0, float(height), 0, 2, 0x80FFFFFF}};
  auto vertex_buffer =
      buffers.GetOrCreate({reinterpret_cast<const uint8_t*>(vertices.data()), sizeof(vertices)},
                          BufferKind::kRaw, error);
  Require(bool(vertex_buffer), error.c_str());
  const std::array<uint16_t, 6> indices = {0, 1, 2, 0, 1, 2};
  auto index_buffer =
      buffers.GetOrCreate({reinterpret_cast<const uint8_t*>(indices.data()), sizeof(indices)},
                          BufferKind::kIndex16, error);
  Require(bool(index_buffer), error.c_str());
  ComPtr<ID3D11SamplerState> sampler;
  D3D11_SAMPLER_DESC sampler_desc = {};
  sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
  sampler_desc.AddressU = sampler_desc.AddressV = sampler_desc.AddressW =
      D3D11_TEXTURE_ADDRESS_CLAMP;
  sampler_desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
  Check(device.device()->CreateSamplerState(&sampler_desc, sampler.GetAddressOf()), "UI sampler");
  for (uint32_t indexed = 0; indexed < 2; ++indexed) {
    device.context()->ClearState();
    constexpr float background[4] = {16.0f / 255, 32.0f / 255, 48.0f / 255, 1};
    device.context()->ClearRenderTargetView(output.target.Get(), background);
    D3D11_RECT scissor = {3, 5, 27, 21};
    Require(
        renderer.DrawBatch(vertex_buffer->raw_view(), indexed ? index_buffer->buffer() : nullptr,
                           source.view.Get(), sampler.Get(), output.target.Get(), width, height,
                           float(width) / 2, float(height) / 2, scissor,
                           D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST, 3, indexed ? 3 : 0, 0, error),
        error.c_str());
    ++draws;
    data = Read(device, output.texture.Get());
    for (uint32_t y = 0; y < height; ++y)
      for (uint32_t x = 0; x < width; ++x) {
        bool covered = x >= uint32_t(scissor.left) && x < uint32_t(scissor.right) &&
                       y >= uint32_t(scissor.top) && y < uint32_t(scissor.bottom);
        uint32_t i = y * width + x;
        for (uint32_t c = 0; c < 3; ++c) {
          int background_value = 16 + 16 * int(c);
          int reference = covered
                              ? int(std::floor(double((expected[i] >> (c * 8)) & 255) * 128 / 255 +
                                               double(background_value) * 127 / 255 + 0.5))
                              : background_value;
          Require(std::abs(int((data[i] >> (c * 8)) & 255) - reference) <= 1,
                  "UI blend or scissor mismatch");
        }
        Require((data[i] >> 24) == 255, "UI blend alpha mismatch");
      }
    pixels += data.size();
  }
  return pixels;
}

float FromBits(uint32_t value) {
  float result;
  std::memcpy(&result, &value, sizeof(result));
  return result;
}
uint32_t ToBits(float value) {
  uint32_t result;
  std::memcpy(&result, &value, sizeof(result));
  return result;
}
// The stock FidelityFX approximations, computed as the shader bytecode does.
float LowRcp(float value) {
  return FromBits(0x7EF07EBBu - ToBits(value));
}
float MediumRcp(float value) {
  float estimate = FromBits(0x7EF19FFFu - ToBits(value));
  return (-estimate * value + 2.0f) * estimate;
}
float LowSqrt(float value) {
  return FromBits((ToBits(value) >> 1) + 0x1FBC4639u);
}
float Saturate(float value) {
  return std::min(std::max(value, 0.0f), 1.0f);
}
// A flat field isolates how the stock CAS and RCAS shaders normalize their
// weights: one Newton step from a bit-trick reciprocal estimate, which scales
// a flat color by weight * rcp(weight) instead of exactly one. Bilinear and
// EASU clamp to the neighborhood, so they keep the color.
float FlatEffectColor(size_t effect, float value, float green, float sharpness) {
  if (effect >= 2 && effect <= 5) {
    float luma = green * green;
    float weight =
        LowSqrt(Saturate(LowRcp(luma) * std::min(1.0f - luma, luma))) * sharpness * 4.0f + 1.0f;
    if (effect >= 4)
      weight *= LowRcp(1.0f / 32.0f);
    return std::sqrt(Saturate(MediumRcp(weight) * value * value * weight));
  }
  if (effect >= 7) {
    // Every channel of a flat neighborhood reaches the RCAS lobe limit.
    float lobe = -0.1875f * sharpness;
    return (lobe * 4.0f * value + value) * MediumRcp(lobe * 4.0f + 1.0f);
  }
  return value;
}

struct EffectResults {
  uint32_t draws = 0, fxaa_dispatches = 0, changed_edge_pixels = 0;
  uint64_t pixels = 0;
};
EffectResults CheckEffects(D3D11Device& device) {
  D3D11ImageRenderer renderer(device);
  std::string error;
  Require(renderer.Initialize(error), error.c_str());
  constexpr uint32_t width = 32, height = 24, target_width = 72, target_height = 56;
  auto source = MakeImage(device, width, height, DXGI_FORMAT_R8G8B8A8_UNORM);
  auto target = MakeImage(device, target_width, target_height, DXGI_FORMAT_R10G10B10A2_UNORM);
  auto luma = MakeImage(device, width, height, DXGI_FORMAT_R16G16B16A16_UNORM);
  auto fxaa = MakeImage(device, width, height, DXGI_FORMAT_R10G10B10A2_UNORM, true);
  const std::array<std::span<const uint8_t>, 9> effects = {
      shaders::guest_output_bilinear_ps,           shaders::guest_output_bilinear_dither_ps,
      shaders::guest_output_ffx_cas_sharpen_ps,    shaders::guest_output_ffx_cas_sharpen_dither_ps,
      shaders::guest_output_ffx_cas_resample_ps,   shaders::guest_output_ffx_cas_resample_dither_ps,
      shaders::guest_output_ffx_fsr_easu_ps,       shaders::guest_output_ffx_fsr_rcas_ps,
      shaders::guest_output_ffx_fsr_rcas_dither_ps};
  std::vector<uint32_t> input(width * height);
  EffectResults result;
  for (uint32_t frame = 0; frame < 2; ++frame) {
    uint32_t color = frame ? 0xFF4184C7 : 0xFFC08040;
    std::fill(input.begin(), input.end(), color);
    device.context()->ClearState();
    device.context()->UpdateSubresource(source.texture.Get(), 0, nullptr, input.data(), width * 4,
                                        0);
    for (size_t effect = 0; effect < effects.size(); ++effect) {
      bool resample = effect >= 4 && effect <= 6;
      uint32_t w = width * (resample ? 2 : 1), h = height * (resample ? 2 : 1);
      int32_t x = effect == 6 ? 0 : 3, y = effect == 6 ? 0 : 4;
      std::array<uint32_t, 8> parameters = {};
      std::memcpy(parameters.data(), &x, sizeof(x));
      std::memcpy(parameters.data() + 1, &y, sizeof(y));
      auto set_float = [&](size_t index, float value) {
        std::memcpy(parameters.data() + index, &value, sizeof(value));
      };
      if (effect <= 1) {
        set_float(2, 1.0f / w);
        set_float(3, 1.0f / h);
      } else if (effect <= 3) {
        set_float(2, -0.2f);
      } else if (effect <= 5) {
        set_float(2, 0.5f);
        set_float(3, 0.5f);
        set_float(4, -0.2f);
      } else if (effect == 6) {
        set_float(0, 0.5f);
        set_float(1, 0.5f);
        set_float(2, 1.0f / width);
        set_float(3, 1.0f / height);
      } else {
        set_float(2, 1.0f);
      }
      device.context()->ClearState();
      const float black[4] = {0, 0, 0, 1};
      device.context()->ClearRenderTargetView(target.target.Get(), black);
      Require(renderer.DrawEffect(
                  source.view.Get(), target.target.Get(), target_width, target_height, {x, y, w, h},
                  effects[effect],
                  {reinterpret_cast<const uint8_t*>(parameters.data()), sizeof(parameters)}, error),
              error.c_str());
      ++result.draws;
      auto data = Read(device, target.texture.Get());
      float green = float((color >> 8) & 255) / 255;
      float sharpness = effect <= 5 ? -0.2f : 1.0f;
      // Hardware may export a 10-bit target through FP16 rounded toward zero
      // (up to half a 10-bit step) before rounding to the nearest step. The
      // stock dither adds at most half an 8-bit step.
      bool dither = effect == 1 || effect == 3 || effect == 5 || effect == 8;
      double tolerance = 1.0 + (dither ? 1023.0 / 510 : 0) + 1.0 / 64;
      for (uint32_t py = 4; py < h - 4; ++py)
        for (uint32_t px = 4; px < w - 4; ++px) {
          uint32_t value = data[(y + py) * target_width + x + px];
          for (uint32_t c = 0; c < 3; ++c) {
            float channel = float((color >> (c * 8)) & 255) / 255;
            double reference = double(FlatEffectColor(effect, channel, green, sharpness)) * 1023;
            int actual = int((value >> (c * 10)) & 1023);
            if (std::abs(actual - reference) > tolerance)
              throw std::runtime_error("Native presentation effect " + std::to_string(effect) +
                                       " changed color " + std::to_string(frame) + " at " +
                                       std::to_string(px) + "," + std::to_string(py) + " channel " +
                                       std::to_string(c) + " from " + std::to_string(reference) +
                                       " to " + std::to_string(actual));
          }
          ++result.pixels;
        }
      Require(data.back() == 0xC0000000, "A native effect wrote outside its output rectangle");
    }
    for (bool extreme : {false, true}) {
      Require(renderer.Draw(source.view.Get(), luma.target.Get(), width, height,
                            {0, 0, width, height}, 0x688, 0, nullptr, error, true),
              error.c_str());
      Require(renderer.ApplyFxaa(luma.view.Get(), fxaa.write.Get(), width, height, extreme, error),
              error.c_str());
      ++result.fxaa_dispatches;
      auto data = Read(device, fxaa.texture.Get());
      for (uint32_t value : data) {
        for (uint32_t c = 0; c < 3; ++c) {
          int reference = int(std::lround(float((color >> (c * 8)) & 255) * 1023 / 255));
          Require(std::abs(int((value >> (c * 10)) & 1023) - reference) <= 1,
                  "Native FXAA changed a flat color or retained the preceding frame");
        }
        ++result.pixels;
      }
    }
  }
  for (uint32_t y = 0; y < height; ++y)
    for (uint32_t x = 0; x < width; ++x)
      input[y * width + x] = x > y ? 0xFFFFFFFF : 0xFF000000;
  device.context()->ClearState();
  device.context()->UpdateSubresource(source.texture.Get(), 0, nullptr, input.data(), width * 4, 0);
  for (bool extreme : {false, true}) {
    Require(renderer.Draw(source.view.Get(), luma.target.Get(), width, height,
                          {0, 0, width, height}, 0x688, 0, nullptr, error, true),
            error.c_str());
    Require(renderer.ApplyFxaa(luma.view.Get(), fxaa.write.Get(), width, height, extreme, error),
            error.c_str());
    ++result.fxaa_dispatches;
    auto data = Read(device, fxaa.texture.Get());
    uint32_t changed = 0;
    for (uint32_t y = 4; y < height - 4; ++y)
      for (uint32_t x = 4; x < width - 4; ++x) {
        uint32_t value = data[y * width + x] & 1023;
        uint32_t expected = x > y ? 1023 : 0;
        if (std::abs(int(x) - int(y)) > 3)
          Require(value == expected, "Native FXAA changed pixels away from an edge");
        changed += value != expected;
      }
    Require(changed > 0, "The native FXAA edge pass was bypassed");
    result.changed_edge_pixels += changed;
  }
  return result;
}
}  // namespace
int main(int argc, char** argv) {
  std::string output;
  try {
    D3D11Device::Options options;
    options.maximum_feature_level = D3D_FEATURE_LEVEL_11_0;
    options.debug = true;
    for (int i = 1; i < argc; ++i) {
      std::string arg = argv[i];
      if (arg == "--warp")
        options.warp = true;
      else if (arg == "--output" && i + 1 < argc)
        output = argv[++i];
      else
        throw std::runtime_error("usage: d3d11_present_check [--warp] [--output report.json]");
    }
    std::string error;
    auto device = D3D11Device::Create(options, error);
    Require(bool(device), error.c_str());
    uint32_t draws = 0;
    auto pixels = Run(*device, draws);
    auto effects = CheckEffects(*device);
    std::string report =
        "{\"passed\":true,\"scope\":\"native image and UI composition\",\"feature_level\":" +
        std::to_string(device->features().level) + ",\"draws\":" + std::to_string(draws) +
        ",\"exact_pixels\":" + std::to_string(pixels) +
        ",\"effect_draws\":" + std::to_string(effects.draws) +
        ",\"effect_interior_pixels\":" + std::to_string(effects.pixels) +
        ",\"fxaa_dispatches\":" + std::to_string(effects.fxaa_dispatches) +
        ",\"fxaa_changed_edge_pixels\":" + std::to_string(effects.changed_edge_pixels) + "}\n";
    std::fputs(report.c_str(), stdout);
    if (!output.empty()) {
      std::ofstream file(output);
      file << report;
      Require(bool(file), "Writing composition report");
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
