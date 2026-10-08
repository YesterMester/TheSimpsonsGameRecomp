#include <rex/ui/d3d11/d3d11_device.h>
#include <rex/ui/d3d11/d3d11_swap_chain.h>

#include <array>
#include <bit>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
using rex::ui::d3d11::D3D11Device;
using rex::ui::d3d11::D3D11SwapChain;

namespace {
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

ComPtr<ID3DBlob> Compile(const char* source, D3D11Device::ShaderStage stage) {
  ComPtr<ID3DBlob> bytecode;
  std::string error;
  HRESULT result =
      D3D11Device::CompileShader(source, "main", stage, bytecode.GetAddressOf(), error);
  if (FAILED(result)) {
    throw std::runtime_error(error);
  }
  return bytecode;
}

uint64_t CheckRawBuffers(D3D11Device& owner) {
  constexpr UINT count = 4096;
  std::array<uint32_t, count> input;
  for (uint32_t i = 0; i < count; ++i) {
    input[i] = i * 0x9E3779B9u ^ 0x5A17C33Du;
  }
  D3D11_BUFFER_DESC desc = {};
  desc.ByteWidth = sizeof(input);
  desc.Usage = D3D11_USAGE_IMMUTABLE;
  desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
  D3D11_SUBRESOURCE_DATA data = {input.data(), 0, 0};
  ComPtr<ID3D11Buffer> source;
  Check(owner.device()->CreateBuffer(&desc, &data, source.GetAddressOf()), "Raw input buffer");
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
  ComPtr<ID3D11Buffer> output;
  Check(owner.device()->CreateBuffer(&desc, nullptr, output.GetAddressOf()), "Raw output buffer");
  D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
  srv_desc.Format = DXGI_FORMAT_R32_TYPELESS;
  srv_desc.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
  srv_desc.BufferEx.NumElements = count;
  srv_desc.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
  ComPtr<ID3D11ShaderResourceView> srv;
  Check(owner.device()->CreateShaderResourceView(source.Get(), &srv_desc, srv.GetAddressOf()),
        "Raw input view");
  D3D11_UNORDERED_ACCESS_VIEW_DESC uav_desc = {};
  uav_desc.Format = DXGI_FORMAT_R32_TYPELESS;
  uav_desc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
  uav_desc.Buffer.NumElements = count;
  uav_desc.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
  ComPtr<ID3D11UnorderedAccessView> uav;
  Check(owner.device()->CreateUnorderedAccessView(output.Get(), &uav_desc, uav.GetAddressOf()),
        "Raw output view");
  auto bytecode = Compile(R"(
ByteAddressBuffer input_data : register(t0);
RWByteAddressBuffer output_data : register(u0);
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  uint i = id.x;
  uint value = input_data.Load(i * 4);
  uint swapped = (value << 24) | ((value & 0xff00) << 8) |
                 ((value >> 8) & 0xff00) | (value >> 24);
  uint rotated = (swapped << 3) | (swapped >> 29);
  output_data.Store(i * 4, rotated ^ (i * 17 + 0x01020304));
})",
                          D3D11Device::ShaderStage::kCompute);
  ComPtr<ID3D11ComputeShader> shader;
  Check(owner.device()->CreateComputeShader(bytecode->GetBufferPointer(), bytecode->GetBufferSize(),
                                            nullptr, shader.GetAddressOf()),
        "SM5 compute shader");
  auto* context = owner.context();
  ID3D11ShaderResourceView* input_view = srv.Get();
  ID3D11UnorderedAccessView* output_view = uav.Get();
  context->CSSetShader(shader.Get(), nullptr, 0);
  context->CSSetShaderResources(0, 1, &input_view);
  context->CSSetUnorderedAccessViews(0, 1, &output_view, nullptr);
  context->Dispatch(count / 64, 1, 1);
  input_view = nullptr;
  output_view = nullptr;
  context->CSSetShaderResources(0, 1, &input_view);
  context->CSSetUnorderedAccessViews(0, 1, &output_view, nullptr);
  desc.Usage = D3D11_USAGE_STAGING;
  desc.BindFlags = 0;
  desc.MiscFlags = 0;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ComPtr<ID3D11Buffer> readback;
  Check(owner.device()->CreateBuffer(&desc, nullptr, readback.GetAddressOf()),
        "Raw readback buffer");
  context->CopyResource(readback.Get(), output.Get());
  Check(owner.WaitForCompletion(), "Raw buffer completion");
  D3D11_MAPPED_SUBRESOURCE mapped = {};
  Check(context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Raw readback map");
  bool exact = true;
  const auto* words = static_cast<const uint32_t*>(mapped.pData);
  for (uint32_t i = 0; i < count; ++i) {
    exact &= words[i] == (std::rotl(std::byteswap(input[i]), 3) ^ (i * 17u + 0x01020304u));
  }
  context->Unmap(readback.Get(), 0);
  Require(exact, "Native raw-buffer compute output differs from the CPU byte-order oracle");
  context->ClearState();
  return count;
}

