// Real SDK locking and native queue publication in a headless runtime.
// No game image, GPU backend, audio device or fabricated completion is used.
#include <rex/runtime.h>
#include <rex/logging.h>

#include "native_gpu_queued_submission.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <exception>
#include <set>
#include <stdexcept>
#include <thread>

namespace port = simpsons::native_gpu_vertices;
using simpsons::LoadGuestU32;
using simpsons::StoreGuestU32;

void Require(bool value, const char* message) {
  if (!value)
    throw std::runtime_error(message);
}

struct RuntimeOps : port::QueuedKernelLock {
  uint32_t device, pcr, node, source;
  std::atomic<uint32_t>* tails;
  std::atomic<uint32_t>* doorbells;
  std::atomic<bool>* attempting = nullptr;

  void Acquire(PPCContext& ctx, uint8_t* base) {
    if (attempting)
      attempting->store(true, std::memory_order_release);
    port::QueuedKernelLock::Acquire(ctx, base);
  }
  void Store(uint8_t* base, uint32_t address, uint32_t value) {
    if (address == port::ring_submission_detail::kDoorbell) {
      Require(!LoadGuestU32(base, device + 11004), "Ring published before releasing the lock");
      Require(value == LoadGuestU32(base, device + 10952), "Doorbell used a stale write index");
      doorbells->fetch_add(1);
      return;  // No GPU exists in this fixture; this boundary is observed only.
    }
    Require(LoadGuestU32(base, device + 11004) == pcr &&
                *simpsons::GuestToHost(base, pcr + 24) == 2,
            "Linked tail was published without the actual kernel lock and IRQL");
    const uint32_t list = device + 13352;
    Require(LoadGuestU32(base, list + 4) == node && LoadGuestU32(base, list + 8) == node + 12 &&
                LoadGuestU32(base, node) == port::chunk_close_detail::Physical(node + 16) - 0x40000000u &&
                LoadGuestU32(base, node + 4) == 0x81000011 &&
                LoadGuestU32(base, node + 8) == source && LoadGuestU32(base, node + 12) == 0xC0000000 &&
                value == port::chunk_close_detail::Physical(node) - 0x40000000u,
            "Tail publication exposed incomplete metadata or command payload");
    REX_MM_STORE_U32(address, value);
    tails->fetch_add(1);
  }
  static void AppendOriginal(PPCContext&, uint8_t*) { throw std::runtime_error("Unexpected original append"); }
  static void PublishOriginal(PPCContext&, uint8_t*) { throw std::runtime_error("Unexpected original ring"); }
};

