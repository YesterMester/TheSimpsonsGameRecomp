#include <rex/ui/d3d11/d3d11_image_renderer.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace rex::ui::d3d11 {
namespace {
namespace shaders {
#include "../../graphics/shaders/bytecode/d3d12_5_1/fxaa_cs.h"
#include "../../graphics/shaders/bytecode/d3d12_5_1/fxaa_extreme_cs.h"
}  // namespace shaders
constexpr char kFullscreen[] = R"(
float4 main(uint id : SV_VertexID) : SV_Position {
  return float4(float2(float(id & 1)*4-1, 1-float(id >> 1)*4), 0, 1);
})";
constexpr char kImage[] = R"(
Texture2D<float4> source_image : register(t0);
ByteAddressBuffer gamma_data : register(t1);
SamplerState source_sampler : register(s0);
cbuffer parameters : register(b0) {
  float2 output_offset; float2 output_size_inv;
  uint channel_mapping; uint gamma_mode; uint fxaa_luma; uint padding;
};
float Channel(float4 value, uint component) {
  uint selector = (channel_mapping >> (component*3)) & 7;
  return selector < 4 ? value[selector] : selector == 5 ? 1 : 0;
}
float4 main(float4 position : SV_Position) : SV_Target {
  float2 uv = (position.xy-output_offset)*output_size_inv;
  float4 value = gamma_mode ? source_image.Load(int3(uint2(position.xy),0))
                            : source_image.SampleLevel(source_sampler, uv, 0);
  value = float4(Channel(value,0), Channel(value,1), Channel(value,2), Channel(value,3));
  if (gamma_mode == 1) {
    uint3 index = uint3(saturate(value.rgb)*255 + 0.5);
    value.r = float((gamma_data.Load(index.r*4) >> 20) & 1023)/1023;
    value.g = float((gamma_data.Load(index.g*4) >> 10) & 1023)/1023;
    value.b = float(gamma_data.Load(index.b*4) & 1023)/1023;
    value.a = 1;
  } else if (gamma_mode == 2) {
    uint3 index = uint3(saturate(value.rgb)*1023 + 0.5);
    [unroll] for (uint component=0; component<3; ++component) {
      uint packed = gamma_data.Load(((index[component] >> 3)*3+component)*4);
      value[component] = min((float(packed & 65535) +
          float((packed >> 16)*(index[component] & 7))*0.125)/65535, 1);
    }
    value.a = 1;
  }
  if (fxaa_luma) value.a = dot(value.rgb, float3(0.299, 0.587, 0.114));
  return value;
})";
constexpr char kImmediate[] = R"(
ByteAddressBuffer vertices : register(t0);
cbuffer parameters : register(b0) {float2 coordinate_size_inv; float2 padding;};
struct Output {float4 position : SV_Position; float2 uv : TEXCOORD0; float4 color : COLOR0;};
Output main(uint id : SV_VertexID) {
  uint4 data = vertices.Load4(id*20);
  uint color = vertices.Load(id*20+16);
  Output result;
  result.position = float4(asfloat(data.xy)*coordinate_size_inv*float2(2,-2)+float2(-1,1),0,1);
  result.uv = asfloat(data.zw);
  result.color = float4(color & 255, (color >> 8) & 255,
                       (color >> 16) & 255, color >> 24)/255;
  return result;
})";
constexpr char kUI[] = R"(
Texture2D<float4> image : register(t0);
SamplerState image_sampler : register(s0);
float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0, float4 color : COLOR0) : SV_Target {
  return image.SampleLevel(image_sampler, uv, 0)*color;
})";
bool Check(HRESULT result, const char* operation, std::string& error) {
  if (SUCCEEDED(result))
    return true;
  char message[128];
  std::snprintf(message, sizeof(message), "%s failed (0x%08X)", operation, unsigned(result));
  error = message;
  return false;
}
}  // namespace