std::vector<uint8_t> Pattern(uint32_t width, uint32_t height, uint32_t seed) {
  std::vector<uint8_t> pixels(size_t(width) * height * 4);
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      size_t offset = (size_t(y) * width + x) * 4;
      pixels[offset] = uint8_t(x * 7 + y * 3 + seed * 11);
      pixels[offset + 1] = uint8_t(x * 2 + y * 13 + seed * 17);
      pixels[offset + 2] = uint8_t(x * 19 + y + seed * 23);
      pixels[offset + 3] = uint8_t(32 + ((x + y + seed) & 223));
    }
  }
  return pixels;
}

class TestWindow {
 public:
  explicit TestWindow(bool visible) {
    if (!visible) {
      return;
    }
    WNDCLASSW desc = {};
    desc.lpfnWndProc = DefWindowProcW;
    desc.hInstance = GetModuleHandleW(nullptr);
    desc.lpszClassName = L"SimpsonsD3D11Check";
    Require(RegisterClassW(&desc) != 0, "Could not register the presentation test window");
    window_ = CreateWindowW(desc.lpszClassName, L"Direct3D 11 rendering check", WS_OVERLAPPEDWINDOW,
                            CW_USEDEFAULT, CW_USEDEFAULT, 768, 512, nullptr, nullptr,
                            desc.hInstance, nullptr);
    if (!window_) {
      UnregisterClassW(desc.lpszClassName, desc.hInstance);
      throw std::runtime_error("Could not create the presentation test window");
    }
    ShowWindow(window_, SW_SHOW);
  }
  ~TestWindow() {
    if (window_) {
      DestroyWindow(window_);
      UnregisterClassW(L"SimpsonsD3D11Check", GetModuleHandleW(nullptr));
    }
  }
  HWND window() const { return window_; }
  void Pump() {
    MSG message;
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
    Require(!window_ || IsWindow(window_), "Presentation test window was closed before completion");
  }

 private:
  HWND window_ = nullptr;
};

