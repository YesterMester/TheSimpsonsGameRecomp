#include <rex/graphics/d3d11/render_target_cache.h>

#include <algorithm>
#include <cstring>
#include <unordered_set>

#include <rex/cvar.h>
#include <rex/logging.h>

#include <rex/graphics/d3d11/profile.h>
#include <rex/graphics/d3d11/shared_memory.h>
#include <rex/graphics/d3d11/texture_cache.h>
#include <rex/graphics/util/native_color_resolve.h>
#include <rex/graphics/util/native_resolve_source.h>

REXCVAR_DEFINE_BOOL(d3d11_native_resolve_textures, true, "GPU/D3D11",
                    "Copy resolved render targets straight into the textures that sample the "
                    "result, instead of decoding them again from the resolved memory");
REXCVAR_DEFINE_BOOL(d3d11_native_resolve_buffers, true, "GPU/D3D11",
                    "Pack supported single-sampled native color images directly into resolve "
                    "buffers without encoding intermediate EDRAM samples");
REXCVAR_DEFINE_BOOL(d3d11_native_resolve_memory, true, "GPU/D3D11",
                    "Keep original-resolution native color resolves in compact GPU buffers and "
                    "materialize compatibility memory only for consumers that require it");
REXCVAR_DEFINE_BOOL(d3d11_native_depth_resolves, true, "GPU/D3D11",
                    "Pack supported depth and stencil directly from native attachments");

