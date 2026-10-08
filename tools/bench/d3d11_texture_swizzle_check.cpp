#include <rex/graphics/d3d11/draw_context.h>
#include <rex/graphics/d3d11/shader_cache.h>

#include <array>
#include <bit>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace rex::graphics::d3d11;
using rex::ui::d3d11::D3D11Device;

namespace {
constexpr uint32_t kWidth = 17, kHeight = 9, kMappings = 6 * 6 * 6 * 6;
using Pixel = std::array<uint32_t, 4>;
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
ComPtr<ID3DBlob> Compile(const std::string& source, const char* profile) {
  ComPtr<ID3DBlob> code, diagnostics;
  HRESULT result = D3DCompile(source.data(), source.size(), "texture_channel_check", nullptr,
                              nullptr, "main", profile, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                              code.GetAddressOf(), diagnostics.GetAddressOf());
  if (FAILED(result))
    throw std::runtime_error(diagnostics ? static_cast<const char*>(diagnostics->GetBufferPointer())
                                         : "Texture channel shader compilation failed");
  return code;
}
std::span<const uint8_t> Bytes(ID3DBlob* blob) {
  return {static_cast<const uint8_t*>(blob->GetBufferPointer()), blob->GetBufferSize()};
}
uint32_t Mapping(uint32_t ordinal) {
  uint32_t mapping = 0;
  for (uint32_t component = 0; component < 4; ++component) {
    mapping |= (ordinal % 6) << (component * 3);
    ordinal /= 6;
  }
  return mapping;
}
struct Results {
  uint32_t draws = 0, mappings = 0, queries = 0, negatives = 0;
  uint64_t pixels = 0;
  uint32_t permutations_without_moves = 0, mappings_with_constants = 0;
};

void CheckMappings(D3D11Device& owner, DrawContext& draws, ShaderCache& shaders,
                   const ShaderProgram* vertex, uint32_t type, uint32_t operation,
                   Results& results) {
  const char* scalar = type == 0 ? "float" : type == 1 ? "uint" : "int";
  DXGI_FORMAT format = type == 0   ? DXGI_FORMAT_R32G32B32A32_FLOAT
                       : type == 1 ? DXGI_FORMAT_R32G32B32A32_UINT
                                   : DXGI_FORMAT_R32G32B32A32_SINT;
  std::vector<Pixel> source(kWidth * kHeight);
  for (uint32_t i = 0; i < source.size(); ++i) {
    for (uint32_t c = 0; c < 4; ++c) {
      uint32_t value = 3 + c * 29 + (i % 19);
      source[i][c] = type == 0   ? std::bit_cast<uint32_t>(float(value))
                     : type == 1 ? value
                                 : uint32_t(-int32_t(value));
    }
  }
  D3D11_TEXTURE2D_DESC desc = {};
  desc.Width = kWidth;
  desc.Height = kHeight;
  desc.MipLevels = desc.ArraySize = 1;
  desc.Format = format;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_IMMUTABLE;
  desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  D3D11_SUBRESOURCE_DATA initial = {source.data(), kWidth * sizeof(Pixel), 0};
  ComPtr<ID3D11Texture2D> input, output, staging;
  Check(owner.device()->CreateTexture2D(&desc, &initial, input.GetAddressOf()), "Channel input");
  ComPtr<ID3D11ShaderResourceView> srv;
  Check(owner.device()->CreateShaderResourceView(input.Get(), nullptr, srv.GetAddressOf()),
        "Channel view");
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_RENDER_TARGET;
  Check(owner.device()->CreateTexture2D(&desc, nullptr, output.GetAddressOf()), "Channel output");
  ComPtr<ID3D11RenderTargetView> rtv;
  Check(owner.device()->CreateRenderTargetView(output.Get(), nullptr, rtv.GetAddressOf()),
        "Channel target");
  desc.Usage = D3D11_USAGE_STAGING;
  desc.BindFlags = 0;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  desc.ArraySize = kMappings;
  Check(owner.device()->CreateTexture2D(&desc, nullptr, staging.GetAddressOf()),
        "Channel result array");
  D3D11_SAMPLER_DESC sampler_desc = {};
  sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
  sampler_desc.AddressU = sampler_desc.AddressV = sampler_desc.AddressW =
      D3D11_TEXTURE_ADDRESS_CLAMP;
  sampler_desc.MaxAnisotropy = 1;
  sampler_desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
  sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
  ComPtr<ID3D11SamplerState> sampler;
  Check(owner.device()->CreateSamplerState(&sampler_desc, sampler.GetAddressOf()),
        "Channel sampler");
  std::string fetch = "tex.Load(int3(uint2(p.xy),0))";
  std::string uv = "p.xy/float2(17,9)";
  if (operation == 1)
    fetch = "tex.Sample(samp," + uv + ")";
  if (operation == 2)
    fetch = "tex.SampleLevel(samp," + uv + ",0)";
  if (operation == 3)
    fetch = "tex.SampleGrad(samp," + uv + ",float2(1.0/17,0),float2(0,1.0/9))";
  if (operation == 4)
    fetch = "tex.SampleBias(samp," + uv + ",0)";
  std::string vector_type = std::string(scalar) + "4";
  auto code = Compile("Texture2D<" + vector_type +
                          "> tex:register(t3);"
                          "SamplerState samp:register(s0);" +
                          vector_type + " main(float4 p:SV_Position):SV_Target { return " + fetch +
                          ".wzyx*2+" + vector_type + "(3,5,7,9); }",
                      "ps_5_1");
  std::array<ID3D11ShaderResourceView*, 4> resources = {nullptr, nullptr, nullptr, srv.Get()};
  std::array<ID3D11SamplerState*, 1> samplers = {sampler.Get()};
  std::array<ID3D11RenderTargetView*, 1> targets = {rtv.Get()};
  DrawCommand command;
  command.programs[size_t(DrawStage::kVertex)] = vertex;
  command.bindings[size_t(DrawStage::kPixel)].resources = resources;
  command.bindings[size_t(DrawStage::kPixel)].samplers = samplers;
  command.render_targets = targets;
  command.state = DrawState::Default(kWidth, kHeight);
  command.topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
  command.count = 3;
  for (uint32_t i = 0; i < kMappings; ++i) {
    TextureSwizzle mapping = {3, Mapping(i)};
    std::string error;
    const ShaderProgram* pixel = shaders.GetOrCreate(Bytes(code.Get()), true, {&mapping, 1}, error);
    if (!pixel)
      throw std::runtime_error(error);
    Require(shaders.GetOrCreate(Bytes(code.Get()), true, {&mapping, 1}, error) == pixel,
            "Native channel variant was not retained");
    bool constant = false;
    for (uint32_t c = 0; c < 4; ++c)
      constant |= ((mapping.swizzle >> (c * 3)) & 7) >= 4;
    Require(pixel->bytecode().texture_swizzle_moves == uint32_t(constant),
            "Channel specialization added unexpected shader instructions");
    if (constant)
      ++results.mappings_with_constants;
    else
      ++results.permutations_without_moves;
    command.programs[size_t(DrawStage::kPixel)] = pixel;
    if (!draws.Draw(command, error))
      throw std::runtime_error(error);
    draws.Invalidate();
    owner.context()->CopySubresourceRegion(staging.Get(), i, 0, 0, 0, output.Get(), 0, nullptr);
    ++results.draws;
    ++results.mappings;
  }
  Check(owner.WaitForCompletion(), "Channel draw completion");
  for (uint32_t i = 0; i < kMappings; ++i) {
    D3D11_MAPPED_SUBRESOURCE mapped;
    Check(owner.context()->Map(staging.Get(), i, D3D11_MAP_READ, 0, &mapped), "Channel result map");
    bool exact = true;
    for (uint32_t y = 0; y < kHeight; ++y) {
      auto* row = reinterpret_cast<const Pixel*>(static_cast<const uint8_t*>(mapped.pData) +
                                                 y * mapped.RowPitch);
      for (uint32_t x = 0; x < kWidth; ++x) {
        const auto& input_pixel = source[y * kWidth + x];
        for (uint32_t c = 0; c < 4; ++c) {
          // Apply the six guest view selectors before the shader's reverse
          // swizzle and arithmetic. This oracle never reads shader tokens.
          uint32_t selector = (Mapping(i) >> ((3 - c) * 3)) & 7;
          uint32_t expected;
          if (type == 0) {
            float value =
                selector < 4 ? std::bit_cast<float>(input_pixel[selector]) : float(selector - 4);
            expected = std::bit_cast<uint32_t>(value * 2 + float(3 + 2 * c));
          } else {
            uint32_t value = selector < 4 ? input_pixel[selector] : selector - 4;
            expected = value * 2 + 3 + 2 * c;
          }
          exact &= row[x][c] == expected;
        }
        ++results.pixels;
      }
    }
    owner.context()->Unmap(staging.Get(), i);
    if (!exact) {
      char message[160];
      std::snprintf(message, sizeof(message),
                    "Texture channel mismatch: type=%u operation=%u mapping=%03X", type, operation,
                    Mapping(i));
      throw std::runtime_error(message);
    }
  }
  std::string error;
  TextureSwizzle absent = {2, 0};
  Require(!shaders.GetOrCreate(Bytes(code.Get()), true, {&absent, 1}, error),
          "Undeclared channel mapping accepted");
  TextureSwizzle invalid = {3, 6};
  Require(!shaders.GetOrCreate(Bytes(code.Get()), true, {&invalid, 1}, error),
          "Invalid channel selector accepted");
  std::array<TextureSwizzle, 2> duplicate = {{{3, 0}, {3, kIdentityTextureSwizzle}}};
  Require(!shaders.GetOrCreate(Bytes(code.Get()), true, duplicate, error),
          "Duplicate channel mapping accepted");
  results.negatives += 3;
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
        throw std::runtime_error("Unknown channel check argument");
    }
    std::string error;
    auto owner = D3D11Device::Create(options, error);
    if (!owner)
      throw std::runtime_error(error);
    DrawContext draws(*owner);
    ShaderCache shaders(*owner);
    auto vertex_code = Compile(
        "float4 main(uint id:SV_VertexID):SV_Position {"
        "float2 p=float2((id<<1)&2,id&2); return float4(p*float2(2,-2)+float2(-1,1),0,1); }",
        "vs_5_1");
    const auto* vertex = shaders.GetOrCreate(Bytes(vertex_code.Get()), true, error);
    if (!vertex)
      throw std::runtime_error(error);
    Results results;
    for (uint32_t operation = 0; operation < 5; ++operation)
      CheckMappings(*owner, draws, shaders, vertex, 0, operation, results);
    for (uint32_t type = 1; type < 3; ++type)
      CheckMappings(*owner, draws, shaders, vertex, type, 0, results);
    ComPtr<ID3D11InfoQueue> messages;
    if (SUCCEEDED(owner->device()->QueryInterface(IID_PPV_ARGS(messages.GetAddressOf())))) {
      for (uint64_t i = 0; i < messages->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
        SIZE_T size = 0;
        Check(messages->GetMessage(i, nullptr, &size), "Channel debug message size");
        std::vector<uint8_t> storage(size);
        auto* message = reinterpret_cast<D3D11_MESSAGE*>(storage.data());
        Check(messages->GetMessage(i, message, &size), "Channel debug message read");
        if (message->Severity <= D3D11_MESSAGE_SEVERITY_WARNING)
          throw std::runtime_error(message->pDescription);
      }
    }
    std::string report =
        "{\"passed\":true,\"feature_level\":" + std::to_string(owner->features().level) +
        ",\"software\":" + (owner->features().software ? "true" : "false") +
        ",\"channel_mappings\":" + std::to_string(results.mappings) +
        ",\"draws\":" + std::to_string(results.draws) +
        ",\"exact_pixels\":" + std::to_string(results.pixels) +
        ",\"permutations_without_extra_instructions\":" +
        std::to_string(results.permutations_without_moves) +
        ",\"constant_mappings_with_one_move\":" + std::to_string(results.mappings_with_constants) +
        ",\"negative_checks\":" + std::to_string(results.negatives) +
        ",\"cache_hits\":" + std::to_string(shaders.statistics().hits) + "}";
    std::puts(report.c_str());
    if (!output.empty())
      std::ofstream(output) << report << '\n';
    return 0;
  } catch (const std::exception& exception) {
    std::fprintf(stderr, "%s\n", exception.what());
    if (!output.empty()) {
      std::string escaped;
      for (char c : std::string(exception.what())) {
        if (c == '\\' || c == '"')
          escaped += '\\';
        escaped += c == '\n' ? ' ' : c;
      }
      std::ofstream(output) << "{\"passed\":false,\"error\":\"" << escaped << "\"}\n";
    }
    return 1;
  }
}
