#include <rex/graphics/d3d11/shader_bytecode.h>
#include <rex/graphics/d3d11/shader_cache.h>
#include <rex/graphics/pipeline/shader/dxbc_translator.h>
#include <rex/ui/d3d11/d3d11_device.h>

#include <d3d11shader.h>

#include <array>
#include <bit>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
using rex::graphics::DxbcShaderTranslator;
using rex::graphics::d3d11::ConvertShaderBytecode;
using rex::graphics::d3d11::ShaderBytecode;
using rex::graphics::d3d11::ShaderCache;
using rex::graphics::d3d11::ShaderProgram;
using rex::ui::d3d11::D3D11Device;

namespace {
constexpr const char* kVertex = R"(
ByteAddressBuffer input_data : register(t7);
cbuffer parameters : register(b3) { float4 values[16]; };
float4 main(uint id : SV_VertexID) : SV_Position {
  return asfloat(input_data.Load4(id * 16)) + values[id & 15];
})";

constexpr const char* kPixel = R"(
Texture2DArray<float4> source_image : register(t11);
SamplerState source_sampler : register(s2);
cbuffer parameters : register(b3) { float4 values[16]; };
float4 main(float4 position : SV_Position) : SV_Target {
  return source_image.SampleLevel(source_sampler, float3(position.xy / float2(96, 64), 0), 0)
      * values[uint(position.x) & 15];
})";

constexpr const char* kCompute = R"(
ByteAddressBuffer input_data : register(t7);
RWByteAddressBuffer output_data : register(u3);
cbuffer parameters : register(b5) { uint4 values[16]; };
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  uint value = input_data.Load(id.x * 4);
  output_data.Store(id.x * 4, (value << 3) ^ (value >> 29) ^ values[id.x & 15].y);
})";

constexpr const char* kVertexAliasedRead = R"(
ByteAddressBuffer input_data : register(t7);
RWByteAddressBuffer alternate : register(u1);
cbuffer parameters : register(b3) { float4 values[16]; };
float4 main(uint id : SV_VertexID) : SV_Position {
  uint4 vertex = (id & 1) ? alternate.Load4(id * 16) : input_data.Load4(id * 16);
  return asfloat(vertex) + values[id & 15];
})";

constexpr const char* kPixelAliasedRead = R"(
ByteAddressBuffer input_data : register(t0);
RWByteAddressBuffer alternate : register(u0);
float4 main(float4 position : SV_Position) : SV_Target {
  uint2 pixel = uint2(position.xy);
  uint address = (pixel.y * 96 + pixel.x) * 16;
  uint4 value = (pixel.x & 1) ? alternate.Load4(address) : input_data.Load4(address);
  return asfloat(value);
})";

void Require(bool passed, const char* message) {
  if (!passed) {
    throw std::runtime_error(message);
  }
}

void Check(HRESULT result, const char* operation) {
  if (FAILED(result)) {
    char message[200];
    std::snprintf(message, sizeof(message), "%s failed (0x%08X)", operation, unsigned(result));
    throw std::runtime_error(message);
  }
}

std::vector<uint8_t> Compile(const char* source, const char* profile, UINT flags) {
  ComPtr<ID3DBlob> code, errors;
  HRESULT result =
      D3DCompile(source, std::strlen(source), "bindings.hlsl", nullptr, nullptr, "main", profile,
                 flags | D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_WARNINGS_ARE_ERRORS |
                     D3DCOMPILE_IEEE_STRICTNESS,
                 0, code.GetAddressOf(), errors.GetAddressOf());
  if (FAILED(result)) {
    throw std::runtime_error(errors
                                 ? std::string(static_cast<const char*>(errors->GetBufferPointer()),
                                               errors->GetBufferSize())
                                 : "Native HLSL compilation failed");
  }
  const auto* data = static_cast<const uint8_t*>(code->GetBufferPointer());
  return {data, data + code->GetBufferSize()};
}

struct Pair {
  std::vector<uint8_t> reference;
  std::vector<uint8_t> model51;
  ShaderBytecode converted;
};