namespace rex::graphics::d3d11 {

D3D11RenderTargetCache::D3D11RenderTargetCache(const RegisterFile& registers,
                                               const memory::Memory& memory, TraceWriter* trace,
                                               ui::d3d11::D3D11Device& device, DrawContext& draws,
                                               uint32_t resolution_scale_x,
                                               uint32_t resolution_scale_y)
    : RenderTargetCache(registers, memory, trace, resolution_scale_x, resolution_scale_y),
      device_(device),
      draws_(draws),
      transfer_(device, draws),
      encoder_(device, draws),
      unscaled_encoder_(device, draws),
      resolve_copy_(device, draws),
      shaders_(device) {}

D3D11RenderTargetCache::~D3D11RenderTargetCache() {
  draws_.Invalidate();
  DestroyAllRenderTargets(true);
  if (initialized_)
    ShutdownCommon();
}

bool D3D11RenderTargetCache::Fail(const char* message) {
  last_error_ = message;
  failed_ = true;
  return false;
}

bool D3D11RenderTargetCache::Initialize(const Options& options) {
  if (initialized_)
    return Fail("Native render target cache is already initialized");
  if (draw_resolution_scale_x() > 3 || draw_resolution_scale_y() > 3)
    return Fail("Native Direct3D render target transfers support resolution scales 1 through 3");
  options_ = options;
  options_.stencil_reference_output &= device_.features().pixel_shader_stencil_reference;
  msaa_2x_supported_ = options.native_2x_msaa;
  // Use one sample convention for all guest color and depth formats, including
  // integer ownership views. A per-format convention would break aliased tiles.
  constexpr std::array<DXGI_FORMAT, 15> formats = {
      DXGI_FORMAT_R8G8B8A8_UNORM,       DXGI_FORMAT_R16G16B16A16_UNORM,
      DXGI_FORMAT_R10G10B10A2_UNORM,    DXGI_FORMAT_R16G16_FLOAT,
      DXGI_FORMAT_R16G16B16A16_FLOAT,   DXGI_FORMAT_R16G16_SNORM,
      DXGI_FORMAT_R16G16B16A16_SNORM,   DXGI_FORMAT_R32_FLOAT,
      DXGI_FORMAT_R32G32_FLOAT,         DXGI_FORMAT_D24_UNORM_S8_UINT,
      DXGI_FORMAT_D32_FLOAT_S8X24_UINT, DXGI_FORMAT_R16G16_UINT,
      DXGI_FORMAT_R16G16B16A16_UINT,    DXGI_FORMAT_R32_UINT,
      DXGI_FORMAT_R32G32_UINT};
  if (msaa_2x_supported_) {
    for (auto format : formats) {
      uint32_t levels = 0;
      if (FAILED(device_.device()->CheckMultisampleQualityLevels(format, 2, &levels)) || !levels) {
        msaa_2x_supported_ = false;
        break;
      }
    }
  }
  DxbcRenderTargetDumpShader::Options encoder_options;
  encoder_options.resolution_scale_x = draw_resolution_scale_x();
  encoder_options.resolution_scale_y = draw_resolution_scale_y();
  encoder_options.msaa_2x_supported = msaa_2x_supported_;
  encoder_options.depth_float24_convert_in_pixel_shader =
      options_.depth_float24_convert_in_pixel_shader;
  encoder_options.depth_float24_round = options_.depth_float24_round;
  if (!encoder_.Initialize(encoder_options, last_error_, true)) {
    failed_ = true;
    return false;
  }
  InitializeCommon();
  initialized_ = true;
  failed_ = false;
  last_error_.clear();
  return true;
}

void D3D11RenderTargetCache::ClearCache() {
  draws_.Invalidate();
  RenderTargetCache::ClearCache();
  transfer_programs_.clear();
  shaders_.Clear();
  transfer_.Clear();
  encoder_.Clear();
  unscaled_encoder_.Clear();
  resolve_copy_.Clear();
  color_views_.fill(nullptr);
  color_view_count_ = 0;
  depth_view_ = nullptr;
  failed_ = false;
  last_error_.clear();
}

RenderTargetCache::RenderTarget* D3D11RenderTargetCache::CreateRenderTarget(RenderTargetKey key) {
  if (failed_)
    return nullptr;
  RenderTargetSurface::Description description;
  description.width = key.GetWidth() * GetRenderTargetScaleX(key);
  description.height = GetRenderTargetHeight(key.pitch_tiles_at_32bpp, key.msaa_samples,
                                             GetRenderTargetScaleY(key)) *
                       GetRenderTargetScaleY(key);
  description.guest_format = key.resource_format;
  description.depth = key.is_depth;
  description.samples = key.msaa_samples;
  description.msaa_2x_supported = msaa_2x_supported_;
  description.gamma_as_unorm16 = options_.gamma_as_unorm16;
  auto surface = RenderTargetSurface::Create(device_, description, last_error_);
  if (!surface) {
    failed_ = true;
    return nullptr;
  }
  draws_.Invalidate();
  // New guest EDRAM contents are zero. Initialize all host samples, including
  // the two unused samples when representing guest 2x MSAA with a 4x image.
  if (key.is_depth) {
    device_.context()->ClearDepthStencilView(surface->depth_view(),
                                             D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 0, 0);
  } else {
    constexpr std::array<float, 4> zero = {};
    device_.context()->ClearRenderTargetView(surface->draw_view(), zero.data());
  }
  return new NativeRenderTarget(key, std::move(surface));
}

bool D3D11RenderTargetCache::RestoreEdramSnapshot(const void* snapshot) {
  if (!initialized_ || failed_ || !snapshot)
    return false;
  auto* target =
      static_cast<NativeRenderTarget*>(PrepareFullEdram1280xRenderTargetForSnapshotRestoration(
          xenos::ColorRenderTargetFormat::k_32_FLOAT));
  if (!target)
    return Fail("Unable to create a native snapshot restoration surface");
  constexpr uint32_t pitch_tiles = 16;
  constexpr uint32_t width = pitch_tiles * xenos::kEdramTileWidthSamples;
  constexpr uint32_t height =
      (xenos::kEdramTileCount / pitch_tiles) * xenos::kEdramTileHeightSamples;
  uint32_t scale_x = draw_resolution_scale_x(), scale_y = draw_resolution_scale_y();
  uint32_t host_width = width * scale_x;
  const auto* words = static_cast<const uint32_t*>(snapshot);
  std::vector<uint32_t> pixels(size_t(host_width) * height * scale_y);
  // The diagnostic snapshot stores sample words in tile order. Restore them
  // unchanged into a native float image; ownership transfers reinterpret those
  // words in the format used by the next draw. Replicate bits when scaled,
  // including NaNs and stencil bytes, rather than converting their values.
  for (uint32_t y = 0; y < height; ++y) {
    uint32_t* row = pixels.data() + size_t(y) * scale_y * host_width;
    for (uint32_t x = 0; x < width; ++x) {
      uint32_t tile =
          (y / xenos::kEdramTileHeightSamples) * pitch_tiles + x / xenos::kEdramTileWidthSamples;
      uint32_t offset = tile * xenos::kEdramTileWidthSamples * xenos::kEdramTileHeightSamples +
                        (y % xenos::kEdramTileHeightSamples) * xenos::kEdramTileWidthSamples +
                        x % xenos::kEdramTileWidthSamples;
      std::fill_n(row + x * scale_x, scale_x, words[offset]);
    }
    for (uint32_t sy = 1; sy < scale_y; ++sy)
      std::memcpy(row + size_t(sy) * host_width, row, size_t(host_width) * sizeof(uint32_t));
  }
  draws_.Invalidate();
  device_.context()->UpdateSubresource(target->surface->image(), 0, nullptr, pixels.data(),
                                       host_width * sizeof(uint32_t), 0);
  if (FAILED(device_.device()->GetDeviceRemovedReason()))
    return Fail("Native snapshot restoration lost the graphics device");
  ResetAccumulatedRenderTargets();
  return true;
}

bool D3D11RenderTargetCache::IsHostDepthEncodingDifferent(
    xenos::DepthRenderTargetFormat format) const {
  return format == xenos::DepthRenderTargetFormat::kD24FS8 &&
         !options_.depth_float24_convert_in_pixel_shader;
}

const ShaderProgram* D3D11RenderTargetCache::TransferProgram(
    DxbcRenderTargetTransferShader::TransferShaderKey key, RenderTargetKey destination) {
  uint32_t scale_x = GetRenderTargetScaleX(destination),
           scale_y = GetRenderTargetScaleY(destination);
  uint64_t cache_key = uint64_t(key.key) | (uint64_t(scale_x) << 32) | (uint64_t(scale_y) << 40);
  auto existing = transfer_programs_.find(cache_key);
  if (existing != transfer_programs_.end())
    return existing->second;
  DxbcRenderTargetTransferShader::Options options;
  options.resolution_scale_x = draw_resolution_scale_x();
  options.resolution_scale_y = draw_resolution_scale_y();
  options.msaa_2x_supported = msaa_2x_supported_;
  options.stencil_reference_output = options_.stencil_reference_output;
  options.depth_float24_convert_in_pixel_shader = options_.depth_float24_convert_in_pixel_shader;
  options.depth_float24_round = options_.depth_float24_round;
  options.portable_integer_division = true;
  std::vector<uint32_t> code;
  const ShaderProgram* program = nullptr;
  if (DxbcRenderTargetTransferShader::Create(key, options, code, last_error_))
    program = shaders_.GetOrCreate({reinterpret_cast<const uint8_t*>(code.data()), code.size() * 4},
                                   false, last_error_);
  // Retain failure too; stop submission instead of recompiling or drawing a
  // partially updated ownership range on the next guest draw.
  transfer_programs_.emplace(cache_key, program);
  if (!program)
    failed_ = true;
  return program;
}

bool D3D11RenderTargetCache::PerformTransfers(std::span<RenderTarget* const> destinations,
                                              const std::vector<Transfer>* transfers,
                                              const Transfer::Rectangle* clear_cutout) {
  using Generator = DxbcRenderTargetTransferShader;
  std::unordered_set<RenderTarget*> writable;
  for (size_t i = 0; i < destinations.size(); ++i)
    if (destinations[i] && !transfers[i].empty())
      writable.insert(destinations[i]);
  std::unordered_map<RenderTarget*, std::unique_ptr<RenderTargetSurface>> snapshots;
  // Preserve sources before any destination in a batch changes. In particular,
  // retained host depth must survive color-to-depth alias transfers to itself.
  for (size_t i = 0; i < destinations.size(); ++i) {
    if (!destinations[i])
      continue;
    for (const auto& operation : transfers[i]) {
      for (auto* source : {operation.source, operation.host_depth_source}) {
        if (!source || !writable.contains(source) || snapshots.contains(source))
          continue;
        auto& target = *static_cast<NativeRenderTarget*>(source);
        g_profile.snapshots.fetch_add(1, std::memory_order_relaxed);
        g_profile.snapshot_bytes.fetch_add(
            uint64_t(target.surface->description().width) * target.surface->description().height *
                target.surface->sample_count() * (source->key().Is64bpp() ? 8 : 4),
            std::memory_order_relaxed);
        auto snapshot = target.surface->Snapshot(device_, draws_, last_error_);
        if (!snapshot) {
          failed_ = true;
          return false;
        }
        snapshots.emplace(source, std::move(snapshot));
      }
    }
  }
  auto surface_for = [&](RenderTarget* target) -> const RenderTargetSurface* {
    if (!target)
      return nullptr;
    auto copy = snapshots.find(target);
    return copy == snapshots.end() ? static_cast<NativeRenderTarget*>(target)->surface.get()
                                   : copy->second.get();
  };
  for (size_t i = 0; i < destinations.size(); ++i) {
    if (!destinations[i])
      continue;
    auto& destination = *static_cast<NativeRenderTarget*>(destinations[i]);
    auto dest_key = destination.key();
    uint32_t scale_x = GetRenderTargetScaleX(dest_key), scale_y = GetRenderTargetScaleY(dest_key);
    for (const auto& operation : transfers[i]) {
      if (!operation.source)
        return Fail("Native ownership transfer has no source");
      auto source_key = operation.source->key();
      std::array<Transfer::Rectangle, Transfer::kMaxRectanglesWithCutout> rectangles;
      uint32_t count = operation.GetRectangles(dest_key.base_tiles, dest_key.GetPitchTiles(),
                                               dest_key.msaa_samples, dest_key.Is64bpp(),
                                               rectangles.data(), clear_cutout);
      if (!count)
        continue;
      std::array<D3D11_RECT, Transfer::kMaxRectanglesWithCutout> native_rectangles;
      for (uint32_t j = 0; j < count; ++j) {
        const auto& rect = rectangles[j];
        native_rectangles[j] = {LONG(rect.x_pixels * scale_x), LONG(rect.y_pixels * scale_y),
                                LONG((rect.x_pixels + rect.width_pixels) * scale_x),
                                LONG((rect.y_pixels + rect.height_pixels) * scale_y)};
      }
      std::span<const D3D11_RECT> rects(native_rectangles.data(), count);
      g_profile.transfers.fetch_add(1, std::memory_order_relaxed);
      for (const auto& rect : rects)
        g_profile.transfer_pixels.fetch_add(
            uint64_t(rect.right - rect.left) * uint64_t(rect.bottom - rect.top),
            std::memory_order_relaxed);
      if (source_key.resource_format == dest_key.resource_format &&
          source_key.is_depth == dest_key.is_depth &&
          source_key.msaa_samples == dest_key.msaa_samples &&
          source_key.GetPitchTiles() == dest_key.GetPitchTiles())
        g_profile.transfer_same_format.fetch_add(1, std::memory_order_relaxed);
      Generator::TransferShaderKey shader_key;
      shader_key.dest_msaa_samples = dest_key.msaa_samples;
      shader_key.dest_resource_format = dest_key.resource_format;
      shader_key.source_msaa_samples = source_key.msaa_samples;
      shader_key.source_resource_format = source_key.resource_format;
      shader_key.dest_original_resolution = dest_key.original_resolution;
      shader_key.source_original_resolution = source_key.original_resolution;
      auto* host_depth = dest_key.is_depth ? operation.host_depth_source : nullptr;
      if (host_depth) {
        auto host_key = host_depth->key();
        shader_key.host_depth_source_original_resolution = host_key.original_resolution;
        shader_key.host_depth_source_msaa_samples = host_key.msaa_samples;
      }
      // The native depth snapshot stays an image instead of becoming an EDRAM
      // buffer, so the shader retains texture addressing and all host precision.
      if (dest_key.is_depth) {
        shader_key.mode =
            host_depth ? (source_key.is_depth ? Generator::TransferMode::kDepthAndHostDepthToDepth
                                              : Generator::TransferMode::kColorAndHostDepthToDepth)
                       : (source_key.is_depth ? Generator::TransferMode::kDepthToDepth
                                              : Generator::TransferMode::kColorToDepth);
      } else {
        shader_key.mode = source_key.is_depth ? Generator::TransferMode::kDepthToColor
                                              : Generator::TransferMode::kColorToColor;
      }
      auto program = TransferProgram(shader_key, dest_key);
      if (!program)
        return false;
      Generator::TransferAddressConstant address;
      address.dest_pitch = dest_key.GetPitchTiles();
      address.source_pitch = source_key.GetPitchTiles();
      address.source_to_dest = int32_t(dest_key.base_tiles) - int32_t(source_key.base_tiles);
      RenderTargetTransfer::Constants constants;
      constants.address = address.constant;
      constants.output = dest_key.is_depth ? RenderTargetTransfer::Output::kDepth
                                           : RenderTargetTransfer::Output::kColor;
      constants.write_stencil_reference = dest_key.is_depth && options_.stencil_reference_output;
      if (host_depth) {
        auto host_key = host_depth->key();
        address.source_pitch = host_key.GetPitchTiles();
        address.source_to_dest = int32_t(dest_key.base_tiles) - int32_t(host_key.base_tiles);
        constants.host_depth_address = address.constant;
      }
      if (dest_key.is_depth && !options_.stencil_reference_output &&
          !transfer_.ClearDepthStencil(*destination.surface, false, 0, true, 0, rects,
                                       last_error_)) {
        failed_ = true;
        return false;
      }
      if (!transfer_.Transfer(*destination.surface, *surface_for(operation.source),
                              surface_for(host_depth), *program, constants, rects, last_error_)) {
        failed_ = true;
        return false;
      }
      if (dest_key.is_depth && !options_.stencil_reference_output) {
        shader_key.mode = source_key.is_depth ? Generator::TransferMode::kDepthToStencilBit
                                              : Generator::TransferMode::kColorToStencilBit;
        shader_key.host_depth_source_msaa_samples = xenos::MsaaSamples::k1X;
        program = TransferProgram(shader_key, dest_key);
        if (!program)
          return false;
        constants.output = RenderTargetTransfer::Output::kStencilBit;
        for (uint32_t bit = 1; bit < 256; bit <<= 1) {
          constants.stencil_bit = uint8_t(bit);
          if (!transfer_.Transfer(*destination.surface, *surface_for(operation.source), nullptr,
                                  *program, constants, rects, last_error_)) {
            failed_ = true;
            return false;
          }
        }
      }
    }
  }
  return true;
}

void D3D11RenderTargetCache::RefreshBindings() {
  auto targets = last_update_accumulated_render_targets();
  depth_view_ =
      targets[0] ? static_cast<NativeRenderTarget*>(targets[0])->surface->depth_view() : nullptr;
  color_view_count_ = 0;
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    color_views_[i] = targets[i + 1]
                          ? static_cast<NativeRenderTarget*>(targets[i + 1])->surface->draw_view()
                          : nullptr;
    if (color_views_[i])
      color_view_count_ = i + 1;
  }
}

