#include <rex/graphics/d3d11/draw_context.h>

#include <rex/graphics/d3d11/profile.h>

#include <d3d11shader.h>

#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstring>

namespace rex::graphics::d3d11 {

namespace {
using Microsoft::WRL::ComPtr;
constexpr uint32_t kColor = 1, kDepth = 2, kStencil = 4;

using ViewRange = DrawViewRange;
static_assert(DrawViewRange::kColor == kColor);

ViewRange ResourceRange(ID3D11View* view) {
  ViewRange range;
  if (!view)
    return range;
  view->GetResource(range.resource.GetAddressOf());
  D3D11_RESOURCE_DIMENSION dimension;
  range.resource->GetType(&dimension);
  uint32_t flags = 0;
  switch (dimension) {
    case D3D11_RESOURCE_DIMENSION_TEXTURE1D: {
      ComPtr<ID3D11Texture1D> texture;
      range.resource.As(&texture);
      D3D11_TEXTURE1D_DESC desc;
      texture->GetDesc(&desc);
      range.mips = desc.MipLevels;
      range.layers = desc.ArraySize;
      range.width = desc.Width;
      flags = desc.BindFlags;
      break;
    }
    case D3D11_RESOURCE_DIMENSION_TEXTURE2D: {
      ComPtr<ID3D11Texture2D> texture;
      range.resource.As(&texture);
      D3D11_TEXTURE2D_DESC desc;
      texture->GetDesc(&desc);
      range.mips = desc.MipLevels;
      range.layers = desc.ArraySize;
      range.width = desc.Width;
      range.height = desc.Height;
      range.samples = desc.SampleDesc.Count;
      range.quality = desc.SampleDesc.Quality;
      flags = desc.BindFlags;
      break;
    }
    case D3D11_RESOURCE_DIMENSION_TEXTURE3D: {
      ComPtr<ID3D11Texture3D> texture;
      range.resource.As(&texture);
      D3D11_TEXTURE3D_DESC desc;
      texture->GetDesc(&desc);
      range.mips = desc.MipLevels;
      range.width = desc.Width;
      range.height = desc.Height;
      flags = desc.BindFlags;
      // A 3D mip is one subresource, irrespective of the view's W slices.
      break;
    }
    default:
      // Buffer view byte ranges still share the same native subresource.
      break;
  }
  if (flags & D3D11_BIND_DEPTH_STENCIL)
    range.aspects = kDepth | kStencil;
  return range;
}

void Mips(ViewRange& range, uint32_t first, uint32_t count) {
  range.mips = count == UINT32_MAX ? range.mips - first : count;
  range.mip = first;
}

ViewRange ReadRange(ID3D11ShaderResourceView* view) {
  auto range = ResourceRange(view);
  if (!view)
    return range;
  D3D11_SHADER_RESOURCE_VIEW_DESC d;
  view->GetDesc(&d);
  switch (d.ViewDimension) {
    case D3D11_SRV_DIMENSION_TEXTURE1D:
      Mips(range, d.Texture1D.MostDetailedMip, d.Texture1D.MipLevels);
      break;
    case D3D11_SRV_DIMENSION_TEXTURE1DARRAY:
      Mips(range, d.Texture1DArray.MostDetailedMip, d.Texture1DArray.MipLevels);
      range.layer = d.Texture1DArray.FirstArraySlice;
      range.layers = d.Texture1DArray.ArraySize;
      break;
    case D3D11_SRV_DIMENSION_TEXTURE2D:
      Mips(range, d.Texture2D.MostDetailedMip, d.Texture2D.MipLevels);
      break;
    case D3D11_SRV_DIMENSION_TEXTURE2DARRAY:
      Mips(range, d.Texture2DArray.MostDetailedMip, d.Texture2DArray.MipLevels);
      range.layer = d.Texture2DArray.FirstArraySlice;
      range.layers = d.Texture2DArray.ArraySize;
      break;
    case D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY:
      range.layer = d.Texture2DMSArray.FirstArraySlice;
      range.layers = d.Texture2DMSArray.ArraySize;
      break;
    case D3D11_SRV_DIMENSION_TEXTURE3D:
      Mips(range, d.Texture3D.MostDetailedMip, d.Texture3D.MipLevels);
      break;
    case D3D11_SRV_DIMENSION_TEXTURECUBE:
      Mips(range, d.TextureCube.MostDetailedMip, d.TextureCube.MipLevels);
      range.layers = 6;
      break;
    case D3D11_SRV_DIMENSION_TEXTURECUBEARRAY:
      Mips(range, d.TextureCubeArray.MostDetailedMip, d.TextureCubeArray.MipLevels);
      range.layer = d.TextureCubeArray.First2DArrayFace;
      range.layers = d.TextureCubeArray.NumCubes * 6;
      break;
    default:
      break;
  }
  if (range.aspects & kDepth) {
    switch (d.Format) {
      case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
      case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
        range.aspects = kStencil;
        break;
      default:
        range.aspects = kDepth;
        break;
    }
  }
  return range;
}

ViewRange WriteRange(ID3D11RenderTargetView* view) {
  auto range = ResourceRange(view);
  if (!view)
    return range;
  D3D11_RENDER_TARGET_VIEW_DESC d;
  view->GetDesc(&d);
  range.mips = 1;
  switch (d.ViewDimension) {
    case D3D11_RTV_DIMENSION_TEXTURE1D:
      range.mip = d.Texture1D.MipSlice;
      break;
    case D3D11_RTV_DIMENSION_TEXTURE1DARRAY:
      range.mip = d.Texture1DArray.MipSlice;
      range.layer = d.Texture1DArray.FirstArraySlice;
      range.layers = d.Texture1DArray.ArraySize;
      break;
    case D3D11_RTV_DIMENSION_TEXTURE2D:
      range.mip = d.Texture2D.MipSlice;
      break;
    case D3D11_RTV_DIMENSION_TEXTURE2DARRAY:
      range.mip = d.Texture2DArray.MipSlice;
      range.layer = d.Texture2DArray.FirstArraySlice;
      range.layers = d.Texture2DArray.ArraySize;
      break;
    case D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY:
      range.layer = d.Texture2DMSArray.FirstArraySlice;
      range.layers = d.Texture2DMSArray.ArraySize;
      break;
    case D3D11_RTV_DIMENSION_TEXTURE3D:
      range.mip = d.Texture3D.MipSlice;
      break;
    default:
      break;
  }
  return range;
}

ViewRange WriteRange(ID3D11UnorderedAccessView* view) {
  auto range = ResourceRange(view);
  if (!view)
    return range;
  D3D11_UNORDERED_ACCESS_VIEW_DESC d;
  view->GetDesc(&d);
  range.mips = 1;
  switch (d.ViewDimension) {
    case D3D11_UAV_DIMENSION_TEXTURE1D:
      range.mip = d.Texture1D.MipSlice;
      break;
    case D3D11_UAV_DIMENSION_TEXTURE1DARRAY:
      range.mip = d.Texture1DArray.MipSlice;
      range.layer = d.Texture1DArray.FirstArraySlice;
      range.layers = d.Texture1DArray.ArraySize;
      break;
    case D3D11_UAV_DIMENSION_TEXTURE2D:
      range.mip = d.Texture2D.MipSlice;
      break;
    case D3D11_UAV_DIMENSION_TEXTURE2DARRAY:
      range.mip = d.Texture2DArray.MipSlice;
      range.layer = d.Texture2DArray.FirstArraySlice;
      range.layers = d.Texture2DArray.ArraySize;
      break;
    case D3D11_UAV_DIMENSION_TEXTURE3D:
      range.mip = d.Texture3D.MipSlice;
      break;
    default:
      break;
  }
  return range;
}

ViewRange WriteRange(ID3D11DepthStencilView* view) {
  auto range = ResourceRange(view);
  if (!view)
    return range;
  D3D11_DEPTH_STENCIL_VIEW_DESC d;
  view->GetDesc(&d);
  range.mips = 1;
  range.aspects = kDepth;
  if (d.Format == DXGI_FORMAT_D24_UNORM_S8_UINT || d.Format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT)
    range.aspects |= kStencil;
  if (d.Flags & D3D11_DSV_READ_ONLY_DEPTH)
    range.aspects &= ~kDepth;
  if (d.Flags & D3D11_DSV_READ_ONLY_STENCIL)
    range.aspects &= ~kStencil;
  switch (d.ViewDimension) {
    case D3D11_DSV_DIMENSION_TEXTURE1D:
      range.mip = d.Texture1D.MipSlice;
      break;
    case D3D11_DSV_DIMENSION_TEXTURE1DARRAY:
      range.mip = d.Texture1DArray.MipSlice;
      range.layer = d.Texture1DArray.FirstArraySlice;
      range.layers = d.Texture1DArray.ArraySize;
      break;
    case D3D11_DSV_DIMENSION_TEXTURE2D:
      range.mip = d.Texture2D.MipSlice;
      break;
    case D3D11_DSV_DIMENSION_TEXTURE2DARRAY:
      range.mip = d.Texture2DArray.MipSlice;
      range.layer = d.Texture2DArray.FirstArraySlice;
      range.layers = d.Texture2DArray.ArraySize;
      break;
    case D3D11_DSV_DIMENSION_TEXTURE2DMSARRAY:
      range.layer = d.Texture2DMSArray.FirstArraySlice;
      range.layers = d.Texture2DMSArray.ArraySize;
      break;
    default:
      break;
  }
  return range;
}

bool Overlaps(const ViewRange& a, const ViewRange& b) {
  return a.resource && a.resource.Get() == b.resource.Get() && (a.aspects & b.aspects) &&
         uint64_t(a.mip) < uint64_t(b.mip) + b.mips && uint64_t(b.mip) < uint64_t(a.mip) + a.mips &&
         uint64_t(a.layer) < uint64_t(b.layer) + b.layers &&
         uint64_t(b.layer) < uint64_t(a.layer) + a.layers;
}

template <typename Description, typename State, typename Cache, typename Create>
State* GetState(const Description& description, Cache& cache, Create create, uint64_t& creations,
                std::string& error) {
  for (const auto& entry : cache) {
    if (!std::memcmp(&entry.description, &description, sizeof(description)))
      return entry.state.Get();
  }
  typename Cache::value_type entry = {};
  entry.description = description;
  HRESULT result = create(&description, entry.state.GetAddressOf());
  if (FAILED(result)) {
    char message[100];
    std::snprintf(message, sizeof(message), "DX11 fixed-function state creation failed (0x%08X)",
                  unsigned(result));
    error = message;
    return nullptr;
  }
  ++creations;
  auto* state = entry.state.Get();
  cache.push_back(std::move(entry));
  return state;
}

// Slots at and above cached_count are known to be null, so only the slots
// either the old or the new bindings use are compared.
template <typename State, size_t Count, typename Set>
void Bind(std::span<State* const> source, std::array<ComPtr<State>, Count>& cached,
          uint32_t& cached_count, Set set, uint64_t& updates) {
  size_t count = std::min(source.size(), Count);
  size_t limit = std::max(count, size_t(cached_count));
  std::array<State*, Count> values;
  size_t first = Count, last = 0;
  for (size_t i = 0; i < limit; ++i) {
    auto* value = i < count ? source[i] : nullptr;
    values[i] = value;
    if (cached[i].Get() != value) {
      first = std::min(first, i);
      last = i + 1;
      cached[i] = value;
    }
  }
  cached_count = uint32_t(count);
  if (first < last) {
    set(uint32_t(first), uint32_t(last - first), values.data() + first);
    ++updates;
  }
}
}  // namespace

DrawState DrawState::Default(uint32_t width, uint32_t height) {
  DrawState state;
  state.rasterizer.FillMode = D3D11_FILL_SOLID;
  state.rasterizer.CullMode = D3D11_CULL_NONE;
  state.rasterizer.DepthClipEnable = TRUE;
  state.rasterizer.ScissorEnable = TRUE;
  state.rasterizer.MultisampleEnable = TRUE;
  state.depth_stencil.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
  state.depth_stencil.DepthFunc = D3D11_COMPARISON_ALWAYS;
  state.depth_stencil.StencilReadMask = state.depth_stencil.StencilWriteMask = 0xFF;
  state.depth_stencil.FrontFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
  state.depth_stencil.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
  state.depth_stencil.FrontFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
  state.depth_stencil.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
  state.depth_stencil.BackFace = state.depth_stencil.FrontFace;
  state.blend.IndependentBlendEnable = TRUE;
  for (auto& rt : state.blend.RenderTarget) {
    rt.SrcBlend = rt.SrcBlendAlpha = D3D11_BLEND_ONE;
    rt.DestBlend = rt.DestBlendAlpha = D3D11_BLEND_ZERO;
    rt.BlendOp = rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  }
  state.viewport = {0, 0, float(width), float(height), 0, 1};
  state.scissor = {0, 0, LONG(width), LONG(height)};
  return state;
}

void DrawContext::SetResources(DrawStage stage, uint32_t first, uint32_t count,
                               ID3D11ShaderResourceView* const* views) {
  auto* context = device_.context();
  switch (stage) {
    case DrawStage::kVertex:
      context->VSSetShaderResources(first, count, views);
      break;
    case DrawStage::kHull:
      context->HSSetShaderResources(first, count, views);
      break;
    case DrawStage::kDomain:
      context->DSSetShaderResources(first, count, views);
      break;
    case DrawStage::kGeometry:
      context->GSSetShaderResources(first, count, views);
      break;
    case DrawStage::kPixel:
      context->PSSetShaderResources(first, count, views);
      break;
    default:
      break;
  }
}

void DrawContext::BindStage(DrawStage stage, const StageBindings& source) {
  auto& cached = bindings_[size_t(stage)];
  auto* context = device_.context();
  Bind(
      source.resources, cached.resources, cached.resource_count,
      [&](uint32_t first, uint32_t count, auto* values) {
        SetResources(stage, first, count, values);
      },
      statistics_.binding_updates);
  Bind(
      source.constants, cached.constants, cached.constant_count,
      [&](uint32_t first, uint32_t count, auto* values) {
        switch (stage) {
          case DrawStage::kVertex:
            context->VSSetConstantBuffers(first, count, values);
            break;
          case DrawStage::kHull:
            context->HSSetConstantBuffers(first, count, values);
            break;
          case DrawStage::kDomain:
            context->DSSetConstantBuffers(first, count, values);
            break;
          case DrawStage::kGeometry:
            context->GSSetConstantBuffers(first, count, values);
            break;
          case DrawStage::kPixel:
            context->PSSetConstantBuffers(first, count, values);
            break;
          default:
            break;
        }
      },
      statistics_.binding_updates);
  Bind(
      source.samplers, cached.samplers, cached.sampler_count,
      [&](uint32_t first, uint32_t count, auto* values) {
        switch (stage) {
          case DrawStage::kVertex:
            context->VSSetSamplers(first, count, values);
            break;
          case DrawStage::kHull:
            context->HSSetSamplers(first, count, values);
            break;
          case DrawStage::kDomain:
            context->DSSetSamplers(first, count, values);
            break;
          case DrawStage::kGeometry:
            context->GSSetSamplers(first, count, values);
            break;
          case DrawStage::kPixel:
            context->PSSetSamplers(first, count, values);
            break;
          default:
            break;
        }
      },
      statistics_.binding_updates);
}

bool DrawContext::OutputsMatch(const DrawCommand& command) const {
  if (!outputs_.valid || outputs_.render_target_count != command.render_targets.size() ||
      outputs_.depth_stencil.Get() != command.depth_stencil ||
      outputs_.unordered_access_count != command.unordered_access.size())
    return false;
  for (size_t i = 0; i < command.render_targets.size(); ++i)
    if (outputs_.render_targets[i].Get() != command.render_targets[i])
      return false;
  for (size_t i = 0; i < command.unordered_access.size(); ++i)
    if (outputs_.unordered_access[i].Get() != command.unordered_access[i])
      return false;
  return true;
}

void DrawContext::ReleaseOutputs() {
  std::array<ID3D11UnorderedAccessView*, D3D11_1_UAV_SLOT_COUNT> null_views = {};
  uint32_t count = device_.features().level >= D3D_FEATURE_LEVEL_11_1
                       ? D3D11_1_UAV_SLOT_COUNT
                       : D3D11_PS_CS_UAV_REGISTER_COUNT;
  device_.context()->OMSetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, count,
                                                               null_views.data(), nullptr);
}

