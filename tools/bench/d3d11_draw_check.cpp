#include <rex/graphics/d3d11/draw_context.h>
#include <rex/graphics/d3d11/sampler_cache.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace rex::graphics::d3d11;
using rex::ui::d3d11::D3D11Device;

namespace {
namespace shaders {
#include "../rexglue-sdk/src/graphics/shaders/bytecode/d3d12_5_1/float24_round_ps.h"
#include "../rexglue-sdk/src/graphics/shaders/bytecode/d3d12_5_1/float24_truncate_ps.h"
}  // namespace shaders
constexpr size_t kVertex = size_t(DrawStage::kVertex), kPixel = size_t(DrawStage::kPixel);
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
const ShaderProgram* Program(ShaderCache& cache, const char* source, const char* profile,
                             bool read_only = true) {
  ComPtr<ID3DBlob> code, diagnostics;
  HRESULT result = D3DCompile(source, std::strlen(source), "native_draw_check", nullptr, nullptr,
                              "main", profile, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                              code.GetAddressOf(), diagnostics.GetAddressOf());
  if (FAILED(result)) {
    throw std::runtime_error(diagnostics ? static_cast<const char*>(diagnostics->GetBufferPointer())
                                         : "Draw check shader compilation failed");
  }
  std::string error;
  auto* program = cache.GetOrCreate(
      {static_cast<const uint8_t*>(code->GetBufferPointer()), code->GetBufferSize()}, read_only,
      error);
  if (!program)
    throw std::runtime_error(error);
  return program;
}
std::shared_ptr<const BufferVersion> Buffer(BufferCache& cache, const void* source, size_t size,
                                            BufferKind kind) {
  std::string error;
  auto buffer = cache.GetOrCreate({static_cast<const uint8_t*>(source), size}, kind, error);
  if (!buffer)
    throw std::runtime_error(error);
  return buffer;
}
void Draw(DrawContext& context, const DrawCommand& command) {
  std::string error;
  if (!context.Draw(command, error))
    throw std::runtime_error(error);
}
struct Texture {
  ComPtr<ID3D11Texture2D> texture;
  ComPtr<ID3D11RenderTargetView> rtv;
  ComPtr<ID3D11ShaderResourceView> srv;
};
Texture CreateTexture(D3D11Device& owner, uint32_t width, uint32_t height, DXGI_FORMAT format,
                      uint32_t mips = 1, uint32_t layers = 1) {
  D3D11_TEXTURE2D_DESC desc = {};
  desc.Width = width;
  desc.Height = height;
  desc.MipLevels = mips;
  desc.ArraySize = layers;
  desc.Format = format;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  Texture texture;
  Check(owner.device()->CreateTexture2D(&desc, nullptr, texture.texture.GetAddressOf()),
        "Draw texture");
  if (mips == 1 && layers == 1) {
    Check(owner.device()->CreateRenderTargetView(texture.texture.Get(), nullptr,
                                                 texture.rtv.GetAddressOf()),
          "Draw RTV");
    Check(owner.device()->CreateShaderResourceView(texture.texture.Get(), nullptr,
                                                   texture.srv.GetAddressOf()),
          "Draw SRV");
  }
  return texture;
}
void ExactPixels(D3D11Device& owner, ID3D11Texture2D* texture, uint32_t subresource,
                 const void* expected, uint32_t row_bytes, uint32_t height) {
  D3D11_TEXTURE2D_DESC desc;
  texture->GetDesc(&desc);
  desc.Usage = D3D11_USAGE_STAGING;
  desc.BindFlags = desc.MiscFlags = 0;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ComPtr<ID3D11Texture2D> staging;
  Check(owner.device()->CreateTexture2D(&desc, nullptr, staging.GetAddressOf()), "Draw readback");
  owner.context()->CopyResource(staging.Get(), texture);
  Check(owner.WaitForCompletion(), "Draw completion");
  D3D11_MAPPED_SUBRESOURCE mapped;
  Check(owner.context()->Map(staging.Get(), subresource, D3D11_MAP_READ, 0, &mapped), "Draw map");
  bool exact = true;
  for (uint32_t y = 0; y < height; ++y) {
    exact &= !std::memcmp(static_cast<const uint8_t*>(mapped.pData) + y * mapped.RowPitch,
                          static_cast<const uint8_t*>(expected) + y * row_bytes, row_bytes);
  }
  owner.context()->Unmap(staging.Get(), subresource);
  Require(exact, "Native draw pixels differ from the independent reference");
}

struct Results {
  uint64_t pixels = 0;
  uint32_t hazards = 0;
  uint32_t transitions = 0;
  uint32_t depth_draws = 0;
  uint32_t float24_prepasses = 0;
  uint32_t blend_draws = 0;
  uint32_t uav_draws = 0;
  uint64_t words = 0;
  uint32_t sampler_draws = 0;
  uint64_t discard_vertex_invocations = 0;
};

void CheckAlternatingFrames(D3D11Device& owner, ShaderCache& shaders, BufferCache& buffers,
                            DrawContext& context, Results& results, const ShaderProgram* vertex) {
  constexpr uint32_t width = 32, height = 24, frames = 64;
  auto a = CreateTexture(owner, width, height, DXGI_FORMAT_R32G32B32A32_UINT);
  auto b = CreateTexture(owner, width, height, DXGI_FORMAT_R32G32B32A32_UINT);
  D3D11_QUERY_DESC query_description = {D3D11_QUERY_PIPELINE_STATISTICS, 0};
  ComPtr<ID3D11Query> discard_query;
  Check(owner.device()->CreateQuery(&query_description, discard_query.GetAddressOf()),
        "Rasterization discard query");
  std::vector<std::array<uint32_t, 4>> expected(width * height);
  for (uint32_t i = 0; i < expected.size(); ++i)
    expected[i] = {i * 37 + 9, i ^ 0x314721u, i * i, i + 0xBC72161u};
  owner.context()->UpdateSubresource(a.texture.Get(), 0, nullptr, expected.data(), width * 16, 0);
  context.Invalidate();
  auto* pixel = Program(shaders, R"(
Texture2D<uint4> source_data : register(t0);
cbuffer parameters : register(b0) {uint4 increment;};
uint4 main(float4 p : SV_Position) : SV_Target {
  return (source_data.Load(int3(uint2(p.xy),0)) + increment) ^
         uint4(uint(p.x)*17u, uint(p.y)*23u, uint(p.x)+uint(p.y), 0x71305Au);
})",
                        "ps_5_1");
  DrawCommand command;
  command.programs[kVertex] = vertex;
  command.programs[kPixel] = pixel;
  command.topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
  command.count = 3;
  command.state = DrawState::Default(width, height);
  auto* input = &a;
  auto* output = &b;
  for (uint32_t frame = 0; frame < frames; ++frame) {
    std::array<uint32_t, 4> increment = {frame + 5, frame * 3, frame * 7 + 1, frame * 11};
    auto constants = Buffer(buffers, increment.data(), sizeof(increment), BufferKind::kConstant);
    std::array<ID3D11Buffer*, 1> cb = {constants->buffer()};
    std::array<ID3D11ShaderResourceView*, 1> srv = {input->srv.Get()};
    std::array<ID3D11RenderTargetView*, 1> rtv = {output->rtv.Get()};
    command.bindings[kPixel].constants = cb;
    command.bindings[kPixel].resources = srv;
    command.render_targets = rtv;
    Draw(context, command);
    // Repeat identical bindings with a zero-count draw to exercise cache hits.
    command.count = 0;
    Draw(context, command);
    command.count = 3;
    for (uint32_t y = 0; y < height; ++y) {
      for (uint32_t x = 0; x < width; ++x) {
        auto& value = expected[y * width + x];
        std::array<uint32_t, 4> mix = {x * 17, y * 23, x + y, 0x71305Au};
        for (uint32_t c = 0; c < 4; ++c)
          value[c] = (value[c] + increment[c]) ^ mix[c];
      }
    }
    ExactPixels(owner, output->texture.Get(), 0, expected.data(), width * 16, height);
    results.pixels += width * height;
    // Priming draws must execute their vertex work without changing the image.
    // Give the pixel shader different constants so accidental rasterization
    // cannot pass by reproducing the preceding pixels.
    const std::array<uint32_t, 4> discard_increment = {0xFFFF0011, 0xFF7216, 99, 717};
    auto discard_constants =
        Buffer(buffers, discard_increment.data(), sizeof(discard_increment), BufferKind::kConstant);
    std::array<ID3D11Buffer*, 1> discard_cb = {discard_constants->buffer()};
    command.bindings[kPixel].constants = discard_cb;
    command.discard_rasterization = true;
    owner.context()->Begin(discard_query.Get());
    Draw(context, command);
    owner.context()->End(discard_query.Get());
    ExactPixels(owner, output->texture.Get(), 0, expected.data(), width * 16, height);
    D3D11_QUERY_DATA_PIPELINE_STATISTICS discard_statistics = {};
    Require(owner.context()->GetData(discard_query.Get(), &discard_statistics,
                                     sizeof(discard_statistics), 0) == S_OK,
            "Rasterization discard statistics are unavailable after completion");
    Require(discard_statistics.VSInvocations >= command.count && !discard_statistics.PSInvocations,
            "Rasterization discard must execute vertices without pixel invocations");
    results.discard_vertex_invocations += discard_statistics.VSInvocations;
    results.pixels += width * height;
    command.discard_rasterization = false;
    command.bindings[kPixel].constants = cb;
    ++results.transitions;
    std::string error;
    // Rejected submissions must not alter the previously valid bindings.
    std::array<ID3D11ShaderResourceView*, 1> conflict = {output->srv.Get()};
    command.bindings[kPixel].resources = conflict;
    Require(!context.Draw(command, error), "Overlapping read/write draw accepted");
    ++results.hazards;
    command.bindings[kPixel].resources = srv;
    Draw(context, command);
    ExactPixels(owner, output->texture.Get(), 0, expected.data(), width * 16, height);
    results.pixels += width * height;
    std::swap(input, output);
  }
}

