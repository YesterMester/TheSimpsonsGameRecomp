// Execute the shared runtime decoder against real guest physical memory.
// Backend-only operations throw: this checks command traversal, not rendering.
#include <rex/graphics/command_processor.h>
#include <rex/graphics/graphics_system.h>
#include <rex/graphics/native_gpu_indirect.h>
#include <rex/graphics/native_gpu_predication.h>
#include <rex/graphics/native_gpu_state.h>
#include <rex/graphics/packet_disassembler.h>
#include <rex/logging.h>
#include <rex/perf/counter.h>
#include <rex/system/xmemory.h>

#include <array>
#include <bit>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <vector>

namespace gfx = rex::graphics;
namespace indirect = gfx::native_gpu_indirect;
void Require(bool value, const char* message) {
  if (!value)
    throw std::runtime_error(message);
}
class ContractGraphics final : public gfx::GraphicsSystem {
 public:
  explicit ContractGraphics(rex::memory::Memory* memory) { memory_ = memory; }
  std::string name() const override { return "Command contract"; }

 private:
  void CreateProvider(bool) override { throw std::runtime_error("Unexpected GPU provider"); }
  std::unique_ptr<gfx::CommandProcessor> CreateCommandProcessor() override {
    throw std::runtime_error("Unexpected GPU worker");
  }
};

class ContractProcessor final : public gfx::CommandProcessor {
 public:
  explicit ContractProcessor(ContractGraphics* system) : CommandProcessor(system, nullptr) {}
  bool Run(uint32_t address, uint32_t words) {
    rex::memory::RingBuffer reader(memory_->TranslatePhysical(address), words * 4);
    reader.set_write_offset(words * 4);
    return ExecutePacket(&reader);
  }
  bool BeginTrace(const std::filesystem::path& path, uint32_t address, uint32_t words) {
    if (!trace_writer_.Open(path, 0x45410809))
      return false;
    trace_writer_.WritePrimaryBufferStart(address, words);
    return true;
  }
  void EndTrace() {
    trace_writer_.WritePrimaryBufferEnd();
    trace_writer_.Close();
  }
  void IssueSwap(uint32_t, uint32_t, uint32_t) override { Unexpected(); }
  void TracePlaybackWroteMemory(uint32_t, uint32_t) override { Unexpected(); }
  void RestoreEdramSnapshot(const void*) override { Unexpected(); }

 private:
  [[noreturn]] static void Unexpected() { throw std::runtime_error("Unexpected backend work"); }
  bool SetupContext() override { Unexpected(); }
  void ShutdownContext() override { Unexpected(); }
  gfx::Shader* LoadShader(gfx::xenos::ShaderType, uint32_t, const uint32_t*, uint32_t) override {
    Unexpected();
  }
  bool IssueDraw(gfx::xenos::PrimitiveType, uint32_t, IndexBufferInfo*, bool) override {
    Unexpected();
  }
  bool IssueCopy() override { Unexpected(); }
};