bool D3D11RenderTargetCache::Update(bool is_rasterization_done,
                                    reg::RB_DEPTHCONTROL normalized_depth_control,
                                    uint32_t normalized_color_mask, const Shader& vertex_shader) {
  if (!initialized_ || failed_)
    return false;
  last_error_.clear();
  if (!RenderTargetCache::Update(is_rasterization_done, normalized_depth_control,
                                 normalized_color_mask, vertex_shader))
    return false;
  if (!PerformTransfers(
          {last_update_accumulated_render_targets(), 1 + xenos::kMaxColorRenderTargets},
          last_update_transfers()))
    return false;
  RefreshBindings();
  return true;
}

void D3D11RenderTargetCache::BindRenderTargets(DrawCommand& command) const {
  command.render_targets = {color_views_.data(), color_view_count_};
  command.depth_stencil = depth_view_;
}

bool D3D11RenderTargetCache::ClearTarget(NativeRenderTarget& target,
                                         const Transfer::Rectangle& rectangle, uint64_t value) {
  auto key = target.key();
  uint32_t scale_x = GetRenderTargetScaleX(key), scale_y = GetRenderTargetScaleY(key);
  D3D11_RECT rect = {LONG(rectangle.x_pixels * scale_x), LONG(rectangle.y_pixels * scale_y),
                     LONG((rectangle.x_pixels + rectangle.width_pixels) * scale_x),
                     LONG((rectangle.y_pixels + rectangle.height_pixels) * scale_y)};
  bool passed = false;
  if (key.is_depth) {
    uint32_t guest_depth = (uint32_t(value) >> 8) & 0xFFFFFF;
    float depth = key.GetDepthFormat() == xenos::DepthRenderTargetFormat::kD24S8
                      ? xenos::UNorm24To32(guest_depth)
                      : xenos::Float20e4To32(guest_depth) * 0.5f;
    passed = transfer_.ClearDepthStencil(*target.surface, true, depth, true, uint8_t(value),
                                         {&rect, 1}, last_error_);
  } else if (target.surface->integer_transfer()) {
    std::array<uint32_t, 4> components = {};
    switch (key.GetColorFormat()) {
      case xenos::ColorRenderTargetFormat::k_32_FLOAT:
        components[0] = uint32_t(value);
        break;
      case xenos::ColorRenderTargetFormat::k_32_32_FLOAT:
        components[0] = uint32_t(value);
        components[1] = uint32_t(value >> 32);
        break;
      default:
        for (uint32_t c = 0; c < 4; ++c)
          components[c] = uint32_t((value >> (c * 16)) & 0xFFFF);
        break;
    }
    passed = transfer_.ClearColorInteger(*target.surface, components, {&rect, 1}, last_error_);
  } else {
    std::array<float, 4> components;
    switch (key.GetColorFormat()) {
      case xenos::ColorRenderTargetFormat::k_8_8_8_8:
      case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA:
        for (uint32_t c = 0; c < 4; ++c) {
          components[c] = float((value >> (c * 8)) & 0xFF) * (1.0f / 255);
          if (c < 3 && key.GetColorFormat() == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA)
            components[c] = xenos::PWLGammaToLinear(components[c]);
        }
        break;
      case xenos::ColorRenderTargetFormat::k_2_10_10_10:
      case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10:
      case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
      case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16:
        for (uint32_t c = 0; c < 3; ++c) {
          uint32_t bits = uint32_t((value >> (c * 10)) & 0x3FF);
          components[c] = key.GetColorFormat() == xenos::ColorRenderTargetFormat::k_2_10_10_10 ||
                                  key.GetColorFormat() ==
                                      xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10
                              ? bits * (1.0f / 1023)
                              : xenos::Float7e3To32(bits);
        }
        components[3] = float((value >> 30) & 3) * (1.0f / 3);
        break;
      default:
        return Fail("Native resolve clear has an unsupported color format");
    }
    passed = transfer_.ClearColor(*target.surface, components, {&rect, 1}, last_error_);
  }
  if (!passed)
    failed_ = true;
  return passed;
}