int main(int argc, char** argv) {
  try {
    Require(argc == 2, "usage: native_gpu_queue_runtime_check log_path");
    rex::InitLogging(argv[1], spdlog::level::info);
    rex::Runtime runtime({});
    rex::RuntimeConfig config;
    config.tool_mode = true;
    Require(runtime.Setup(std::move(config)) == 0, "Headless runtime setup failed");
    auto* memory = runtime.memory();
    auto* base = runtime.virtual_membase();
    const uint32_t device = memory->SystemHeapAlloc(22268);
    const uint32_t pcr = memory->SystemHeapAlloc(sizeof(rex::system::X_KPCR));
    const uint32_t worker_pcr = memory->SystemHeapAlloc(sizeof(rex::system::X_KPCR));
    const uint32_t stack = memory->SystemHeapAlloc(4096);
    const uint32_t worker_stack = memory->SystemHeapAlloc(4096);
    const uint32_t heap = memory->SystemHeapAlloc(0x40000, 4096, rex::memory::kSystemHeapPhysical);
    const uint32_t sync = memory->SystemHeapAlloc(96, 32, rex::memory::kSystemHeapPhysical);
    const uint32_t ring = memory->SystemHeapAlloc(128, 128, rex::memory::kSystemHeapPhysical);
    Require(device && pcr && worker_pcr && stack && worker_stack && heap && sync && ring,
            "Could not allocate real guest test storage");
    const uint32_t list = device + 13352, initial_tail = heap + 0x8000;
    const uint32_t source = port::chunk_close_detail::Physical(heap + 0x3000);
    std::atomic<uint32_t> tails = 0, doorbells = 0;
    const auto reset = [&] {
      std::memset(simpsons::GuestToHost(base, device), 0, 22268);
      std::memset(simpsons::GuestToHost(base, pcr), 0, sizeof(rex::system::X_KPCR));
      std::memset(simpsons::GuestToHost(base, worker_pcr), 0, sizeof(rex::system::X_KPCR));
      std::memset(simpsons::GuestToHost(base, sync), 0, 96);
      StoreGuestU32(base, device + 14880, heap);
      StoreGuestU32(base, device + 14884, heap + 0x40000);
      StoreGuestU32(base, device + 10896, sync);
      StoreGuestU32(base, device + 14872, ring);
      StoreGuestU32(base, device + 14876, 31);
      StoreGuestU32(base, device + 11000, 1);
      StoreGuestU32(base, list + 4, heap + 0x4000);
      StoreGuestU32(base, list + 8, initial_tail);
      StoreGuestU32(base, list + 16, device);
      StoreGuestU32(base, initial_tail, 0xC0000000);
    };
    const auto input = [&](uint32_t owner, uint32_t sp, uint32_t node) {
      PPCContext ctx{};
      ctx.r1.u64 = 0xA5A5000000000000ull | sp;
      ctx.r3.u64 = 0xB6B6000000000000ull | device;
      ctx.r4.u64 = 0xC7C7000000000000ull | node;
      ctx.r5.u64 = 0xD8D8000000000000ull | source;
      ctx.r6.u64 = 17;
      ctx.r7.u64 = 1;
      ctx.r8.u64 = 0xE9E9000000000000ull | list;
      ctx.r13.u64 = owner;
      ctx.lr = 0x82400000;
      return ctx;
    };
    uint32_t sequential = 0, contention = 0;
    for (uint32_t old_irql : {0u, 1u, 2u, 3u}) {
      reset();
      *simpsons::GuestToHost(base, pcr + 24) = uint8_t(old_irql);
      auto ctx = input(pcr, stack + 4096, heap + 0x1000);
      const uint64_t expected = ctx.r4.u64 + 16;
      Require(port::SubmitQueuedCommandBuffer(ctx, base,
                  RuntimeOps{{}, device, pcr, heap + 0x1000, source, &tails, &doorbells}),
              "Real-kernel queue call fell back");
      Require(ctx.r3.u64 == expected && !LoadGuestU32(base, device + 11004) &&
                  LoadGuestU32(base, device + 11000) == 2 &&
                  *simpsons::GuestToHost(base, pcr + 24) == (old_irql < 2 ? old_irql : 2),
              "Real queue changed return, counter, owner or SDK IRQL behavior");
      ++sequential;
    }
    for (bool drain : {false, true}) {
      reset();
      std::atomic<bool> locked = false, attempting = false;
      std::exception_ptr worker_error;
      std::jthread worker([&](std::stop_token stop) {
        try {
          PPCContext other{};
          other.r13.u64 = worker_pcr;
          auto* lock = memory->TranslateVirtual<rex::X_KSPINLOCK*>(device + 11004);
          const auto old = rex::kernel::xboxkrnl::xeKeKfAcquireSpinLock(&other, lock);
          locked.store(true, std::memory_order_release);
          while (!attempting.load(std::memory_order_acquire) && !stop.stop_requested())
            std::this_thread::yield();
          std::this_thread::sleep_for(std::chrono::milliseconds(20));
          if (drain)
            StoreGuestU32(base, device + 11000, 0);
          rex::kernel::xboxkrnl::xeKeKfReleaseSpinLock(&other, lock, old);
        } catch (...) {
          worker_error = std::current_exception();
          locked.store(true, std::memory_order_release);
        }
      });
      while (!locked.load(std::memory_order_acquire))
        std::this_thread::yield();
      auto ctx = input(pcr, stack + 4096, heap + 0x1000);
      const auto began = std::chrono::steady_clock::now();
      const bool accepted = port::SubmitQueuedCommandBuffer(ctx, base,
          RuntimeOps{{}, device, pcr, heap + 0x1000, source, &tails, &doorbells, &attempting});
      const auto blocked = std::chrono::steady_clock::now() - began;
      worker.join();
      if (worker_error)
        std::rethrow_exception(worker_error);
      Require(accepted && !LoadGuestU32(base, device + 11004) &&
                  LoadGuestU32(base, device + 11000) == (drain ? 1u : 2u) &&
                  blocked >= std::chrono::milliseconds(10),
              "Contended submission skipped the actual lock or lost its recheck");
      ++contention;
    }
    reset();
    const uint32_t before_tails = tails.load();
    std::exception_ptr errors[2];
    const auto submit_many = [&](uint32_t number) {
      try {
        for (uint32_t i = 0; i < 64; ++i) {
          const uint32_t node = heap + 0x10000 + (number * 64 + i) * 16;
          const uint32_t owner = number ? worker_pcr : pcr;
          auto ctx = input(owner, (number ? worker_stack : stack) + 4096, node);
          Require(port::SubmitQueuedCommandBuffer(ctx, base,
                      RuntimeOps{{}, device, owner, node, source, &tails, &doorbells}),
                  "Concurrent queue call fell back");
          Require(ctx.r3.u64 == (0xC7C7000000000000ull | node) + 16,
                  "Concurrent queue changed the full-width return");
        }
      } catch (...) {
        errors[number] = std::current_exception();
      }
    };
    std::thread first(submit_many, 0), second(submit_many, 1);
    first.join();
    second.join();
    for (const auto& error : errors)
      if (error)
        std::rethrow_exception(error);
    Require(tails.load() - before_tails == 128 && LoadGuestU32(base, device + 11000) == 129 &&
                !LoadGuestU32(base, device + 11004), "Concurrent queue lost an increment or tail");
    std::set<uint32_t> visited;
    uint32_t slot = initial_tail;
    for (uint32_t i = 0; i < 128; ++i) {
      const uint32_t physical = LoadGuestU32(base, slot) + 0x40000000u;
      const uint32_t node = heap + physical - port::chunk_close_detail::Physical(heap);
      Require(node >= heap + 0x10000 && node < heap + 0x10800 && !(node & 15) &&
                  visited.insert(node).second && LoadGuestU32(base, node + 4) == 0x81000011 &&
                  LoadGuestU32(base, node + 8) == source,
              "Concurrent linked traversal found a missing, duplicate or incomplete node");
      slot = node + 12;
    }
    Require(LoadGuestU32(base, slot) == 0xC0000000 && doorbells == 1,
            "Concurrent linked traversal lost its final sentinel or drain publication");
    std::printf("{\"passed\":true,\"sequential_calls\":%u,\"contention_scenarios\":%u,"
                "\"concurrent_calls\":128,\"linked_nodes_traversed\":128,"
                "\"actual_sdk_lock_executed\":true,\"headless_runtime\":true,"
                "\"original_leaf_fallbacks\":0,\"hardware_mmio_executed\":false,"
                "\"backend_draws_executed\":false}\n", sequential, contention);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "Queue runtime: %s\n", error.what());
    return 1;
  }
}
