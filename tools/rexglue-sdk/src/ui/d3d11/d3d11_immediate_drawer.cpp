#include <rex/ui/d3d11/d3d11_immediate_drawer.h>

#include <rex/logging.h>
#include <rex/ui/d3d11/d3d11_presenter.h>

namespace rex::ui::d3d11 {
std::unique_ptr<D3D11ImmediateDrawer> D3D11ImmediateDrawer::Create(D3D11Device& device) {
  auto drawer = std::unique_ptr<D3D11ImmediateDrawer>(new D3D11ImmediateDrawer(device));
  std::string error;
  if (!drawer->renderer_.Initialize(error)) {
    REXGPU_ERROR("DX11 UI initialization failed: {}", error);
    return nullptr;
  }
  constexpr uint8_t white[4] = {255, 255, 255, 255};
  drawer->white_ = drawer->CreateTexture(1, 1, ImmediateTextureFilter::kNearest, false, white);
  return drawer->white_ ? std::move(drawer) : nullptr;
}
std::unique_ptr<ImmediateTexture> D3D11ImmediateDrawer::CreateTexture(uint32_t width,
                                                                      uint32_t height,
                                                                      ImmediateTextureFilter filter,
                                                                      bool repeated,
                                                                      const uint8_t* data) {
  if (!width || !height || width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
      height > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION)
    return nullptr;
  auto texture = std::make_unique<Texture>(width, height);
  D3D11_TEXTURE2D_DESC desc = {};
  desc.Width = width;
  desc.Height = height;
  desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
  desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  desc.Usage = data ? D3D11_USAGE_IMMUTABLE : D3D11_USAGE_DEFAULT;
  D3D11_SUBRESOURCE_DATA initial = {data, width * 4, 0};
  HRESULT result = device_.device()->CreateTexture2D(&desc, data ? &initial : nullptr,
                                                     texture->resource.GetAddressOf());
  if (SUCCEEDED(result))
    result = device_.device()->CreateShaderResourceView(texture->resource.Get(), nullptr,
                                                        texture->view.GetAddressOf());
  D3D11_SAMPLER_DESC sampler = {};
  sampler.Filter = filter == ImmediateTextureFilter::kNearest
                       ? D3D11_FILTER_MIN_MAG_MIP_POINT
                       : D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
  sampler.AddressU = sampler.AddressV = sampler.AddressW =
      repeated ? D3D11_TEXTURE_ADDRESS_WRAP : D3D11_TEXTURE_ADDRESS_CLAMP;
  sampler.ComparisonFunc = D3D11_COMPARISON_NEVER;
  if (SUCCEEDED(result))
    result = device_.device()->CreateSamplerState(&sampler, texture->sampler.GetAddressOf());
  return SUCCEEDED(result) ? std::move(texture) : nullptr;
}
void D3D11ImmediateDrawer::Begin(UIDrawContext& context, float width, float height) {
  context_lock_ = std::unique_lock(device_.context_mutex());
  ImmediateDrawer::Begin(context, width, height);
}
void D3D11ImmediateDrawer::BeginDrawBatch(const ImmediateDrawBatch& batch) {
  EndDrawBatch();
  if (!ui_draw_context() || !batch.vertices || batch.vertex_count <= 0 ||
      batch.vertex_count > 3355443 || batch.index_count < 0 || batch.index_count > 33554432 ||
      (batch.index_count && !batch.indices))
    return;
  std::string error;
  vertices_ = buffers_.GetOrCreate({reinterpret_cast<const uint8_t*>(batch.vertices),
                                    size_t(batch.vertex_count) * sizeof(ImmediateVertex)},
                                   graphics::d3d11::BufferKind::kRaw, error);
  if (!vertices_) {
    REXGPU_ERROR("DX11 UI vertices: {}", error);
    return;
  }
  vertex_count_ = uint32_t(batch.vertex_count);
  if (batch.index_count) {
    index_data_.assign(batch.indices, batch.indices + batch.index_count);
    indices_ = buffers_.GetOrCreate({reinterpret_cast<const uint8_t*>(batch.indices),
                                     size_t(batch.index_count) * sizeof(uint16_t)},
                                    graphics::d3d11::BufferKind::kIndex16, error);
    if (!indices_) {
      REXGPU_ERROR("DX11 UI indices: {}", error);
      EndDrawBatch();
    }
  }
}
void D3D11ImmediateDrawer::Draw(const ImmediateDraw& draw) {
  if (!ui_draw_context() || !vertices_ || draw.count <= 0 || draw.index_offset < 0)
    return;
  if (indices_) {
    if (uint32_t(draw.index_offset) > index_data_.size() ||
        uint32_t(draw.count) > index_data_.size() - uint32_t(draw.index_offset))
      return;
    for (uint32_t i = 0; i < uint32_t(draw.count); ++i) {
      int64_t index = int64_t(draw.base_vertex) + index_data_[uint32_t(draw.index_offset) + i];
      if (index < 0 || index >= vertex_count_)
        return;
    }
  } else if (draw.base_vertex < 0 || uint32_t(draw.base_vertex) > vertex_count_ ||
             uint32_t(draw.count) > vertex_count_ - uint32_t(draw.base_vertex))
    return;
  uint32_t x, y, width, height;
  if (!ScissorToRenderTarget(draw, x, y, width, height))
    return;
  auto* texture = static_cast<Texture*>(draw.texture ? draw.texture : white_.get());
  auto& context = static_cast<D3D11UIDrawContext&>(*ui_draw_context());
  std::string error;
  if (!renderer_.DrawBatch(
          vertices_->raw_view(), indices_ ? indices_->buffer() : nullptr, texture->view.Get(),
          texture->sampler.Get(), context.render_target(), context.render_target_width(),
          context.render_target_height(), coordinate_space_width(), coordinate_space_height(),
          {LONG(x), LONG(y), LONG(x + width), LONG(y + height)},
          draw.primitive_type == ImmediatePrimitiveType::kLines
              ? D3D11_PRIMITIVE_TOPOLOGY_LINELIST
              : D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST,
          uint32_t(draw.count), uint32_t(indices_ ? draw.index_offset : draw.base_vertex),
          indices_ ? draw.base_vertex : 0, error))
    REXGPU_ERROR("DX11 UI draw: {}", error);
}
void D3D11ImmediateDrawer::EndDrawBatch() {
  vertices_.reset();
  indices_.reset();
  index_data_.clear();
  vertex_count_ = 0;
}
void D3D11ImmediateDrawer::End() {
  EndDrawBatch();
  device_.context()->ClearState();
  ImmediateDrawer::End();
  context_lock_.unlock();
}
}  // namespace rex::ui::d3d11