uint64_t CheckRenderTargets(D3D11Device& owner, bool present, uint32_t& presentations) {
  auto vertex_code = Compile(R"(
float4 main(uint id : SV_VertexID) : SV_Position {
  float2 p = float2((id << 1) & 2, id & 2);
  return float4(p * float2(2, -2) + float2(-1, 1), 0, 1);
})",
                             D3D11Device::ShaderStage::kVertex);
  auto pixel_code = Compile(R"(
Texture2D<float4> source_image : register(t0);
float4 main(float4 position : SV_Position) : SV_Target {
  return source_image.Load(int3(int2(position.xy), 0));
})",
                            D3D11Device::ShaderStage::kPixel);
  ComPtr<ID3D11VertexShader> vertex_shader;
  ComPtr<ID3D11PixelShader> pixel_shader;
  Check(owner.device()->CreateVertexShader(vertex_code->GetBufferPointer(),
                                           vertex_code->GetBufferSize(), nullptr,
                                           vertex_shader.GetAddressOf()),
        "SM5 vertex shader");
  Check(
      owner.device()->CreatePixelShader(pixel_code->GetBufferPointer(), pixel_code->GetBufferSize(),
                                        nullptr, pixel_shader.GetAddressOf()),
      "SM5 pixel shader");
  D3D11_RASTERIZER_DESC raster_desc = {};
  raster_desc.FillMode = D3D11_FILL_SOLID;
  raster_desc.CullMode = D3D11_CULL_NONE;
  raster_desc.DepthClipEnable = TRUE;
  ComPtr<ID3D11RasterizerState> raster;
  Check(owner.device()->CreateRasterizerState(&raster_desc, raster.GetAddressOf()), "Raster state");
  TestWindow window(present);
  std::unique_ptr<D3D11SwapChain> chain;
  auto* context = owner.context();
  uint64_t checked_pixels = 0;
  for (uint32_t scale : {1u, 2u}) {
    uint32_t width = 96 * scale, height = 64 * scale;
    if (present) {
      window.Pump();
      if (!chain) {
        std::string error;
        chain = D3D11SwapChain::Create(owner, window.window(), width, height, error);
        Require(bool(chain), error.c_str());
      } else {
        Check(chain->Resize(width, height), "Swap-chain resize");
      }
    }
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> input;
    ComPtr<ID3D11ShaderResourceView> input_view;
    Check(owner.device()->CreateTexture2D(&desc, nullptr, input.GetAddressOf()), "Source texture");
    Check(owner.device()->CreateShaderResourceView(input.Get(), nullptr, input_view.GetAddressOf()),
          "Source texture view");
    std::vector<ComPtr<ID3D11Texture2D>> versions;
    for (uint32_t seed = 0; seed < 16; ++seed) {
      if (present) {
        window.Pump();
        Check(chain->WaitForFrame(), "Frame-latency wait");
      }
      auto pixels = Pattern(width, height, seed);
      context->UpdateSubresource(input.Get(), 0, nullptr, pixels.data(), width * 4, 0);
      desc.BindFlags = D3D11_BIND_RENDER_TARGET;
      ComPtr<ID3D11Texture2D> output;
      ComPtr<ID3D11RenderTargetView> target;
      Check(owner.device()->CreateTexture2D(&desc, nullptr, output.GetAddressOf()),
            "Owned render target");
      Check(owner.device()->CreateRenderTargetView(output.Get(), nullptr, target.GetAddressOf()),
            "Owned render target view");
      D3D11_VIEWPORT viewport = {0, 0, float(width), float(height), 0, 1};
      context->IASetInputLayout(nullptr);
      context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      context->VSSetShader(vertex_shader.Get(), nullptr, 0);
      context->PSSetShader(pixel_shader.Get(), nullptr, 0);
      context->RSSetState(raster.Get());
      context->RSSetViewports(1, &viewport);
      ID3D11ShaderResourceView* source_view = input_view.Get();
      ID3D11RenderTargetView* target_view = target.Get();
      context->PSSetShaderResources(0, 1, &source_view);
      context->OMSetRenderTargets(1, &target_view, nullptr);
      context->Draw(3, 0);
      source_view = nullptr;
      context->PSSetShaderResources(0, 1, &source_view);
      context->OMSetRenderTargets(0, nullptr, nullptr);
      if (present) {
        context->CopyResource(chain->back_buffer(), output.Get());
        HRESULT status = chain->Present((seed & 1) == 0);
        Check(status, "Flip presentation");
        Require(status == S_OK,
                "Presentation test window is occluded; frame delivery was not checked");
        ++presentations;
      }
      versions.push_back(std::move(output));
    }
    // Read every older frame after all later updates and submissions. A stale
    // view or exchanged backing image cannot pass by checking only the last.
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> readback;
    Check(owner.device()->CreateTexture2D(&desc, nullptr, readback.GetAddressOf()),
          "Image readback");
    for (uint32_t seed = 0; seed < versions.size(); ++seed) {
      context->CopyResource(readback.Get(), versions[seed].Get());
      Check(owner.WaitForCompletion(), "Image completion");
      D3D11_MAPPED_SUBRESOURCE mapped = {};
      Check(context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Image readback map");
      auto expected = Pattern(width, height, seed);
      bool exact = true;
      const auto* pixels = static_cast<const uint8_t*>(mapped.pData);
      for (uint32_t y = 0; y < height; ++y) {
        exact &= !std::memcmp(pixels + size_t(y) * mapped.RowPitch,
                              expected.data() + size_t(y) * width * 4, size_t(width) * 4);
      }
      context->Unmap(readback.Get(), 0);
      Require(exact, "An older native render target lost its exact channel data or frame version");
      checked_pixels += uint64_t(width) * height;
    }
    context->ClearState();
  }
  Check(owner.WaitForCompletion(), "Final presentation completion");
  return checked_pixels;
}