void CheckSubresources(D3D11Device& owner, ShaderCache& shaders, DrawContext& context,
                       Results& results, const ShaderProgram* vertex) {
  constexpr uint32_t size = 16;
  auto texture = CreateTexture(owner, size, size, DXGI_FORMAT_R32G32B32A32_UINT, 2, 2);
  D3D11_RENDER_TARGET_VIEW_DESC rd = {};
  rd.Format = DXGI_FORMAT_R32G32B32A32_UINT;
  rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
  rd.Texture2DArray.ArraySize = 1;
  rd.Texture2DArray.FirstArraySlice = 1;
  D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
  sd.Format = rd.Format;
  sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
  sd.Texture2DArray.ArraySize = 1;
  sd.Texture2DArray.MipLevels = 1;
  ComPtr<ID3D11RenderTargetView> rtv;
  ComPtr<ID3D11ShaderResourceView> srv;
  Check(owner.device()->CreateRenderTargetView(texture.texture.Get(), &rd, rtv.GetAddressOf()),
        "Array RTV");
  Check(owner.device()->CreateShaderResourceView(texture.texture.Get(), &sd, srv.GetAddressOf()),
        "Array SRV");
  std::vector<std::array<uint32_t, 4>> expected(size * size, {17, 31, 47, 63});
  owner.context()->UpdateSubresource(texture.texture.Get(), 0, nullptr, expected.data(), size * 16,
                                     0);
  context.Invalidate();
  auto* pixel = Program(shaders, R"(
Texture2DArray<uint4> source_data : register(t0);
uint4 main(float4 p : SV_Position) : SV_Target {
 return source_data.Load(int4(uint2(p.xy),0,0)) + uint4(1,3,5,7);
})",
                        "ps_5_1");
  DrawCommand command;
  command.programs[kVertex] = vertex;
  command.programs[kPixel] = pixel;
  command.state = DrawState::Default(size, size);
  command.topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
  command.count = 3;
  std::array<ID3D11RenderTargetView*, 1> outputs = {rtv.Get()};
  std::array<ID3D11ShaderResourceView*, 1> inputs = {srv.Get()};
  command.render_targets = outputs;
  command.bindings[kPixel].resources = inputs;
  Draw(context, command);
  for (auto& value : expected)
    value = {18, 34, 52, 70};
  ExactPixels(owner, texture.texture.Get(), 2, expected.data(), size * 16, size);
  results.pixels += size * size;
  // The other mip in the same layer is also legal to read while writing mip 0.
  sd.Texture2DArray.FirstArraySlice = 1;
  sd.Texture2DArray.MostDetailedMip = 1;
  srv.Reset();
  Check(owner.device()->CreateShaderResourceView(texture.texture.Get(), &sd, srv.GetAddressOf()),
        "Mip SRV");
  std::vector<std::array<uint32_t, 4>> mip((size / 2) * (size / 2), {3, 11, 29, 53});
  context.Invalidate();
  owner.context()->UpdateSubresource(texture.texture.Get(), 3, nullptr, mip.data(), size / 2 * 16,
                                     0);
  command.state = DrawState::Default(size / 2, size / 2);
  inputs[0] = srv.Get();
  Draw(context, command);
  // Read back the upper-left viewport; remaining texels retain the earlier pass.
  for (uint32_t y = 0; y < size / 2; ++y)
    for (uint32_t x = 0; x < size / 2; ++x)
      expected[y * size + x] = {4, 14, 34, 60};
  ExactPixels(owner, texture.texture.Get(), 2, expected.data(), size * 16, size);
  results.pixels += size * size;
}

