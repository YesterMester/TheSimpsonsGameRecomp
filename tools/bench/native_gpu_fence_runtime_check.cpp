// Run the new wait roots with the actual sleeping poll hook, original timer
// helpers and real shared command receiver. No GPU backend or game is launched.
#define NATIVE_GPU_REAL_WAIT_RUNTIME
#define NATIVE_GPU_FENCE_WAITS_ENTRY unused_fence_model_main
#include "native_gpu_fence_waits_check.cpp"

#include <rex/runtime.h>
#include <rex/graphics/command_processor.h>
#include <rex/graphics/graphics_system.h>
#include <rex/graphics/native_gpu_sync.h>
#include <rex/graphics/ring_progress.h>
#include <rex/logging.h>

#include <atomic>
#include <chrono>
#include <exception>
#include <thread>

REX_EXTERN(sub_82452018);
REX_EXTERN(__imp__sub_82452018);
static std::atomic<bool> runtime_poll_entered = false, runtime_abort_next = false;
static std::atomic<uint32_t> runtime_poll_count = 0;
static std::chrono::steady_clock::time_point runtime_wait_started;

REX_FUNC(__imp__sub_82452018) {
  const uint32_t tls = LoadGuestU32(base, ctx.r13.u32 + 256);
  const uint32_t device = LoadGuestU32(base, ctx.r3.u32);
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - runtime_wait_started).count();
  StoreGuestU32(base, tls + 88, uint32_t(ms));
  if (runtime_abort_next.exchange(false))
    *simpsons::GuestToHost(base, device + 10941) |= 2;
  reference_sub_82452018(ctx, base);
  runtime_poll_count.fetch_add(1);
  runtime_poll_entered.store(true, std::memory_order_release);
}

void RuntimeAssert(bool ok, const char* message) {
  if (!ok)
    throw std::runtime_error(message);
}
namespace gfx = rex::graphics;
class FenceGraphics final : public gfx::GraphicsSystem {
 public:
  explicit FenceGraphics(rex::memory::Memory* memory) { memory_ = memory; }
  std::string name() const override { return "Fence contract"; }
 private:
  void CreateProvider(bool) override { throw std::runtime_error("Unexpected GPU provider"); }
  std::unique_ptr<gfx::CommandProcessor> CreateCommandProcessor() override {
    throw std::runtime_error("Unexpected GPU worker");
  }
};
class FenceProcessor final : public gfx::CommandProcessor {
 public:
  explicit FenceProcessor(FenceGraphics* graphics) : CommandProcessor(graphics, nullptr) {}
  bool Run(uint32_t physical) {
    rex::memory::RingBuffer reader(memory_->TranslatePhysical(physical), 16);
    reader.set_write_offset(16);
    return ExecutePacket(&reader);
  }
  void IssueSwap(uint32_t, uint32_t, uint32_t) override { Unexpected(); }
  void TracePlaybackWroteMemory(uint32_t, uint32_t) override { Unexpected(); }
  void RestoreEdramSnapshot(const void*) override { Unexpected(); }
 private:
  [[noreturn]] static void Unexpected() { throw std::runtime_error("Unexpected backend work"); }
  bool SetupContext() override { Unexpected(); }
  void ShutdownContext() override { Unexpected(); }
  gfx::Shader* LoadShader(gfx::xenos::ShaderType, uint32_t, const uint32_t*, uint32_t) override { Unexpected(); }
  bool IssueDraw(gfx::xenos::PrimitiveType, uint32_t, IndexBufferInfo*, bool) override { Unexpected(); }
  bool IssueCopy() override { Unexpected(); }
};
struct RuntimeWaitOps {
  static void Begin(PPCContext& ctx, uint8_t* base) { wait_begin_entry(ctx, base); }
  static void Poll(PPCContext& ctx, uint8_t* base) { sub_82452018(ctx, base); }
  static void End(PPCContext& ctx, uint8_t* base) { wait_end_entry(ctx, base); }
  static void Dispatch(PPCContext&, uint8_t*) { throw std::runtime_error("Unexpected dispatch"); }
};

