#include <rex/graphics/native_gpu_calls.h>
#include <rex/graphics/native_gpu_state.h>
#include <rex/graphics/native_gpu_coherency.h>
#include <rex/graphics/native_gpu_program.h>
#include <rex/graphics/native_gpu_predication.h>
#include <rex/graphics/native_gpu_ports.h>
#include <rex/graphics/native_gpu_sync.h>
#include <rex/graphics/native_gpu_indirect.h>
#include <rex/graphics/native_gpu_shader_reference.h>
#include <rex/graphics/native_gpu_direct.h>
#include <rex/graphics/native_gpu_presentation.h>

#include <atomic>

namespace rex::graphics::native_gpu_calls {
namespace {
std::atomic<bool> enabled{false};
}

bool Enabled() {
  return enabled.load(std::memory_order_acquire);
}

uint32_t Capabilities() {
  return Enabled()
             ? kVerticesCapability | kIndexedVerticesCapability | kRangeDrawCapability |
                   native_gpu_state::kCapability | native_gpu_coherency::kCapability |
                   native_gpu_program::kCapability | native_gpu_predication::kCapability |
                   native_gpu_ports::kCapability | native_gpu_sync::kCapability |
                   native_gpu_indirect::kCapability | native_gpu_shader_reference::kCapability |
                   native_gpu_predication::kRangeCapability |
                   native_gpu_indirect::kTypedCapability | native_gpu_direct::kCapability |
                   native_gpu_indirect::kBufferCapability | native_gpu_presentation::kCapability
             : 0;
}
void SetEnabled(bool value) {
  if (value) {
    native_gpu_state::ResetCounts();
    native_gpu_coherency::ResetCounts();
    native_gpu_program::ResetCounts();
    native_gpu_predication::ResetCounts();
    native_gpu_ports::ResetCounts();
    native_gpu_sync::ResetCounts();
    native_gpu_indirect::ResetCounts();
    native_gpu_shader_reference::ResetCounts();
    native_gpu_direct::ResetCounts();
  }
  enabled.store(value, std::memory_order_release);
}

}  // namespace rex::graphics::native_gpu_calls

namespace rex::graphics::native_gpu_state {
namespace {
std::atomic<uint64_t> calls{0}, registers{0};
}

void RecordCall(uint32_t register_count) {
  calls.fetch_add(1, std::memory_order_relaxed);
  registers.fetch_add(register_count, std::memory_order_relaxed);
}
uint64_t CallCount() {
  return calls.load(std::memory_order_relaxed);
}
uint64_t RegisterCount() {
  return registers.load(std::memory_order_relaxed);
}
void ResetCounts() {
  calls.store(0, std::memory_order_relaxed);
  registers.store(0, std::memory_order_relaxed);
}
}  // namespace rex::graphics::native_gpu_state

namespace rex::graphics::native_gpu_coherency {
namespace {
std::atomic<uint64_t> calls{0};
}
void RecordCall() {
  calls.fetch_add(1, std::memory_order_relaxed);
}
uint64_t CallCount() {
  return calls.load(std::memory_order_relaxed);
}
void ResetCounts() {
  calls.store(0, std::memory_order_relaxed);
}
}  // namespace rex::graphics::native_gpu_coherency

namespace rex::graphics::native_gpu_program {
namespace {
std::atomic<uint64_t> shaders{0}, invalidations{0};
}
void RecordShader() {
  shaders.fetch_add(1, std::memory_order_relaxed);
}
void RecordInvalidation() {
  invalidations.fetch_add(1, std::memory_order_relaxed);
}
uint64_t ShaderCount() {
  return shaders.load(std::memory_order_relaxed);
}
uint64_t InvalidationCount() {
  return invalidations.load(std::memory_order_relaxed);
}
void ResetCounts() {
  shaders.store(0, std::memory_order_relaxed);
  invalidations.store(0, std::memory_order_relaxed);
}
}  // namespace rex::graphics::native_gpu_program

namespace rex::graphics::native_gpu_predication {
namespace {
std::atomic<uint64_t> masks{0}, states{0}, skipped{0};
}
void RecordMask() {
  masks.fetch_add(1, std::memory_order_relaxed);
}
void RecordState(bool applied) {
  (applied ? states : skipped).fetch_add(1, std::memory_order_relaxed);
}
uint64_t MaskCount() {
  return masks.load(std::memory_order_relaxed);
}
uint64_t StateCount() {
  return states.load(std::memory_order_relaxed);
}
uint64_t SkippedStateCount() {
  return skipped.load(std::memory_order_relaxed);
}
void ResetCounts() {
  masks.store(0, std::memory_order_relaxed);
  states.store(0, std::memory_order_relaxed);
  skipped.store(0, std::memory_order_relaxed);
}
}  // namespace rex::graphics::native_gpu_predication

