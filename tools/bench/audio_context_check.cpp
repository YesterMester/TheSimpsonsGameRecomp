#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <rex/audio/xma/context.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/types.h>
#include <stdexcept>
#include <thread>
#include <tracy/Tracy.hpp>
#if defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace rex::kernel::xboxkrnl {
uint32_t XMASetOutputBufferReadOffset_entry(mapped_void, uint32_t);
uint32_t XMAGetOutputBufferReadOffset_entry(mapped_void);
uint32_t XMAGetOutputBufferWriteOffset_entry(mapped_void);
uint32_t XMAGetInputBufferReadOffset_entry(mapped_void);
uint32_t XMAIsInputBuffer0Valid_entry(mapped_void);
uint32_t XMAIsInputBuffer1Valid_entry(mapped_void);
uint32_t XMAIsOutputBufferValid_entry(mapped_void);
uint32_t XMAGetPacketMetadata_entry(mapped_void);
uint32_t XMASetInputBufferReadOffset_entry(mapped_void, uint32_t);
uint32_t XMASetInputBuffer0Valid_entry(mapped_void);
uint32_t XMASetInputBuffer1Valid_entry(mapped_void);
uint32_t XMASetOutputBufferValid_entry(mapped_void);
uint32_t XMASetLoopData_entry(mapped_void, ppc_ptr_t<rex::audio::XMA_CONTEXT_DATA>);
}  // namespace rex::kernel::xboxkrnl
namespace api = rex::kernel::xboxkrnl;