bool D3D11ImageRenderer::Initialize(std::string& error) {
  error.clear();
  if (constants_)
    return true;
  Microsoft::WRL::ComPtr<ID3DBlob> code;
  const std::array<const char*, 4> sources = {kFullscreen, kImmediate, kImage, kUI};
  for (uint32_t i = 0; i < sources.size(); ++i) {
    code.Reset();
    if (FAILED(D3D11Device::CompileShader(
            sources[i], "main",
            i < 2 ? D3D11Device::ShaderStage::kVertex : D3D11Device::ShaderStage::kPixel,
            code.GetAddressOf(), error)))
      return false;
    HRESULT result;
    if (i < 2)
      result = device_.device()->CreateVertexShader(
          code->GetBufferPointer(), code->GetBufferSize(), nullptr,
          i ? immediate_.ReleaseAndGetAddressOf() : fullscreen_.ReleaseAndGetAddressOf());
    else
      result = device_.device()->CreatePixelShader(
          code->GetBufferPointer(), code->GetBufferSize(), nullptr,
          i == 3 ? ui_.ReleaseAndGetAddressOf() : image_.ReleaseAndGetAddressOf());
    if (!Check(result, "Native composition shader", error)) {
      fullscreen_.Reset();
      return false;
    }
  }
  D3D11_RASTERIZER_DESC raster = {};
  raster.FillMode = D3D11_FILL_SOLID;
  raster.CullMode = D3D11_CULL_NONE;
  raster.DepthClipEnable = TRUE;
  raster.ScissorEnable = TRUE;
  if (!Check(device_.device()->CreateRasterizerState(&raster, rasterizer_.ReleaseAndGetAddressOf()),
             "Native composition rasterizer", error))
    return false;
  D3D11_DEPTH_STENCIL_DESC depth = {};
  depth.DepthFunc = D3D11_COMPARISON_ALWAYS;
  if (!Check(device_.device()->CreateDepthStencilState(&depth, depth_.ReleaseAndGetAddressOf()),
             "Native composition depth state", error))
    return false;
  D3D11_BLEND_DESC blend = {};
  blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  if (!Check(device_.device()->CreateBlendState(&blend, opaque_.ReleaseAndGetAddressOf()),
             "Native composition opaque state", error))
    return false;
  auto& target = blend.RenderTarget[0];
  target.BlendEnable = TRUE;
  target.SrcBlend = D3D11_BLEND_SRC_ALPHA;
  target.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
  target.BlendOp = D3D11_BLEND_OP_ADD;
  target.SrcBlendAlpha = D3D11_BLEND_ONE;
  target.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
  target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
  if (!Check(device_.device()->CreateBlendState(&blend, blend_.ReleaseAndGetAddressOf()),
             "Native UI blend state", error))
    return false;
  D3D11_SAMPLER_DESC sampler = {};
  sampler.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
  sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  sampler.ComparisonFunc = D3D11_COMPARISON_NEVER;
  if (!Check(device_.device()->CreateSamplerState(&sampler, sampler_.ReleaseAndGetAddressOf()),
             "Native composition sampler", error))
    return false;
  D3D11_BUFFER_DESC desc = {};
  desc.ByteWidth = 32;
  desc.Usage = D3D11_USAGE_DYNAMIC;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  return Check(device_.device()->CreateBuffer(&desc, nullptr, constants_.ReleaseAndGetAddressOf()),
               "Native composition constants", error);
}

bool D3D11ImageRenderer::Constants(std::span<const uint8_t> bytes, std::string& error) {
  if (bytes.size() > 32) {
    error = "Native composition constants exceed their allocation";
    return false;
  }
  D3D11_MAPPED_SUBRESOURCE mapped = {};
  if (!Check(device_.context()->Map(constants_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped),
             "Native composition constant upload", error))
    return false;
  std::memset(mapped.pData, 0, 32);
  std::memcpy(mapped.pData, bytes.data(), bytes.size());
  device_.context()->Unmap(constants_.Get(), 0);
  auto* constants = constants_.Get();
  device_.context()->VSSetConstantBuffers(0, 1, &constants);
  device_.context()->PSSetConstantBuffers(0, 1, &constants);
  return true;
}