bool D3D11RenderTargetCache::DumpRenderTargets(uint32_t base, uint32_t row_length, uint32_t rows,
                                               uint32_t pitch) {
  if (!initialized_ || failed_)
    return false;
  last_error_.clear();
  if (!encoder_.EnsureBuffer(last_error_))
    return Fail(last_error_.c_str());
  std::vector<ResolveCopyDumpRectangle> rectangles;
  GetResolveCopyRectanglesToDump(base, row_length, rows, pitch, rectangles);
  for (const auto& rectangle : rectangles) {
    auto& target = *static_cast<NativeRenderTarget*>(rectangle.render_target);
    auto key = target.key();
    if (GetRenderTargetScaleX(key) != draw_resolution_scale_x() ||
        GetRenderTargetScaleY(key) != draw_resolution_scale_y())
      return Fail("Native resolve encoding from an original-resolution target is unsupported");
    std::array<ResolveCopyDumpRectangle::Dispatch, ResolveCopyDumpRectangle::kMaxDispatches>
        dispatches;
    uint32_t count = rectangle.GetDispatches(pitch, row_length, dispatches.data());
    for (uint32_t i = 0; i < count; ++i) {
      const auto& dispatch = dispatches[i];
      if (!encoder_.Encode(*target.surface, base + dispatch.offset, key.base_tiles, pitch,
                           key.GetPitchTiles(), dispatch.width_tiles, dispatch.height_tiles,
                           last_error_)) {
        failed_ = true;
        return false;
      }
    }
  }
  return true;
}