void CheckReflection(const Pair& pair) {
  ComPtr<ID3D11ShaderReflection> reference, converted;
  Check(D3DReflect(pair.reference.data(), pair.reference.size(), IID_ID3D11ShaderReflection,
                   reinterpret_cast<void**>(reference.GetAddressOf())),
        "SM 5.0 reflection");
  Check(D3DReflect(pair.converted.data.data(), pair.converted.data.size(),
                   IID_ID3D11ShaderReflection, reinterpret_cast<void**>(converted.GetAddressOf())),
        "Converted shader reflection");
  D3D11_SHADER_DESC a = {}, b = {};
  Check(reference->GetDesc(&a), "Reference shader description");
  Check(converted->GetDesc(&b), "Converted shader description");
  Require(a.BoundResources == b.BoundResources && a.ConstantBuffers == b.ConstantBuffers &&
              a.InputParameters == b.InputParameters && a.OutputParameters == b.OutputParameters,
          "Converted shader reflection counts differ from native SM 5.0");
  for (UINT i = 0; i < a.BoundResources; ++i) {
    D3D11_SHADER_INPUT_BIND_DESC original = {}, changed = {};
    Check(reference->GetResourceBindingDesc(i, &original), "Reference resource binding");
    Check(converted->GetResourceBindingDescByName(original.Name, &changed),
          "Converted resource binding");
    Require(original.Type == changed.Type && original.BindPoint == changed.BindPoint &&
                original.BindCount == changed.BindCount &&
                original.Dimension == changed.Dimension &&
                original.ReturnType == changed.ReturnType,
            "Converted shader resource bindings differ from native SM 5.0");
  }
  for (UINT i = 0; i < a.ConstantBuffers; ++i) {
    D3D11_SHADER_BUFFER_DESC original = {}, changed = {};
    Check(reference->GetConstantBufferByIndex(i)->GetDesc(&original), "Reference constant buffer");
    Check(converted->GetConstantBufferByName(original.Name)->GetDesc(&changed),
          "Converted constant buffer");
    Require(original.Type == changed.Type && original.Size == changed.Size &&
                original.Variables == changed.Variables,
            "Converted constant-buffer layout differs from native SM 5.0");
  }
}

Pair BuildPair(const char* source, const char* stage, UINT flags) {
  Pair result;
  result.reference = Compile(source, (std::string(stage) + "_5_0").c_str(), flags);
  result.model51 = Compile(source, (std::string(stage) + "_5_1").c_str(), flags);
  std::string error;
  if (!ConvertShaderBytecode(result.model51, {}, result.converted, error)) {
    throw std::runtime_error(error);
  }
  CheckReflection(result);
  return result;
}

Pair BuildAliasedVertexPair(UINT flags) {
  Pair result;
  result.reference = Compile(kVertex, "vs_5_0", flags);
  auto model51 = Compile(kVertexAliasedRead, "vs_5_1", flags);
  rex::graphics::d3d11::ShaderBytecodeOptions options;
  options.read_only_uav_aliases.push_back({1, 7});
  std::string error;
  if (!ConvertShaderBytecode(model51, options, result.converted, error)) {
    throw std::runtime_error(error);
  }
  Require(result.converted.aliased_uav_reads.size() == 1,
          "The vertex shader's read-only UAV was not converted");
  CheckReflection(result);
  return result;
}

Pair BuildAliasedPixelPair(UINT flags) {
  Pair result;
  std::string reference(kPixelAliasedRead);
  std::string declaration = "RWByteAddressBuffer alternate : register(u0);";
  reference.erase(reference.find(declaration), declaration.size());
  std::string alternate = "alternate.Load4";
  reference.replace(reference.find(alternate), alternate.size(), "input_data.Load4");
  result.reference = Compile(reference.c_str(), "ps_5_0", flags);
  result.model51 = Compile(kPixelAliasedRead, "ps_5_1", flags);
  rex::graphics::d3d11::ShaderBytecodeOptions options;
  options.read_only_uav_aliases.push_back({0, 0});
  std::string error;
  if (!ConvertShaderBytecode(result.model51, options, result.converted, error)) {
    throw std::runtime_error(error);
  }
  Require(result.converted.aliased_uav_reads.size() == 1,
          "The pixel shader's read-only UAV was not converted");
  CheckReflection(result);
  return result;
}

ComPtr<ID3D11Buffer> Buffer(D3D11Device& owner, const void* bytes, UINT size, UINT bindings,
                            UINT misc = 0) {
  D3D11_BUFFER_DESC desc = {};
  desc.ByteWidth = size;
  desc.Usage = bytes ? D3D11_USAGE_IMMUTABLE : D3D11_USAGE_DEFAULT;
  desc.BindFlags = bindings;
  desc.MiscFlags = misc;
  D3D11_SUBRESOURCE_DATA data = {bytes, 0, 0};
  ComPtr<ID3D11Buffer> result;
  Check(owner.device()->CreateBuffer(&desc, bytes ? &data : nullptr, result.GetAddressOf()),
        "Shader binding buffer");
  return result;
}