ID3D11GeometryShader* DrawContext::DiscardShader(const ShaderProgram& source, std::string& error) {
  const auto& bytes = source.bytecode().data;
  for (const auto& cached : discard_shaders_)
    if (cached.source == bytes) {
      error = cached.error;
      return cached.shader.Get();
    }
  CachedDiscardShader result;
  result.source = bytes;
  // The prior stage's bytecode supplies the passthrough output signature when
  // there is no guest geometry shader. Disabling the rasterized stream keeps
  // vertex work and memexport running without sending invalid positions to
  // the scan converter.
  // A passthrough shader needs a real output in its stream declaration, even
  // with no buffers bound. Reflect one existing component so this works with
  // vertex, domain and geometry signatures without inventing a semantic.
  ComPtr<ID3D11ShaderReflection> reflection;
  HRESULT reflected = D3DReflect(bytes.data(), bytes.size(), __uuidof(ID3D11ShaderReflection),
                                 reinterpret_cast<void**>(reflection.GetAddressOf()));
  D3D11_SHADER_DESC description = {};
  D3D11_SIGNATURE_PARAMETER_DESC output = {};
  bool found_output = false;
  if (SUCCEEDED(reflected) && SUCCEEDED(reflection->GetDesc(&description))) {
    for (UINT i = 0; i < description.OutputParameters; ++i) {
      if (SUCCEEDED(reflection->GetOutputParameterDesc(i, &output)) && output.SemanticName &&
          (output.Mask & 15)) {
        found_output = true;
        break;
      }
    }
  }
  if (!found_output) {
    result.error = "The native rasterization-discard shader has no output signature";
  } else {
    const D3D11_SO_DECLARATION_ENTRY discard_entry = {
        output.Stream,
        output.SemanticName,
        output.SemanticIndex,
        BYTE(std::countr_zero(unsigned(output.Mask & 15))),
        1,
        0};
    const UINT discard_stride = sizeof(uint32_t);
    HRESULT created = device_.device()->CreateGeometryShaderWithStreamOutput(
        bytes.data(), bytes.size(), &discard_entry, 1, &discard_stride, 1,
        D3D11_SO_NO_RASTERIZED_STREAM, nullptr, result.shader.GetAddressOf());
    if (FAILED(created)) {
      char message[128];
      std::snprintf(message, sizeof(message),
                    "Unable to create the native rasterization-discard shader (0x%08X)",
                    unsigned(created));
      result.error = message;
    }
  }
  if (discard_shaders_.size() == 64)
    discard_shaders_.erase(discard_shaders_.begin());
  discard_shaders_.push_back(std::move(result));
  error = discard_shaders_.back().error;
  return discard_shaders_.back().shader.Get();
}