bool D3D11RenderTargetCache::Resolve(D3D11SharedMemory& shared_memory, D3D11TextureCache& textures,
                                     const memory::Memory& memory, TraceWriter& trace,
                                     uint32_t& written_address, uint32_t& written_length) {
  written_address = written_length = 0;
  if (!initialized_ || failed_)
    return false;
  last_error_.clear();
  draw_util::ResolveInfo resolve;
  // Native signed-normalized attachments have the same -1..1 blend range as
  // the existing host-render-target path. Preserve the common resolve's format
  // and exponent-bias corrections for this storage.
  if (!draw_util::GetResolveInfo(register_file(), memory, trace, draw_resolution_scale_x(),
                                 draw_resolution_scale_y(), true, true, resolve))
    return Fail("Unable to obtain native resolve register and rectangle data");
  const bool source_original_resolution =
      IsDrawResolutionScaled() && resolve.IsCopyingDepth() &&
      IsOriginalResolutionRenderTarget(resolve.depth_original_base,
                                       resolve.depth_edram_info.pitch_tiles, true);
  if (source_original_resolution &&
      !draw_util::GetResolveInfo(register_file(), memory, trace, 1, 1, true, true, resolve))
    return Fail("Unable to obtain original-resolution resolve data");
  if (!resolve.coordinate_info.width_div_8 || !resolve.height_div_8)
    return true;
  // Textures sampling the destination must be found while their data still
  // matches memory, before the resolved range is invalidated.
  NativeResolveCopy native_copy;
  if (resolve.copy_dest_extent_length &&
      (REXCVAR_GET(d3d11_native_resolve_textures) || REXCVAR_GET(d3d11_native_resolve_buffers)))
    PlanNativeResolveCopy(resolve, textures, native_copy);
  if (failed_)
    return false;
  if (native_copy.alias_transferred && shared_memory.native_diagnostics())
    shared_memory.native_diagnostics()->RecordOperation(
        NativeGpuDiagnostics::Operation::kAliasTransfer);
  if (resolve.copy_dest_extent_length) {
    bool direct = native_copy.source_surface && REXCVAR_GET(d3d11_native_resolve_buffers);
    bool native_memory = direct && REXCVAR_GET(d3d11_native_resolve_memory) &&
                         shared_memory.CanRetainNativeResolve(resolve.copy_dest_extent_start,
                                                              resolve.copy_dest_extent_length);
    if (auto* diagnostics = shared_memory.native_diagnostics()) {
      if (!direct) {
        diagnostics->RecordDependency(NativeGpuDiagnostics::Dependency::kResolveFallback,
                                      resolve.copy_dest_extent_length,
                                      "Direct3D 11 resolve uses encoded render target samples");
        diagnostics->RecordDependency(NativeGpuDiagnostics::Dependency::kEdram,
                                      resolve.copy_dest_extent_length,
                                      "encode native Direct3D 11 attachments for a resolve");
      } else {
        diagnostics->RecordOperation(NativeGpuDiagnostics::Operation::kResolve);
      }
      if (!native_memory)
        diagnostics->RecordDependency(NativeGpuDiagnostics::Dependency::kMemoryMirror,
                                      resolve.copy_dest_extent_length,
                                      "write original-resolution Direct3D 11 resolve bytes");
    }
    bool scaled = IsDrawResolutionScaled() && !source_original_resolution;
    draw_util::ResolveCopyShaderConstants constants;
    uint32_t groups_x = 0, groups_y = 0;
    auto shader = resolve.GetCopyShader(scaled ? draw_resolution_scale_x() : 1u,
                                        scaled ? draw_resolution_scale_y() : 1u, constants,
                                        groups_x, groups_y);
    if (shader == draw_util::ResolveCopyShaderIndex::kUnknown)
      return Fail("Native resolve has no copy shader for this format");
    uint32_t base, width, height, pitch;
    resolve.GetCopyEdramTileSpan(base, width, height, pitch);
    if (!direct) {
      bool allocated = encoder_.buffer() != nullptr;
      if (!DumpRenderTargets(base, width, height, pitch))
        return false;
      if (!allocated) {
        if (auto* diagnostics = shared_memory.native_diagnostics())
          diagnostics->RecordAllocation(NativeGpuDiagnostics::Allocation::kEdram,
                                        encoder_.byte_size(), encoder_.byte_size());
      }
    }
    const auto& info = draw_util::resolve_copy_shader_info[size_t(shader)];
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> scaled_destination;
    D3D11SharedMemory::NativeResolveHandle native_destination;
    if (native_memory) {
      native_destination = shared_memory.PrepareNativeResolve(resolve.copy_dest_extent_start,
                                                              resolve.copy_dest_extent_length,
                                                              info.dest_bpe_log2, last_error_);
      if (!native_destination)
        return Fail(last_error_.c_str());
    }
    ID3D11UnorderedAccessView* destination;
    if (scaled) {
      uint64_t end = uint64_t(resolve.copy_dest_extent_start) + resolve.copy_dest_extent_length;
      if (end > SharedMemory::kBufferSize || end <= resolve.copy_dest_base)
        return Fail("Native scaled resolve destination exceeds guest memory");
      scaled_destination = textures.ScaledResolveDestination(
          resolve.copy_dest_base, uint32_t(end - resolve.copy_dest_base), info.dest_bpe_log2);
      if (!scaled_destination)
        return Fail(textures.last_error().c_str());
      destination = scaled_destination.Get();
    } else if (native_destination) {
      destination = native_destination->writes[info.dest_bpe_log2 - 2].Get();
    } else {
      if (!shared_memory.RequestRange(resolve.copy_dest_extent_start,
                                      resolve.copy_dest_extent_length))
        return Fail("Unable to preserve native resolve destination pages");
      destination = shared_memory.typed_uav(info.dest_bpe_log2);
    }
    auto pack_native = [&](ID3D11UnorderedAccessView* output, bool output_scaled,
                           uint32_t address) {
      return resolve.IsCopyingDepth()
                 ? resolve_copy_.CopyDepthSurface(
                       *native_copy.source_surface, resolve, output, output_scaled, last_error_,
                       address, !depth_float24_convert_in_pixel_shader() && depth_float24_round())
                 : resolve_copy_.CopyColorSurface(*native_copy.source_surface, resolve, output,
                                                  output_scaled, last_error_, address);
    };
    GpuSwitch(GpuCategory::kResolveCopies);
    bool copied = direct
                      ? pack_native(destination, scaled,
                                    !scaled && native_destination ? native_destination->address : 0)
                      : resolve_copy_.Copy(encoder_, shader, constants, destination, groups_x,
                                           groups_y, scaled, last_error_);
    if (!copied) {
      failed_ = true;
      return false;
    }
    if (scaled) {
      // Original-resolution consumers use the same GPU samples and preserved
      // padding. Supported native buffers defer their compatibility copy until
      // a CPU readback, vertex fetch or memory export actually requests it.
      if (!direct && !unscaled_encoder_.buffer()) {
        DxbcRenderTargetDumpShader::Options unscaled_options;
        if (!unscaled_encoder_.Initialize(unscaled_options, last_error_)) {
          failed_ = true;
          return false;
        }
        if (auto* diagnostics = shared_memory.native_diagnostics())
          diagnostics->RecordAllocation(NativeGpuDiagnostics::Allocation::kEdram,
                                        unscaled_encoder_.byte_size(),
                                        unscaled_encoder_.byte_size());
      }
      draw_util::ResolveCopyShaderConstants unscaled_constants;
      uint32_t unscaled_groups_x, unscaled_groups_y;
      auto unscaled_shader =
          resolve.GetCopyShader(1, 1, unscaled_constants, unscaled_groups_x, unscaled_groups_y);
      // GetCopyShader sizes the dispatch from its arguments, but copies the
      // coordinate metadata from ResolveInfo verbatim.
      unscaled_constants.dest_relative.coordinate_info.draw_resolution_scale_x = 1;
      unscaled_constants.dest_relative.coordinate_info.draw_resolution_scale_y = 1;
      if (unscaled_shader == draw_util::ResolveCopyShaderIndex::kUnknown)
        return Fail("Native resolve has no original-resolution copy shader");
      if (!native_destination && !shared_memory.RequestRange(resolve.copy_dest_extent_start,
                                                             resolve.copy_dest_extent_length))
        return Fail("Unable to preserve original-resolution resolve pages");
      const auto& unscaled_info = draw_util::resolve_copy_shader_info[size_t(unscaled_shader)];
      GpuSwitch(GpuCategory::kResolveMirrors);
      bool mirrored =
          direct ? pack_native(native_destination
                                   ? native_destination->writes[info.dest_bpe_log2 - 2].Get()
                                   : shared_memory.typed_uav(unscaled_info.dest_bpe_log2),
                               false, native_destination ? native_destination->address : 0)
                 : unscaled_encoder_.DownsampleFrom(
                       encoder_, base, width, height, pitch,
                       constants.dest_relative.edram_info.format_is_64bpp,
                       constants.dest_relative.edram_info.msaa_samples, last_error_) &&
                       resolve_copy_.Copy(unscaled_encoder_, unscaled_shader, unscaled_constants,
                                          shared_memory.typed_uav(unscaled_info.dest_bpe_log2),
                                          unscaled_groups_x, unscaled_groups_y, false, last_error_);
      if (!mirrored) {
        failed_ = true;
        return false;
      }
    }
    // The common notification invalidates textures and protects GPU-written
    // pages from being replaced by a stale CPU upload. Both resolution copies
    // preserve neighboring bytes in partially written pages.
    if (native_destination)
      shared_memory.BeginNativeResolveWrite(native_destination);
    textures.MarkRangeAsResolved(resolve.copy_dest_extent_start, resolve.copy_dest_extent_length,
                                 source_original_resolution);
    if (native_destination && !shared_memory.EndNativeResolveWrite())
      return Fail("Unable to preserve native resolve publication");
    // The textures sampling it get the render target's pixels directly, so
    // they match the memory again without being decoded from it.
    if (REXCVAR_GET(d3d11_native_resolve_textures))
      PerformNativeResolveCopy(native_copy, textures);
    written_address = resolve.copy_dest_extent_start;
    written_length = resolve.copy_dest_extent_length;
  }
  GpuSwitch(GpuCategory::kResolveClears);
  if ((resolve.IsClearingDepth() || resolve.IsClearingColor()) && !ResolveClear(resolve))
    return false;
  return true;
}