ComPtr<ID3D11ShaderResourceView> RawView(D3D11Device& owner, ID3D11Buffer* buffer, UINT words) {
  D3D11_SHADER_RESOURCE_VIEW_DESC desc = {};
  desc.Format = DXGI_FORMAT_R32_TYPELESS;
  desc.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
  desc.BufferEx.NumElements = words;
  desc.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
  ComPtr<ID3D11ShaderResourceView> result;
  Check(owner.device()->CreateShaderResourceView(buffer, &desc, result.GetAddressOf()),
        "Raw shader view");
  return result;
}

uint64_t CheckCompute(D3D11Device& owner, std::span<const uint8_t> code,
                      const ShaderProgram* cached = nullptr) {
  constexpr UINT count = 4096;
  std::array<uint32_t, count> words;
  std::array<uint32_t, 64> constants;
  for (uint32_t i = 0; i < count; ++i)
    words[i] = i * 0x9E3779B9u ^ 0x5A17C33Du;
  for (uint32_t i = 0; i < constants.size(); ++i)
    constants[i] = i * 0x7215u + 0x01020304u;
  auto source = Buffer(owner, words.data(), sizeof(words), D3D11_BIND_SHADER_RESOURCE,
                       D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS);
  auto output = Buffer(owner, nullptr, sizeof(words), D3D11_BIND_UNORDERED_ACCESS,
                       D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS);
  auto parameters = Buffer(owner, constants.data(), sizeof(constants), D3D11_BIND_CONSTANT_BUFFER);
  auto input_view = RawView(owner, source.Get(), count);
  D3D11_UNORDERED_ACCESS_VIEW_DESC output_desc = {};
  output_desc.Format = DXGI_FORMAT_R32_TYPELESS;
  output_desc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
  output_desc.Buffer.NumElements = count;
  output_desc.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
  ComPtr<ID3D11UnorderedAccessView> output_view;
  Check(owner.device()->CreateUnorderedAccessView(output.Get(), &output_desc,
                                                  output_view.GetAddressOf()),
        "Raw output view");
  ComPtr<ID3D11ComputeShader> shader;
  if (cached) {
    shader = cached->compute();
    Require(bool(shader), "Cached compute shader has the wrong stage");
  } else {
    Check(owner.device()->CreateComputeShader(code.data(), code.size(), nullptr,
                                              shader.GetAddressOf()),
          "Shader binding compute program");
  }
  auto* context = owner.context();
  ID3D11Buffer* parameter_buffer = parameters.Get();
  ID3D11ShaderResourceView* srv = input_view.Get();
  ID3D11UnorderedAccessView* uav = output_view.Get();
  context->CSSetShader(shader.Get(), nullptr, 0);
  context->CSSetShaderResources(7, 1, &srv);
  context->CSSetConstantBuffers(5, 1, &parameter_buffer);
  context->CSSetUnorderedAccessViews(3, 1, &uav, nullptr);
  context->Dispatch(count / 64, 1, 1);
  context->ClearState();
  D3D11_BUFFER_DESC staging_desc = {};
  staging_desc.ByteWidth = sizeof(words);
  staging_desc.Usage = D3D11_USAGE_STAGING;
  staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ComPtr<ID3D11Buffer> staging;
  Check(owner.device()->CreateBuffer(&staging_desc, nullptr, staging.GetAddressOf()),
        "Compute readback");
  context->CopyResource(staging.Get(), output.Get());
  Check(owner.WaitForCompletion(), "Compute completion");
  D3D11_MAPPED_SUBRESOURCE mapped = {};
  Check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Compute readback map");
  bool exact = true;
  const auto* actual = static_cast<const uint32_t*>(mapped.pData);
  for (uint32_t i = 0; i < count; ++i) {
    exact &= actual[i] == (std::rotl(words[i], 3) ^ constants[(i & 15) * 4 + 1]);
  }
  context->Unmap(staging.Get(), 0);
  Require(exact, "Shader bindings changed the computed raw-buffer output");
  return count;
}