void CheckFloat24Prepasses(D3D11Device& owner, ShaderCache& shaders, BufferCache& buffers,
                           DrawContext& context, Results& results) {
  constexpr uint32_t width = 32, height = 24;
  auto readback = CreateTexture(owner, width, height, DXGI_FORMAT_R8G8B8A8_UNORM);
  auto* vertex = Program(shaders, R"(
cbuffer values : register(b0) {float raster_depth; float stored_depth;};
float4 main(uint id : SV_VertexID) : SV_Position {
  float2 p = id==0 ? float2(-1,-1) : id==1 ? float2(-1,3) : float2(3,-1);
  return float4(p,raster_depth,1);
})",
                         "vs_5_1");
  auto* pixel = Program(shaders, R"(
cbuffer values : register(b0) {float raster_depth; float stored_depth;};
void main(out float4 color : SV_Target, out float depth : SV_Depth) {
  color = 1; depth = stored_depth;
})",
                        "ps_5_1");
  // Normal, subnormal and underflowing float24 depths. The CPU reference
  // quantizes a binary significand independently of the stock shader's bits.
  constexpr std::array<float, 8> depths = {
      0.12345678f,         0.32159263f,           0.030037f,  0.00032159263f, 0.000012345678f,
      0.0000000012345678f, 0.000000000012345678f, 0.49999991f};
  for (uint32_t samples : {1u, 2u, 4u}) {
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = desc.ArraySize = 1;
    desc.SampleDesc.Count = samples;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    Texture color;
    Check(owner.device()->CreateTexture2D(&desc, nullptr, color.texture.GetAddressOf()),
          "Float24 prepass color");
    Check(owner.device()->CreateRenderTargetView(color.texture.Get(), nullptr,
                                                 color.rtv.GetAddressOf()),
          "Float24 prepass color view");
    desc.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    ComPtr<ID3D11Texture2D> depth;
    ComPtr<ID3D11DepthStencilView> dsv;
    Check(owner.device()->CreateTexture2D(&desc, nullptr, depth.GetAddressOf()),
          "Float24 prepass depth");
    Check(owner.device()->CreateDepthStencilView(depth.Get(), nullptr, dsv.GetAddressOf()),
          "Float24 prepass depth view");
    for (bool round : {false, true}) {
      std::span<const uint8_t> code = round
                                          ? std::span<const uint8_t>(shaders::float24_round_ps)
                                          : std::span<const uint8_t>(shaders::float24_truncate_ps);
      std::string error;
      auto* helper = shaders.GetOrCreate(code, false, error);
      Require(helper && helper->pixel(), error.c_str());
      for (float raster_depth : depths) {
        double value = double(raster_depth) * 2;
        double step = std::ldexp(1.0, std::max(std::ilogb(value) - 20, -34));
        double significand = value / step;
        float stored_depth =
            float((round ? std::nearbyint(significand) : std::floor(significand)) * step * 0.5);
        // Omitting the helper reproduces the old mismatch for inputs that
        // aren't already exactly representable in float24 storage.
        std::array<float, 4> values = {raster_depth, stored_depth};
        auto cb = Buffer(buffers, values.data(), sizeof(values), BufferKind::kConstant);
        std::array<ID3D11Buffer*, 1> constants = {cb->buffer()};
        for (bool convert_prepass : {false, true}) {
          context.Invalidate();
          const float black[4] = {};
          owner.context()->ClearRenderTargetView(color.rtv.Get(), black);
          owner.context()->ClearDepthStencilView(dsv.Get(), D3D11_CLEAR_DEPTH, 1, 0);
          DrawCommand command;
          command.programs[kVertex] = vertex;
          command.programs[kPixel] = convert_prepass ? helper : nullptr;
          command.topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
          command.count = 3;
          command.state = DrawState::Default(width, height);
          command.state.depth_stencil.DepthEnable = TRUE;
          command.state.depth_stencil.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
          command.state.depth_stencil.DepthFunc = D3D11_COMPARISON_ALWAYS;
          command.depth_stencil = dsv.Get();
          command.bindings[kVertex].constants = constants;
          command.bindings[kPixel].constants = constants;
          Draw(context, command);
          command.programs[kPixel] = pixel;
          command.state.depth_stencil.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
          command.state.depth_stencil.DepthFunc = D3D11_COMPARISON_EQUAL;
          std::array<ID3D11RenderTargetView*, 1> rtv = {color.rtv.Get()};
          command.render_targets = rtv;
          Draw(context, command);
          context.Invalidate();
          if (samples > 1)
            owner.context()->ResolveSubresource(readback.texture.Get(), 0, color.texture.Get(), 0,
                                                DXGI_FORMAT_R8G8B8A8_UNORM);
          else
            owner.context()->CopyResource(readback.texture.Get(), color.texture.Get());
          std::vector<std::array<uint8_t, 4>> expected(width * height);
          if (convert_prepass || stored_depth == raster_depth)
            std::fill(expected.begin(), expected.end(), std::array<uint8_t, 4>{255, 255, 255, 255});
          try {
            ExactPixels(owner, readback.texture.Get(), 0, expected.data(), width * 4, height);
          } catch (const std::exception& error) {
            throw std::runtime_error(std::string(error.what()) + "; float24 prepass samples=" +
                                     std::to_string(samples) + " round=" + std::to_string(round) +
                                     " converted=" + std::to_string(convert_prepass) +
                                     " depth=" + std::to_string(raster_depth));
          }
          results.pixels += width * height;
          ++results.float24_prepasses;
        }
      }
    }
  }
}

