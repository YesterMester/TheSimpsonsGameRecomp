#include <rex/ui/d3d11/d3d11_presenter.h>

#include <rex/logging.h>
#include <rex/ui/surface_win.h>
#include <cstring>

namespace rex::ui::d3d11 {
namespace {
namespace shaders {
#include "../shaders/bytecode/d3d12_5_1/guest_output_bilinear_ps.h"
#include "../shaders/bytecode/d3d12_5_1/guest_output_bilinear_dither_ps.h"
#include "../shaders/bytecode/d3d12_5_1/guest_output_ffx_cas_sharpen_ps.h"
#include "../shaders/bytecode/d3d12_5_1/guest_output_ffx_cas_sharpen_dither_ps.h"
#include "../shaders/bytecode/d3d12_5_1/guest_output_ffx_cas_resample_ps.h"
#include "../shaders/bytecode/d3d12_5_1/guest_output_ffx_cas_resample_dither_ps.h"
#include "../shaders/bytecode/d3d12_5_1/guest_output_ffx_fsr_easu_ps.h"
#include "../shaders/bytecode/d3d12_5_1/guest_output_ffx_fsr_rcas_ps.h"
#include "../shaders/bytecode/d3d12_5_1/guest_output_ffx_fsr_rcas_dither_ps.h"
}  // namespace shaders
const std::array<std::span<const uint8_t>, 9> kEffects = {
    shaders::guest_output_bilinear_ps,           shaders::guest_output_bilinear_dither_ps,
    shaders::guest_output_ffx_cas_sharpen_ps,    shaders::guest_output_ffx_cas_sharpen_dither_ps,
    shaders::guest_output_ffx_cas_resample_ps,   shaders::guest_output_ffx_cas_resample_dither_ps,
    shaders::guest_output_ffx_fsr_easu_ps,       shaders::guest_output_ffx_fsr_rcas_ps,
    shaders::guest_output_ffx_fsr_rcas_dither_ps};
}  // namespace
std::unique_ptr<D3D11Presenter> D3D11Presenter::Create(D3D11Device& device,
                                                       HostGpuLossCallback callback) {
  auto presenter = std::unique_ptr<D3D11Presenter>(new D3D11Presenter(device, std::move(callback)));
  std::string error;
  if (!presenter->renderer_.Initialize(error) || !presenter->InitializeCommonSurfaceIndependent()) {
    REXGPU_ERROR("DX11 presenter initialization failed: {}", error);
    return nullptr;
  }
  return presenter;
}
D3D11Presenter::~D3D11Presenter() {
  std::lock_guard lock(device_.context_mutex());
  device_.context()->ClearState();
  device_.WaitForCompletion();
}
Presenter::SurfacePaintConnectResult
D3D11Presenter::ConnectOrReconnectPaintingToSurfaceFromUIThread(Surface& surface, uint32_t width,
                                                                uint32_t height, bool was_paintable,
                                                                bool& implicit_vsync) {
  implicit_vsync = false;
  if (surface.GetType() != Surface::kTypeIndex_Win32Hwnd)
    return SurfacePaintConnectResult::kFailureSurfaceUnusable;
  HWND window = static_cast<Win32HwndSurface&>(surface).hwnd();
  std::lock_guard lock(device_.context_mutex());
  if (was_paintable && swap_chain_ && window_ == window && width_ == width && height_ == height)
    return SurfacePaintConnectResult::kSuccessUnchanged;
  if (swap_chain_ && window_ == window) {
    if (FAILED(swap_chain_->Resize(width, height))) {
      swap_chain_.reset();
      window_ = nullptr;
      return SurfacePaintConnectResult::kFailure;
    }
  } else {
    device_.context()->ClearState();
    swap_chain_.reset();
    std::string error;
    swap_chain_ = D3D11SwapChain::Create(device_, window, width, height, error);
    if (!swap_chain_) {
      REXGPU_ERROR("DX11 presentation connection failed: {}", error);
      window_ = nullptr;
      return SurfacePaintConnectResult::kFailure;
    }
  }
  window_ = window;
  width_ = width;
  height_ = height;
  return SurfacePaintConnectResult::kSuccess;
}
void D3D11Presenter::DisconnectPaintingFromSurfaceFromUIThreadImpl() {
  std::lock_guard lock(device_.context_mutex());
  device_.context()->ClearState();
  swap_chain_.reset();
  window_ = nullptr;
  width_ = height_ = 0;
}
bool D3D11Presenter::PrepareImage(Image& image, uint32_t width, uint32_t height) {
  if (image.texture && image.width == width && image.height == height)
    return true;
  if (!width || !height || width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
      height > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION)
    return false;
  Image next;
  D3D11_TEXTURE2D_DESC desc = {};
  desc.Width = width;
  desc.Height = height;
  desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
  desc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
  desc.BindFlags =
      D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS;
  HRESULT result = device_.device()->CreateTexture2D(&desc, nullptr, next.texture.GetAddressOf());
  if (SUCCEEDED(result))
    result = device_.device()->CreateShaderResourceView(next.texture.Get(), nullptr,
                                                        next.view.GetAddressOf());
  if (SUCCEEDED(result))
    result = device_.device()->CreateRenderTargetView(next.texture.Get(), nullptr,
                                                      next.target.GetAddressOf());
  if (SUCCEEDED(result))
    result = device_.device()->CreateUnorderedAccessView(next.texture.Get(), nullptr,
                                                         next.write.GetAddressOf());
  if (FAILED(result))
    return false;
  next.width = width;
  next.height = height;
  image = std::move(next);
  return true;
}
bool D3D11Presenter::RefreshGuestOutputImpl(
    uint32_t mailbox_index, uint32_t width, uint32_t height,
    std::function<bool(GuestOutputRefreshContext&)> refresher, bool& is_8bpc) {
  std::lock_guard lock(device_.context_mutex());
  if (mailbox_index >= images_.size() || !PrepareImage(images_[mailbox_index], width, height))
    return false;
  RefreshContext context(is_8bpc, images_[mailbox_index].target.Get(),
                         images_[mailbox_index].write.Get());
  // Only the producer-owned mailbox slot is written. Publication to the
  // consumer happens after these commands have been enqueued successfully.
  return refresher(context);
}
Presenter::PaintResult D3D11Presenter::PaintAndPresentImpl(bool execute_ui_drawers) {
  static_assert(kEffects.size() == size_t(GuestOutputPaintEffect::kCount));
  uint32_t index;
  GuestOutputProperties properties;
  GuestOutputPaintConfig config;
  auto consumer_lock = ConsumeGuestOutput(index, &properties, &config);
  std::lock_guard context_lock(device_.context_mutex());
  if (!swap_chain_)
    return PaintResult::kNotPresentedConnectionOutdated;
  HRESULT result = swap_chain_->WaitForFrame();
  if (FAILED(result))
    return FAILED(device_.device()->GetDeviceRemovedReason()) ? PaintResult::kGpuLostResponsible
                                                              : PaintResult::kNotPresented;
  device_.context()->ClearState();
  constexpr float black[4] = {0, 0, 0, 1};
  device_.context()->ClearRenderTargetView(swap_chain_->render_target(), black);
  if (index < images_.size() && images_[index].texture) {
    auto flow =
        GetGuestOutputPaintFlow(properties, width_, height_, D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION,
                                D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION, config);
    for (size_t i = 0; i < flow.effect_count; ++i) {
      auto size = flow.effect_output_sizes[i];
      bool final = i + 1 == flow.effect_count;
      ID3D11RenderTargetView* target;
      int32_t x, y;
      uint32_t target_width, target_height;
      if (final) {
        target = swap_chain_->render_target();
        target_width = width_;
        target_height = height_;
        x = flow.output_x;
        y = flow.output_y;
      } else {
        if (i >= intermediates_.size() || !PrepareImage(intermediates_[i], size.first, size.second))
          return PaintResult::kNotPresented;
        target = intermediates_[i].target.Get();
        target_width = size.first;
        target_height = size.second;
        x = y = 0;
      }
      std::array<uint8_t, 32> parameters = {};
      size_t parameter_size = 0;
      auto set_parameters = [&](const auto& constants) {
        parameter_size = sizeof(constants);
        std::memcpy(parameters.data(), &constants, parameter_size);
      };
      auto effect = flow.effects[i];
      switch (effect) {
        case GuestOutputPaintEffect::kBilinear:
        case GuestOutputPaintEffect::kBilinearDither: {
          BilinearConstants constants;
          constants.Initialize(flow, i);
          set_parameters(constants);
        } break;
        case GuestOutputPaintEffect::kCasSharpen:
        case GuestOutputPaintEffect::kCasSharpenDither: {
          CasSharpenConstants constants;
          constants.Initialize(flow, i, config);
          set_parameters(constants);
        } break;
        case GuestOutputPaintEffect::kCasResample:
        case GuestOutputPaintEffect::kCasResampleDither: {
          CasResampleConstants constants;
          constants.Initialize(flow, i, config);
          set_parameters(constants);
        } break;
        case GuestOutputPaintEffect::kFsrEasu: {
          FsrEasuConstants constants;
          constants.Initialize(flow, i);
          set_parameters(constants);
        } break;
        case GuestOutputPaintEffect::kFsrRcas:
        case GuestOutputPaintEffect::kFsrRcasDither: {
          FsrRcasConstants constants;
          constants.Initialize(flow, i, config);
          set_parameters(constants);
        } break;
        default:
          return PaintResult::kNotPresented;
      }
      auto* source = i ? intermediates_[i - 1].view.Get() : images_[index].view.Get();
      std::string error;
      if (!renderer_.DrawEffect(source, target, target_width, target_height,
                                {x, y, size.first, size.second}, kEffects[size_t(effect)],
                                {parameters.data(), parameter_size}, error)) {
        REXGPU_ERROR("DX11 image presentation failed: {}", error);
        return FAILED(device_.device()->GetDeviceRemovedReason()) ? PaintResult::kGpuLostResponsible
                                                                  : PaintResult::kNotPresented;
      }
    }
  }
  if (execute_ui_drawers) {
    D3D11UIDrawContext context(*this, width_, height_, swap_chain_->render_target());
    ExecuteUIDrawersFromUIThread(context);
  }
  device_.context()->ClearState();
  result = swap_chain_->Present(false);
  if (FAILED(result))
    return FAILED(device_.device()->GetDeviceRemovedReason())
               ? PaintResult::kGpuLostResponsible
               : PaintResult::kNotPresentedConnectionOutdated;
  return result == DXGI_STATUS_OCCLUDED ? PaintResult::kPresentedSuboptimal
                                        : PaintResult::kPresented;
}
bool D3D11Presenter::CaptureGuestOutput(RawImage& image) {
  uint32_t index;
  auto consumer_lock = ConsumeGuestOutput(index, nullptr, nullptr);
  std::lock_guard context_lock(device_.context_mutex());
  if (index >= images_.size() || !images_[index].texture)
    return false;
  auto& source = images_[index];
  D3D11_TEXTURE2D_DESC desc;
  source.texture->GetDesc(&desc);
  desc.Usage = D3D11_USAGE_STAGING;
  desc.BindFlags = desc.MiscFlags = 0;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
  if (FAILED(device_.device()->CreateTexture2D(&desc, nullptr, staging.GetAddressOf())))
    return false;
  device_.context()->ClearState();
  device_.context()->CopyResource(staging.Get(), source.texture.Get());
  D3D11_MAPPED_SUBRESOURCE mapped = {};
  if (FAILED(device_.context()->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
    return false;
  image.width = desc.Width;
  image.height = desc.Height;
  image.stride = desc.Width * 4;
  image.data.resize(image.stride * desc.Height);
  for (uint32_t y = 0; y < desc.Height; ++y) {
    const auto* row = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(mapped.pData) +
                                                        y * mapped.RowPitch);
    auto* output = reinterpret_cast<uint32_t*>(image.data.data() + y * image.stride);
    for (uint32_t x = 0; x < desc.Width; ++x)
      output[x] = Packed10bpcRGBTo8bpcBytes(row[x]);
  }
  device_.context()->Unmap(staging.Get(), 0);
  return true;
}
}  // namespace rex::ui::d3d11