int main(int argc, char** argv) {
  try {
    struct RuntimeProfiling {
      RuntimeProfiling() { rex::perf::Profiler::Startup(); }
      ~RuntimeProfiling() { rex::perf::Profiler::Shutdown(); }
    } profiling;
    Require(argc == 2, "usage: native_gpu_buffer_consumer_check output_directory");
    const std::filesystem::path output = argv[1];
    std::filesystem::create_directories(output);
    rex::InitLogging((output / "runtime.log").string().c_str(), spdlog::level::info);
    rex::memory::Memory memory;
    Require(memory.Initialize(), "Could not initialize real guest memory");
    ContractGraphics graphics(&memory);
    ContractProcessor processor(&graphics);
    const auto store = [&](uint32_t address, std::initializer_list<uint32_t> words) {
      auto* data = memory.TranslatePhysical<uint32_t*>(address);
      for (uint32_t word : words)
        *data++ = std::byteswap(word);
    };
    uint32_t state_header;
    Require(gfx::native_gpu_state::EncodeHeader(0x2208, 1, state_header), "Bad test state");
    gfx::native_gpu_calls::SetEnabled(true);
    Require(gfx::native_gpu_calls::Capabilities() & indirect::kBufferCapability,
            "Runtime did not advertise its buffer consumer");
    uint64_t comparisons = 0, nested = 0, malformed = 0;
    for (bool native : {false, true}) {
      const uint32_t header = native ? indirect::kBufferHeader : 0xC0013F00u;
      for (uint32_t version = 0; version < 8; ++version) {
        // The child is modified after writing the descriptor. The command
        // thread must read its current contents, including at the same address.
        store(0x10000, {header, 0x20000, 2});
        store(0x20000, {state_header, 0x11111111});
        store(0x20004, {0x22000000u + version});
        const uint64_t epoch = processor.guest_progress_epoch();
        Require(processor.Run(0x10000, 3) &&
                    graphics.register_file()->values[0x2208] == 0x22000000u + version &&
                    processor.guest_progress_epoch() == epoch,
                "Buffer traversal changed deferred source reads, registers or progress epoch");
        ++comparisons;
      }
      store(0x10000, {header, 0x20000, 3});
      store(0x20000, {header, 0x30000, 2});
      store(0x30000, {state_header, 0x12345678});
      Require(processor.BeginTrace(output / (native ? "native.xtr" : "legacy.xtr"), 0x10000, 3),
              "Could not start nested buffer trace");
      Require(processor.Run(0x10000, 3) && graphics.register_file()->values[0x2208] == 0x12345678,
              "Nested buffer traversal lost its child state");
      processor.EndTrace();
      ++nested;
    }
    const uint64_t calls = indirect::BufferCallCount(), words = indirect::BufferWordCount();
    Require(calls == 10 && words == 21, "Native buffer accounting changed constants counters");
    Require(!indirect::CallCount() && !indirect::TypedCallCount(),
            "Native command buffers were counted as constant loads");
    for (const indirect::BufferCall call : std::array{
             indirect::BufferCall{3, 1}, indirect::BufferCall{0x20000000, 1},
             indirect::BufferCall{0x1FFFFFFC, 2}, indirect::BufferCall{0x20000, 0},
             indirect::BufferCall{0x20000, 0x100000}}) {
      store(0x10000, {indirect::kBufferHeader, call.address, call.count});
      Require(!processor.Run(0x10000, 3), "Invalid native buffer arguments were accepted");
      ++malformed;
    }
    for (uint32_t header : {indirect::kBufferHeader ^ 1u, indirect::kBufferHeader ^ 2u,
                            indirect::kBufferHeader ^ 0x10000u}) {
      store(0x10000, {header, 0x20000, 2});
      Require(!processor.Run(0x10000, 3), "Malformed native buffer header was accepted");
      ++malformed;
    }
    Require(indirect::BufferCallCount() == calls && indirect::BufferWordCount() == words,
            "Rejected buffer changed execution counters");
    store(0x10000, {indirect::kBufferHeader, 0x20000, 2});
    gfx::PacketInfo info;
    Require(gfx::PacketDisassembler::DisasmPacket(memory.TranslatePhysical(0x10000), &info) &&
                info.type_info && std::string(info.type_info->name) == "NATIVE_COMMAND_BUFFER" &&
                info.actions.empty(),
            "Buffer disassembly read a constant payload or invented register writes");
    // Buffer transport remains unconditional when draw bins are unselected.
    store(0x10000, {gfx::native_gpu_predication::kMaskLowHeader, 0});
    Require(processor.Run(0x10000, 2), "Could not clear draw bins");
    store(0x10000, {indirect::kBufferHeader, 0x20000, 2});
    store(0x20000, {state_header, 0x87654321});
    Require(processor.Run(0x10000, 3) && graphics.register_file()->values[0x2208] == 0x87654321,
            "A draw predicate skipped unconditional command-buffer state");
    gfx::native_gpu_calls::SetEnabled(false);
    std::printf("{\"passed\":true,\"deferred_source_comparisons\":%llu,"
                "\"nested_buffers\":%llu,\"rejected_records\":%llu,"
                "\"shared_runtime_executed\":true,\"backend_draws_executed\":false}\n",
                (unsigned long long)comparisons, (unsigned long long)nested,
                (unsigned long long)malformed);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "Buffer consumer: %s\n", error.what());
    return 1;
  }
}