uint64_t CheckDrawing(D3D11Device& owner, std::span<const uint8_t> vertex,
                      std::span<const uint8_t> pixel, const ShaderProgram* cached_vertex = nullptr,
                      const ShaderProgram* cached_pixel = nullptr,
                      const ShaderProgram* geometry = nullptr, uint32_t primitive = 0) {
  constexpr UINT width = 96, height = 64;
  std::vector<float> positions = {-1, 1, 0, 1, 3, 1, 0, 1, -1, -3, 0, 1};
  D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
  if (primitive == 1) {
    positions = {0, 0, 0, 1};
    topology = D3D11_PRIMITIVE_TOPOLOGY_POINTLIST;
  } else if (primitive == 3) {
    positions = {-1, 1, 0, 1, 1, 1, 0, 1, 1, -1, 0, 1, -1, -1, 0, 1};
    topology = D3D11_PRIMITIVE_TOPOLOGY_LINELIST_ADJ;
  }
  std::array<float, 64> vertex_constants = {}, pixel_constants;
  pixel_constants.fill(1);
  auto vertices = Buffer(owner, positions.data(), UINT(positions.size() * sizeof(float)),
                         D3D11_BIND_SHADER_RESOURCE, D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS);
  auto vertex_view = RawView(owner, vertices.Get(), UINT(positions.size()));
  auto vs_constants =
      Buffer(owner, vertex_constants.data(), sizeof(vertex_constants), D3D11_BIND_CONSTANT_BUFFER);
  auto ps_constants =
      Buffer(owner, pixel_constants.data(), sizeof(pixel_constants), D3D11_BIND_CONSTANT_BUFFER);
  std::vector<uint8_t> pixels(size_t(width) * height * 4);
  for (UINT y = 0; y < height; ++y) {
    for (UINT x = 0; x < width; ++x) {
      size_t at = (size_t(y) * width + x) * 4;
      pixels[at] = uint8_t(x * 7 + y * 3);
      pixels[at + 1] = uint8_t(x * 2 + y * 13);
      pixels[at + 2] = uint8_t(x * 19 + y);
      pixels[at + 3] = uint8_t(32 + ((x + y) & 223));
    }
  }
  std::vector<float> raw_pixels(pixels.size());
  for (size_t i = 0; i < pixels.size(); ++i) {
    raw_pixels[i] = float(pixels[i]) / 255.0f;
  }
  auto raw_image = Buffer(owner, raw_pixels.data(), UINT(raw_pixels.size() * sizeof(float)),
                          D3D11_BIND_SHADER_RESOURCE, D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS);
  auto raw_image_view = RawView(owner, raw_image.Get(), UINT(raw_pixels.size()));
  D3D11_TEXTURE2D_DESC desc = {};
  desc.Width = width;
  desc.Height = height;
  desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
  desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  desc.Usage = D3D11_USAGE_IMMUTABLE;
  desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  D3D11_SUBRESOURCE_DATA data = {pixels.data(), width * 4, 0};
  ComPtr<ID3D11Texture2D> source, output, staging;
  Check(owner.device()->CreateTexture2D(&desc, &data, source.GetAddressOf()),
        "Shader source image");
  D3D11_SHADER_RESOURCE_VIEW_DESC view_desc = {};
  view_desc.Format = desc.Format;
  view_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
  view_desc.Texture2DArray.MipLevels = view_desc.Texture2DArray.ArraySize = 1;
  ComPtr<ID3D11ShaderResourceView> image_view;
  Check(
      owner.device()->CreateShaderResourceView(source.Get(), &view_desc, image_view.GetAddressOf()),
      "Shader image view");
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_RENDER_TARGET;
  Check(owner.device()->CreateTexture2D(&desc, nullptr, output.GetAddressOf()),
        "Shader output image");
  ComPtr<ID3D11RenderTargetView> target;
  Check(owner.device()->CreateRenderTargetView(output.Get(), nullptr, target.GetAddressOf()),
        "Shader output target");
  ComPtr<ID3D11VertexShader> vs;
  ComPtr<ID3D11PixelShader> ps;
  if (cached_vertex) {
    vs = cached_vertex->vertex();
    Require(bool(vs), "Cached vertex shader has the wrong stage");
  } else {
    Check(owner.device()->CreateVertexShader(vertex.data(), vertex.size(), nullptr,
                                             vs.GetAddressOf()),
          "Vertex binding shader");
  }
  if (cached_pixel) {
    ps = cached_pixel->pixel();
    Require(bool(ps), "Cached pixel shader has the wrong stage");
  } else {
    Check(owner.device()->CreatePixelShader(pixel.data(), pixel.size(), nullptr, ps.GetAddressOf()),
          "Pixel binding shader");
  }
  D3D11_SAMPLER_DESC sampler_desc = {};
  sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
  sampler_desc.AddressU = sampler_desc.AddressV = sampler_desc.AddressW =
      D3D11_TEXTURE_ADDRESS_CLAMP;
  sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
  ComPtr<ID3D11SamplerState> sampler;
  Check(owner.device()->CreateSamplerState(&sampler_desc, sampler.GetAddressOf()),
        "Shader sampler");
  D3D11_RASTERIZER_DESC rasterizer_desc = {};
  rasterizer_desc.FillMode = D3D11_FILL_SOLID;
  rasterizer_desc.CullMode = D3D11_CULL_NONE;
  rasterizer_desc.DepthClipEnable = TRUE;
  ComPtr<ID3D11RasterizerState> rasterizer;
  Check(owner.device()->CreateRasterizerState(&rasterizer_desc, rasterizer.GetAddressOf()),
        "Shader rasterizer");
  auto* context = owner.context();
  DxbcShaderTranslator::SystemConstants system = {};
  system.point_constant_diameter[0] = float(width);
  system.point_constant_diameter[1] = float(height);
  system.point_screen_diameter_to_ndc_radius[0] = 1.0f / float(width);
  system.point_screen_diameter_to_ndc_radius[1] = 1.0f / float(height);
  std::vector<uint8_t> system_bytes((sizeof(system) + 15) & ~size_t(15));
  std::memcpy(system_bytes.data(), &system, sizeof(system));
  auto system_buffer =
      Buffer(owner, system_bytes.data(), UINT(system_bytes.size()), D3D11_BIND_CONSTANT_BUFFER);
  ID3D11Buffer* system_cb = system_buffer.Get();
  context->GSSetConstantBuffers(UINT(DxbcShaderTranslator::CbufferRegister::kSystemConstants), 1,
                                &system_cb);
  if (geometry)
    Require(geometry->geometry() != nullptr, "Cached primitive shader has the wrong stage");
  context->GSSetShader(geometry ? geometry->geometry() : nullptr, nullptr, 0);
  ID3D11ShaderResourceView* srv = vertex_view.Get();
  ID3D11Buffer* constants = vs_constants.Get();
  context->VSSetShader(vs.Get(), nullptr, 0);
  context->VSSetShaderResources(0, 1, &srv);
  context->VSSetShaderResources(7, 1, &srv);
  context->VSSetConstantBuffers(3, 1, &constants);
  srv = image_view.Get();
  constants = ps_constants.Get();
  ID3D11SamplerState* sampler_state = sampler.Get();
  context->PSSetShader(ps.Get(), nullptr, 0);
  auto* raw_srv = raw_image_view.Get();
  context->PSSetShaderResources(0, 1, &raw_srv);
  context->PSSetShaderResources(11, 1, &srv);
  context->PSSetConstantBuffers(3, 1, &constants);
  context->PSSetSamplers(2, 1, &sampler_state);
  context->IASetPrimitiveTopology(topology);
  context->RSSetState(rasterizer.Get());
  D3D11_VIEWPORT viewport = {0, 0, float(width), float(height), 0, 1};
  context->RSSetViewports(1, &viewport);
  ID3D11RenderTargetView* rtv = target.Get();
  context->OMSetRenderTargets(1, &rtv, nullptr);
  context->Draw(UINT(positions.size() / 4), 0);
  context->ClearState();
  desc.Usage = D3D11_USAGE_STAGING;
  desc.BindFlags = 0;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  Check(owner.device()->CreateTexture2D(&desc, nullptr, staging.GetAddressOf()),
        "Shader image readback");
  context->CopyResource(staging.Get(), output.Get());
  Check(owner.WaitForCompletion(), "Shader draw completion");
  D3D11_MAPPED_SUBRESOURCE mapped = {};
  Check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Shader image readback map");
  bool exact = true;
  for (UINT y = 0; y < height; ++y) {
    exact &= std::memcmp(static_cast<const uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch,
                         pixels.data() + size_t(y) * width * 4, width * 4) == 0;
  }
  context->Unmap(staging.Get(), 0);
  Require(exact, "Converted vertex, texture, sampler or constant bindings changed the draw output");
  return uint64_t(width) * height;
}