void CheckDebugMessages(ID3D11Device* device) {
  ComPtr<ID3D11InfoQueue> queue;
  if (FAILED(device->QueryInterface(IID_PPV_ARGS(queue.GetAddressOf())))) {
    return;
  }
  for (uint64_t index = 0; index < queue->GetNumStoredMessagesAllowedByRetrievalFilter(); ++index) {
    SIZE_T bytes = 0;
    Check(queue->GetMessage(index, nullptr, &bytes), "Debug message size");
    std::vector<uint8_t> storage(bytes);
    auto* message = reinterpret_cast<D3D11_MESSAGE*>(storage.data());
    Check(queue->GetMessage(index, message, &bytes), "Debug message read");
    if (message->Severity <= D3D11_MESSAGE_SEVERITY_ERROR) {
      throw std::runtime_error(message->pDescription);
    }
  }
}

std::string Quote(std::string_view value) {
  std::string result = "\"";
  for (unsigned char c : value) {
    if (c == '"' || c == '\\') {
      result += '\\';
    }
    if (c < 32) {
      char escaped[7];
      std::snprintf(escaped, sizeof(escaped), "\\u%04X", unsigned(c));
      result += escaped;
    } else {
      result += char(c);
    }
  }
  return result + '"';
}
}  // namespace

int main(int argc, char** argv) {
  std::string output;
  try {
    D3D11Device::Options options;
    options.debug = true;
    bool present = false;
    for (int i = 1; i < argc; ++i) {
      std::string_view arg = argv[i];
      if (arg == "--warp") {
        options.warp = true;
      } else if (arg == "--present") {
        present = true;
      } else if (arg == "--adapter" && i + 1 < argc) {
        options.adapter_index = std::stoi(argv[++i]);
      } else if (arg == "--output" && i + 1 < argc) {
        output = argv[++i];
      } else {
        throw std::runtime_error(
            "Usage: d3d11_check [--warp | --adapter N] [--present] [--output report.json]");
      }
    }
    std::string error;
    auto owner = D3D11Device::Create(options, error);
    Require(bool(owner), error.c_str());
    uint64_t words = CheckRawBuffers(*owner);
    uint32_t presentations = 0;
    uint64_t pixels = CheckRenderTargets(*owner, present, presentations);
    CheckDebugMessages(owner->device());
    const auto& features = owner->features();
    std::string report =
        "{\n  \"passed\": true,\n  \"scope\": \"device, resources and optional presentation\",\n";
    report += "  \"adapter\": " + Quote(features.adapter_name) + ",\n";
    report += "  \"feature_level\": " + std::to_string(unsigned(features.level)) + ",\n";
    report += "  \"software\": " + std::string(features.software ? "true" : "false") + ",\n";
    report += "  \"debug_layer\": " + std::string(features.debug_layer ? "true" : "false") + ",\n";
    report +=
        "  \"rovs\": " + std::string(features.rasterizer_ordered_views ? "true" : "false") + ",\n";
    report += "  \"raw_words\": " + std::to_string(words) + ",\n";
    report += "  \"retained_image_versions\": 32,\n  \"scales\": [1, 2],\n";
    report += "  \"exact_pixels\": " + std::to_string(pixels) + ",\n";
    report += "  \"presentations\": " + std::to_string(presentations) + "\n}\n";
    std::fputs(report.c_str(), stdout);
    if (!output.empty()) {
      std::ofstream file(output);
      file << report;
      Require(bool(file), "Could not write the qualification report");
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
