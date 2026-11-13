#include "simpsons_init.h"
#include "guest_memory_fixture.h"
#include "native_gpu_shader_resources.h"
#include <rex/ppc/barriers.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <exception>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

// Atomic because the contention checks execute the complete reference on more
// than one thread. Counters observe branches without changing guest state.
static std::atomic<uint64_t> copy_calls{0}, sort_swaps{0}, merged_fetches{0};
static std::atomic<uint64_t> matched_attributes{0}, missing_attributes{0};
static std::atomic<uint64_t> patch_calls{0}, version_increments{0};
#include "native_gpu_shader_resources_reference.inc"

namespace port = simpsons::native_gpu_calls;
namespace resource_detail = port::shader_resource_detail;
using simpsons::LoadGuestU32;
using simpsons::StoreGuestU32;
namespace {
constexpr uint32_t kSize = 0x100000, kDevice = 0x1000, kResource = 0x10000;
constexpr uint32_t kDeclaration = 0x30000, kCode = 0x40000, kFence = 0x70000, kStack = 0xF0000;
constexpr uint32_t kTables = port::vertex_declaration_detail::kSwizzleTable;
constexpr uint32_t kCounter = resource_detail::kVersionCounter;
std::unique_ptr<GuestMemoryFixture> original, native;
std::array<uint8_t, kSize> pristine;
constexpr std::array<PPCRegister PPCContext::*, 18> kNonvolatile = {
    &PPCContext::r14, &PPCContext::r15, &PPCContext::r16, &PPCContext::r17, &PPCContext::r18,
    &PPCContext::r19, &PPCContext::r20, &PPCContext::r21, &PPCContext::r22, &PPCContext::r23,
    &PPCContext::r24, &PPCContext::r25, &PPCContext::r26, &PPCContext::r27, &PPCContext::r28,
    &PPCContext::r29, &PPCContext::r30, &PPCContext::r31};
uint64_t cases = 0, rejected = 0, cache_hits = 0, busy = 0, rebuilt = 0;
uint64_t concurrency_versions = 0;
uint32_t current_phase = 0, current_seed = 0, current_count = 0;

void Require(bool ok, const char* why) {
  if (!ok)
    throw std::runtime_error(why);
}

uint32_t Next(uint32_t& state) {
  state ^= state << 13;
  state ^= state >> 17;
  state ^= state << 5;
  return state;
}

PPCContext Prepare(uint32_t phase, uint32_t version, uint32_t count, uint32_t variant,
                   uint32_t seed, uint32_t current = 100, uint32_t in_flight = 0,
                   uint32_t completed = 98) {
  auto* base = original->data();
  std::memcpy(base, pristine.data(), kSize);
  std::memset(base + kDevice, 0, 22256);
  uint32_t random = seed + 1;
  const uint32_t relative = 0x2000 + variant * 0x400;
  const uint32_t selected = kResource + relative, record = kResource + variant * 416;
  StoreGuestU32(base, kResource + (variant + 112) * 8, relative);
  StoreGuestU32(base, kResource + 32, kCode);
  StoreGuestU32(base, selected + 872, seed & 4);
  StoreGuestU32(base, selected + 896, 9);
  StoreGuestU32(base, selected + 900, count);
  StoreGuestU32(base, kDeclaration + 24, 32);
  StoreGuestU32(base, kDeclaration + 48, version);
  for (uint32_t i = 0; i < 32; ++i) {
    StoreGuestU32(base, kDeclaration + 52 + i * 12, ((seed & 1) ? 0 : i * 3) << 16 | (i % 4) * 4);
    StoreGuestU32(base, kDeclaration + 56 + i * 12, Next(random));
    StoreGuestU32(base, kDeclaration + 60 + i * 12, ((i & 15) << 16) | ((i >> 4) << 8));
  }
  for (uint32_t i = 0; i < count; ++i) {
    for (uint32_t j = 0; j < 3; ++j)
      StoreGuestU32(base, record + 68 + i * 12 + j * 4, Next(random));
    const uint32_t marker = i == 0 ? 0x100000 : i == count - 1 ? 0x300000 : 0;
    const uint32_t output = (seed & 4) ? (i & 1 ? 4095 : 0) : Next(random) & 4095;
    StoreGuestU32(base, selected + 944 + i * 4, marker | (i >> 4) << 16 | (i & 15) << 12 | output);
  }
  for (uint32_t i = 0; i < 96; ++i)
    *simpsons::GuestToHost(base, kDevice + 12520 + i) = uint8_t(seed * 17 + i * 3);
  StoreGuestU32(base, record + 40, phase == 2 ? version + 1 : version);
  resource_detail::Store64(base, record + 48, resource_detail::Load64(base, kDevice + 12520));
  resource_detail::Store64(base, record + 56, resource_detail::Load64(base, kDevice + 12528));
  for (uint32_t i = 0; i < 4; ++i)
    StoreGuestU32(base, kDeclaration + 32 + i * 4, phase == 1 ? 0 : UINT32_MAX);
  if (phase == 1 || phase == 3)
    StoreGuestU32(base, record + 48, LoadGuestU32(base, record + 48) ^ (1u << (seed & 31)));
  StoreGuestU32(base, record + 64, in_flight);
  StoreGuestU32(base, kDevice + 10896, kFence);
  StoreGuestU32(base, kDevice + 10908, current);
  StoreGuestU32(base, kFence, completed);
  *simpsons::GuestToHost(base, kStack - 640) = uint8_t(seed);
  *simpsons::GuestToHost(base, kStack - 639) = uint8_t(seed);
  for (uint32_t i = 0; i < 16; i += 4)
    StoreGuestU32(base, kTables + i, Next(random));
  for (uint32_t i = 0; i < 64; ++i)
    *simpsons::GuestToHost(base, kTables + 16 + i) = uint8_t((seed & 1) ? 1 : Next(random) & 15);
  constexpr std::array<uint32_t, 8> counters = {0,          1,          2,          0xFFFFFFFD,
                                                0xFFFFFFFE, UINT32_MAX, 0x7FFFFFFF, 0x80000000};
  StoreGuestU32(base, kCounter, counters[seed & 7]);
  std::memcpy(native->data(), base, kSize);
  std::memcpy(simpsons::GuestToHost(native->data(), kTables), simpsons::GuestToHost(base, kTables),
              80);
  StoreGuestU32(native->data(), kCounter, LoadGuestU32(base, kCounter));
  PPCContext ctx{};
  ctx.r1.u64 = 0xABCD000000000000ull | kStack;
  ctx.r3.u64 = 0xA5A5000000000000ull | kDevice;
  ctx.r4.u64 = 0xFEED000000000000ull | kResource;
  ctx.r5.u64 = 0xBCDE000000000000ull | kDeclaration;
  ctx.r6.u64 = variant;
  ctx.r13.u64 = seed & 1 ? 0x8020 : 0;
  ctx.lr = 0xDEADBEEF82345678ull;
  ctx.msr = 0xA5A58020;
  ctx.xer.ca = 1;
  for (uint32_t i = 0; i < kNonvolatile.size(); ++i)
    (ctx.*kNonvolatile[i]).u64 = 0x1234ABCD00000000ull | (i * 0x10203u);
  return ctx;
}

void Compare(PPCContext input) {
  auto expected = input, actual = input;
  const uint64_t patches = patch_calls.load();
  const int32_t locks = ppc_global_lock_count_().load();
  reference_sub_8245F8C0(expected, original->data());
  Require(port::PrepareShaderResource(actual, native->data()), "Safe shader resource was rejected");
  Require(expected.r1.u64 == actual.r1.u64 && expected.r3.u64 == actual.r3.u64 &&
              expected.lr == actual.lr && expected.msr == actual.msr &&
              expected.r13.u64 == actual.r13.u64,
          "Resource preparation changed stack, return, link or interrupt ABI");
  for (auto reg : kNonvolatile)
    Require((expected.*reg).u64 == (actual.*reg).u64,
            "Resource preparation changed a nonvolatile GPR");
  Require(ppc_global_lock_count_().load() == locks,
          "Resource preparation leaked interrupt nesting");
  if (std::memcmp(original->data(), native->data(), kStack - 728) ||
      std::memcmp(original->data() + kStack, native->data() + kStack, kSize - kStack)) {
    for (uint32_t i = 0; i < kSize; ++i)
      if (!(i >= kStack - 728 && i < kStack) && original->data()[i] != native->data()[i]) {
        std::fprintf(stderr, "First difference at %08X: expected=%02X actual=%02X\n", i,
                     original->data()[i], native->data()[i]);
        break;
      }
    throw std::runtime_error(
        "Resource preparation changed code, state, metadata or neighboring memory");
  }
  Require(LoadGuestU32(original->data(), kCounter) == LoadGuestU32(native->data(), kCounter) &&
              !std::memcmp(simpsons::GuestToHost(original->data(), kTables),
                           simpsons::GuestToHost(native->data(), kTables), 80),
          "Resource preparation changed the version sequence or lookup tables");
  if (!actual.r3.u32)
    ++busy;
  else if (patch_calls.load() == patches)
    ++cache_hits;
  else
    ++rebuilt;
  ++cases;
}

void Reject(uint32_t problem) {
  auto ctx = Prepare(2, 0, 7, 0, 3);
  auto* base = native->data();
  constexpr uint32_t selected = kResource + 0x2000;
  switch (problem) {
    case 0:
      ctx.r1.u32 |= 1;
      break;
    case 1:
      ctx.r1.u32 = 64;
      break;
    case 2:
      ctx.r3.u32 |= 1;
      break;
    case 3:
      ctx.r4.u32 |= 1;
      break;
    case 4:
      ctx.r5.u32 |= 1;
      break;
    case 5:
      ctx.r6.u64 = UINT64_MAX;
      break;
    case 6:
      ctx.r6.u64 = UINT32_MAX;
      break;
    case 7:
      ctx.r4.u32 = kDevice;
      break;
    case 8:
      ctx.r5.u32 = kDevice;
      break;
    case 9:
      ctx.r5.u32 = kResource;
      break;
    case 10:
      StoreGuestU32(base, kResource + 64, 1);
      StoreGuestU32(base, kDevice + 10896, kFence + 1);
      break;
    case 11:
      StoreGuestU32(base, kResource + 64, 1);
      StoreGuestU32(base, kDevice + 10896, kStack - 16);
      break;
    case 12:
      StoreGuestU32(base, kResource + 896, UINT32_MAX);
      break;
    case 13:
      StoreGuestU32(base, selected + 900, 33);
      break;
    case 14:
      StoreGuestU32(base, selected + 896, UINT32_MAX);
      break;
    case 15:
      StoreGuestU32(base, kDeclaration + 24, 257);
      break;
    case 16:
      StoreGuestU32(base, kDeclaration + 52, 96u << 16);
      StoreGuestU32(base, selected + 944, 0);
      break;
    case 17:
      StoreGuestU32(base, kResource + 32, UINT32_MAX);
      break;
    case 18:
      StoreGuestU32(base, kResource + 32, UINT32_MAX - 3);
      break;
    case 19:
      StoreGuestU32(base, kResource + 32, kResource);
      break;
    case 20:
      StoreGuestU32(base, kResource + 32, kDevice);
      break;
    case 21:
      StoreGuestU32(base, kResource + 32, kDeclaration);
      break;
    case 22:
      StoreGuestU32(base, kResource + 32, kStack - 16);
      break;
    case 23:
      StoreGuestU32(base, kResource + 32, kCounter);
      break;
    case 24:
      StoreGuestU32(base, kResource + 64, 1);
      StoreGuestU32(base, kFence, 1);
      StoreGuestU32(base, kResource + 32, kFence);
      break;
    case 25:
      StoreGuestU32(base, kResource + 64, 1);
      StoreGuestU32(base, kDevice + 10896, kCounter);
      StoreGuestU32(base, kCounter, 1);
      break;
    case 26:
      ctx.r3.u32 = kResource + 0x2200;
      break;
    case 27:
      ctx.r5.u32 = kStack - 16;
      break;
    case 28:
      ctx.r3.u32 = kStack - 16;
      break;
    case 29:
      StoreGuestU32(base, kResource + 32, kResource);
      StoreGuestU32(base, selected + 944, 4095);
      StoreGuestU32(base, selected + 948, 0);
      break;
    case 30:
      ctx.r4.u32 = 0x82D50000;
      native->Commit(ctx.r4.u32, 0x8000);
      std::memcpy(simpsons::GuestToHost(base, ctx.r4.u32), base + kResource, 0x8000);
      StoreGuestU32(base, kCounter, 0xFFFFFFFD);
      break;
  }
  if (problem >= 18 && problem <= 24)
    StoreGuestU32(base, selected + 944, 0);
  const auto saved = ctx;
  std::memcpy(original->data(), base, kSize);
  const uint32_t counter = LoadGuestU32(base, kCounter);
  std::vector<uint8_t> high_resource;
  if (problem == 30)
    high_resource.assign(simpsons::GuestToHost(base, ctx.r4.u32),
                         simpsons::GuestToHost(base, ctx.r4.u32) + 0x8000);
  const int32_t locks = ppc_global_lock_count_().load();
  Require(!port::PrepareShaderResource(ctx, base), "Unsafe shader resource was accepted");
  Require(!std::memcmp(&ctx, &saved, sizeof(ctx)) && !std::memcmp(base, original->data(), kSize) &&
              LoadGuestU32(base, kCounter) == counter && ppc_global_lock_count_().load() == locks,
          "Rejected resource changed state, instructions, version or lock nesting");
  if (!high_resource.empty())
    Require(!std::memcmp(high_resource.data(), simpsons::GuestToHost(base, ctx.r4.u32), 0x8000),
            "Rejected counter alias changed the shader resource");
  ++rejected;
}

void ConcurrentVersions() {
  constexpr uint32_t threads = 4, iterations = 1024, stride = 0x100000;
  constexpr uint32_t initial = 0xFFFFF800;
  auto shared = std::make_unique<GuestMemoryFixture>(threads * stride);
  shared->Commit(kCounter, 4);
  std::vector<uint32_t> expected;
  uint32_t last = initial;
  for (uint32_t i = 0; i < threads * iterations; ++i) {
    do {
      ++last;
    } while (last == 0 || last == UINT32_MAX);
    expected.push_back(last);
  }
  std::sort(expected.begin(), expected.end());
  for (uint32_t mode = 0; mode < 3; ++mode) {
    auto* base = shared->data();
    StoreGuestU32(base, kCounter, initial);
    std::array<PPCContext, threads> contexts{};
    std::array<std::vector<uint32_t>, threads> results;
    std::array<std::exception_ptr, threads> errors{};
    for (uint32_t i = 0; i < threads; ++i) {
      const uint32_t offset = i * stride;
      auto& ctx = contexts[i];
      ctx.r1.u32 = kStack + offset;
      ctx.r3.u32 = kDevice + offset;
      ctx.r4.u32 = kResource + offset;
      ctx.r5.u32 = kDeclaration + offset;
      ctx.lr = 0x82345678;
      StoreGuestU32(base, ctx.r4.u32 + 40, 1);
      StoreGuestU32(base, ctx.r4.u32 + 64, 0);
      StoreGuestU32(base, ctx.r4.u32 + 32, kCode + offset);
      StoreGuestU32(base, ctx.r4.u32 + 896, 0x2000);
      StoreGuestU32(base, ctx.r4.u32 + 0x2000 + 872, 0);
      StoreGuestU32(base, ctx.r4.u32 + 0x2000 + 900, 0);
      results[i].resize(iterations);
    }
    std::atomic<bool> start{false};
    std::array<std::thread, threads> workers;
    for (uint32_t i = 0; i < threads; ++i)
      workers[i] = std::thread([&, i] {
        try {
          while (!start.load(std::memory_order_acquire))
            std::this_thread::yield();
          auto ctx = contexts[i];
          for (uint32_t j = 0; j < iterations; ++j) {
            ctx.r1 = contexts[i].r1;
            ctx.r3 = contexts[i].r3;
            ctx.r4 = contexts[i].r4;
            ctx.r5 = contexts[i].r5;
            ctx.r6.u64 = 0;
            ctx.lr = contexts[i].lr;
            StoreGuestU32(base, ctx.r5.u32 + 48, 0);
            if (mode == 0 || (mode == 2 && (i & 1)))
              reference_sub_8245F8C0(ctx, base);
            else
              Require(port::PrepareShaderResource(ctx, base), "Contended resource was rejected");
            Require(ctx.r3.u64 == 1, "Contended resource returned busy");
            results[i][j] = LoadGuestU32(base, contexts[i].r5.u32 + 48);
          }
        } catch (...) {
          errors[i] = std::current_exception();
        }
      });
    start.store(true, std::memory_order_release);
    for (auto& worker : workers)
      worker.join();
    for (const auto& error : errors)
      if (error)
        std::rethrow_exception(error);
    std::vector<uint32_t> observed;
    for (const auto& result : results)
      observed.insert(observed.end(), result.begin(), result.end());
    std::sort(observed.begin(), observed.end());
    Require(observed == expected && LoadGuestU32(base, kCounter) == last &&
                ppc_global_lock_count_().load() == 0,
            "Contended native/original writers lost, duplicated or reordered version IDs");
    concurrency_versions += observed.size();
  }
}
}  // namespace

