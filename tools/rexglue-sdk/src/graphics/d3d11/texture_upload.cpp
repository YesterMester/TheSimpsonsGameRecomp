#include <rex/graphics/d3d11/texture_upload.h>

#include <algorithm>
#include <array>
#include <cstdio>

namespace rex::graphics::d3d11 {

namespace {
using Microsoft::WRL::ComPtr;
std::array<uint32_t, 4> ComponentBits(DXGI_FORMAT format) {
  switch (format) {
    case DXGI_FORMAT_R8_UINT:
      return {8, 0, 0, 0};
    case DXGI_FORMAT_R8G8_UINT:
      return {8, 8, 0, 0};
    case DXGI_FORMAT_R8G8B8A8_UINT:
      return {8, 8, 8, 8};
    case DXGI_FORMAT_R10G10B10A2_UINT:
      return {10, 10, 10, 2};
    case DXGI_FORMAT_R16_UINT:
      return {16, 0, 0, 0};
    case DXGI_FORMAT_R16G16_UINT:
      return {16, 16, 0, 0};
    case DXGI_FORMAT_R16G16B16A16_UINT:
      return {16, 16, 16, 16};
    case DXGI_FORMAT_R32_UINT:
      return {32, 0, 0, 0};
    case DXGI_FORMAT_R32G32_UINT:
      return {32, 32, 0, 0};
    case DXGI_FORMAT_R32G32B32A32_UINT:
      return {32, 32, 32, 32};
    default:
      return {};
  }
}
bool NativeError(HRESULT result, const char* operation, std::string& error) {
  if (SUCCEEDED(result))
    return false;
  char message[160];
  std::snprintf(message, sizeof(message), "%s failed (0x%08X)", operation, unsigned(result));
  error = message;
  return true;
}
}  // namespace

bool TextureUpload::Upload(ID3D11ShaderResourceView* source, ID3D11UnorderedAccessView* destination,
                           const Layout& layout, std::string& error) {
  error.clear();
  if (!source || !destination || !layout.width || !layout.height || !layout.depth_or_layers) {
    error = "The DX11 texture upload has an empty source, destination or extent";
    return false;
  }
  D3D11_SHADER_RESOURCE_VIEW_DESC source_desc;
  source->GetDesc(&source_desc);
  if (source_desc.ViewDimension != D3D11_SRV_DIMENSION_BUFFEREX ||
      source_desc.Format != DXGI_FORMAT_R32_TYPELESS ||
      !(source_desc.BufferEx.Flags & D3D11_BUFFEREX_SRV_FLAG_RAW)) {
    error = "The DX11 texture upload source must be a native raw buffer view";
    return false;
  }
  D3D11_UNORDERED_ACCESS_VIEW_DESC dest_desc;
  destination->GetDesc(&dest_desc);
  auto bits = ComponentBits(dest_desc.Format);
  uint32_t texel_bytes = (bits[0] + bits[1] + bits[2] + bits[3]) / 8;
  if (layout.packed_source != PackedSource::kNone) {
    if (uint32_t(layout.packed_source) > uint32_t(PackedSource::kB4G4R4A4) ||
        dest_desc.Format != DXGI_FORMAT_R32G32B32A32_UINT) {
      error = "Packed native uploads require an RGBA32 float storage family";
      return false;
    }
    texel_bytes = 2;
  }
  bool two_d = dest_desc.ViewDimension == D3D11_UAV_DIMENSION_TEXTURE2D;
  bool array = dest_desc.ViewDimension == D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
  bool three_d = dest_desc.ViewDimension == D3D11_UAV_DIMENSION_TEXTURE3D;
  if (!texel_bytes || (!two_d && !array && !three_d)) {
    error = "The DX11 texture upload destination has an unsupported native format or dimension";
    return false;
  }
  uint64_t row_bytes = uint64_t(layout.width) * texel_bytes;
  if (row_bytes > layout.row_pitch ||
      (layout.depth_or_layers > 1 &&
       uint64_t(layout.row_pitch) * layout.height > layout.slice_pitch)) {
    error = "The DX11 texture upload source pitches overlap texels";
    return false;
  }
  uint64_t source_end = uint64_t(layout.offset) +
                        uint64_t(layout.depth_or_layers - 1) * layout.slice_pitch +
                        uint64_t(layout.height - 1) * layout.row_pitch + row_bytes;
  if (source_end > uint64_t(source_desc.BufferEx.NumElements) * 4 || source_end > UINT32_MAX) {
    error = "The DX11 texture upload exceeds the native source view";
    return false;
  }
  ComPtr<ID3D11Resource> source_resource, dest_resource;
  source->GetResource(source_resource.GetAddressOf());
  destination->GetResource(dest_resource.GetAddressOf());
  if (source_resource.Get() == dest_resource.Get()) {
    error = "The DX11 texture upload source aliases its destination";
    return false;
  }
  uint32_t width, height, depth;
  if (three_d) {
    ComPtr<ID3D11Texture3D> texture;
    if (NativeError(dest_resource.As(&texture), "DX11 texture upload dimension query", error))
      return false;
    D3D11_TEXTURE3D_DESC desc;
    texture->GetDesc(&desc);
    uint32_t mip = dest_desc.Texture3D.MipSlice;
    width = std::max(1u, desc.Width >> mip);
    height = std::max(1u, desc.Height >> mip);
    uint32_t available = std::max(1u, desc.Depth >> mip) - dest_desc.Texture3D.FirstWSlice;
    depth = dest_desc.Texture3D.WSize == UINT32_MAX ? available : dest_desc.Texture3D.WSize;
  } else {
    ComPtr<ID3D11Texture2D> texture;
    if (NativeError(dest_resource.As(&texture), "DX11 texture upload dimension query", error))
      return false;
    D3D11_TEXTURE2D_DESC desc;
    texture->GetDesc(&desc);
    uint32_t mip = array ? dest_desc.Texture2DArray.MipSlice : dest_desc.Texture2D.MipSlice;
    width = std::max(1u, desc.Width >> mip);
    height = std::max(1u, desc.Height >> mip);
    uint32_t available = array ? desc.ArraySize - dest_desc.Texture2DArray.FirstArraySlice : 1;
    depth = array && dest_desc.Texture2DArray.ArraySize != UINT32_MAX
                ? dest_desc.Texture2DArray.ArraySize
                : available;
  }
  if (layout.width > width || layout.height > height || layout.depth_or_layers > depth) {
    error = "The DX11 texture upload exceeds the native destination view";
    return false;
  }
  // The shader below writes mip-relative volume coordinates. Bind the whole
  // mip so the view's FirstWSlice is not also applied to destination_z.
  ComPtr<ID3D11UnorderedAccessView> volume_view;
  if (three_d && dest_desc.Texture3D.FirstWSlice) {
    auto full_desc = dest_desc;
    full_desc.Texture3D.FirstWSlice = 0;
    full_desc.Texture3D.WSize = UINT32_MAX;
    if (NativeError(device_.device()->CreateUnorderedAccessView(dest_resource.Get(), &full_desc,
                                                                volume_view.GetAddressOf()),
                    "DX11 texture upload volume view", error))
      return false;
    destination = volume_view.Get();
  }
  uint64_t key = uint64_t(layout.packed_source) << 40 | uint64_t(dest_desc.ViewDimension) << 32 |
                 uint32_t(dest_desc.Format);
  auto found = kernels_.find(key);
  if (found == kernels_.end()) {
    Kernel kernel;
    std::string code = "ByteAddressBuffer source_data : register(t0);\nRWTexture";
    code += three_d ? "3D" : array ? "2DArray" : "2D";
    code += R"(<uint4> target : register(u0);
cbuffer packed_values : register(b1) {float4 normalized_values[28];};
float NormalizePacked(uint value, uint bits) {
  if (bits == 1) return float(value);
  uint index = value + (bits == 6 ? 32 : bits == 4 ? 96 : 0);
  return normalized_values[index >> 2][index & 3];
}
cbuffer parameters : register(b0) {
 uint offset; uint row_pitch; uint slice_pitch; uint width;
 uint height; uint depth; uint destination_z; uint padding1;
};
uint ReadBits(uint byte_offset, uint bit_offset, uint count) {
 uint bit_address = (byte_offset & 3u) * 8u + bit_offset;
 uint address = (byte_offset & ~3u) + ((bit_address >> 5u) * 4u);
 uint shift = bit_address & 31u;
 uint value = source_data.Load(address) >> shift;
 if (shift != 0u && shift + count > 32u) value |= source_data.Load(address + 4u) << (32u-shift);
 return count == 32u ? value : value & ((1u << count)-1u);
}
[numthreads(8,8,1)] void main(uint3 p : SV_DispatchThreadID) {
 if (p.x >= width || p.y >= height || p.z >= depth) return;
 uint address = offset + p.z * slice_pitch + p.y * row_pitch + p.x * )";
    code += std::to_string(texel_bytes) + "u;\n uint4 value = uint4(";
    uint32_t bit_offset = 0;
    std::array<uint32_t, 4> packed_offsets = {0, 0, 0, 0}, packed_bits = {0, 0, 0, 0};
    switch (layout.packed_source) {
      case PackedSource::kB5G6R5:
        packed_offsets = {11, 5, 0, 0};
        packed_bits = {5, 6, 5, 0};
        break;
      case PackedSource::kB5G5R5A1:
        packed_offsets = {10, 5, 0, 15};
        packed_bits = {5, 5, 5, 1};
        break;
      case PackedSource::kB4G4R4A4:
        packed_offsets = {8, 4, 0, 12};
        packed_bits = {4, 4, 4, 4};
        break;
      default:
        break;
    }
    for (size_t i = 0; i < bits.size(); ++i) {
      if (i)
        code += ",";
      if (layout.packed_source != PackedSource::kNone) {
        code += packed_bits[i] ? "asuint(NormalizePacked(ReadBits(address," +
                                     std::to_string(packed_offsets[i]) + "u," +
                                     std::to_string(packed_bits[i]) + "u)," +
                                     std::to_string(packed_bits[i]) + "u))"
                               : "asuint(1.0f)";
      } else
        code += bits[i] ? "ReadBits(address," + std::to_string(bit_offset) + "u," +
                              std::to_string(bits[i]) + "u)"
                        : "0u";
      bit_offset += bits[i];
    }
    code += ");\n target[";
    code += two_d ? "p.xy" : three_d ? "p + uint3(0,0,destination_z)" : "p";
    code += "] = value;\n}\n";
    ComPtr<ID3DBlob> bytecode;
    HRESULT result = ui::d3d11::D3D11Device::CompileShader(
        code, "main", ui::d3d11::D3D11Device::ShaderStage::kCompute, bytecode.GetAddressOf(),
        kernel.error);
    if (SUCCEEDED(result)) {
      result = device_.device()->CreateComputeShader(bytecode->GetBufferPointer(),
                                                     bytecode->GetBufferSize(), nullptr,
                                                     kernel.shader.GetAddressOf());
      NativeError(result, "DX11 texture upload shader creation", kernel.error);
    }
    found = kernels_.emplace(key, std::move(kernel)).first;
  }
  if (!found->second.shader) {
    error = found->second.error;
    return false;
  }
  std::array<uint32_t, 8> constants = {layout.offset,
                                       layout.row_pitch,
                                       layout.slice_pitch,
                                       layout.width,
                                       layout.height,
                                       layout.depth_or_layers,
                                       three_d ? dest_desc.Texture3D.FirstWSlice : 0,
                                       0};
  D3D11_BUFFER_DESC constants_desc = {};
  constants_desc.ByteWidth = sizeof(constants);
  constants_desc.Usage = D3D11_USAGE_IMMUTABLE;
  constants_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  D3D11_SUBRESOURCE_DATA initial = {constants.data(), 0, 0};
  ComPtr<ID3D11Buffer> constants_buffer;
  if (NativeError(device_.device()->CreateBuffer(&constants_desc, &initial,
                                                 constants_buffer.GetAddressOf()),
                  "DX11 texture upload constants", error))
    return false;
  if (layout.packed_source != PackedSource::kNone && !packed_normalization_) {
    // These universal UNORM constants keep the source precision exactly.
    // Constant-divisor GPU reciprocals otherwise differ by a float ULP on
    // some drivers. Texel decoding and image publication still run on the GPU.
    std::array<float, 112> normalized;
    for (uint32_t i = 0; i < 32; ++i)
      normalized[i] = float(double(i) / 31);
    for (uint32_t i = 0; i < 64; ++i)
      normalized[32 + i] = float(double(i) / 63);
    for (uint32_t i = 0; i < 16; ++i)
      normalized[96 + i] = float(double(i) / 15);
    D3D11_BUFFER_DESC desc = {};
    desc.ByteWidth = sizeof(normalized);
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA data = {normalized.data(), 0, 0};
    if (NativeError(
            device_.device()->CreateBuffer(&desc, &data, packed_normalization_.GetAddressOf()),
            "Native packed texture normalization", error))
      return false;
  }
  draws_.Invalidate();
  auto* context = device_.context();
  auto* cb = constants_buffer.Get();
  context->CSSetShader(found->second.shader.Get(), nullptr, 0);
  context->CSSetConstantBuffers(0, 1, &cb);
  auto* normalization =
      layout.packed_source != PackedSource::kNone ? packed_normalization_.Get() : nullptr;
  context->CSSetConstantBuffers(1, 1, &normalization);
  context->CSSetShaderResources(0, 1, &source);
  context->CSSetUnorderedAccessViews(0, 1, &destination, nullptr);
  context->Dispatch((layout.width + 7) / 8, (layout.height + 7) / 8, layout.depth_or_layers);
  ID3D11UnorderedAccessView* null_uav = nullptr;
  ID3D11ShaderResourceView* null_srv = nullptr;
  ID3D11Buffer* null_buffer = nullptr;
  context->CSSetUnorderedAccessViews(0, 1, &null_uav, nullptr);
  context->CSSetShaderResources(0, 1, &null_srv);
  context->CSSetConstantBuffers(0, 1, &null_buffer);
  context->CSSetConstantBuffers(1, 1, &null_buffer);
  context->CSSetShader(nullptr, nullptr, 0);
  return !NativeError(device_.device()->GetDeviceRemovedReason(), "DX11 texture upload", error);
}

void TextureUpload::Clear() {
  draws_.Invalidate();
  kernels_.clear();
  packed_normalization_.Reset();
}

}  // namespace rex::graphics::d3d11