void CheckRejectedInputs() {
  std::string error;
  ShaderBytecode output;
  auto valid = Compile(kCompute, "cs_5_1", D3DCOMPILE_OPTIMIZATION_LEVEL3);
  auto damaged = valid;
  damaged.back() ^= 1;
  Require(
      !ConvertShaderBytecode(damaged, {}, output, error) && output.data.empty() && !error.empty(),
      "A damaged shader was accepted");
  Require(!ConvertShaderBytecode(std::span(valid).first(28), {}, output, error),
          "A truncated shader was accepted");
  auto model50 = Compile(kCompute, "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3);
  Require(!ConvertShaderBytecode(model50, {}, output, error), "SM 5.0 input was converted twice");
  auto space = Compile(R"(
ByteAddressBuffer data : register(t0, space1);
float4 main(uint id : SV_VertexID) : SV_Position { return asfloat(data.Load4(id * 16)); }
)",
                       "vs_5_1", D3DCOMPILE_OPTIMIZATION_LEVEL3);
  Require(!ConvertShaderBytecode(space, {}, output, error),
          "A nonzero register space was accepted");
  auto bindless = Compile(R"(
Texture2D<float4> images[2] : register(t0);
float4 main(float4 p : SV_Position) : SV_Target { return images[uint(p.x) & 1].Load(int3(p.xy,0)); }
)",
                          "ps_5_1", D3DCOMPILE_OPTIMIZATION_LEVEL3);
  Require(!ConvertShaderBytecode(bindless, {}, output, error),
          "A resource range was accepted as a single binding");
  auto read_alias = Compile(kVertexAliasedRead, "vs_5_1", D3DCOMPILE_OPTIMIZATION_LEVEL3);
  rex::graphics::d3d11::ShaderBytecodeOptions aliases;
  aliases.read_only_uav_aliases.push_back({1, 9});
  Require(!ConvertShaderBytecode(read_alias, aliases, output, error),
          "A read-only UAV alias without a matching SRV was accepted");
  auto writing = Compile(R"(
ByteAddressBuffer input_data : register(t7);
RWByteAddressBuffer alternate : register(u1);
float4 main(uint id : SV_VertexID) : SV_Position {
  uint4 value = input_data.Load4(id * 16);
  alternate.Store4(id * 16, value);
  return asfloat(value);
})",
                         "vs_5_1", D3DCOMPILE_OPTIMIZATION_LEVEL3);
  aliases.read_only_uav_aliases[0].srv_register = 7;
  Require(!ConvertShaderBytecode(writing, aliases, output, error),
          "A UAV write was discarded by read-only alias conversion");
}