int main(int argc, char** argv) {
  try {
    RuntimeAssert(argc == 2, "usage: native_gpu_fence_runtime_check log_path");
    rex::InitLogging(argv[1], spdlog::level::info);
    rex::Runtime runtime({});
    rex::RuntimeConfig config;
    config.tool_mode = true;
    RuntimeAssert(runtime.Setup(std::move(config)) == 0, "Could not set up headless runtime");
    auto* memory = runtime.memory();
    auto* base = runtime.virtual_membase();
    real_wait_runtime = true;
    const uint32_t device = memory->SystemHeapAlloc(22268);
    const uint32_t pcr = memory->SystemHeapAlloc(sizeof(rex::system::X_KPCR));
    const uint32_t tls = memory->SystemHeapAlloc(512);
    const uint32_t stack = memory->SystemHeapAlloc(4096);
    const uint32_t sync = memory->SystemHeapAlloc(96, 32, rex::memory::kSystemHeapPhysical);
    const uint32_t event = memory->SystemHeapAlloc(32, 32, rex::memory::kSystemHeapPhysical);
    RuntimeAssert(device && pcr && tls && stack && sync && event, "Could not allocate wait storage");
    FenceGraphics graphics(memory);
    FenceProcessor processor(&graphics);
    gfx::native_gpu_calls::SetEnabled(true);
    const auto reset = [&] {
      std::memset(simpsons::GuestToHost(base, device), 0, 22268);
      std::memset(simpsons::GuestToHost(base, pcr), 0, sizeof(rex::system::X_KPCR));
      std::memset(simpsons::GuestToHost(base, tls), 0, 512);
      std::memset(simpsons::GuestToHost(base, sync), 0, 96);
      StoreGuestU32(base, device + 10896, sync);
      StoreGuestU32(base, device + 10908, 4);
      StoreGuestU32(base, device + 10888, 9);
      StoreGuestU32(base, pcr + 256, tls);
      StoreGuestU32(base, tls + 332, 9);
      runtime_poll_entered = false;
      runtime_abort_next = false;
      runtime_wait_started = std::chrono::steady_clock::now();
    };
    const auto input = [&](bool marker) {
      PPCContext ctx{};
      ctx.r1.u64 = 0xABCD000000000000ull | (stack + 4096);
      ctx.r3.u64 = 0xBCDE000000000000ull | device;
      ctx.r4.u64 = 0xCDEF000000000000ull | (marker ? 2 : 0x2000);
      ctx.r5.u64 = 0xDEF0000000000000ull | (marker ? 2 : 1);
      ctx.r13.u64 = pcr;
      ctx.r29.u64 = 0x123456789ABCDEF0ull;
      ctx.r30.u64 = 0x23456789ABCDEF01ull;
      ctx.r31.u64 = 0x3456789ABCDEF012ull;
      ctx.lr = 0xFAFA000082345678ull;
      return ctx;
    };
    uint32_t consumer_cases = 0, unrelated_checks = 0, aborted = 0;
    int64_t maximum_wakeup_us = 0;
    for (bool marker : {false, true})
      for (bool native : {false, true})
        for (bool unrelated : {false, true}) {
          if (unrelated && (!native || !marker))
            continue;
          reset();
          const uint32_t value = marker ? 2 : 0x2000;
          const uint32_t destination = sync + (marker ? 0 : 4);
          const uint32_t header = native ? gfx::native_gpu_sync::kEventHeader : 0xC0025800u;
          for (uint32_t i = 0; i < 2; ++i) {
            StoreGuestU32(base, event + i * 16, header);
            StoreGuestU32(base, event + i * 16 + 4, 3);
            StoreGuestU32(base, event + i * 16 + 8,
                          port::chunk_close_detail::Physical(i ? sync + 64 : destination) | 2);
            StoreGuestU32(base, event + i * 16 + 12, value);
          }
          const uint32_t seen = gfx::GetRingProgressCount();
          std::atomic<bool> observer_entered = false, returned = false;
          std::atomic<int64_t> wakeup_us = 0;
          std::exception_ptr producer_error;
          std::jthread observer([&] {
            const auto began = std::chrono::steady_clock::now();
            observer_entered.store(true, std::memory_order_release);
            gfx::WaitForRingProgress(seen, 250000);
            wakeup_us.store(std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - began).count());
          });
          std::jthread producer([&](std::stop_token stop) {
            try {
              while ((!runtime_poll_entered.load(std::memory_order_acquire) ||
                      !observer_entered.load(std::memory_order_acquire)) && !stop.stop_requested())
                std::this_thread::yield();
              if (stop.stop_requested())
                return;
              std::this_thread::sleep_for(std::chrono::milliseconds(20));
              if (unrelated) {
                RuntimeAssert(processor.Run(port::chunk_close_detail::Physical(event + 16)), "Unrelated event failed");
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                RuntimeAssert(!returned.load(), "Unrelated progress falsely completed the target wait");
              }
              RuntimeAssert(processor.Run(port::chunk_close_detail::Physical(event)), "Completion event failed");
            } catch (...) {
              producer_error = std::current_exception();
            }
          });
          auto ctx = input(marker);
          const auto original = ctx;
          const bool accepted = marker ? port::WaitForGpuMarker(ctx, base, RuntimeWaitOps{})
                                       : port::WaitForAllocationFence(ctx, base, RuntimeWaitOps{});
          returned.store(true);
          producer.join();
          observer.join();
          if (producer_error)
            std::rethrow_exception(producer_error);
          RuntimeAssert(accepted && LoadGuestU32(base, destination) == value,
                        "Native wait returned without the real consumer's completion");
          RuntimeAssert(gfx::GetRingProgressCount() == seen + (unrelated ? 2 : 1),
                        "Shader completion did not publish exactly one progress notification");
          RuntimeAssert(wakeup_us.load() < 200000, "Completion waiter slept until its timeout");
          maximum_wakeup_us = std::max(maximum_wakeup_us, wakeup_us.load());
          RuntimeAssert(ctx.r1.u64 == original.r1.u64 && ctx.lr == uint32_t(original.lr) &&
                            ctx.r29.u64 == original.r29.u64 && ctx.r30.u64 == original.r30.u64 &&
                            ctx.r31.u64 == original.r31.u64,
                        "Real completion changed stack/link or nonvolatile ABI");
          ++consumer_cases;
          unrelated_checks += unrelated;
        }
    for (bool marker : {false, true}) {
      reset();
      runtime_abort_next = true;
      auto ctx = input(marker);
      const bool accepted = marker ? port::WaitForGpuMarker(ctx, base, RuntimeWaitOps{})
                                   : port::WaitForAllocationFence(ctx, base, RuntimeWaitOps{});
      RuntimeAssert(accepted && LoadGuestU32(base, sync) == 0 && LoadGuestU32(base, sync + 4) == 0,
                    "An abort fabricated a completed fence");
      ++aborted;
    }
    gfx::native_gpu_calls::SetEnabled(false);
    std::printf("{\"passed\":true,\"consumer_completion_cases\":%u,\"unrelated_progress_checks\":%u,"
                "\"abort_exits\":%u,\"original_sleeping_poll_executed\":true,"
                "\"shared_receiver_executed\":true,\"waiter_maximum_elapsed_us\":%lld,"
                "\"backend_draws_executed\":false,\"hardware_gpu_executed\":false}\n",
                consumer_cases, unrelated_checks, aborted, (long long)maximum_wakeup_us);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "Fence runtime: %s\n", error.what());
    return 1;
  }
}