void CheckFixedFunction(D3D11Device& owner, ShaderCache& shaders, BufferCache& buffers,
                        DrawContext& context, Results& results) {
  constexpr uint32_t width = 32, height = 24;
  auto color = CreateTexture(owner, width, height, DXGI_FORMAT_R8G8B8A8_UNORM);
  auto* vertex = Program(shaders, R"(
cbuffer values : register(b0) {float4 color; float depth;};
float4 main(uint id : SV_VertexID) : SV_Position {
 float2 p = id==0 ? float2(-1,-1) : id==1 ? float2(-1,3) : float2(3,-1);
 return float4(p,depth,1);
})",
                         "vs_5_1");
  auto* pixel = Program(shaders, R"(
cbuffer values : register(b0) {float4 color; float depth;};
float4 main() : SV_Target { return color; }
)",
                        "ps_5_1");
  D3D11_TEXTURE2D_DESC desc = {};
  desc.Width = width;
  desc.Height = height;
  desc.ArraySize = desc.MipLevels = 1;
  desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
  desc.SampleDesc.Count = 1;
  desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
  ComPtr<ID3D11Texture2D> depth;
  ComPtr<ID3D11DepthStencilView> dsv;
  Check(owner.device()->CreateTexture2D(&desc, nullptr, depth.GetAddressOf()), "Depth texture");
  Check(owner.device()->CreateDepthStencilView(depth.Get(), nullptr, dsv.GetAddressOf()),
        "Depth view");
  context.Invalidate();
  owner.context()->ClearDepthStencilView(dsv.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1, 0);
  float black[4] = {};
  owner.context()->ClearRenderTargetView(color.rtv.Get(), black);
  DrawCommand command;
  command.programs[kVertex] = vertex;
  command.programs[kPixel] = pixel;
  command.topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
  command.count = 3;
  command.state = DrawState::Default(width, height);
  command.state.depth_stencil.DepthEnable = TRUE;
  command.state.depth_stencil.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
  command.state.depth_stencil.DepthFunc = D3D11_COMPARISON_LESS;
  command.depth_stencil = dsv.Get();
  std::array<ID3D11RenderTargetView*, 1> rtv = {color.rtv.Get()};
  command.render_targets = rtv;
  // Mixed native 16-bit and 32-bit retained index buffers must both draw.
  std::array<uint16_t, 6> indices16 = {2, 2, 2, 0, 1, 2};
  std::array<uint32_t, 3> indices32 = {0, 1, 2};
  auto i16 = Buffer(buffers, indices16.data(), sizeof(indices16), BufferKind::kIndex16);
  auto i32 = Buffer(buffers, indices32.data(), sizeof(indices32), BufferKind::kIndex32);
  auto submit = [&](std::array<float, 8> data) {
    auto cb = Buffer(buffers, data.data(), sizeof(data), BufferKind::kConstant);
    std::array<ID3D11Buffer*, 1> constants = {cb->buffer()};
    command.bindings[kVertex].constants = constants;
    command.bindings[kPixel].constants = constants;
    Draw(context, command);
  };
  command.indices = i16;
  command.first = 3;
  submit({1, 0, 0, 1, 0.5f});
  ++results.depth_draws;
  command.indices = i32;
  command.first = 0;
  submit({0, 1, 0, 1, 0.75f});
  ++results.depth_draws;
  std::vector<std::array<uint8_t, 4>> expected(width * height, {255, 0, 0, 255});
  ExactPixels(owner, color.texture.Get(), 0, expected.data(), width * 4, height);
  results.pixels += width * height;
  command.state.depth_stencil.DepthEnable = FALSE;
  command.state.depth_stencil.StencilEnable = TRUE;
  command.state.depth_stencil.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
  command.state.depth_stencil.FrontFace.StencilPassOp = D3D11_STENCIL_OP_REPLACE;
  command.state.depth_stencil.BackFace = command.state.depth_stencil.FrontFace;
  command.state.stencil_reference = 9;
  command.state.scissor = {0, 0, LONG(width / 2), LONG(height)};
  submit({0, 0, 1, 1, 0.5f});
  ++results.depth_draws;
  command.state.depth_stencil.FrontFace.StencilFunc = D3D11_COMPARISON_EQUAL;
  command.state.depth_stencil.FrontFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
  command.state.depth_stencil.BackFace = command.state.depth_stencil.FrontFace;
  command.state.scissor = {0, 0, LONG(width), LONG(height)};
  submit({0, 1, 0, 1, 0.5f});
  ++results.depth_draws;
  for (uint32_t y = 0; y < height; ++y)
    for (uint32_t x = 0; x < width / 2; ++x)
      expected[y * width + x] = {0, 255, 0, 255};
  ExactPixels(owner, color.texture.Get(), 0, expected.data(), width * 4, height);
  results.pixels += width * height;
  command.state.depth_stencil.StencilEnable = FALSE;
  command.depth_stencil = nullptr;
  auto& blend = command.state.blend.RenderTarget[0];
  blend.BlendEnable = TRUE;
  blend.SrcBlend = D3D11_BLEND_ONE;
  blend.DestBlend = D3D11_BLEND_ONE;
  blend.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_BLUE;
  submit({0, 0, 1, 0, 0.5f});
  ++results.blend_draws;
  for (auto& value : expected)
    value[2] = 255;
  ExactPixels(owner, color.texture.Get(), 0, expected.data(), width * 4, height);
  results.pixels += width * height;
  std::string error;
  command.first = 3;
  Require(!context.Draw(command, error), "Out-of-bounds retained indices accepted");
  ++results.hazards;
  command.first = 0;
  auto smaller_target = CreateTexture(owner, width / 2, height / 2, DXGI_FORMAT_R8G8B8A8_UNORM);
  std::array<ID3D11RenderTargetView*, 2> mismatched = {color.rtv.Get(), smaller_target.rtv.Get()};
  command.render_targets = mismatched;
  Require(!context.Draw(command, error), "Mismatched render target dimensions accepted");
  ++results.hazards;
}