const ShaderProgram* Cached(ShaderCache& cache, const std::vector<uint8_t>& code, bool read_only) {
  std::string error;
  auto* program = cache.GetOrCreate(code, read_only, error);
  if (!program) {
    throw std::runtime_error(error);
  }
  Require(cache.GetOrCreate(code, read_only, error) == program && error.empty(),
          "Native shader cache did not reuse its shader");
  return program;
}

const ShaderProgram* CachedPrimitive(ShaderCache& cache, const std::filesystem::path& folder,
                                     uint32_t primitive) {
  char name[64];
  std::snprintf(name, sizeof(name), "geometry_%08X.sm51", primitive);
  std::ifstream file(folder / name, std::ios::binary | std::ios::ate);
  auto size = file.tellg();
  Require(size > 0 && size <= 1024 * 1024, "Missing primitive shader bytecode");
  file.seekg(0);
  std::vector<uint8_t> code(size_t(size), 0);
  file.read(reinterpret_cast<char*>(code.data()), size);
  Require(bool(file), "Could not read primitive shader bytecode");
  return Cached(cache, code, false);
}

void CheckCachedFailure(ShaderCache& cache, const std::vector<uint8_t>& valid) {
  auto invalid = valid;
  invalid[4] ^= 0x20;
  auto before = cache.statistics();
  std::string error, first_error;
  for (uint32_t i = 0; i < 1000; ++i) {
    Require(!cache.GetOrCreate(invalid, false, error) && !error.empty(),
            "A damaged shader was accepted by the native cache");
    if (i == 0) {
      first_error = error;
    }
    Require(error == first_error, "Cached shader failure changed");
  }
  const auto& after = cache.statistics();
  Require(after.conversions == before.conversions + 1 && after.creations == before.creations &&
              after.failures == before.failures + 1 && after.hits == before.hits + 999,
          "A failed shader was compiled repeatedly");
}

