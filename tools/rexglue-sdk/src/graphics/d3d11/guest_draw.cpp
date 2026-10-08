#include <rex/graphics/d3d11/guest_draw.h>

#include <rex/graphics/d3d11/profile.h>
#include <rex/graphics/util/bytes_equal.h>
#include <rex/graphics/util/vertex_index_bounds.h>

#include <algorithm>
#include <bit>
#include <cstring>

namespace rex::graphics::d3d11 {

ID3D11Buffer* GuestDraw::UpdateConstants(ConstantBlock block, std::span<const uint8_t> data,
                                         std::string& error) {
  uint32_t size = (uint32_t(data.size()) + 15) & ~uint32_t(15);
  if (!size || size > D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT * 16) {
    error = "Invalid native constant buffer size";
    return nullptr;
  }
  auto& constants = constants_[block][size];
  if (constants.buffer && constants.contents.size() == data.size() &&
      draw_util::BytesEqual(constants.contents.data(), data.data(), data.size())) {
    g_profile.buffer_hits.fetch_add(1, std::memory_order_relaxed);
    return constants.buffer.Get();
  }
  auto* context = device_.context();
  if (!constants.buffer) {
    D3D11_BUFFER_DESC desc = {};
    desc.ByteWidth = size;
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device_.device()->CreateBuffer(&desc, nullptr, constants.buffer.GetAddressOf()))) {
      error = "Unable to create a native constant buffer";
      return nullptr;
    }
  }
  D3D11_MAPPED_SUBRESOURCE mapped = {};
  if (FAILED(context->Map(constants.buffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
    error = "Unable to update a native constant buffer";
    return nullptr;
  }
  std::memcpy(mapped.pData, data.data(), data.size());
  std::memset(static_cast<uint8_t*>(mapped.pData) + data.size(), 0, size - data.size());
  context->Unmap(constants.buffer.Get(), 0);
  constants.contents.assign(data.begin(), data.end());
  g_profile.buffer_creations.fetch_add(1, std::memory_order_relaxed);
  g_profile.buffer_creation_bytes.fetch_add(size, std::memory_order_relaxed);
  return constants.buffer.Get();
}

void GuestDraw::GetVertexRanges(const D3D11Shader& vertex, const D3D11Shader* pixel,
                                const PrimitiveProcessor::ProcessingResult& primitive) {
  vertex_ranges_valid_ = {};
  // The same proof as the Vulkan native vertex streams, for attributes that
  // fetch at the vertex index itself. Requesting a whole stream is always
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
  if (primitive.index_buffer_type == IndexBufferType::kNone) {
    uint32_t first_index = base & xenos::kVertexIndexMask;
    // A wrapped range is not contiguous after the shader's mask.
    if (uint64_t(first_index) + primitive.host_draw_vertex_count - 1 > xenos::kVertexIndexMask)
      return;
    // The shader's unsigned add, 24-bit mask and clamp keep the order.
    for (uint32_t index : {0u, primitive.host_draw_vertex_count - 1}) {
      index = std::clamp((index + base) & xenos::kVertexIndexMask, index_min, index_max);
      first = std::min(first, index);
      last = std::max(last, index);
    }
  } else if (primitive.index_buffer_type == IndexBufferType::kHostConverted &&
             primitive.native_index_snapshot) {
    // The snapshot holds the raw guest indices of an ordinary indexed draw.
    xenos::Endian endian = registers_.Get<reg::VGT_DMA_SIZE>().swap_mode;
    bool index_16 = primitive.host_index_format == xenos::IndexFormat::kInt16;
    if (index_16 && endian != xenos::Endian::kNone && endian != xenos::Endian::k8in16)
      endian = endian == xenos::Endian::k8in32 ? xenos::Endian::k8in16 : xenos::Endian::kNone;
    auto bounds = draw_util::GetVertexIndexBounds(
        primitive.native_index_snapshot, primitive.host_draw_vertex_count, index_16, endian,
        primitive.host_primitive_reset_enabled, base, index_min, index_max);
    first = bounds.first;
    last = bounds.last;
  } else {
    return;
  }
  // The vertex index is a float in the shader; it stays exact below 2^22.
  if (first == UINT32_MAX || last >= (1u << 22))
    return;
  for (const auto& binding : vertex.vertex_bindings()) {
    if (binding.fetch_constant >= vertex_ranges_.size())
      continue;
    auto fetch = registers_.GetVertexFetch(binding.fetch_constant);
    int64_t first_word = INT64_MAX, end_word = 0;
    bool proven = fetch.type == xenos::FetchConstantType::kVertex && fetch.size;
    for (const auto& attribute : binding.attributes) {
      const auto& instruction = attribute.fetch_instr;
      if (instruction.attributes.stride != binding.stride_words ||
          instruction.operands[1].storage_index != binding.fetch_constant ||
          attribute.index_expression.offset_constant != UINT32_MAX ||
          attribute.index_expression.scale_constant != UINT32_MAX) {
        proven = false;
        break;
      }
      uint32_t needed = xenos::GetVertexFormatNeededWords(
          instruction.attributes.data_format, instruction.result.GetUsedResultComponents());
      for (uint32_t word = 0; word < 4; ++word) {
        if (!(needed & (1u << word)))
          continue;
        int64_t offset = int64_t(instruction.attributes.offset) + word;
        first_word = std::min(first_word, int64_t(first) * binding.stride_words + offset);
        end_word = std::max(end_word, int64_t(last) * binding.stride_words + offset + 1);
      }
    }
    if (!proven || first_word < 0 || end_word <= first_word || end_word > int64_t(fetch.size))
      continue;
    uint64_t bit = uint64_t(1) << (binding.fetch_constant & 63);
    auto& range = vertex_ranges_[binding.fetch_constant];
    if (vertex_ranges_valid_[binding.fetch_constant >> 6] & bit) {
      range.first = std::min(range.first, uint32_t(first_word));
      range.second = std::max(range.second, uint32_t(end_word));
    } else {
      range = {uint32_t(first_word), uint32_t(end_word)};
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
  data[3] = {reinterpret_cast<const uint8_t*>(&registers_[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0]),
             32 * 6 * sizeof(uint32_t)};
  // Both stages share the other blocks, whose contents are the same for both.
  const std::array<ConstantBlock, 4> blocks = {
      kSystemConstants, pixel_stage ? kPixelFloats : kVertexFloats, kBoolLoop, kFetch};
  for (uint32_t slot = 0; slot < data.size(); ++slot) {
    output.constant_views[slot] = UpdateConstants(blocks[slot], data[slot], error);
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
  if (shared_memory_is_uav)
    output.resources[0] = nullptr;
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

bool GuestDraw::Submit(D3D11Shader* vertex, D3D11Shader* pixel, std::string& error) {
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
  // Request every used stream. The common page tracker preserves GPU-owned
  // data while uploading changed CPU-owned ranges; optional zero streams still
  // receive valid backing before the shader can fetch through address zero.
  // Vertex data is only read by this draw, so pages the game rewrites every
  // frame may be uploaded on each use without being watched. Streams with a
  // proven range request only the vertices the draw can fetch.
  GetVertexRanges(*vertex, pixel, primitive);
  const auto& map = vertex->constant_register_map();
  for (uint32_t block = 0; block < std::size(map.vertex_fetch_bitmap); ++block) {
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
        address += range.first << 2;
        length = (range.second - range.first) << 2;
      }
      if (!memory_.RequestRange(address, length, true)) {
        error = "Unable to request native vertex data";
        return false;
      }
    }
  }
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
  for (const auto& range : exports)
    if (!memory_.RequestRange(range.base_address_dwords << 2, range.size_bytes)) {
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
  draw_util::GetHostViewportInfo(registers_, textures_.draw_resolution_scale_x(),
                                 textures_.draw_resolution_scale_y(), true,
                                 D3D11_VIEWPORT_BOUNDS_MAX, D3D11_VIEWPORT_BOUNDS_MAX, false, depth,
                                 render_targets_.depth_float24_convert_in_pixel_shader(), true,
                                 pixel && pixel->writes_depth(), viewport);
  DxbcShaderTranslator::SystemConstants constants = {};
  BuildGuestSystemConstants(constants, registers_, textures_, render_targets_.gamma_as_unorm16(),
                            memexport, polygonal, primitive.line_loop_closing_index,
                            primitive.host_shader_index_endian, viewport, used_textures, depth,
                            colors);
  auto& vertex_bindings = vertex_bindings_;
  auto& pixel_bindings = pixel_bindings_;
  if (!PrepareBindings(vertex, false, memexport, constants, vertex_bindings, error) ||
      !PrepareBindings(pixel, true, memexport, constants, pixel_bindings, error))
    return false;
  clock.Mark(ProfilePhase::kDrawBindings);
  // Final shader variants see the actual image channel mappings. Preparation
  // and texture conversion precede the native draw's last resource bindings.
  if (!pipelines_.PrepareDraw(*vertex, pixel, primitive.host_vertex_shader_type,
                              primitive.host_primitive_type, depth, colors,
                              render_targets_.GetLastUpdateBoundRenderTargets(), command, viewport,
                              prepared, error, vertex_bindings.swizzles, pixel_bindings.swizzles))
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
  render_targets_.BindRenderTargets(command);
  std::array<ID3D11UnorderedAccessView*, 1> unordered = {memory_.raw_uav()};
  if (memexport)
    command.unordered_access = unordered;
  command.discard_rasterization =
      !rasterized || verdict == draw_util::InvalidVertexFetchVerdict::kPrimeWithoutRasterization;
  if (!draws_.Draw(command, error))
    return false;
  clock.Mark(ProfilePhase::kDrawSubmit);
  for (const auto& range : exports) {
    memory_.RangeWrittenByGpu(range.base_address_dwords << 2, range.size_bytes);
    if (!memory_.ReadbackCpuRange(range.base_address_dwords << 2, range.size_bytes, error))
      return false;
  }
  clock.Mark(ProfilePhase::kDrawReadback);
  return true;
}

void GuestDraw::ClearCache() {
  draws_.Invalidate();
  buffers_.Clear();
  samplers_.Clear();
  for (auto& block : constants_)
    block.clear();
}

}  // namespace rex::graphics::d3d11