void CheckGraphicsUav(D3D11Device& owner, ShaderCache& shaders, DrawContext& context,
                      Results& results, const ShaderProgram* vertex) {
  constexpr uint32_t width = 32, height = 24;
  auto color = CreateTexture(owner, width, height, DXGI_FORMAT_R32G32B32A32_UINT);
  D3D11_BUFFER_DESC desc = {};
  desc.ByteWidth = width * height * 4;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
  desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
  ComPtr<ID3D11Buffer> buffer;
  Check(owner.device()->CreateBuffer(&desc, nullptr, buffer.GetAddressOf()), "Graphics UAV buffer");
  D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
  ud.Format = DXGI_FORMAT_R32_TYPELESS;
  ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
  ud.Buffer.NumElements = width * height;
  ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
  ComPtr<ID3D11UnorderedAccessView> uav;
  Check(owner.device()->CreateUnorderedAccessView(buffer.Get(), &ud, uav.GetAddressOf()),
        "Graphics UAV");
  auto* pixel = Program(shaders, R"(
RWByteAddressBuffer output_data : register(u0);
uint4 main(float4 p : SV_Position) : SV_Target {
 uint2 xy = uint2(p.xy);
 uint value = (xy.x * 17u) ^ (xy.y * 31u + 0x6182Cu);
 output_data.Store((xy.y * 32u + xy.x) * 4u, value);
 return uint4(value, xy.x, xy.y, 13);
})",
                        "ps_5_1", false);
  DrawCommand command;
  command.programs[kVertex] = vertex;
  command.programs[kPixel] = pixel;
  command.state = DrawState::Default(width, height);
  command.topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
  command.count = 3;
  std::array<ID3D11RenderTargetView*, 1> targets = {color.rtv.Get()};
  std::array<ID3D11UnorderedAccessView*, 1> outputs = {uav.Get()};
  command.render_targets = targets;
  command.unordered_access = outputs;
  Draw(context, command);
  ++results.uav_draws;
  std::vector<uint32_t> words;
  std::vector<std::array<uint32_t, 4>> pixels;
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      uint32_t value = (x * 17) ^ (y * 31 + 0x6182Cu);
      words.push_back(value);
      pixels.push_back({value, x, y, 13});
    }
  }
  ExactPixels(owner, color.texture.Get(), 0, pixels.data(), width * 16, height);
  results.pixels += width * height;
  context.Invalidate();
  desc.Usage = D3D11_USAGE_STAGING;
  desc.BindFlags = desc.MiscFlags = 0;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ComPtr<ID3D11Buffer> staging;
  Check(owner.device()->CreateBuffer(&desc, nullptr, staging.GetAddressOf()),
        "Graphics UAV readback");
  owner.context()->CopyResource(staging.Get(), buffer.Get());
  Check(owner.WaitForCompletion(), "Graphics UAV completion");
  D3D11_MAPPED_SUBRESOURCE mapped;
  Check(owner.context()->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Graphics UAV map");
  bool exact = !std::memcmp(mapped.pData, words.data(), words.size() * 4);
  owner.context()->Unmap(staging.Get(), 0);
  Require(exact, "The graphics UAV did not use the register above the four render targets");
  results.words += words.size();
}