bool D3D11ImageRenderer::Bind(ID3D11RenderTargetView* target, uint32_t width, uint32_t height,
                              const D3D11_RECT& scissor, bool blend, std::string& error) {
  error.clear();
  if (!target || !width || !height || width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
      height > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION || !constants_) {
    error = "Native composition has invalid output state";
    return false;
  }
  auto* context = device_.context();
  context->ClearState();
  context->OMSetRenderTargets(1, &target, nullptr);
  context->OMSetDepthStencilState(depth_.Get(), 0);
  context->OMSetBlendState(blend ? blend_.Get() : opaque_.Get(), nullptr, UINT32_MAX);
  D3D11_VIEWPORT viewport = {0, 0, float(width), float(height), 0, 1};
  context->RSSetViewports(1, &viewport);
  context->RSSetScissorRects(1, &scissor);
  context->RSSetState(rasterizer_.Get());
  return true;
}

bool D3D11ImageRenderer::Draw(ID3D11ShaderResourceView* source, ID3D11RenderTargetView* target,
                              uint32_t width, uint32_t height, Rectangle rectangle,
                              uint32_t swizzle, uint32_t gamma_mode,
                              ID3D11ShaderResourceView* gamma, std::string& error, bool fxaa_luma) {
  if (!source || !rectangle.width || !rectangle.height || gamma_mode > 2 ||
      (gamma_mode && !gamma) || swizzle > 0xFFF) {
    error = "Invalid native image input";
    return false;
  }
  for (uint32_t c = 0; c < 4; ++c)
    if (((swizzle >> (c * 3)) & 7) > 5) {
      error = "Invalid native image channel selector";
      return false;
    }
  const D3D11_RECT scissor = {0, 0, LONG(width), LONG(height)};
  if (!Bind(target, width, height, scissor, false, error))
    return false;
  struct Parameters {
    float x, y, inv_w, inv_h;
    uint32_t swizzle, gamma, luma, pad;
  };
  Parameters parameters = {float(rectangle.x),
                           float(rectangle.y),
                           1.0f / rectangle.width,
                           1.0f / rectangle.height,
                           swizzle,
                           gamma_mode,
                           uint32_t(fxaa_luma),
                           0};
  if (!Constants({reinterpret_cast<const uint8_t*>(&parameters), sizeof(parameters)}, error))
    return false;
  auto* context = device_.context();
  context->VSSetShader(fullscreen_.Get(), nullptr, 0);
  context->PSSetShader(image_.Get(), nullptr, 0);
  std::array<ID3D11ShaderResourceView*, 2> sources = {source, gamma};
  context->PSSetShaderResources(0, 2, sources.data());
  auto* sampler = sampler_.Get();
  context->PSSetSamplers(0, 1, &sampler);
  D3D11_VIEWPORT viewport = {float(rectangle.x),
                             float(rectangle.y),
                             float(rectangle.width),
                             float(rectangle.height),
                             0,
                             1};
  context->RSSetViewports(1, &viewport);
  context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  context->Draw(3, 0);
  return Check(device_.device()->GetDeviceRemovedReason(), "Native image draw", error);
}

bool D3D11ImageRenderer::ApplyFxaa(ID3D11ShaderResourceView* source,
                                   ID3D11UnorderedAccessView* target, uint32_t width,
                                   uint32_t height, bool extreme, std::string& error) {
  if (!source || !target || !width || !height || width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
      height > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION || !constants_) {
    error = "Invalid native FXAA image state";
    return false;
  }
  std::span<const uint8_t> bytecode = extreme ? std::span<const uint8_t>(shaders::fxaa_extreme_cs)
                                              : std::span<const uint8_t>(shaders::fxaa_cs);
  const auto* program = shaders_.GetOrCreate(bytecode, false, error);
  if (!program || !program->compute())
    return false;
  struct Parameters {
    uint32_t width, height;
    float inverse_width, inverse_height;
  } parameters = {width, height, 1.0f / width, 1.0f / height};
  auto* context = device_.context();
  context->ClearState();
  if (!Constants({reinterpret_cast<const uint8_t*>(&parameters), sizeof(parameters)}, error))
    return false;
  auto* constants = constants_.Get();
  auto* sampler = sampler_.Get();
  context->CSSetConstantBuffers(0, 1, &constants);
  context->CSSetShader(program->compute(), nullptr, 0);
  context->CSSetShaderResources(0, 1, &source);
  context->CSSetUnorderedAccessViews(0, 1, &target, nullptr);
  context->CSSetSamplers(0, 1, &sampler);
  context->Dispatch((width + 15) / 16, (height + 7) / 8, 1);
  context->ClearState();
  return Check(device_.device()->GetDeviceRemovedReason(), "Native FXAA dispatch", error);
}