RenderTargetCache::RenderTarget* D3D11RenderTargetCache::PrepareNativeResolveAlias(
    const draw_util::ResolveInfo& resolve, uint32_t base, uint32_t row_length, uint32_t rows,
    uint32_t pitch, std::vector<ResolveCopyDumpRectangle>& rectangles) {
  NativeResolveOwnershipPlan plan;
  if (!PlanNativeResolveOwnership(resolve, base, row_length, rows, pitch, plan, true))
    return nullptr;
  const auto dest_key = plan.destination;
  const bool depth = dest_key.is_depth;
  auto* dest = static_cast<NativeRenderTarget*>(GetOrCreateRenderTarget(dest_key));
  if (!dest)
    return nullptr;
  using Generator = DxbcRenderTargetTransferShader;
  for (const auto& transfer : plan.transfers) {
    const auto source_key = transfer.source->key();
    Generator::TransferShaderKey shader_key;
    shader_key.dest_msaa_samples = dest_key.msaa_samples;
    shader_key.dest_resource_format = dest_key.resource_format;
    shader_key.source_msaa_samples = source_key.msaa_samples;
    shader_key.source_resource_format = source_key.resource_format;
    shader_key.dest_original_resolution = dest_key.original_resolution;
    shader_key.source_original_resolution = source_key.original_resolution;
    shader_key.host_depth_source_original_resolution =
        transfer.host_depth_source ? transfer.host_depth_source->key().original_resolution : 0;
    if (depth) {
      shader_key.mode =
          transfer.host_depth_source
              ? (source_key.is_depth ? Generator::TransferMode::kDepthAndHostDepthToDepth
                                     : Generator::TransferMode::kColorAndHostDepthToDepth)
              : (source_key.is_depth ? Generator::TransferMode::kDepthToDepth
                                     : Generator::TransferMode::kColorToDepth);
    } else {
      shader_key.mode = source_key.is_depth ? Generator::TransferMode::kDepthToColor
                                            : Generator::TransferMode::kColorToColor;
    }
    if (transfer.host_depth_source)
      shader_key.host_depth_source_msaa_samples = transfer.host_depth_source->key().msaa_samples;
    if (!TransferProgram(shader_key, dest_key))
      return nullptr;
    if (depth && !options_.stencil_reference_output) {
      shader_key.mode = source_key.is_depth ? Generator::TransferMode::kDepthToStencilBit
                                            : Generator::TransferMode::kColorToStencilBit;
      shader_key.host_depth_source_msaa_samples = xenos::MsaaSamples::k1X;
      if (!TransferProgram(shader_key, dest_key))
        return nullptr;
    }
  }
  // Resolving is a read. Keep the original owners and host depth values;
  // publishing this decoded view would accumulate format-rounding losses.
  const auto& transfers = plan.transfers;
  RenderTarget* targets[] = {dest};
  if (!PerformTransfers(targets, &transfers))
    return nullptr;
  RefreshBindings();
  GetResolveCopyRectanglesToDump(base, row_length, rows, pitch, rectangles);
  return dest;
}