void CheckSamplers(D3D11Device& owner, ShaderCache& shaders, DrawContext& context, Results& results,
                   const ShaderProgram* vertex) {
  constexpr uint32_t width = 16, height = 4;
  auto source = CreateTexture(owner, 4, 1, DXGI_FORMAT_R8G8B8A8_UNORM);
  auto target = CreateTexture(owner, width, height, DXGI_FORMAT_R8G8B8A8_UNORM);
  std::array<std::array<uint8_t, 4>, 4> colors = {
      {{0, 0, 0, 255}, {255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255}}};
  context.Invalidate();
  owner.context()->UpdateSubresource(source.texture.Get(), 0, nullptr, colors.data(), 16, 0);
  auto* pixel = Program(shaders, R"(
Texture2D<float4> source_data : register(t0);
SamplerState source_sampler : register(s0);
float4 main(float4 p : SV_Position) : SV_Target {
 float x = floor(p.x) - 6.0;
 return source_data.SampleLevel(source_sampler, float2((x+0.5)/4.0,0.5), 0.0);
})",
                        "ps_5_1");
  SamplerCache samplers(owner, 2);
  std::vector<ComPtr<ID3D11SamplerState>> retained;
  DrawCommand command;
  command.programs[kVertex] = vertex;
  command.programs[kPixel] = pixel;
  command.state = DrawState::Default(width, height);
  command.topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
  command.count = 3;
  std::array<ID3D11RenderTargetView*, 1> output = {target.rtv.Get()};
  std::array<ID3D11ShaderResourceView*, 1> input = {source.srv.Get()};
  command.render_targets = output;
  command.bindings[kPixel].resources = input;
  constexpr std::array modes = {D3D11_TEXTURE_ADDRESS_WRAP, D3D11_TEXTURE_ADDRESS_MIRROR,
                                D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_BORDER};
  for (auto mode : modes) {
    D3D11_SAMPLER_DESC desc = {};
    desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    desc.AddressU = mode;
    desc.AddressV = desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    desc.MaxAnisotropy = 1;
    desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    desc.BorderColor[0] = desc.BorderColor[1] = desc.BorderColor[2] = desc.BorderColor[3] = 1;
    std::string error;
    auto sampler = samplers.GetOrCreate(desc, error);
    if (!sampler)
      throw std::runtime_error(error);
    Require(samplers.GetOrCreate(desc, error).Get() == sampler.Get(),
            "Native sampler was not reused");
    retained.push_back(sampler);
  }
  Require(samplers.statistics().evictions == 2, "Native sampler eviction was not exercised");
  // Use evicted objects, then clear the cache while their native bindings live.
  samplers.Clear();
  for (size_t test = 0; test < modes.size(); ++test) {
    std::array<ID3D11SamplerState*, 1> sampler = {retained[test].Get()};
    command.bindings[kPixel].samplers = sampler;
    Draw(context, command);
    ++results.sampler_draws;
    std::vector<std::array<uint8_t, 4>> expected;
    for (uint32_t y = 0; y < height; ++y) {
      for (uint32_t x = 0; x < width; ++x) {
        int32_t index = int32_t(x) - 6;
        switch (modes[test]) {
          case D3D11_TEXTURE_ADDRESS_WRAP:
            index = (index % 4 + 4) % 4;
            break;
          case D3D11_TEXTURE_ADDRESS_MIRROR: {
            int32_t mirrored = (index % 8 + 8) % 8;
            index = mirrored < 4 ? mirrored : 7 - mirrored;
            break;
          }
          case D3D11_TEXTURE_ADDRESS_CLAMP:
            index = std::clamp(index, 0, 3);
            break;
          default:
            break;
        }
        expected.push_back(index < 0 || index >= 4 ? std::array<uint8_t, 4>{255, 255, 255, 255}
                                                   : colors[index]);
      }
    }
    ExactPixels(owner, target.texture.Get(), 0, expected.data(), width * 4, height);
    results.pixels += width * height;
  }
}