bool D3D11ImageRenderer::DrawEffect(ID3D11ShaderResourceView* source,
                                    ID3D11RenderTargetView* target, uint32_t width, uint32_t height,
                                    Rectangle rectangle, std::span<const uint8_t> pixel_shader,
                                    std::span<const uint8_t> parameters, std::string& error) {
  if (!source || !rectangle.width || !rectangle.height || parameters.empty() ||
      parameters.size() > 32) {
    error = "Invalid native presentation effect";
    return false;
  }
  const auto* program = shaders_.GetOrCreate(pixel_shader, false, error);
  if (!program || !program->pixel())
    return false;
  const D3D11_RECT scissor = {0, 0, LONG(width), LONG(height)};
  if (!Bind(target, width, height, scissor, false, error) || !Constants(parameters, error))
    return false;
  auto* context = device_.context();
  context->VSSetShader(fullscreen_.Get(), nullptr, 0);
  context->PSSetShader(program->pixel(), nullptr, 0);
  context->PSSetShaderResources(0, 1, &source);
  auto* sampler = sampler_.Get();
  context->PSSetSamplers(0, 1, &sampler);
  D3D11_VIEWPORT viewport = {float(rectangle.x),
                             float(rectangle.y),
                             float(rectangle.width),
                             float(rectangle.height),
                             0,
                             1};
  context->RSSetViewports(1, &viewport);
  context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  context->Draw(3, 0);
  return Check(device_.device()->GetDeviceRemovedReason(), "Native presentation effect draw",
               error);
}

bool D3D11ImageRenderer::DrawBatch(ID3D11ShaderResourceView* vertices, ID3D11Buffer* indices,
                                   ID3D11ShaderResourceView* texture, ID3D11SamplerState* sampler,
                                   ID3D11RenderTargetView* target, uint32_t width, uint32_t height,
                                   float coordinate_width, float coordinate_height,
                                   D3D11_RECT scissor, D3D11_PRIMITIVE_TOPOLOGY topology,
                                   uint32_t count, uint32_t first, int32_t base_vertex,
                                   std::string& error) {
  if (!vertices || !texture || !sampler || !(coordinate_width > 0) || !(coordinate_height > 0) ||
      !std::isfinite(coordinate_width) || !std::isfinite(coordinate_height) ||
      (topology != D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST &&
       topology != D3D11_PRIMITIVE_TOPOLOGY_LINELIST)) {
    error = "Invalid native UI input";
    return false;
  }
  if (!Bind(target, width, height, scissor, true, error))
    return false;
  std::array<float, 4> parameters = {1.0f / coordinate_width, 1.0f / coordinate_height, 0, 0};
  if (!Constants({reinterpret_cast<const uint8_t*>(parameters.data()), sizeof(parameters)}, error))
    return false;
  auto* context = device_.context();
  context->VSSetShader(immediate_.Get(), nullptr, 0);
  context->PSSetShader(ui_.Get(), nullptr, 0);
  context->VSSetShaderResources(0, 1, &vertices);
  context->PSSetShaderResources(0, 1, &texture);
  context->PSSetSamplers(0, 1, &sampler);
  context->IASetPrimitiveTopology(topology);
  if (indices) {
    context->IASetIndexBuffer(indices, DXGI_FORMAT_R16_UINT, 0);
    context->DrawIndexed(count, first, base_vertex);
  } else
    context->Draw(count, first);
  return Check(device_.device()->GetDeviceRemovedReason(), "Native UI draw", error);
}

}  // namespace rex::ui::d3d11