bool DrawContext::Draw(const DrawCommand& command, std::string& error) {
  error.clear();
  auto reject = [&](const char* message) {
    error = message;
    ++statistics_.rejected_draws;
    return false;
  };
  constexpr size_t vertex = size_t(DrawStage::kVertex), hull = size_t(DrawStage::kHull),
                   domain = size_t(DrawStage::kDomain), geometry = size_t(DrawStage::kGeometry),
                   pixel = size_t(DrawStage::kPixel);
  uint32_t uav_count = device_.features().level >= D3D_FEATURE_LEVEL_11_1
                           ? D3D11_1_UAV_SLOT_COUNT
                           : D3D11_PS_CS_UAV_REGISTER_COUNT;
  if (!command.programs[vertex] || !command.programs[vertex]->vertex() ||
      (command.programs[hull] && !command.programs[hull]->hull()) ||
      (command.programs[domain] && !command.programs[domain]->domain()) ||
      (bool(command.programs[hull]) != bool(command.programs[domain])) ||
      (command.programs[geometry] && !command.programs[geometry]->geometry()) ||
      (command.programs[pixel] && !command.programs[pixel]->pixel()))
    return reject("The DX11 draw programs do not match their native stages");
  if (command.topology == D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED || command.render_targets.size() > 4 ||
      command.unordered_access.size() > uav_count - 4)
    return reject("The DX11 draw topology or output register range is invalid");
  for (const auto& stage : command.bindings) {
    if (stage.constants.size() > D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT ||
        stage.resources.size() > D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT ||
        stage.samplers.size() > D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT)
      return reject("The DX11 draw input register range is invalid");
  }
  if (command.indices) {
    auto kind = command.indices->kind();
    if (kind != BufferKind::kIndex16 && kind != BufferKind::kIndex32)
      return reject("The DX11 draw index buffer has the wrong native type");
    if ((uint64_t(command.first) + command.count) * (kind == BufferKind::kIndex16 ? 2 : 4) >
        command.indices->size())
      return reject("The DX11 draw exceeds its retained native index buffer");
  } else if (command.base_vertex) {
    return reject("The DX11 non-indexed draw has an index-only base vertex");
  }
  auto& writes = writes_;
  writes.clear();
  for (auto* view : command.render_targets)
    if (view)
      writes.push_back(WriteRange(view));
  if (command.depth_stencil)
    writes.push_back(WriteRange(command.depth_stencil));
  size_t attachment_count = writes.size();
  for (auto* view : command.unordered_access)
    if (view)
      writes.push_back(WriteRange(view));
  // Render targets and depth come first among the writes.
  std::span<const ViewRange> attachments(writes.data(), attachment_count);
  for (size_t i = 1; i < attachments.size(); ++i) {
    const auto& a = attachments[0];
    const auto& b = attachments[i];
    if (std::max(1u, a.width >> a.mip) != std::max(1u, b.width >> b.mip) ||
        std::max(1u, a.height >> a.mip) != std::max(1u, b.height >> b.mip) ||
        a.layers != b.layers || a.samples != b.samples || a.quality != b.quality)
      return reject("The DX11 draw output dimensions or sample counts do not match");
  }
  for (size_t i = 0; i < writes.size(); ++i) {
    for (size_t j = 0; j < i; ++j) {
      if (Overlaps(writes[i], writes[j]))
        return reject("The DX11 draw writes overlapping native output subresources");
    }
  }
  for (const auto& stage : command.bindings) {
    for (auto* view : stage.resources) {
      if (!view)
        continue;
      auto read = ReadRange(view);
      for (const auto& write : writes) {
        if (Overlaps(read, write))
          return reject("The DX11 draw reads and writes the same native subresource");
      }
    }
  }
  auto* native = device_.device();
  auto* rasterizer = GetState<D3D11_RASTERIZER_DESC, ID3D11RasterizerState>(
      command.state.rasterizer, rasterizers_,
      [&](auto* desc, auto** output) { return native->CreateRasterizerState(desc, output); },
      statistics_.state_creations, error);
  auto* depth = GetState<D3D11_DEPTH_STENCIL_DESC, ID3D11DepthStencilState>(
      command.state.depth_stencil, depths_,
      [&](auto* desc, auto** output) { return native->CreateDepthStencilState(desc, output); },
      statistics_.state_creations, error);
  auto* blend = GetState<D3D11_BLEND_DESC, ID3D11BlendState>(
      command.state.blend, blends_,
      [&](auto* desc, auto** output) { return native->CreateBlendState(desc, output); },
      statistics_.state_creations, error);
  if (!rasterizer || !depth || !blend) {
    ++statistics_.rejected_draws;
    return false;
  }
  ID3D11GeometryShader* geometry_shader =
      command.programs[geometry] ? command.programs[geometry]->geometry() : nullptr;
  if (command.discard_rasterization) {
    auto* signature = command.programs[geometry] ? command.programs[geometry]
                      : command.programs[domain] ? command.programs[domain]
                                                 : command.programs[vertex];
    geometry_shader = DiscardShader(*signature, error);
    if (!geometry_shader) {
      ++statistics_.rejected_draws;
      return false;
    }
  }
  auto* context = device_.context();
  // Another thread (presentation, UI or a screenshot) may have used the
  // shared context since the previous draw.
  if (!initialized_ || context_epoch_ != device_.context_mutex().epoch()) {
    Invalidate();
    context_epoch_ = device_.context_mutex().epoch();
  }
  // Unchanged outputs need no transition: the reads were checked against
  // exactly these writes above.
  if (!OutputsMatch(command)) {
    // Remove old writes before binding a texture that was the previous output.
    ReleaseOutputs();
    // Explicitly remove cached reads before an output transition. Relying on
    // the runtime's automatic unbinding would leave the CPU cache claiming the
    // SRV is still bound when a later pass returns to exactly the same view.
    for (size_t stage = 0; stage < bindings_.size(); ++stage) {
      for (uint32_t slot = 0; slot < bindings_[stage].resource_count; ++slot) {
        auto& view = bindings_[stage].resources[slot];
        if (!view)
          continue;
        auto read = ReadRange(view.Get());
        bool conflicts = std::any_of(writes.begin(), writes.end(),
                                     [&](const auto& write) { return Overlaps(read, write); });
        if (conflicts) {
          ID3D11ShaderResourceView* null_view = nullptr;
          SetResources(DrawStage(stage), slot, 1, &null_view);
          view.Reset();
          ++statistics_.explicit_resource_unbinds;
        }
      }
    }
    std::array<ID3D11RenderTargetView*, 4> targets = {};
    std::copy(command.render_targets.begin(), command.render_targets.end(), targets.begin());
    context->OMSetRenderTargetsAndUnorderedAccessViews(
        command.unordered_access.empty() ? uint32_t(command.render_targets.size()) : 4,
        targets.data(), command.depth_stencil, 4, uint32_t(command.unordered_access.size()),
        command.unordered_access.data(), nullptr);
    outputs_ = {};
    for (size_t i = 0; i < command.render_targets.size(); ++i)
      outputs_.render_targets[i] = command.render_targets[i];
    outputs_.render_target_count = uint32_t(command.render_targets.size());
    outputs_.depth_stencil = command.depth_stencil;
    for (size_t i = 0; i < command.unordered_access.size(); ++i)
      outputs_.unordered_access[i] = command.unordered_access[i];
    outputs_.unordered_access_count = uint32_t(command.unordered_access.size());
    outputs_.valid = true;
    ++statistics_.output_changes;
    g_profile.output_changes.fetch_add(1, std::memory_order_relaxed);
  }
  context->VSSetShader(command.programs[vertex]->vertex(), nullptr, 0);
  context->HSSetShader(command.programs[hull] ? command.programs[hull]->hull() : nullptr, nullptr,
                       0);
  context->DSSetShader(command.programs[domain] ? command.programs[domain]->domain() : nullptr,
                       nullptr, 0);
  context->GSSetShader(geometry_shader, nullptr, 0);
  context->PSSetShader(command.programs[pixel] ? command.programs[pixel]->pixel() : nullptr,
                       nullptr, 0);
  for (size_t stage = 0; stage < bindings_.size(); ++stage)
    BindStage(DrawStage(stage), command.bindings[stage]);
  context->RSSetState(rasterizer);
  context->RSSetViewports(1, &command.state.viewport);
  context->RSSetScissorRects(1, &command.state.scissor);
  context->OMSetDepthStencilState(depth, command.state.stencil_reference);
  context->OMSetBlendState(blend, command.state.blend_factor.data(), command.state.sample_mask);
  context->IASetInputLayout(nullptr);
  context->IASetPrimitiveTopology(command.topology);
  if (command.indices) {
    context->IASetIndexBuffer(command.indices->buffer(),
                              command.indices->kind() == BufferKind::kIndex16
                                  ? DXGI_FORMAT_R16_UINT
                                  : DXGI_FORMAT_R32_UINT,
                              0);
    context->DrawIndexed(command.count, command.first, command.base_vertex);
    ++statistics_.indexed_draws;
  } else {
    context->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
    context->Draw(command.count, command.first);
  }
  ++statistics_.draws;
  return true;
}

void DrawContext::Invalidate() {
  g_profile.context_invalidations.fetch_add(1, std::memory_order_relaxed);
  device_.context()->ClearState();
  bindings_ = {};
  outputs_ = {};
  initialized_ = true;
}

void DrawContext::Clear() {
  Invalidate();
  rasterizers_.clear();
  depths_.clear();
  blends_.clear();
  discard_shaders_.clear();
  statistics_ = {};
}

}  // namespace rex::graphics::d3d11