void CheckReadOnlyDepth(D3D11Device& owner, ShaderCache& shaders, DrawContext& context,
                        Results& results, const ShaderProgram* vertex) {
  constexpr uint32_t width = 16, height = 8;
  auto color = CreateTexture(owner, width, height, DXGI_FORMAT_R8G8B8A8_UNORM);
  D3D11_TEXTURE2D_DESC desc = {};
  desc.Width = width;
  desc.Height = height;
  desc.ArraySize = desc.MipLevels = 1;
  desc.Format = DXGI_FORMAT_R24G8_TYPELESS;
  desc.SampleDesc.Count = 1;
  desc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
  ComPtr<ID3D11Texture2D> texture;
  Check(owner.device()->CreateTexture2D(&desc, nullptr, texture.GetAddressOf()), "Read-only depth");
  D3D11_DEPTH_STENCIL_VIEW_DESC dd = {};
  dd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
  dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
  ComPtr<ID3D11DepthStencilView> write, read;
  Check(owner.device()->CreateDepthStencilView(texture.Get(), &dd, write.GetAddressOf()),
        "Writable depth view");
  dd.Flags = D3D11_DSV_READ_ONLY_DEPTH;
  Check(owner.device()->CreateDepthStencilView(texture.Get(), &dd, read.GetAddressOf()),
        "Read-only depth view");
  D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
  sd.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
  sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
  sd.Texture2D.MipLevels = 1;
  ComPtr<ID3D11ShaderResourceView> srv;
  Check(owner.device()->CreateShaderResourceView(texture.Get(), &sd, srv.GetAddressOf()),
        "Depth SRV");
  context.Invalidate();
  owner.context()->ClearDepthStencilView(write.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL,
                                         0.25f, 0);
  auto* pixel = Program(shaders, R"(
Texture2D<float> source_depth : register(t0);
float4 main(float4 p : SV_Position) : SV_Target {
 return float4(source_depth.Load(int3(uint2(p.xy),0)), 1, 0, 1);
})",
                        "ps_5_1");
  DrawCommand command;
  command.programs[kVertex] = vertex;
  command.programs[kPixel] = pixel;
  command.state = DrawState::Default(width, height);
  command.topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
  command.count = 3;
  command.depth_stencil = read.Get();
  command.state.depth_stencil.StencilEnable = TRUE;
  command.state.depth_stencil.FrontFace.StencilPassOp = D3D11_STENCIL_OP_REPLACE;
  command.state.depth_stencil.BackFace = command.state.depth_stencil.FrontFace;
  command.state.stencil_reference = 7;
  std::array<ID3D11RenderTargetView*, 1> output = {color.rtv.Get()};
  std::array<ID3D11ShaderResourceView*, 1> input = {srv.Get()};
  command.render_targets = output;
  command.bindings[kPixel].resources = input;
  Draw(context, command);
  ++results.depth_draws;
  std::vector<std::array<uint8_t, 4>> expected(width * height, {64, 255, 0, 255});
  ExactPixels(owner, color.texture.Get(), 0, expected.data(), width * 4, height);
  results.pixels += width * height;
  std::string error;
  command.depth_stencil = write.Get();
  Require(!context.Draw(command, error), "Writable depth was accepted while reading its depth SRV");
  ++results.hazards;
}

