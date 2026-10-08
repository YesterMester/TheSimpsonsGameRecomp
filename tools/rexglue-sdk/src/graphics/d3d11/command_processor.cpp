#include <rex/graphics/d3d11/command_processor.h>

#include <rex/cvar.h>
#include <rex/graphics/d3d11/graphics_system.h>
#include <rex/graphics/d3d11/profile.h>
#include <rex/logging.h>
#include <rex/kernel/xboxkrnl/video.h>
#include <rex/memory/utils.h>
#include <rex/ui/d3d11/d3d11_presenter.h>

REXCVAR_DECLARE(int32_t, gpu_wait_stats);
REXCVAR_DECLARE(bool, native_2x_msaa);
REXCVAR_DECLARE(bool, native_stencil_value_output);
REXCVAR_DECLARE(bool, native_stencil_value_output_d3d12_intel);
REXCVAR_DECLARE(bool, depth_float24_convert_in_pixel_shader);
REXCVAR_DECLARE(bool, depth_float24_round);

namespace rex::graphics::d3d11 {
D3D11CommandProcessor::D3D11CommandProcessor(D3D11GraphicsSystem* graphics,
                                             system::KernelState* kernel)
    : CommandProcessor(graphics, kernel) {}
ui::d3d11::D3D11Device& D3D11CommandProcessor::Device() const {
  return static_cast<ui::d3d11::D3D11Provider*>(graphics_system_->provider())->device();
}
bool D3D11CommandProcessor::Fail(std::string error) {
  if (failure_.empty()) {
    failure_ = std::move(error);
    REXGPU_ERROR("Native DX11 rendering stopped: {}", failure_);
  }
  return false;
}
bool D3D11CommandProcessor::SetupContext() {
  std::lock_guard lock(Device().context_mutex());
  if (!CommandProcessor::SetupContext())
    return false;
  uint32_t scale_x, scale_y;
  TextureCache::GetConfigDrawResolutionScale(scale_x, scale_y);
  draws_ = std::make_unique<DrawContext>(Device());
  shared_memory_ = std::make_unique<D3D11SharedMemory>(Device(), *draws_, *memory_, trace_writer_);
  std::string error;
  if (!shared_memory_->Initialize(error))
    return Fail(error);
  targets_ = std::make_unique<D3D11RenderTargetCache>(*register_file_, *memory_, &trace_writer_,
                                                      Device(), *draws_, scale_x, scale_y);
  // The same options as the Direct3D 12 and Vulkan host render targets, except
  // that gamma targets always use the 16-bit storage qualified for DX11.
  D3D11RenderTargetCache::Options target_options;
  target_options.native_2x_msaa = REXCVAR_GET(native_2x_msaa);
  // The cache also requires pixel shader stencil reference support. Intel
  // drivers have mishandled this output on Direct3D 12.
  target_options.stencil_reference_output = REXCVAR_GET(native_stencil_value_output) &&
                                            (REXCVAR_GET(native_stencil_value_output_d3d12_intel) ||
                                             Device().features().vendor_id != 0x8086);
  target_options.depth_float24_convert_in_pixel_shader =
      REXCVAR_GET(depth_float24_convert_in_pixel_shader);
  target_options.depth_float24_round = REXCVAR_GET(depth_float24_round);
  if (!targets_->Initialize(target_options))
    return Fail(targets_->last_error());
  textures_ = std::make_unique<D3D11TextureCache>(*register_file_, *shared_memory_, Device(),
                                                  *draws_, scale_x, scale_y);
  primitives_ = std::make_unique<D3D11PrimitiveProcessor>(*register_file_, *memory_, trace_writer_,
                                                          *shared_memory_, Device());
  if (!primitives_->Initialize())
    return Fail("Unable to initialize native primitives");
  PipelineCache::Options options;
  options.resolution_scale_x = scale_x;
  options.resolution_scale_y = scale_y;
  options.msaa_2x_supported = targets_->msaa_2x_supported();
  options.depth_float24_convert_in_pixel_shader = targets_->depth_float24_convert_in_pixel_shader();
  options.depth_float24_round = targets_->depth_float24_round();
  pipelines_ = std::make_unique<PipelineCache>(Device(), *register_file_, options);
  guest_draw_ = std::make_unique<GuestDraw>(*register_file_, Device(), *draws_, *shared_memory_,
                                            *textures_, *targets_, *primitives_, *pipelines_);
  output_ = std::make_unique<ui::d3d11::D3D11ImageRenderer>(Device());
  if (!output_->Initialize(error))
    return Fail(error);
  gamma_buffers_ = std::make_unique<BufferCache>(Device(), 65536);
  gpu_profiler_ = std::make_unique<GpuProfiler>(Device());
  g_gpu_profiler = gpu_profiler_.get();
  SetProfileEnabled(REXCVAR_GET(gpu_wait_stats) > 0);
  textures_->BeginFrame();
  return true;
}
void D3D11CommandProcessor::ShutdownContext() {
  std::lock_guard lock(Device().context_mutex());
  Device().context()->ClearState();
  Device().WaitForCompletion();
  g_gpu_profiler = nullptr;
  gpu_profiler_.reset();
  guest_draw_.reset();
  output_.reset();
  fxaa_write_.Reset();
  fxaa_read_.Reset();
  fxaa_image_.Reset();
  gamma_buffers_.reset();
  active_vertex_shader_ = active_pixel_shader_ = nullptr;
  pipelines_.reset();
  primitives_.reset();
  textures_.reset();
  targets_.reset();
  if (shared_memory_)
    shared_memory_->Shutdown();
  shared_memory_.reset();
  draws_.reset();
  CommandProcessor::ShutdownContext();
}
void D3D11CommandProcessor::ClearCaches() {
  std::lock_guard lock(Device().context_mutex());
  if (!draws_)
    return;
  draws_->Clear();
  guest_draw_->ClearCache();
  primitives_->ClearCache();
  textures_->ClearCache();
  targets_->ClearCache();
  active_vertex_shader_ = active_pixel_shader_ = nullptr;
  pipelines_->Clear();
  gamma_buffers_->Clear();
  CommandProcessor::ClearCaches();
}
void D3D11CommandProcessor::InvalidateGpuMemory() {
  std::lock_guard lock(Device().context_mutex());
  if (shared_memory_)
    shared_memory_->InvalidateAllPages();
}
void D3D11CommandProcessor::WriteRegister(uint32_t index, uint32_t value) {
  CommandProcessor::WriteRegister(index, value);
  if (textures_ && index >= XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 &&
      index <= XE_GPU_REG_SHADER_CONSTANT_FETCH_31_5)
    textures_->TextureFetchConstantWritten((index - XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0) / 6);
}
void D3D11CommandProcessor::WriteRegistersFromMem(uint32_t start_index, uint32_t* base,
                                                  uint32_t num_registers) {
  if (!num_registers)
    return;
  uint32_t end_index = start_index + num_registers - 1;
  // Constants and context registers have no write side effects in the base
  // WriteRegister, whose special cases are all below 0x2000. Guest draws read
  // the register file directly, so a range is a plain copy, as on Vulkan.
  // Texture bindings are told about changed fetch constants.
  constexpr uint32_t kFirstContextRegister = 0x2000;
  if (start_index < kFirstContextRegister || end_index > XE_GPU_REG_SHADER_CONSTANT_LOOP_31) {
    CommandProcessor::WriteRegistersFromMem(start_index, base, num_registers);
    return;
  }
  memory::copy_and_swap(register_file_->values + start_index, base, num_registers);
  if (textures_ && start_index <= XE_GPU_REG_SHADER_CONSTANT_FETCH_31_5 &&
      end_index >= XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0) {
    uint32_t first = std::max(start_index, uint32_t(XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0));
    uint32_t last = std::min(end_index, uint32_t(XE_GPU_REG_SHADER_CONSTANT_FETCH_31_5));
    textures_->TextureFetchConstantsWritten((first - XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0) / 6,
                                            (last - XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0) / 6);
  }
}
Shader* D3D11CommandProcessor::LoadShader(xenos::ShaderType type, uint32_t address,
                                          const uint32_t* words, uint32_t count) {
  if (!pipelines_ || !failure_.empty())
    return nullptr;
  return pipelines_->LoadShader(type, {words, count});
}
bool D3D11CommandProcessor::IssueDraw(xenos::PrimitiveType type, uint32_t count,
                                      IndexBufferInfo* indices, bool explicit_mode) {
  if (!failure_.empty())
    return false;
  if (register_file_->Get<reg::RB_MODECONTROL>().edram_mode == xenos::EdramMode::kCopy)
    return IssueCopy();
  ProfileClock clock;
  // The draw context detects use of the shared context by other threads, and
  // work on this thread that changes bindings invalidates it explicitly.
  std::lock_guard lock(Device().context_mutex());
  g_profile.draws.fetch_add(1, std::memory_order_relaxed);
  GpuSwitch(GpuCategory::kDraws);
  clock.Mark(ProfilePhase::kDrawContext);
  std::string error;
  return guest_draw_->Submit(static_cast<D3D11Shader*>(active_vertex_shader_),
                             static_cast<D3D11Shader*>(active_pixel_shader_), error) ||
         Fail(error);
}
bool D3D11CommandProcessor::IssueCopy() {
  if (!failure_.empty())
    return false;
  std::lock_guard lock(Device().context_mutex());
  ProfileClock clock;
  draws_->Invalidate();
  g_profile.resolves.fetch_add(1, std::memory_order_relaxed);
  GpuSwitch(GpuCategory::kResolves);
  uint32_t address, length;
  bool resolved =
      targets_->Resolve(*shared_memory_, *textures_, *memory_, trace_writer_, address, length);
  clock.Mark(ProfilePhase::kResolve);
  return resolved || Fail(targets_->last_error());
}
void D3D11CommandProcessor::OnPrimaryBufferEnd() {
  std::lock_guard lock(Device().context_mutex());
  Device().context()->Flush();
}
void D3D11CommandProcessor::TracePlaybackWroteMemory(uint32_t address, uint32_t length) {
  if (!shared_memory_)
    return;
  shared_memory_->MemoryInvalidationCallback(address, length, true);
  primitives_->MemoryInvalidationCallback(address, length, true);
}
void D3D11CommandProcessor::RestoreEdramSnapshot(const void* snapshot) {
  Fail("Native DX11 EDRAM snapshot restoration is not connected");
}
void D3D11CommandProcessor::IssueSwap(uint32_t pointer, uint32_t width, uint32_t height) {
  if (!failure_.empty() || !graphics_system_->presenter())
    return;
  ProfileClock clock;
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> source, gamma;
  std::shared_ptr<const BufferVersion> gamma_version;
  xenos::TextureFormat format;
  uint32_t swizzle, source_width, source_height, gamma_mode;
  bool scaled;
  std::string error;
  {
    std::lock_guard lock(Device().context_mutex());
    draws_->Invalidate();
    GpuSwitch(GpuCategory::kSwap);
    source = textures_->RequestSwapTexture(format, swizzle, source_width, source_height, scaled);
    if (!source) {
      Fail(textures_->last_error());
      return;
    }
    width =
        width ? std::min(width * (scaled ? textures_->draw_resolution_scale_x() : 1), source_width)
              : source_width;
    height = height ? std::min(height * (scaled ? textures_->draw_resolution_scale_y() : 1),
                               source_height)
                    : source_height;
    gamma_mode = (format == xenos::TextureFormat::k_2_10_10_10 ||
                  format == xenos::TextureFormat::k_2_10_10_10_AS_16_16_16_16)
                     ? 2
                     : 1;
    auto* data = gamma_mode == 2 ? reinterpret_cast<const uint8_t*>(gamma_ramp_pwl_rgb())
                                 : reinterpret_cast<const uint8_t*>(gamma_ramp_256_entry_table());
    gamma_version = gamma_buffers_->GetOrCreate({data, gamma_mode == 2 ? 128 * 3 * 4u : 256 * 4u},
                                                BufferKind::kRaw, error);
    if (!gamma_version) {
      Fail(error);
      return;
    }
    gamma = gamma_version->raw_view();
  }
  // The presenter owns mailbox publication. Never take its producer/consumer
  // lock while already holding the device lock: UI painting uses that order.
  system::X_VIDEO_MODE video_mode;
  kernel::xboxkrnl::VdQueryVideoMode(&video_mode);
  uint32_t display_width = std::max(1u, uint32_t(video_mode.display_width));
  uint32_t display_height = std::max(1u, uint32_t(video_mode.display_height));
  bool refreshed = graphics_system_->presenter()->RefreshGuestOutput(
      width, height, display_width, display_height,
      [&](ui::Presenter::GuestOutputRefreshContext& base) {
        std::lock_guard lock(Device().context_mutex());
        auto& context = static_cast<ui::d3d11::D3D11Presenter::RefreshContext&>(base);
        auto effect = GetActualSwapPostEffect();
        bool fxaa = effect == SwapPostEffect::kFxaa || effect == SwapPostEffect::kFxaaExtreme;
        context.SetIs8bpc(gamma_mode == 1 && !fxaa);
        draws_->Invalidate();
        if (fxaa && !PrepareFxaaImage(width, height, error))
          return false;
        if (!output_->Draw(source.Get(), fxaa ? fxaa_write_.Get() : context.render_target(), width,
                           height, {0, 0, width, height}, swizzle, gamma_mode, gamma.Get(), error,
                           fxaa))
          return false;
        return !fxaa || output_->ApplyFxaa(fxaa_read_.Get(), context.unordered_access(), width,
                                           height, effect == SwapPostEffect::kFxaaExtreme, error);
      });
  if (!refreshed && !error.empty()) {
    Fail(error);
    return;
  }
  {
    std::lock_guard lock(Device().context_mutex());
    draws_->Invalidate();
    primitives_->EndFrame();
    shared_memory_->OnGuestFrameEnd();
    textures_->BeginFrame();
    gpu_profiler_->EndFrame();
    SetProfileEnabled(REXCVAR_GET(gpu_wait_stats) > 0);
    Device().context()->Flush();
  }
  clock.Mark(ProfilePhase::kSwap);
}

bool D3D11CommandProcessor::PrepareFxaaImage(uint32_t width, uint32_t height, std::string& error) {
  if (fxaa_image_) {
    D3D11_TEXTURE2D_DESC previous;
    fxaa_image_->GetDesc(&previous);
    if (previous.Width == width && previous.Height == height)
      return true;
  }
  D3D11_TEXTURE2D_DESC description = {};
  description.Width = width;
  description.Height = height;
  description.MipLevels = description.ArraySize = description.SampleDesc.Count = 1;
  description.Format = DXGI_FORMAT_R16G16B16A16_UNORM;
  description.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> image;
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> read;
  Microsoft::WRL::ComPtr<ID3D11RenderTargetView> write;
  HRESULT result = Device().device()->CreateTexture2D(&description, nullptr, image.GetAddressOf());
  if (SUCCEEDED(result))
    result = Device().device()->CreateShaderResourceView(image.Get(), nullptr, read.GetAddressOf());
  if (SUCCEEDED(result))
    result = Device().device()->CreateRenderTargetView(image.Get(), nullptr, write.GetAddressOf());
  if (FAILED(result)) {
    error = "Unable to create the native FXAA gamma and luma image";
    return false;
  }
  fxaa_image_ = std::move(image);
  fxaa_read_ = std::move(read);
  fxaa_write_ = std::move(write);
  return true;
}
}  // namespace rex::graphics::d3d11