void D3D11RenderTargetCache::PlanNativeResolveCopy(const draw_util::ResolveInfo& resolve,
                                                   D3D11TextureCache& textures,
                                                   NativeResolveCopy& copy) {
  copy.target_count = 0;
  // Sample selection and conversion read the authoritative attachment directly.
  // Matching fast copies preserve integer floating-point payloads, including NaNs.
  bool depth = resolve.IsCopyingDepth();
  const draw_util::ResolveEdramInfo& edram =
      depth ? resolve.depth_edram_info : resolve.color_edram_info;
  if ((depth &&
       (!REXCVAR_GET(d3d11_native_depth_resolves) || resolve.copy_dest_info.copy_dest_swap)) ||
      edram.msaa_samples > xenos::MsaaSamples::k4X ||
      uint32_t(resolve.copy_dest_info.copy_dest_endian) > uint32_t(xenos::Endian128::k8in128))
    return;
  auto guest_format = xenos::ColorRenderTargetFormat(edram.format);
  auto depth_format = xenos::DepthRenderTargetFormat(edram.format);
  if (depth ? (uint32_t(depth_format) > uint32_t(xenos::DepthRenderTargetFormat::kD24FS8) ||
               uint32_t(resolve.copy_dest_info.copy_dest_format) !=
                   uint32_t(depth_format == xenos::DepthRenderTargetFormat::kD24FS8
                                ? xenos::TextureFormat::k_24_8_FLOAT
                                : xenos::TextureFormat::k_24_8))
            : (draw_util::GetNativeColorResolvePacking(
                   xenos::ColorFormat(resolve.copy_dest_info.copy_dest_format)) ==
               draw_util::NativeColorResolveDestination::kUnsupported))
    return;
  uint32_t base, row_length, rows, pitch;
  resolve.GetCopyEdramTileSpan(base, row_length, rows, pitch);
  std::vector<ResolveCopyDumpRectangle> rectangles;
  GetResolveCopyRectanglesToDump(base, row_length, rows, pitch, rectangles);
  auto* native_source =
      PrepareNativeResolveAlias(resolve, base, row_length, rows, pitch, rectangles);
  copy.alias_transferred = native_source != nullptr;
  if (failed_)
    return;
  if (!native_source)
    native_source = FindNativeResolveSource(base, row_length, rows, pitch);
  if (!native_source)
    return;
  auto& source = *static_cast<NativeRenderTarget*>(native_source);
  RenderTargetKey key = source.key();
  xenos::ColorRenderTargetFormat source_format =
      key.is_depth ? xenos::ColorRenderTargetFormat::k_8_8_8_8 : key.GetColorFormat();
  if (bool(key.is_depth) != depth ||
      key.base_tiles != (depth ? resolve.depth_original_base : resolve.color_original_base) ||
      key.msaa_samples != edram.msaa_samples || key.GetPitchTiles() != edram.pitch_tiles ||
      (depth ? key.GetDepthFormat() != depth_format
             : draw_util::GetNativeColorResolvePacking(source_format) ==
                   draw_util::NativeColorResolvePacking::kUnsupported))
    return;
  // A different-format owner can share the same EDRAM tiles. Its pixels must
  // first be transferred in their bit representation; a native typed image
  // copy or normalized component pack cannot reinterpret another family.
  auto packing = draw_util::GetNativeColorResolvePacking(source_format);
  ID3D11Texture2D* image = source.surface->image();
  if (!image)
    return;
  D3D11_TEXTURE2D_DESC image_desc;
  image->GetDesc(&image_desc);
  uint32_t scale_x = GetRenderTargetScaleX(key), scale_y = GetRenderTargetScaleY(key);
  uint32_t x1 = resolve.rect_x0 + (uint32_t(resolve.coordinate_info.width_div_8)
                                   << xenos::kResolveAlignmentPixelsLog2);
  uint32_t y1 = resolve.rect_y0 + (resolve.height_div_8 << xenos::kResolveAlignmentPixelsLog2);
  auto source_layout = draw_util::GetNativeResolveSourceLayout(
      resolve.rect_x0, resolve.rect_y0, x1 - resolve.rect_x0, y1 - resolve.rect_y0, scale_x,
      scale_y, image_desc.Width, image_desc.Height, edram.msaa_samples, key.msaa_samples,
      !depth && key.Is64bpp(), edram.pitch_tiles);
  if (!source_layout)
    return;
  uint32_t dest_pitch = uint32_t(resolve.copy_dest_coordinate_info.pitch_aligned_div_32)
                        << xenos::kTextureTileWidthHeightLog2;
  if (x1 > dest_pitch)
    return;
  if (depth || (source_layout.packed & 0x80000000u) ||
      key.msaa_samples != xenos::MsaaSamples::k1X ||
      !xenos::IsColorResolveFormatBitwiseEquivalent(
          source_format, xenos::ColorFormat(resolve.copy_dest_info.copy_dest_format)) ||
      resolve.rect_x0 || resolve.rect_y0 || resolve.copy_dest_info.copy_dest_array ||
      resolve.copy_dest_info.copy_dest_exp_bias ||
      packing >= draw_util::NativeColorResolvePacking::kSint16x2 ||
      draw_util::NativeColorResolveUsesIntegerSource(packing) ||
      uint32_t(resolve.copy_dest_info.copy_dest_endian) > uint32_t(xenos::Endian128::k16in32)) {
    copy.source = image;
    copy.source_surface = source.surface.get();
    copy.scale_x = scale_x;
    copy.scale_y = scale_y;
    return;
  }
  D3D11TextureCache::NativeResolveTarget found[D3D11TextureCache::kMaxNativeResolveTargets];
  uint32_t found_count = textures.FindNativeResolveTargets(
      resolve.copy_dest_base_raw, dest_pitch,
      xenos::TextureFormat(resolve.copy_dest_info.copy_dest_format),
      xenos::Endian(resolve.copy_dest_info.copy_dest_endian),
      IsDrawResolutionScaled() && !key.original_resolution, found);
  for (uint32_t i = 0; i < found_count; ++i) {
    // The whole texture must come from the copied rectangle.
    if (found[i].width > x1 || found[i].height > y1 ||
        found[i].width * scale_x > image_desc.Width ||
        found[i].height * scale_y > image_desc.Height)
      continue;
    copy.targets[copy.target_count++] = found[i];
  }
  copy.source = image;
  copy.source_surface = source.surface.get();
  copy.scale_x = scale_x;
  copy.scale_y = scale_y;
  copy.red_blue_swapped = resolve.copy_dest_info.copy_dest_swap != 0;
}