void CheckDebugMessages(D3D11Device& owner) {
  ComPtr<ID3D11InfoQueue> queue;
  if (FAILED(owner.device()->QueryInterface(IID_PPV_ARGS(queue.GetAddressOf()))))
    return;
  for (uint64_t i = 0; i < queue->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
    SIZE_T bytes = 0;
    Check(queue->GetMessage(i, nullptr, &bytes), "Debug message size");
    std::vector<uint8_t> storage(bytes);
    auto* message = reinterpret_cast<D3D11_MESSAGE*>(storage.data());
    Check(queue->GetMessage(i, message, &bytes), "Debug message read");
    if (message->Severity <= D3D11_MESSAGE_SEVERITY_WARNING)
      throw std::runtime_error(message->pDescription);
  }
}

Results Run(D3D11Device& owner, DrawContext::Statistics& statistics) {
  ShaderCache shaders(owner);
  BufferCache buffers(owner, 2048);
  DrawContext context(owner);
  Results results;
  auto* vertex = Program(shaders, R"(
float4 main(uint id : SV_VertexID) : SV_Position {
 float2 p = id==0 ? float2(-1,-1) : id==1 ? float2(-1,3) : float2(3,-1);
 return float4(p,0.5,1);
})",
                         "vs_5_1");
  try {
    CheckAlternatingFrames(owner, shaders, buffers, context, results, vertex);
  } catch (...) {
    CheckDebugMessages(owner);
    throw;
  }
  CheckSubresources(owner, shaders, context, results, vertex);
  CheckFixedFunction(owner, shaders, buffers, context, results);
  CheckFloat24Prepasses(owner, shaders, buffers, context, results);
  CheckGraphicsUav(owner, shaders, context, results, vertex);
  CheckSamplers(owner, shaders, context, results, vertex);
  CheckReadOnlyDepth(owner, shaders, context, results, vertex);
  statistics = context.statistics();
  Require(statistics.explicit_resource_unbinds >= 62,
          "Native resource transitions were not exercised");
  context.Clear();
  CheckDebugMessages(owner);
  return results;
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
        throw std::runtime_error("usage: d3d11_draw_check [--warp] [--output report.json]");
    }
    std::string error;
    auto owner = D3D11Device::Create(options, error);
    if (!owner)
      throw std::runtime_error(error);
    DrawContext::Statistics statistics;
    auto result = Run(*owner, statistics);
    std::string report =
        "{\n  \"passed\": true,\n  \"scope\": \"native draw submission fixtures\",\n";
    report += "  \"feature_level\": " + std::to_string(owner->features().level) + ",\n";
    report +=
        "  \"software\": " + std::string(owner->features().software ? "true" : "false") + ",\n";
    report += "  \"exact_pixels\": " + std::to_string(result.pixels) + ",\n";
    report += "  \"alternating_frames\": " + std::to_string(result.transitions) + ",\n";
    report += "  \"rejected_hazards_and_ranges\": " + std::to_string(result.hazards) + ",\n";
    report += "  \"depth_stencil_draws\": " + std::to_string(result.depth_draws) + ",\n";
    report += "  \"float24_prepass_cases\": " + std::to_string(result.float24_prepasses) + ",\n";
    report += "  \"blend_draws\": " + std::to_string(result.blend_draws) + ",\n";
    report += "  \"graphics_uav_draws\": " + std::to_string(result.uav_draws) + ",\n";
    report += "  \"exact_words\": " + std::to_string(result.words) + ",\n";
    report += "  \"sampler_draws\": " + std::to_string(result.sampler_draws) + ",\n";
    report +=
        "  \"discard_vertex_invocations\": " + std::to_string(result.discard_vertex_invocations) +
        ",\n";
    report += "  \"draws\": " + std::to_string(statistics.draws) + ",\n";
    report +=
        "  \"explicit_resource_unbinds\": " + std::to_string(statistics.explicit_resource_unbinds) +
        ",\n";
    report += "  \"state_creations\": " + std::to_string(statistics.state_creations) + "\n}\n";
    std::fputs(report.c_str(), stdout);
    if (!output.empty()) {
      std::ofstream file(output);
      file << report;
      Require(bool(file), "Could not write draw qualification");
    }
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    if (!output.empty()) {
      std::ofstream file(output);
      file << "{\"passed\":false,\"error\":\"";
      for (char c : std::string(error.what())) {
        if (c == '\\' || c == '"')
          file << '\\';
        if (c == '\n')
          file << "\\n";
        else if (c == '\r')
          file << "\\r";
        else if (static_cast<unsigned char>(c) >= 32)
          file << c;
      }
      file << "\"}\n";
    }
    return 1;
  }
}