namespace rex::graphics::native_gpu_ports {
namespace {
std::atomic<uint64_t> calls{0}, writes{0};
}
void RecordCall(uint32_t count) {
  calls.fetch_add(1, std::memory_order_relaxed);
  writes.fetch_add(count, std::memory_order_relaxed);
}
uint64_t CallCount() {
  return calls.load(std::memory_order_relaxed);
}
uint64_t WriteCount() {
  return writes.load(std::memory_order_relaxed);
}
void ResetCounts() {
  calls.store(0, std::memory_order_relaxed);
  writes.store(0, std::memory_order_relaxed);
}
}  // namespace rex::graphics::native_gpu_ports

namespace rex::graphics::native_gpu_sync {
namespace {
std::atomic<uint64_t> coherency{0}, events{0};
}
void RecordCoherency() {
  coherency.fetch_add(1, std::memory_order_relaxed);
}
void RecordEvent() {
  events.fetch_add(1, std::memory_order_relaxed);
}
uint64_t CoherencyCount() {
  return coherency.load(std::memory_order_relaxed);
}
uint64_t EventCount() {
  return events.load(std::memory_order_relaxed);
}
void ResetCounts() {
  coherency.store(0, std::memory_order_relaxed);
  events.store(0, std::memory_order_relaxed);
}
}  // namespace rex::graphics::native_gpu_sync

namespace rex::graphics::native_gpu_indirect {
namespace {
std::atomic<uint64_t> calls{0}, words{0}, typed_calls{0};
std::atomic<uint64_t> buffer_calls{0}, buffer_words{0};
}
void RecordCall(uint32_t count) {
  calls.fetch_add(1, std::memory_order_relaxed);
  words.fetch_add(count, std::memory_order_relaxed);
}
void RecordTypedCall(uint32_t count) {
  RecordCall(count);
  typed_calls.fetch_add(1, std::memory_order_relaxed);
}
uint64_t TypedCallCount() {
  return typed_calls.load(std::memory_order_relaxed);
}
uint64_t CallCount() {
  return calls.load(std::memory_order_relaxed);
}
uint64_t WordCount() {
  return words.load(std::memory_order_relaxed);
}
void RecordBufferCall(uint32_t count) {
  buffer_calls.fetch_add(1, std::memory_order_relaxed);
  buffer_words.fetch_add(count, std::memory_order_relaxed);
}
uint64_t BufferCallCount() {
  return buffer_calls.load(std::memory_order_relaxed);
}
uint64_t BufferWordCount() {
  return buffer_words.load(std::memory_order_relaxed);
}
void ResetCounts() {
  calls.store(0, std::memory_order_relaxed);
  words.store(0, std::memory_order_relaxed);
  typed_calls.store(0, std::memory_order_relaxed);
  buffer_calls.store(0, std::memory_order_relaxed);
  buffer_words.store(0, std::memory_order_relaxed);
}
}  // namespace rex::graphics::native_gpu_indirect

namespace rex::graphics::native_gpu_shader_reference {
namespace {
std::atomic<uint64_t> calls{0}, skipped{0};
}
void RecordCall() {
  calls.fetch_add(1, std::memory_order_relaxed);
}
void RecordSkip() {
  skipped.fetch_add(1, std::memory_order_relaxed);
}
uint64_t CallCount() {
  return calls.load(std::memory_order_relaxed);
}
uint64_t SkipCount() {
  return skipped.load(std::memory_order_relaxed);
}
void ResetCounts() {
  calls.store(0, std::memory_order_relaxed);
  skipped.store(0, std::memory_order_relaxed);
}
}  // namespace rex::graphics::native_gpu_shader_reference

namespace rex::graphics::native_gpu_direct {
namespace {
std::atomic<uint64_t> calls{0}, skipped{0};
}
void RecordCall() {
  calls.fetch_add(1, std::memory_order_relaxed);
}
void RecordSkip() {
  skipped.fetch_add(1, std::memory_order_relaxed);
}
uint64_t CallCount() {
  return calls.load(std::memory_order_relaxed);
}
uint64_t SkipCount() {
  return skipped.load(std::memory_order_relaxed);
}
void ResetCounts() {
  calls.store(0, std::memory_order_relaxed);
  skipped.store(0, std::memory_order_relaxed);
}
}  // namespace rex::graphics::native_gpu_direct