void D3D11RenderTargetCache::PerformNativeResolveCopy(const NativeResolveCopy& copy,
                                                      D3D11TextureCache& textures) {
  for (uint32_t i = 0; i < copy.target_count; ++i) {
    const auto& target = copy.targets[i];
    D3D11_BOX box = {0, 0, 0, target.width * copy.scale_x, target.height * copy.scale_y, 1};
    GpuSwitch(GpuCategory::kResolveCopies);
    device_.context()->CopySubresourceRegion(target.resource, 0, 0, 0, 0, copy.source, 0, &box);
    textures.EndNativeResolveWrite(target, copy.red_blue_swapped);
    if (++native_resolve_copies_ <= 8 || !(native_resolve_copies_ & 4095)) {
      REXGPU_INFO("[d3d11-native-resolve] {} resolves copied into the textures sampling them",
                  native_resolve_copies_);
    }
  }
}

bool D3D11RenderTargetCache::ResolveClear(const draw_util::ResolveInfo& resolve) {
  if (!initialized_ || failed_)
    return false;
  last_error_.clear();
  Transfer::Rectangle rectangle;
  std::array<RenderTarget*, 2> targets = {};
  std::array<std::vector<Transfer>, 2> transfers;
  if (!PrepareHostRenderTargetsResolveClear(resolve, rectangle, targets[0], transfers[0],
                                            targets[1], transfers[1]))
    return !failed_;
  if (!PerformTransfers(targets, transfers.data(), &rectangle))
    return false;
  if (targets[0] && !ClearTarget(*static_cast<NativeRenderTarget*>(targets[0]), rectangle,
                                 resolve.rb_depth_clear))
    return false;
  if (targets[1] &&
      !ClearTarget(*static_cast<NativeRenderTarget*>(targets[1]), rectangle,
                   uint64_t(resolve.rb_color_clear) | (uint64_t(resolve.rb_color_clear_lo) << 32)))
    return false;
  ResetAccumulatedRenderTargets();
  color_views_.fill(nullptr);
  color_view_count_ = 0;
  depth_view_ = nullptr;
  return true;
}

}  // namespace rex::graphics::d3d11