static void Require(bool ok, const char* why) {
  if (!ok)
    throw std::runtime_error(why);
}
int main() {
  try {
    rex::InitLoggingEarly();
#ifdef TRACY_ENABLE
    tracy::StartupProfiler();
#endif
    rex::audio::XmaContext context;
    size_t checked = 0;
    for (uint32_t capacity = 2; capacity <= 31; ++capacity) {
      for (uint32_t read = 0; read < capacity; ++read) {
        alignas(64) std::array<uint8_t, 64> bytes = {};
        rex::audio::XMA_CONTEXT_DATA initial(bytes.data());
        initial.output_buffer_read_offset = read;
        initial.output_buffer_write_offset = (read + 1) % capacity;
        initial.output_buffer_block_count = capacity;
        initial.output_buffer_valid = 1;
        initial.input_buffer_0_valid = 1;
        initial.input_buffer_1_valid = 1;
        initial.input_buffer_0_ptr = 0x01000000;
        initial.input_buffer_1_ptr = 0x02000000;
        initial.output_buffer_ptr = 0x03000000;
        initial.loop_count = 3;
        initial.input_buffer_read_offset = 32;
        initial.Store(bytes.data());
        auto decoded = initial;
        decoded.output_buffer_write_offset = (read + 2) % capacity;
        decoded.input_buffer_read_offset = 4096;
        decoded.input_buffer_0_valid = 0;
        decoded.current_buffer = 1;
        decoded.loop_count = 2;
        decoded.error_status = 4;
        // The mixer consumes output while a native decode is still working.
        // Its next sample must never be rolled back by the decoder's commit.
        auto consumer = initial;
        consumer.output_buffer_read_offset = (read + 1) % capacity;
        consumer.stop_when_done = read & 1;
        consumer.interrupt_when_done = !(read & 1);
        for (size_t i = 0; i < 6; ++i)
          consumer.unk_dwords_10_15[i] = 0x12340000 + i;
        consumer.Store(bytes.data());
        context.StoreContextMerged(decoded, initial, bytes.data());
        rex::audio::XMA_CONTEXT_DATA result(bytes.data());
        Require(result.output_buffer_read_offset == consumer.output_buffer_read_offset,
                "decoder rewound the mixer's output read cursor");
        Require(result.output_buffer_write_offset == decoded.output_buffer_write_offset,
                "decoder did not publish its produced samples");
        Require(result.input_buffer_read_offset == decoded.input_buffer_read_offset &&
                    result.current_buffer == decoded.current_buffer &&
                    result.error_status == decoded.error_status &&
                    result.loop_count == decoded.loop_count,
                "decoder progress was not published");
        Require(!result.input_buffer_0_valid && result.input_buffer_1_valid &&
                    result.output_buffer_valid,
                "decoder buffer validity changed incorrectly");
        Require(result.stop_when_done == consumer.stop_when_done &&
                    result.interrupt_when_done == consumer.interrupt_when_done &&
                    !std::memcmp(result.unk_dwords_10_15, consumer.unk_dwords_10_15,
                                 sizeof(result.unk_dwords_10_15)),
                "decoder changed consumer controls or reserved words");
        ++checked;
      }
    }
    size_t api_checks = 0;
    size_t getter_checks = 0;
    for (uint32_t seed = 1; seed <= 512; ++seed) {
      alignas(64) std::array<uint32_t, 16> words;
      for (size_t i = 0; i < words.size(); ++i)
        words[i] = seed * 0x9E3779B9u + i * 0x1234567u;
      auto ptr = mapped_void::from_host(words.data());
      rex::audio::XMA_CONTEXT_DATA expected(words.data());
      auto read_matches = [&](uint32_t actual, uint32_t field) {
        Require(actual == field, "native mixer getter read the wrong field or byte order");
        ++getter_checks;
      };
      read_matches(api::XMAGetOutputBufferReadOffset_entry(ptr),
                   expected.output_buffer_read_offset);
      read_matches(api::XMAGetOutputBufferWriteOffset_entry(ptr),
                   expected.output_buffer_write_offset);
      read_matches(api::XMAGetInputBufferReadOffset_entry(ptr), expected.input_buffer_read_offset);
      read_matches(api::XMAIsInputBuffer0Valid_entry(ptr), expected.input_buffer_0_valid);
      read_matches(api::XMAIsInputBuffer1Valid_entry(ptr), expected.input_buffer_1_valid);
      read_matches(api::XMAIsOutputBufferValid_entry(ptr), expected.output_buffer_valid);
      read_matches(api::XMAGetPacketMetadata_entry(ptr), expected.packet_metadata);
      auto verify = [&] {
        alignas(64) std::array<uint8_t, 64> oracle;
        expected.Store(oracle.data());
        Require(!std::memcmp(words.data(), oracle.data(), 64),
                "native audio setter changed unrelated fields");
        ++api_checks;
      };
      api::XMASetOutputBufferReadOffset_entry(ptr, seed);
      expected.output_buffer_read_offset = seed;
      verify();
      api::XMASetInputBufferReadOffset_entry(ptr, seed * 971u);
      expected.input_buffer_read_offset = seed * 971u;
      verify();
      api::XMASetInputBuffer0Valid_entry(ptr);
      expected.input_buffer_0_valid = 1;
      verify();
      api::XMASetInputBuffer1Valid_entry(ptr);
      expected.input_buffer_1_valid = 1;
      verify();
      api::XMASetOutputBufferValid_entry(ptr);
      expected.output_buffer_valid = 1;
      verify();
      auto loop = expected;
      loop.loop_start = seed * 973u;
      loop.loop_end = seed * 997u;
      loop.loop_count = seed;
      loop.loop_subframe_end = seed;
      loop.loop_subframe_skip = seed * 3u;
      api::XMASetLoopData_entry(ptr, ppc_ptr_t<rex::audio::XMA_CONTEXT_DATA>(&loop, 0));
      expected.loop_start = loop.loop_start;
      expected.loop_end = loop.loop_end;
      expected.loop_count = loop.loop_count;
      expected.loop_subframe_end = loop.loop_subframe_end;
      expected.loop_subframe_skip = loop.loop_subframe_skip;
      verify();
    }
    // The producer and consumer share header words but update different
    // fields. A full context store can lose either side's final progress.
    alignas(64) std::array<uint8_t, 64> bytes = {};
    rex::audio::XMA_CONTEXT_DATA initial(bytes.data());
    auto produced = initial;
    constexpr uint32_t rounds = 100000;
    std::thread consumer([&] {
      for (uint32_t i = 1; i <= rounds; ++i) {
        api::XMASetOutputBufferReadOffset_entry(mapped_void::from_host(bytes.data()), i);
        rex::audio::XMA_CONTEXT_DATA::UpdateWord(bytes.data(), 0, 4095u, i);
        rex::audio::XMA_CONTEXT_DATA::UpdateWord(bytes.data(), 1, 4095u, i + 17);
      }
    });
    for (uint32_t i = 1; i <= rounds; ++i) {
      auto before = produced;
      produced.output_buffer_write_offset = i & 31;
      produced.input_buffer_read_offset = i;
      produced.current_buffer = i & 1;
      context.StoreContextMerged(produced, before, bytes.data());
    }
    consumer.join();
    rex::audio::XMA_CONTEXT_DATA result(bytes.data());
    Require(result.output_buffer_read_offset == (rounds & 31) &&
                result.input_buffer_0_packet_count == (rounds & 4095) &&
                result.input_buffer_1_packet_count == ((rounds + 17) & 4095),
            "decoder lost concurrently published consumer fields");
    Require(result.output_buffer_write_offset == (rounds & 31) &&
                result.input_buffer_read_offset == rounds && result.current_buffer == (rounds & 1),
            "consumer lost concurrently published decoder fields");
    // Exercise the native getters as the PCM ring advances. Publishing a
    // write position must make its sample payload visible to the consumer;
    // publishing a read position must finish consumption before reuse.
    initial.Store(bytes.data());
    auto ptr = mapped_void::from_host(bytes.data());
    std::array<uint32_t, 32> pcm = {};
    std::atomic<bool> payload_matches = true;
    std::thread mixer([&] {
      for (uint32_t i = 1; i <= rounds; ++i) {
        uint32_t position = i & 31;
        while (api::XMAGetOutputBufferWriteOffset_entry(ptr) != position) {
          std::this_thread::yield();
        }
        if (pcm[position] != i) {
          payload_matches.store(false, std::memory_order_relaxed);
        }
        api::XMASetOutputBufferReadOffset_entry(ptr, position);
      }
    });
    produced = initial;
    for (uint32_t i = 1; i <= rounds; ++i) {
      while (api::XMAGetOutputBufferReadOffset_entry(ptr) != ((i - 1) & 31)) {
        std::this_thread::yield();
      }
      pcm[i & 31] = i;
      auto before = produced;
      produced.output_buffer_write_offset = i & 31;
      context.StoreContextMerged(produced, before, bytes.data());
    }
    mixer.join();
    Require(payload_matches.load(std::memory_order_relaxed),
            "native mixer observed a position before its sample payload");
#if defined(__linux__)
    // Cancel work after its fast enabled check, while it waits for the
    // context lock. It must not touch a released or disabled voice later.
    for (bool release : {false, true}) {
      std::unique_lock pending(context.lock_);
      context.set_is_allocated(true);
      context.set_is_enabled(true);
      std::atomic<long> tid = 0;
      bool worked = true;
      std::thread decoder([&] {
        tid.store(syscall(SYS_gettid), std::memory_order_release);
        worked = context.Work();
      });
      bool blocked = false;
      auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
      while (std::chrono::steady_clock::now() < deadline && !blocked) {
        if (long id = tid.load(std::memory_order_acquire)) {
          std::string state;
          std::ifstream("/proc/self/task/" + std::to_string(id) + "/wchan") >> state;
          blocked = state.find("futex") != std::string::npos;
        }
        if (!blocked) {
          std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
      }
      context.set_is_enabled(false);
      if (release) {
        context.set_is_allocated(false);
      }
      pending.unlock();
      decoder.join();
      Require(blocked, "could not observe the decoder waiting on the context lock");
      Require(!worked, "decoder claimed work after the voice was cancelled");
    }
#endif
    std::printf(
        "XMA COMMIT consumer advances preserved=%zu decoder progress=pass "
        "wraparound=pass concurrent updates=100000 native setters=%zu "
        "native getters=%zu PCM publications=100000\n",
        checked, api_checks, getter_checks);
#ifdef TRACY_ENABLE
    tracy::ShutdownProfiler();
#endif
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
#ifdef TRACY_ENABLE
    tracy::ShutdownProfiler();
#endif
    return 2;
  }
}