uint64_t CheckCompiledDirectory(D3D11Device& owner, ShaderCache& cache,
                                const std::filesystem::path& path) {
  uint64_t count = 0;
  for (const auto& entry : std::filesystem::directory_iterator(path)) {
    if (!entry.is_regular_file() || entry.path().extension() != ".dxbc") {
      continue;
    }
    std::ifstream file(entry.path(), std::ios::binary | std::ios::ate);
    auto size = file.tellg();
    Require(size > 0 && size <= 16 * 1024 * 1024, "Invalid compiled shader size");
    file.seekg(0);
    std::vector<uint8_t> code(size_t(size), 0);
    file.read(reinterpret_cast<char*>(code.data()), size);
    Require(bool(file), "Could not read a compiled shader");
    ComPtr<ID3D11ShaderReflection> reflection;
    Check(D3DReflect(code.data(), code.size(), IID_ID3D11ShaderReflection,
                     reinterpret_cast<void**>(reflection.GetAddressOf())),
          "Game shader reflection");
    D3D11_SHADER_DESC desc = {};
    Check(reflection->GetDesc(&desc), "Game shader description");
    Require((desc.Version & 0xFFFF) == 0x50, "Game shader is not native SM 5.0");
    HRESULT result;
    if ((desc.Version >> 16) == 0) {
      ComPtr<ID3D11PixelShader> shader;
      result = owner.device()->CreatePixelShader(code.data(), code.size(), nullptr,
                                                 shader.GetAddressOf());
    } else if ((desc.Version >> 16) == 1) {
      ComPtr<ID3D11VertexShader> shader;
      result = owner.device()->CreateVertexShader(code.data(), code.size(), nullptr,
                                                  shader.GetAddressOf());
    } else {
      throw std::runtime_error("The game inventory contains an unexpected shader stage");
    }
    if (FAILED(result)) {
      throw std::runtime_error("Game shader creation failed: " + entry.path().filename().string());
    }
    auto source_path = entry.path();
    source_path.replace_extension(".sm51");
    std::ifstream source_file(source_path, std::ios::binary | std::ios::ate);
    auto source_size = source_file.tellg();
    Require(source_size > 0 && source_size <= 16 * 1024 * 1024,
            "The inventory must include original SM 5.1 programs");
    source_file.seekg(0);
    std::vector<uint8_t> source_code(size_t(source_size), 0);
    source_file.read(reinterpret_cast<char*>(source_code.data()), source_size);
    Require(bool(source_file), "Could not read the original game shader");
    // This inventory was analyzed separately and contains no memexport in
    // either stage. The game draw path must prove the same promise per draw.
    const auto* cached = Cached(cache, source_code, true);
    Require(cached->bytecode().data == code, "The native cache changed an exported game shader");
    ++count;
  }
  Require(count != 0, "The shader directory contains no compiled DX11 shaders");
  return count;
}

std::string Quote(const std::string& value) {
  std::string result = "\"";
  for (unsigned char c : value) {
    if (c == '\\' || c == '\"')
      result += '\\';
    if (c < 32)
      result += ' ';
    else
      result += char(c);
  }
  return result + '\"';
}
}  // namespace

