#include <rex/graphics/d3d11/primitive_processor.h>

namespace rex::graphics::d3d11 {

bool D3D11PrimitiveProcessor::Initialize() {
  // Native geometry shaders handle points, rectangles and quads. The common
  // processor normalizes triangle fans, line loops, endian and restart indices.
  return InitializeCommon(true, false, false, true, true, true);
}

bool D3D11PrimitiveProcessor::InitializeBuiltinIndexBuffer(
    size_t size_bytes, std::function<void(void*)> fill_callback) {
  if (!size_bytes || size_bytes > 128 * 1024 * 1024)
    return false;
  builtin_data_.resize(size_bytes);
  fill_callback(builtin_data_.data());
  return true;
}

void* D3D11PrimitiveProcessor::RequestHostConvertedIndexBufferForCurrentFrame(
    xenos::IndexFormat format, uint32_t index_count, bool coalign_for_simd,
    uint32_t coalignment_original_address, size_t& backend_handle_out) {
  uint64_t length = uint64_t(index_count) * (format == xenos::IndexFormat::kInt16 ? 2 : 4);
  if (!length || length > 128 * 1024 * 1024)
    return nullptr;
  Converted indices;
  indices.format = format;
  indices.length = uint32_t(length);
  indices.bytes.resize(size_t(length) + XE_GPU_PRIMITIVE_PROCESSOR_SIMD_SIZE);
  indices.offset =
      coalign_for_simd
          ? uint32_t(GetSimdCoalignmentOffset(indices.bytes.data(), coalignment_original_address))
          : 0;
  backend_handle_out = converted_.size();
  converted_.push_back(std::move(indices));
  auto& result = converted_.back();
  return result.bytes.data() + result.offset;
}

std::shared_ptr<const BufferVersion> D3D11PrimitiveProcessor::ConvertedBuffer(size_t handle,
                                                                              std::string& error) {
  if (handle >= converted_.size()) {
    error = "Native converted index handle is invalid";
    return nullptr;
  }
  auto& indices = converted_[handle];
  if (!indices.version)
    indices.version = buffers_.GetOrCreate(
        {indices.bytes.data() + indices.offset, indices.length},
        indices.format == xenos::IndexFormat::kInt16 ? BufferKind::kIndex16 : BufferKind::kIndex32,
        error);
  return indices.version;
}

std::shared_ptr<const BufferVersion> D3D11PrimitiveProcessor::BuiltinBuffer(
    xenos::IndexFormat format, std::string& error) {
  uint32_t index = format == xenos::IndexFormat::kInt16 ? 0 : 1;
  auto& version = builtin_versions_[index];
  if (!version)
    version = buffers_.GetOrCreate(builtin_data_,
                                   index ? BufferKind::kIndex32 : BufferKind::kIndex16, error);
  return version;
}

void D3D11PrimitiveProcessor::EndFrame() {
  ClearPerFrameCache();
  converted_.clear();
}

void D3D11PrimitiveProcessor::ClearCache() {
  EndFrame();
  buffers_.Clear();
  // Builtin versions remain pinned and immutable for the processor lifetime.
}

}  // namespace rex::graphics::d3d11
