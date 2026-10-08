#pragma once

#include <array>

#include <rex/graphics/d3d11/buffer_cache.h>
#include <rex/graphics/primitive_processor.h>

namespace rex::graphics::d3d11 {

class D3D11PrimitiveProcessor final : public PrimitiveProcessor {
 public:
  D3D11PrimitiveProcessor(const RegisterFile& registers, memory::Memory& memory, TraceWriter& trace,
                          SharedMemory& shared_memory, ui::d3d11::D3D11Device& device)
      : PrimitiveProcessor(registers, memory, trace, shared_memory), buffers_(device) {}
  bool Initialize();
  void EndFrame();
  void ClearCache();
  std::shared_ptr<const BufferVersion> ConvertedBuffer(size_t handle, std::string& error);
  std::shared_ptr<const BufferVersion> BuiltinBuffer(xenos::IndexFormat format, std::string& error);
  uint32_t BuiltinFirstIndex(size_t handle, xenos::IndexFormat format) const {
    return uint32_t(GetBuiltinIndexBufferOffsetBytes(handle) >>
                    (format == xenos::IndexFormat::kInt16 ? 1 : 2));
  }

 protected:
  bool InitializeBuiltinIndexBuffer(size_t size_bytes,
                                    std::function<void(void*)> fill_callback) override;
  void* RequestHostConvertedIndexBufferForCurrentFrame(xenos::IndexFormat format,
                                                       uint32_t index_count, bool coalign_for_simd,
                                                       uint32_t coalignment_original_address,
                                                       size_t& backend_handle_out) override;

 private:
  struct Converted {
    xenos::IndexFormat format;
    uint32_t offset, length;
    std::vector<uint8_t> bytes;
    std::shared_ptr<const BufferVersion> version;
  };
  BufferCache buffers_;
  std::vector<uint8_t> builtin_data_;
  std::array<std::shared_ptr<const BufferVersion>, 2> builtin_versions_;
  std::deque<Converted> converted_;
};

}  // namespace rex::graphics::d3d11