int main(int argc, char** argv) {
  if (argc > 2 || (argc == 2 && !std::freopen(argv[1], "w", stdout)))
    return 2;
  try {
    original = std::make_unique<GuestMemoryFixture>(kSize);
    native = std::make_unique<GuestMemoryFixture>(kSize);
    for (auto* memory : {original.get(), native.get()}) {
      memory->Commit(kTables, 80);
      memory->Commit(kCounter, 4);
    }
    for (uint32_t i = 0; i < kSize; ++i)
      pristine[i] = uint8_t(i * 37 + (i >> 6));
    for (uint32_t phase = 0; phase < 4; ++phase)
      for (uint32_t version : {0u, 1u, 2u, 0xFFFFFFFEu, UINT32_MAX})
        for (uint32_t count : {0u, 1u, 7u, 32u})
          for (uint32_t variant : {0u, 1u})
            for (uint32_t seed = 0; seed < 8; ++seed) {
              current_phase = phase;
              current_seed = seed;
              current_count = count;
              Compare(Prepare(phase, version, count, variant, seed));
            }
    constexpr std::array<uint32_t, 7> fences = {0,          1,          2,         0x7FFFFFFF,
                                                0x80000000, 0xFFFFFFFE, UINT32_MAX};
    for (uint32_t current : fences)
      for (uint32_t pending : fences)
        for (uint32_t completed : fences)
          for (uint32_t phase : {0u, 2u, 3u})
            Compare(Prepare(phase, 0, 7, 0, current & 7, current, pending, completed));
    for (uint32_t seed = 0; seed < 8; ++seed) {
      auto ctx = Prepare(2, 0, 32, 1, seed);
      REX_ENTER_GLOBAL_LOCK();
      try {
        Compare(ctx);
      } catch (...) {
        REX_LEAVE_GLOBAL_LOCK();
        throw;
      }
      REX_LEAVE_GLOBAL_LOCK();
    }
    // A cached or busy variant must not dereference unused shader/allocator data.
    for (bool cached : {false, true}) {
      auto ctx = Prepare(cached ? 0 : 2, 0, 7, 0, 1, 100, cached ? 0 : 99, 98);
      for (auto* base : {original->data(), native->data()}) {
        StoreGuestU32(base, kResource + 32, UINT32_MAX);
        StoreGuestU32(base, kResource + 896, UINT32_MAX);
        StoreGuestU32(base, kDeclaration + 24, UINT32_MAX);
      }
      Compare(ctx);
    }
    for (uint32_t word = 0; word < 4; ++word)
      for (bool relevant : {false, true}) {
        auto ctx = Prepare(0, 0, 7, 0, word);
        for (auto* base : {original->data(), native->data()}) {
          StoreGuestU32(base, kResource + 48 + word * 4,
                        LoadGuestU32(base, kResource + 48 + word * 4) ^ 0x80000001);
          StoreGuestU32(base, kDeclaration + 32 + word * 4, relevant ? UINT32_MAX : 0);
        }
        Compare(ctx);
      }
    for (uint32_t view :
         {0x7F040000u, 0x82040000u, 0x90040000u, 0xA0040000u, 0xC0040000u, 0xE0040000u})
      for (uint32_t alignment : {0u, 4u})
        for (uint32_t kind = 0; kind < 4; ++kind) {
          const uint32_t address = view + alignment;
          auto ctx = Prepare(2, 0, 32, 1, 7);
          const uint32_t source = kind == 0 ? kDevice : kind == 1 ? kResource : kDeclaration;
          const uint32_t bytes = kind == 0   ? 22256
                                 : kind == 1 ? 0x8000
                                 : kind == 2 ? 436
                                             : 4096 * 12 + 4;
          for (auto* memory : {original.get(), native.get()}) {
            memory->Commit(address, bytes + 4096);
            if (kind == 3) {
              std::memcpy(simpsons::GuestToHost(memory->data(), address), pristine.data(), bytes);
              StoreGuestU32(memory->data(), kResource + 32, address);
            } else {
              std::memcpy(simpsons::GuestToHost(memory->data(), address), memory->data() + source,
                          bytes);
            }
          }
          if (kind == 0)
            ctx.r3.u32 = address;
          if (kind == 1)
            ctx.r4.u32 = address;
          if (kind == 2)
            ctx.r5.u32 = address;
          Compare(ctx);
          Require(!std::memcmp(simpsons::GuestToHost(original->data(), address),
                               simpsons::GuestToHost(native->data(), address), bytes),
                  "Resource preparation changed high-view state or instructions");
        }
    for (uint32_t problem = 0; problem < 31; ++problem) {
      current_seed = problem;
      Reject(problem);
    }
    Require(cache_hits && busy && rebuilt && version_increments.load(),
            "Resource oracle missed a main branch");
    const uint64_t sequential_patches = patch_calls.load(), sequential_copies = copy_calls.load();
    const uint64_t sequential_versions = version_increments.load();
    ConcurrentVersions();
    std::printf(
        "{\"passed\":true,\"cases\":%llu,\"cache_hits\":%llu,\"busy_returns\":%llu,"
        "\"rebuilt\":%llu,\"real_declaration_calls\":%llu,\"real_memcpy_calls\":%llu,"
        "\"reference_version_increments\":%llu,\"concurrent_versions\":%llu,"
        "\"unchanged_fallbacks\":%llu}\n",
        (unsigned long long)cases, (unsigned long long)cache_hits, (unsigned long long)busy,
        (unsigned long long)rebuilt, (unsigned long long)sequential_patches,
        (unsigned long long)sequential_copies, (unsigned long long)sequential_versions,
        (unsigned long long)concurrency_versions, (unsigned long long)rejected);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s; phase=%u count=%u seed=%u\n", error.what(), current_phase,
                 current_count, current_seed);
    return 1;
  }
}
