#include <rex/graphics/d3d11/guest_draw.h>

#include <rex/cvar.h>
#include <rex/graphics/d3d11/profile.h>
#include <rex/graphics/util/bytes_equal.h>
#include <rex/graphics/util/vertex_fetch_bounds.h>
#include <rex/graphics/util/vertex_index_bounds.h>
#include <rex/logging.h>

#include <algorithm>
#include <bit>
#include <cstring>

REXCVAR_DEFINE_BOOL(d3d11_native_vertex_buffers, true, "GPU/D3D11",
                    "Read proven CPU/GPU-owned vertex streams from retained resource-local buffers "
                    "instead of the guest-address compatibility buffer");

namespace rex::graphics::d3d11 {

ID3D11Buffer* GuestDraw::UpdateConstants(ConstantBlock block, std::span<const uint8_t> data,
                                         uint32_t& first, uint32_t& count, std::string& error) {
  if (data.empty() || data.size() > D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT * 16) {
    error = "Invalid native constant buffer size";
    return nullptr;
  }
  uint32_t size = (uint32_t(data.size()) + 15) & ~uint32_t(15);
  auto [entry, inserted] = constants_[block].try_emplace(size);
  auto& constants = entry->second;
  constexpr uint32_t kPageSize = 64 * 1024, kPageBudget = 4 * 1024 * 1024;
  uint32_t capacity =
      constant_ranges_ && constant_ring_bytes_ <= kPageBudget - kPageSize ? kPageSize : 0;
  auto result = constants.Upload(device_, data, capacity, error);
  if (result == ConstantBuffer::Update::kFailed) {
    if (inserted)
      constants_[block].erase(entry);
    return nullptr;
  }
  if (inserted)
    constant_ring_bytes_ += constants.capacity();
  if (result == ConstantBuffer::Update::kUnchanged) {
    g_profile.buffer_hits.fetch_add(1, std::memory_order_relaxed);
  } else {
    g_profile.buffer_creations.fetch_add(1, std::memory_order_relaxed);
    g_profile.buffer_creation_bytes.fetch_add(constants.upload_bytes(), std::memory_order_relaxed);
  }
  first = constants.first();
  count = constants.count();
  return constants.buffer();
}

void GuestDraw::GetVertexRanges(const D3D11Shader& vertex, const D3D11Shader* pixel,
                                const PrimitiveProcessor::ProcessingResult& primitive) {
  vertex_ranges_valid_ = {};
  // The same proof as the Vulkan native vertex streams, including exact
  // constant index offsets and power-of-two scales. Requesting a whole stream is always
  // correct; a smaller range only skips bytes the draw cannot read.
  if (!vertex.has_static_vertex_addresses() || (pixel && !pixel->vertex_bindings().empty()) ||
      primitive.host_vertex_shader_type != Shader::HostVertexShaderType::kVertex)
    return;
  uint32_t index_min = registers_[XE_GPU_REG_VGT_MIN_VTX_INDX];
  uint32_t index_max = registers_[XE_GPU_REG_VGT_MAX_VTX_INDX];
  uint32_t base = registers_[XE_GPU_REG_VGT_INDX_OFFSET];
  if (index_min > index_max || !primitive.host_draw_vertex_count)
    return;
  uint32_t first = UINT32_MAX, last = 0;
  using IndexBufferType = PrimitiveProcessor::ProcessedIndexBufferType;
  if (primitive.index_buffer_type == IndexBufferType::kNone ||
      primitive.index_buffer_type == IndexBufferType::kHostBuiltinForAuto) {
    auto bounds = draw_util::GetAutoVertexIndexBounds(primitive.guest_draw_vertex_count, base,
                                                      index_min, index_max);
    first = bounds.first;
    last = bounds.last;
  } else if (primitive.index_buffer_type == IndexBufferType::kHostConverted &&
             primitive.native_index_snapshot) {
    // The snapshot has the exact format and byte order of the converted draw.
    xenos::Endian endian = primitive.host_shader_index_endian;
    bool index_16 = primitive.host_index_format == xenos::IndexFormat::kInt16;
    auto bounds = draw_util::GetVertexIndexBounds(
        primitive.native_index_snapshot, primitive.host_draw_vertex_count, index_16, endian,
        primitive.host_primitive_reset_enabled, base, index_min, index_max);
    first = bounds.first;
    last = bounds.last;
  } else {
    return;
  }
  // The shared proof handles binary32 conversion and arithmetic bounds.
  if (first > last)
    return;
  std::array<uint64_t, 2> rejected = {};
  for (const auto& binding : vertex.vertex_bindings()) {
    if (binding.fetch_constant >= vertex_ranges_.size())
      continue;
    auto fetch = registers_.GetVertexFetch(binding.fetch_constant);
    int64_t first_word = INT64_MAX, end_word = INT64_MIN;
    bool proven = fetch.type == xenos::FetchConstantType::kVertex && fetch.size;
    for (const auto& attribute : binding.attributes) {
      const auto& instruction = attribute.fetch_instr;
      if (instruction.attributes.stride != binding.stride_words ||
          instruction.operands[1].storage_index != binding.fetch_constant) {
        proven = false;
        break;
      }
      auto indices = instruction.attributes.stride
                         ? draw_util::GetVertexFetchIndexBounds(
                               first, last, instruction.attributes.is_index_rounded,
                               attribute.index_expression.offset_constant,
                               attribute.index_expression.scale_constant,
                               [&](uint32_t index) {
                                 return std::bit_cast<float>(
                                     registers_[XE_GPU_REG_SHADER_CONSTANT_000_X + index]);
                               })
                         : draw_util::VertexFetchIndexBounds{0, 0};
      if (!indices) {
        proven = false;
        break;
      }
      uint32_t needed = xenos::GetVertexFormatNeededWords(
          instruction.attributes.data_format, instruction.result.GetUsedResultComponents());
      for (uint32_t word = 0; word < 4; ++word) {
        if (!(needed & (1u << word)))
          continue;
        int64_t offset = int64_t(instruction.attributes.offset) + word;
        first_word = std::min(first_word, indices.first * binding.stride_words + offset);
        end_word = std::max(end_word, indices.last * binding.stride_words + offset + 1);
      }
    }
    uint64_t bit = uint64_t(1) << (binding.fetch_constant & 63);
    uint32_t block = binding.fetch_constant >> 6;
    if (!proven || !draw_util::GetVertexFetchByteRange(fetch.address, first_word, end_word,
                                                       SharedMemory::kBufferSize)) {
      rejected[block] |= bit;
      vertex_ranges_valid_[block] &= ~bit;
      continue;
    }
    if (rejected[block] & bit)
      continue;
    auto& range = vertex_ranges_[binding.fetch_constant];
    if (vertex_ranges_valid_[binding.fetch_constant >> 6] & bit) {
      range.first = std::min(range.first, int32_t(first_word));
      range.second = std::max(range.second, int32_t(end_word));
    } else {
      range = {int32_t(first_word), int32_t(end_word)};
      vertex_ranges_valid_[binding.fetch_constant >> 6] |= bit;
    }
  }
}

bool GuestDraw::PrepareBindings(const D3D11Shader* shader, bool pixel_stage,
                                bool shared_memory_is_uav,
                                const DxbcShaderTranslator::SystemConstants& system_constants,
                                Bindings& output, std::string& error) {
  std::array<std::span<const uint8_t>, 4> data;
  data[0] = {reinterpret_cast<const uint8_t*>(&system_constants), sizeof(system_constants)};
  auto& floats = pixel_stage ? pixel_floats_ : vertex_floats_;
  floats.clear();
  if (shader) {
    auto& map = shader->constant_register_map();
    uint32_t base = shader->type() == xenos::ShaderType::kPixel ? XE_GPU_REG_SHADER_CONSTANT_256_X
                                                                : XE_GPU_REG_SHADER_CONSTANT_000_X;
    for (uint32_t block = 0; block < 4; ++block) {
      uint64_t remaining = map.float_bitmap[block];
      while (remaining) {
        uint32_t index = std::countr_zero(remaining);
        remaining &= remaining - 1;
        const uint32_t* value = &registers_[base + block * 256 + index * 4];
        floats.insert(floats.end(), value, value + 4);
      }
    }
  }
  if (floats.empty())
    floats.resize(4);
  data[1] = {reinterpret_cast<const uint8_t*>(floats.data()), floats.size() * 4};
  data[2] = {reinterpret_cast<const uint8_t*>(&registers_[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031]),
             (8 + 32) * sizeof(uint32_t)};
  const uint32_t* fetch_constants = !pixel_stage && native_vertex_streams_active_
                                        ? native_vertex_fetch_constants_.data()
                                        : &registers_[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0];
  data[3] = {reinterpret_cast<const uint8_t*>(fetch_constants), 32 * 6 * sizeof(uint32_t)};
  // Both stages share the other blocks, whose contents are the same for both.
  const std::array<ConstantBlock, 4> blocks = {kSystemConstants,
                                               pixel_stage ? kPixelFloats : kVertexFloats,
                                               kBoolLoop, pixel_stage ? kPixelFetch : kVertexFetch};
  for (uint32_t slot = 0; slot < data.size(); ++slot) {
    output.constant_views[slot] = UpdateConstants(
        blocks[slot], data[slot], output.constant_first[slot], output.constant_count[slot], error);
    if (!output.constant_views[slot])
      return false;
  }
  output.samplers.clear();
  output.sampler_views.clear();
  output.resources.clear();
  output.swizzles.clear();
  if (shader) {
    if (!textures_.BuildShaderBindings(shader->GetTextureBindingsAfterTranslation(),
                                       output.resources, output.swizzles, error))
      return false;
    for (const auto& binding : shader->GetSamplerBindingsAfterTranslation()) {
      auto sampler =
          samplers_.GetOrCreate(BuildGuestSamplerDescription(registers_, binding), error);
      if (!sampler)
        return false;
      output.sampler_views.push_back(sampler.Get());
      output.samplers.push_back(std::move(sampler));
    }
  }
  if (output.resources.empty())
    output.resources.push_back(memory_.raw_srv());
  if (!pixel_stage && native_vertex_streams_active_ && !native_memexport_buffer_)
    output.resources[0] = native_vertex_buffer_->raw_view();
  if (shared_memory_is_uav)
    output.resources[0] = nullptr;
  return true;
}

bool GuestDraw::PrepareNativeVertexStreams(const D3D11Shader& vertex, const D3D11Shader* pixel,
                                           const PrimitiveProcessor::ProcessingResult& primitive,
                                           bool memexport, std::string& error) {
  native_vertex_streams_active_ = false;
  native_vertex_buffer_.reset();
  native_memexport_buffer_.reset();
  native_memexport_write_.Reset();
  if (!REXCVAR_GET(d3d11_native_vertex_buffers) ||
      (memexport && (!draw_util::CanUseNativeMemExports(registers_, vertex) ||
                     (pixel && !draw_util::CanUseNativeMemExports(registers_, *pixel)))) ||
      !vertex.has_static_vertex_addresses() || (pixel && !pixel->vertex_bindings().empty()) ||
      primitive.host_vertex_shader_type != Shader::HostVertexShaderType::kVertex)
    return true;
  struct Range {
    uint32_t fetch, address, size, offset, first_word;
  };
  std::array<Range, xenos::kVertexFetchConstantCount> ranges;
  uint32_t range_count = 0, size = 64;
  bool gpu_streams = false;
  // The zero prefix also backs optional streams. The shader's zero-size clamp
  // keeps their results zero, without reading live guest address zero.
  const auto& map = vertex.constant_register_map();
  uint32_t used_textures = vertex.GetUsedTextureMaskAfterTranslation() |
                           (pixel ? pixel->GetUsedTextureMaskAfterTranslation() : 0);
  for (uint32_t block = 0; block < std::size(map.vertex_fetch_bitmap); ++block) {
    uint32_t remaining = map.vertex_fetch_bitmap[block];
    while (remaining) {
      uint32_t bit = std::countr_zero(remaining);
      remaining &= remaining - 1;
      uint32_t fetch_index = block * 32 + bit;
      if (fetch_index >= vertex_ranges_.size())
        return true;
      // Fetch constants share storage between vertex and texture fetches.
      // Never rebase a word that either stage may interpret as texture state.
      if (used_textures & (uint32_t(1) << (fetch_index / 3)))
        return true;
      auto fetch = registers_.GetVertexFetch(fetch_index);
      if (!fetch.size)
        continue;
      if (fetch.type != xenos::FetchConstantType::kVertex ||
          !(vertex_ranges_valid_[fetch_index >> 6] & (uint64_t(1) << (fetch_index & 63))))
        return true;
      const auto& range = vertex_ranges_[fetch_index];
      int64_t address = (int64_t(fetch.address) + range.first) * 4;
      uint32_t length = (range.second - range.first) << 2;
      if (address < 0 || uint64_t(address) >= SharedMemory::kBufferSize ||
          length > SharedMemory::kBufferSize - uint64_t(address) ||
          (memory_.IsRangeGpuWritten(uint32_t(address), length) &&
           !memory_.CanReadNativeResolveRange(uint32_t(address), length)))
        return true;
      uint32_t offset = (size + 15) & ~uint32_t(15);
      if (length > 128u * 1024 * 1024 - offset)
        return true;
      ranges[range_count++] = {fetch_index, uint32_t(address), length, offset,
                               uint32_t(range.first)};
      size = offset + length;
      gpu_streams |= memory_.IsRangeGpuWritten(uint32_t(address), length);
    }
  }
  if (memexport && !exports_.empty()) {
    uint32_t first = SharedMemory::kBufferSize, end = 0;
    auto include = [&](uint64_t address, uint32_t length) {
      if (!length || address >= SharedMemory::kBufferSize ||
          length > SharedMemory::kBufferSize - address)
        return false;
      first = std::min(first, uint32_t(address));
      end = std::max(end, uint32_t(address) + length);
      return true;
    };
    for (const auto& range : exports_)
      if (!include(uint64_t(range.base_address_dwords) << 2, range.size_bytes))
        return true;
    for (uint32_t i = 0; i < range_count; ++i)
      if (!include(ranges[i].address, ranges[i].size))
        return true;
    if (!memory_.CanRetainNativeResolve(first, end - first) ||
        (memory_.IsRangeGpuWritten(first, end - first) &&
         !memory_.CanReadNativeResolveRange(first, end - first)))
      return true;
    auto native = memory_.PrepareNativeResolve(first, end - first, 2, error);
    if (!native)
      return false;
    D3D11_UNORDERED_ACCESS_VIEW_DESC view = {};
    view.Format = DXGI_FORMAT_R32_TYPELESS;
    view.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    view.Buffer.NumElements = native->length >> 2;
    view.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
    if (FAILED(device_.device()->CreateUnorderedAccessView(
            native->buffer.Get(), &view, native_memexport_write_.GetAddressOf()))) {
      error = "Unable to bind resource-local shader memory exports";
      return false;
    }
    std::memcpy(native_vertex_fetch_constants_.data(),
                &registers_[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0],
                sizeof(native_vertex_fetch_constants_));
    for (uint32_t i = 0; i < range_count; ++i) {
      uint32_t& fetch = native_vertex_fetch_constants_[ranges[i].fetch * 2];
      fetch = ((fetch & ~uint32_t(3)) - native->address) | (fetch & 3);
    }
    native_memexport_buffer_ = std::move(native);
    native_vertex_streams_active_ = true;
    if (auto* diagnostics = memory_.native_diagnostics())
      diagnostics->RecordOperation(NativeGpuDiagnostics::Operation::kVertexStreams);
    return true;
  }
  std::memcpy(native_vertex_fetch_constants_.data(),
              &registers_[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0],
              sizeof(native_vertex_fetch_constants_));
  std::vector<Microsoft::WRL::ComPtr<ID3D11Resource>> gpu_sources;
  std::vector<BufferCache::GpuCopy> gpu_copies;
  std::unique_lock<std::recursive_mutex> snapshot_lock;
  if (gpu_streams)
    snapshot_lock = thread::global_critical_region::AcquireDirect();
  std::array<BufferCache::CpuRange, xenos::kVertexFetchConstantCount> cpu_ranges;
  if (!gpu_streams) {
    for (uint32_t i = 0; i < range_count; ++i) {
      const auto& range = ranges[i];
      auto bytes = memory_.BorrowCpuRange(range.address, range.size);
      if (bytes.empty())
        return true;
      cpu_ranges[i] = {bytes.data(), range.offset, range.size};
    }
    native_vertex_buffer_ =
        buffers_.GetOrCreate({cpu_ranges.data(), range_count}, size, BufferKind::kRaw, error);
    if (!native_vertex_buffer_)
      return false;
  }
  if (!native_vertex_buffer_)
    native_vertex_snapshot_.assign(size, 0);
  for (uint32_t i = 0; i < range_count; ++i) {
    const auto& range = ranges[i];
    if (native_vertex_buffer_) {
      // Trace the immutable snapshot selected from the CPU ranges.
      memory_.TraceCpuRangeRead(
          range.address, native_vertex_buffer_->cpu_bytes().subspan(range.offset, range.size));
    } else if (memory_.IsRangeGpuWritten(range.address, range.size)) {
      auto view = memory_.ReadNativeResolveRange(range.address, range.size, 2, error);
      if (!view)
        return false;
      D3D11_SHADER_RESOURCE_VIEW_DESC desc;
      view->GetDesc(&desc);
      Microsoft::WRL::ComPtr<ID3D11Resource> source;
      view->GetResource(source.GetAddressOf());
      gpu_copies.push_back({static_cast<ID3D11Buffer*>(source.Get()), desc.Buffer.FirstElement << 2,
                            range.offset, range.size});
      gpu_sources.push_back(std::move(source));
    } else if (!memory_.CopyCpuRange(range.address,
                                     {native_vertex_snapshot_.data() + range.offset, range.size})) {
      return true;
    }
    // DXBC computes byte addresses with 32-bit integer add/multiply. Subtract
    // the first proven word modulo 2^32 so every possible fetch lands in this
    // compact range; no shader or guest register is changed.
    uint32_t& fetch_word = native_vertex_fetch_constants_[range.fetch * 2];
    fetch_word = ((range.offset - (range.first_word << 2)) & ~uint32_t(3)) | (fetch_word & 3);
  }
  if (!native_vertex_buffer_) {
    if (gpu_copies.empty()) {
      native_vertex_buffer_ =
          buffers_.GetOrCreate(native_vertex_snapshot_, BufferKind::kRaw, error);
    } else {
      draws_.Invalidate();
      native_vertex_buffer_ =
          buffers_.SnapshotMixedRanges(native_vertex_snapshot_, gpu_copies, error);
    }
  }
  if (!native_vertex_buffer_)
    return false;
  native_vertex_streams_active_ = true;
  native_vertex_bytes_ += size;
  if (++native_vertex_draws_ <= 8 || !(native_vertex_draws_ & 8191)) {
    REXGPU_INFO("[d3d11-native-vertex] {} draws, {} KiB in retained native streams",
                native_vertex_draws_, native_vertex_bytes_ >> 10);
  }
  if (auto* diagnostics = memory_.native_diagnostics())
    diagnostics->RecordOperation(NativeGpuDiagnostics::Operation::kVertexStreams);
  return true;
}

bool GuestDraw::PrepareIndices(const PrimitiveProcessor::ProcessingResult& primitive,
                               DrawCommand& command, std::string& error) {
  using Type = PrimitiveProcessor::ProcessedIndexBufferType;
  switch (primitive.index_buffer_type) {
    case Type::kNone:
      return true;
    case Type::kGuestDMA: {
      uint32_t length = primitive.host_draw_vertex_count *
                        (primitive.host_index_format == xenos::IndexFormat::kInt16 ? 2 : 4);
      auto kind = primitive.host_index_format == xenos::IndexFormat::kInt16 ? BufferKind::kIndex16
                                                                            : BufferKind::kIndex32;
      if (primitive.native_index_snapshot) {
        command.indices = buffers_.GetOrCreate(
            {static_cast<const uint8_t*>(primitive.native_index_snapshot), length}, kind, error);
        break;
      }
      if (!memory_.RequestRange(primitive.guest_index_base, length, true)) {
        error = "Unable to request native GPU index data";
        return false;
      }
      draws_.Invalidate();
      command.indices = buffers_.SnapshotGpuRange(
          memory_.buffer(), primitive.guest_index_base, length,
          primitive.host_index_format == xenos::IndexFormat::kInt16 ? BufferKind::kIndex16
                                                                    : BufferKind::kIndex32,
          error);
      break;
    }
    case Type::kHostConverted:
      command.indices = primitives_.ConvertedBuffer(primitive.host_index_buffer_handle, error);
      break;
    case Type::kHostBuiltinForAuto:
    case Type::kHostBuiltinForDMA:
      command.indices = primitives_.BuiltinBuffer(primitive.host_index_format, error);
      command.first = primitives_.BuiltinFirstIndex(primitive.host_index_buffer_handle,
                                                    primitive.host_index_format);
      break;
  }
  return bool(command.indices);
}

bool GuestDraw::Submit(D3D11Shader* vertex, D3D11Shader* pixel, std::string& error,
                       bool readback_memexport) {
  error.clear();
  ProfileClock clock;
  if (!vertex) {
    error = "Native draw has no vertex shader";
    return false;
  }
  pipelines_.AnalyzeShader(*vertex);
  auto verdict = draw_util::ClassifyInvalidVertexFetch(registers_, *vertex);
  if (verdict == draw_util::InvalidVertexFetchVerdict::kVeto) {
    error = "Native draw has invalid vertex fetch constants";
    return false;
  }
  bool polygonal = draw_util::IsPrimitivePolygonal(registers_);
  bool rasterized = draw_util::IsRasterizationPotentiallyDone(registers_, polygonal);
  if (rasterized && !registers_.Get<reg::RB_SURFACE_INFO>().surface_pitch)
    return true;
  if (!rasterized ||
      registers_.Get<reg::RB_MODECONTROL>().edram_mode != xenos::EdramMode::kColorDepth)
    pixel = nullptr;
  if (pixel) {
    pipelines_.AnalyzeShader(*pixel);
    if (!draw_util::IsPixelShaderNeededWithRasterization(*pixel, registers_))
      pixel = nullptr;
  }
  bool memexport = vertex->memexport_eM_written() || (pixel && pixel->memexport_eM_written());
  if (vertex->memexport_eM_written() && device_.features().level < D3D_FEATURE_LEVEL_11_1) {
    error = "Native vertex memexport requires feature level 11.1";
    return false;
  }
  if (!rasterized && !memexport)
    return true;
  clock.Mark(ProfilePhase::kDrawSetup);
  PrimitiveProcessor::ProcessingResult primitive;
  if (!primitives_.Process(primitive, true)) {
    error = "Unable to normalize native draw primitives";
    return false;
  }
  clock.Mark(ProfilePhase::kDrawPrimitives);
  GpuSwitch(GpuCategory::kRenderTargets);
  if (!primitive.host_draw_vertex_count)
    return true;
  auto depth = draw_util::GetNormalizedDepthControl(registers_);
  uint32_t colors =
      pixel ? draw_util::GetNormalizedColorMask(registers_, pixel->writes_color_targets()) : 0;
  if (!render_targets_.Update(rasterized, depth, colors, *vertex)) {
    error = render_targets_.last_error();
    return false;
  }
  clock.Mark(ProfilePhase::kDrawRenderTargets);
  GpuSwitch(GpuCategory::kDraws);
  PipelineCache::PreparedShaders prepared;
  if (!pipelines_.PrepareShaders(*vertex, pixel, primitive.host_vertex_shader_type,
                                 primitive.host_primitive_type, depth, prepared, error))
    return false;
  clock.Mark(ProfilePhase::kDrawShaders);
  uint32_t used_textures = vertex->GetUsedTextureMaskAfterTranslation() |
                           (pixel ? pixel->GetUsedTextureMaskAfterTranslation() : 0);
  GpuSwitch(GpuCategory::kTextures);
  textures_.RequestTextures(used_textures);
  GpuSwitch(GpuCategory::kDraws);
  if (!textures_.last_error().empty()) {
    error = textures_.last_error();
    return false;
  }
  clock.Mark(ProfilePhase::kDrawTextures);
  // Use compact native snapshots when every stream and its CPU or GPU owner
  // is proven. Retained GPU versions are copied directly into the snapshot.
  // Otherwise the common page tracker preserves GPU-owned data and uploads
  // changed CPU ranges. Proven ranges still limit those compatibility uploads
  // to vertices the draw can fetch; rewritten pages can use streamed snapshots.
  auto& exports = exports_;
  exports.clear();
  if (vertex->memexport_eM_written())
    draw_util::AddMemExportRanges(registers_, *vertex, exports);
  if (pixel && pixel->memexport_eM_written())
    draw_util::AddMemExportRanges(registers_, *pixel, exports);
  if (memexport && exports.empty()) {
    error = "Native memexport destinations could not be established";
    return false;
  }
  GetVertexRanges(*vertex, pixel, primitive);
  std::unique_lock<std::recursive_mutex> native_export_lock;
  if (memexport)
    native_export_lock = thread::global_critical_region::AcquireDirect();
  {
    NativeGpuDiagnostics::RouteScope route(memory_.native_diagnostics(),
                                           NativeGpuDiagnostics::RouteSystem::kGeometry);
    if (!PrepareNativeVertexStreams(*vertex, pixel, primitive, memexport, error))
      return false;
    if (native_vertex_streams_active_)
      route.Native();
    else
      route.Retained();
    route.Finish(true);
  }
  if (!native_memexport_buffer_ && native_export_lock.owns_lock())
    native_export_lock.unlock();
  if (!native_vertex_streams_active_) {
    if (auto* diagnostics = memory_.native_diagnostics())
      diagnostics->RecordDependency(NativeGpuDiagnostics::Dependency::kVertexFallback, 0,
                                    "Direct3D 11 vertex fetch needs guest-address storage");
  }
  const auto& map = vertex->constant_register_map();
  for (uint32_t block = 0;
       !native_vertex_streams_active_ && block < std::size(map.vertex_fetch_bitmap); ++block) {
    uint32_t remaining = map.vertex_fetch_bitmap[block];
    while (remaining) {
      uint32_t bit = std::countr_zero(remaining);
      remaining &= remaining - 1;
      uint32_t fetch_constant = block * 32 + bit;
      auto fetch = registers_.GetVertexFetch(fetch_constant);
      uint32_t address = fetch.address << 2, length = fetch.size << 2;
      if (!length && verdict != draw_util::InvalidVertexFetchVerdict::kNone)
        length = 4096;
      if (fetch_constant < vertex_ranges_.size() &&
          (vertex_ranges_valid_[fetch_constant >> 6] >> (fetch_constant & 63)) & 1) {
        const auto& range = vertex_ranges_[fetch_constant];
        address = uint32_t(int64_t(address) + int64_t(range.first) * 4);
        length = (range.second - range.first) << 2;
      }
      if (!memory_.RequestRange(address, length, true)) {
        error = "Unable to request native vertex data";
        return false;
      }
    }
  }
  for (const auto& range : exports)
    if (!native_memexport_buffer_ &&
        !memory_.RequestRange(range.base_address_dwords << 2, range.size_bytes)) {
      error = "Unable to preserve native memexport destination bytes";
      return false;
    }
  clock.Mark(ProfilePhase::kDrawStreams);
  DrawCommand command;
  command.count = primitive.host_draw_vertex_count;
  if (primitive.IsTessellated()) {
    if (!pipelines_.PrepareTessellation(primitive.host_vertex_shader_type,
                                        primitive.tessellation_mode, command, error))
      return false;
  } else {
    switch (primitive.host_primitive_type) {
      case xenos::PrimitiveType::kPointList:
        command.topology = D3D11_PRIMITIVE_TOPOLOGY_POINTLIST;
        break;
      case xenos::PrimitiveType::kLineList:
        command.topology = D3D11_PRIMITIVE_TOPOLOGY_LINELIST;
        break;
      case xenos::PrimitiveType::kLineStrip:
        command.topology = D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP;
        break;
      case xenos::PrimitiveType::kTriangleList:
      case xenos::PrimitiveType::kRectangleList:
        command.topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        break;
      case xenos::PrimitiveType::kTriangleStrip:
        command.topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
        break;
      case xenos::PrimitiveType::kQuadList:
        command.topology = D3D11_PRIMITIVE_TOPOLOGY_LINELIST_ADJ;
        break;
      default:
        error = "Native draw has an unsupported normalized primitive type";
        return false;
    }
  }
  if (!PrepareIndices(primitive, command, error))
    return false;
  draw_util::ViewportInfo viewport;
  const bool original_resolution = render_targets_.IsLastUpdateOriginalResolution();
  draw_util::GetHostViewportInfo(registers_,
                                 original_resolution ? 1u : textures_.draw_resolution_scale_x(),
                                 original_resolution ? 1u : textures_.draw_resolution_scale_y(),
                                 true, D3D11_VIEWPORT_BOUNDS_MAX, D3D11_VIEWPORT_BOUNDS_MAX, false,
                                 depth, render_targets_.depth_float24_convert_in_pixel_shader(),
                                 true, pixel && pixel->writes_depth(), viewport);
  DxbcShaderTranslator::SystemConstants constants = {};
  BuildGuestSystemConstants(constants, registers_, textures_, render_targets_.gamma_as_unorm16(),
                            memexport, polygonal, primitive.line_loop_closing_index,
                            primitive.host_shader_index_endian, viewport, used_textures, depth,
                            colors, original_resolution);
  constants.native_memexport_base =
      native_memexport_buffer_ ? native_memexport_buffer_->address : 0;
  auto& vertex_bindings = vertex_bindings_;
  auto& pixel_bindings = pixel_bindings_;
  if (!PrepareBindings(vertex, false, memexport, constants, vertex_bindings, error) ||
      !PrepareBindings(pixel, true, memexport, constants, pixel_bindings, error))
    return false;
  clock.Mark(ProfilePhase::kDrawBindings);
  // Final shader variants see the actual image channel mappings. Preparation
  // and texture conversion precede the native draw's last resource bindings.
  if (!pipelines_.PrepareDraw(
          *vertex, pixel, primitive.host_vertex_shader_type, primitive.host_primitive_type, depth,
          colors, render_targets_.GetLastUpdateBoundRenderTargets(), command, viewport, prepared,
          error, vertex_bindings.swizzles, pixel_bindings.swizzles, original_resolution))
    return false;
  clock.Mark(ProfilePhase::kDrawPipelines);
  command.bindings[size_t(DrawStage::kVertex)] = {
      vertex_bindings.constant_views, vertex_bindings.resources, vertex_bindings.sampler_views};
  command.bindings[size_t(DrawStage::kPixel)] = {
      pixel_bindings.constant_views, pixel_bindings.resources, pixel_bindings.sampler_views};
  if (primitive.IsTessellated()) {
    command.bindings[size_t(DrawStage::kHull)] = {vertex_bindings.constant_views, {}, {}};
    command.bindings[size_t(DrawStage::kDomain)] = command.bindings[size_t(DrawStage::kVertex)];
    command.bindings[size_t(DrawStage::kVertex)] = {vertex_bindings.constant_views, {}, {}};
  }
  command.bindings[size_t(DrawStage::kGeometry)] = {vertex_bindings.constant_views, {}, {}};
  if (constant_ranges_) {
    for (size_t stage = 0; stage < command.bindings.size(); ++stage) {
      auto& binding = command.bindings[stage];
      if (binding.constants.empty())
        continue;
      const auto& source = stage == size_t(DrawStage::kPixel) ? pixel_bindings : vertex_bindings;
      binding.constant_first = source.constant_first;
      binding.constant_count = source.constant_count;
    }
  }
  render_targets_.BindRenderTargets(command);
  std::array<ID3D11UnorderedAccessView*, 1> unordered = {
      native_memexport_buffer_ ? native_memexport_write_.Get() : memory_.raw_uav()};
  if (memexport)
    command.unordered_access = unordered;
  command.discard_rasterization =
      !rasterized || verdict == draw_util::InvalidVertexFetchVerdict::kPrimeWithoutRasterization;
  if (!draws_.Draw(command, error))
    return false;
  clock.Mark(ProfilePhase::kDrawSubmit);
  if (native_memexport_buffer_)
    memory_.BeginNativeResolveWrite(native_memexport_buffer_);
  for (const auto& range : exports)
    memory_.RangeWrittenByGpu(range.base_address_dwords << 2, range.size_bytes);
  if (native_memexport_buffer_ && !memory_.EndNativeResolveWrite()) {
    error = "Unable to retain native shader memory-export ownership";
    return false;
  }
  if (native_export_lock.owns_lock())
    native_export_lock.unlock();
  for (const auto& range : exports) {
    if (readback_memexport &&
        !memory_.ReadbackCpuRange(range.base_address_dwords << 2, range.size_bytes, error))
      return false;
  }
  clock.Mark(ProfilePhase::kDrawReadback);
  return true;
}

void GuestDraw::ClearCache() {
  draws_.Invalidate();
  buffers_.Clear();
  native_vertex_buffer_.reset();
  native_memexport_buffer_.reset();
  native_memexport_write_.Reset();
  native_vertex_snapshot_.clear();
  native_vertex_streams_active_ = false;
  samplers_.Clear();
  for (auto& block : constants_)
    block.clear();
  constant_ring_bytes_ = 0;
}

}  // namespace rex::graphics::d3d11