int main(int argc, char** argv) {
  std::string output;
  std::filesystem::path shader_directory;
  std::filesystem::path geometry_directory;
  try {
    D3D11Device::Options options;
    options.debug = true;
    for (int i = 1; i < argc; ++i) {
      std::string arg(argv[i]);
      if (arg == "--warp")
        options.warp = true;
      else if (arg == "--fl11-0")
        options.maximum_feature_level = D3D_FEATURE_LEVEL_11_0;
      else if (arg == "--output" && i + 1 < argc)
        output = argv[++i];
      else if (arg == "--shader-directory" && i + 1 < argc)
        shader_directory = argv[++i];
      else if (arg == "--geometry-directory" && i + 1 < argc)
        geometry_directory = argv[++i];
      else
        throw std::runtime_error(
            "usage: d3d11_shader_check [--warp] [--fl11-0] "
            "[--shader-directory compiled-shaders] [--geometry-directory primitive-shaders] "
            "[--output report.json]");
    }
    std::string error;
    auto device = D3D11Device::Create(options, error);
    Require(bool(device), error.c_str());
    ShaderCache shader_cache(*device);
    uint64_t words = 0, pixels = 0;
    uint64_t primitive_draws = 0;
    for (UINT flags : {UINT(D3DCOMPILE_SKIP_OPTIMIZATION), UINT(D3DCOMPILE_OPTIMIZATION_LEVEL3)}) {
      auto vs = BuildPair(kVertex, "vs", flags);
      auto ps = BuildPair(kPixel, "ps", flags);
      auto cs = BuildPair(kCompute, "cs", flags);
      auto aliased_vs = BuildAliasedVertexPair(flags);
      auto aliased_ps = BuildAliasedPixelPair(flags);
      words += CheckCompute(*device, cs.reference);
      words += CheckCompute(*device, cs.converted.data);
      pixels += CheckDrawing(*device, vs.reference, ps.reference);
      pixels += CheckDrawing(*device, vs.converted.data, ps.converted.data);
      pixels += CheckDrawing(*device, aliased_vs.converted.data, ps.converted.data);
      pixels += CheckDrawing(*device, vs.reference, aliased_ps.reference);
      const auto* cached_cs = Cached(shader_cache, cs.model51, false);
      const auto* cached_vs = Cached(shader_cache, vs.model51, true);
      const auto* cached_ps = Cached(shader_cache, ps.model51, true);
      words += CheckCompute(*device, cached_cs->bytecode().data, cached_cs);
      pixels += CheckDrawing(*device, cached_vs->bytecode().data, cached_ps->bytecode().data,
                             cached_vs, cached_ps);
      std::string shared_source(kVertexAliasedRead);
      shared_source.replace(shared_source.find("t7"), 2, "t0");
      shared_source.replace(shared_source.find("u1"), 2, "u0");
      auto shared_code = Compile(shared_source.c_str(), "vs_5_1", flags);
      const auto* shared_vs = Cached(shader_cache, shared_code, true);
      const auto* shared_ps = Cached(shader_cache, aliased_ps.model51, true);
      Require(shared_vs->bytecode().aliased_uav_reads.size() == 1 &&
                  shared_vs->bytecode().aliased_uav_reads[0].host_register == 0,
              "The native cache did not route read-only shared memory to its SRV");
      pixels += CheckDrawing(*device, shared_vs->bytecode().data, cached_ps->bytecode().data,
                             shared_vs, cached_ps);
      pixels += CheckDrawing(*device, cached_vs->bytecode().data, shared_ps->bytecode().data,
                             cached_vs, shared_ps);
      CheckCachedFailure(shader_cache, cs.model51);
      if (!geometry_directory.empty()) {
        for (uint32_t primitive : {1u, 2u, 3u}) {
          const auto* geometry = CachedPrimitive(shader_cache, geometry_directory, primitive);
          pixels += CheckDrawing(*device, cached_vs->bytecode().data, cached_ps->bytecode().data,
                                 cached_vs, cached_ps, geometry, primitive);
          ++primitive_draws;
        }
      }
    }
    CheckRejectedInputs();
    uint64_t game_shaders = shader_directory.empty()
                                ? 0
                                : CheckCompiledDirectory(*device, shader_cache, shader_directory);
    std::string report =
        "{\n  \"passed\": true,\n  \"scope\": \"native SM 5.0 shader bindings\",\n";
    report += "  \"adapter\": " + Quote(device->features().adapter_name) + ",\n";
    report +=
        "  \"software\": " + std::string(device->features().software ? "true" : "false") + ",\n";
    report += "  \"feature_level\": " + std::to_string(device->features().level) + ",\n";
    report += "  \"shader_pairs\": 10,\n  \"negative_cases\": 7,\n";
    report += "  \"game_shader_programs_created\": " + std::to_string(game_shaders) + ",\n";
    report += "  \"primitive_draws\": " + std::to_string(primitive_draws) + ",\n";
    report +=
        "  \"cached_shader_creations\": " + std::to_string(shader_cache.statistics().creations) +
        ",\n";
    report += "  \"cached_shader_hits\": " + std::to_string(shader_cache.statistics().hits) + ",\n";
    report += "  \"exact_compute_words\": " + std::to_string(words) + ",\n";
    report += "  \"exact_pixels\": " + std::to_string(pixels) + "\n}\n";
    std::fputs(report.c_str(), stdout);
    if (!output.empty()) {
      std::ofstream file(output);
      file << report;
      Require(bool(file), "Could not write the shader qualification report");
    }
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    if (!output.empty()) {
      std::ofstream file(output);
      file << "{\"passed\":false,\"error\":" << Quote(error.what()) << "}\n";
    }
    return 1;
  }
}
