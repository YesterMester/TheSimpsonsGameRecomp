// Check adapter/fallback decisions and batched ownership against a per-page
// reference. No game files, window or GPU device are needed.
#include <rex/graphics/util/gpu_page_mask.h>
#include <rex/ui/graphics_backend.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace {
void Require(bool value, const char* message) {
  if (!value)
    throw std::runtime_error(message);
}

uint64_t CheckBackends() {
  using namespace rex::ui;
  uint64_t cases = 0;
  for (uint32_t built = 0; built < 8; ++built) {
    for (auto request : {"", "any", "vulkan", "d3d12", "d3d11", "invalid"}) {
      auto order = GetGraphicsBackendOrder(request, built & 1, built & 2, built & 4);
      std::vector<GraphicsBackend> expected;
      for (auto [name, backend, mask] : {std::tuple{"vulkan", GraphicsBackend::kVulkan, 1u},
                                         std::tuple{"d3d12", GraphicsBackend::kD3D12, 2u},
                                         std::tuple{"d3d11", GraphicsBackend::kD3D11, 4u}}) {
        if ((built & mask) &&
            (!*request || std::strcmp(request, "any") == 0 || std::strcmp(request, name) == 0))
          expected.push_back(backend);
      }
      Require(order.count == expected.size(), "Backend selection lost or added a renderer");
      for (size_t i = 0; i < expected.size(); ++i)
        Require(order.backends[i] == expected[i], "Backend fallback order changed");
      ++cases;
    }
  }
  for (int requested : {10000, 100, 0}) {
    for (uint32_t support = 0; support < 8; ++support) {
      std::vector<int> attempts;
      auto result = TryGraphicsQueuePriority(requested, 10000, 100, 0, [&](int priority) {
        attempts.push_back(priority);
        return bool(support & (priority == 10000 ? 4 : priority == 100 ? 2 : 1));
      });
      int expected = -1;
      for (int priority : {0, 100, 10000})
        if (priority <= requested && (support & (priority == 10000 ? 4 : priority == 100 ? 2 : 1)))
          expected = priority;
      Require(result.has_value() == (expected >= 0), "Queue creation failure was hidden");
      if (result)
        Require(*result == expected, "Unsupported queue priority prevented a lower priority");
      Require(!attempts.empty() && attempts.front() == requested, "Requested priority was skipped");
      ++cases;
    }
  }
  Require(PreferGraphicsDevice({false, 0, false}, {true, 4, false}),
          "A software renderer displaced usable hardware");
  Require(PreferGraphicsDevice({false, 4, true}, {false, 4, false}),
          "An equally capable discrete GPU was not preferred");
  Require(PreferGraphicsDevice({false, 4, false}, {false, 3, true}),
          "Existing feature preferences were lost");
  Require(!PreferGraphicsDevice({false, 4, true}, {false, 4, true}),
          "Equivalent adapters lost their stable enumeration order");
  return cases + 4;
}

uint64_t CheckPages() {
  using namespace rex::graphics::draw_util;
  uint64_t cases = 0, random = 0x94D049BB133111EBull;
  for (uint32_t pattern = 0; pattern < 12; ++pattern) {
    std::array<uint64_t, 9> words;
    for (auto& word : words) {
      random ^= random << 13;
      random ^= random >> 7;
      random ^= random << 17;
      word = pattern == 0   ? 0
             : pattern == 1 ? UINT64_MAX
             : pattern == 2 ? 0xAAAAAAAAAAAAAAAAull
             : pattern == 3 ? 0x8000000000000001ull
                            : random;
    }
    for (uint32_t first = 0; first <= words.size() * 64; ++first) {
      for (size_t count : {0u, 1u, 2u, 3u, 31u, 32u, 63u, 64u, 65u, 127u, 128u, 129u, 255u, 512u}) {
        if (first + count > words.size() * 64)
          continue;
        std::vector<uint8_t> storage(count + 2, 0xCD), valid(count);
        std::span<uint8_t> snapshot(storage.data() + 1, count);
        Require(CopyGpuWrittenPages(words, first, snapshot),
                "A valid ownership range was rejected");
        Require(storage.front() == 0xCD && storage.back() == 0xCD,
                "Ownership snapshot overran its output");
        bool expected_any = false;
        for (size_t i = 0; i < count; ++i) {
          bool gpu = (words[(first + i) / 64] >> ((first + i) % 64)) & 1;
          Require(snapshot[i] == uint8_t(gpu),
                  "Ownership snapshot differs from a single-page read");
          valid[i] = ((first + i + pattern) % 7 == 0) ? 0 : uint8_t(1 + (i % 255));
          expected_any |= gpu && valid[i];
        }
        Require(AnyGpuWrittenPage(words, first, valid) == expected_any,
                "Live native owner detection changed");
        ++cases;
      }
    }
    std::array<uint8_t, 3> unchanged{7, 8, 9};
    Require(!CopyGpuWrittenPages(words, uint32_t(words.size() * 64 - 1), unchanged) &&
                unchanged == std::array<uint8_t, 3>{7, 8, 9},
            "An invalid ownership range modified its output");
    Require(AnyGpuWrittenPage(words, UINT32_MAX, unchanged), "An unknown owner was discarded");
    unchanged.fill(0);
    Require(!AnyGpuWrittenPage(words, UINT32_MAX, unchanged), "An invalidated owner was retained");
    ++cases;
  }
  return cases;
}

std::pair<double, double> BenchmarkPages() {
  constexpr size_t count = 1024, iterations = 10000;
  std::array<uint64_t, count / 64> words;
  words.fill(UINT64_MAX);
  std::array<uint8_t, count> pages;
  std::recursive_mutex mutex;
  uint64_t checksum = 0;
  auto measure = [&](bool batched) {
    auto start = std::chrono::steady_clock::now();
    for (size_t iteration = 0; iteration < iterations; ++iteration) {
      words[iteration % words.size()] ^= 1;
      if (batched) {
        std::lock_guard lock(mutex);
        Require(rex::graphics::draw_util::CopyGpuWrittenPages(words, 0, pages),
                "Benchmark snapshot failed");
      } else {
        for (size_t page = 0; page < count; ++page) {
          std::lock_guard lock(mutex);
          pages[page] = uint8_t((words[page / 64] >> (page % 64)) & 1);
        }
      }
      checksum += pages[iteration % count];
    }
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
        .count();
  };
  double single = measure(false), batch = measure(true);
  Require(checksum != 0, "Ownership benchmark had no observable output");
  return {single, batch};
}

uint64_t CheckValidityPublication() {
  using namespace rex::graphics::draw_util;
  uint64_t word = 0, cases = 0;
  for (uint64_t value = 0; value < 4096; ++value) {
    uint64_t pattern = (value * 0x9E3779B97F4A7C15ull) ^ (value << 47);
    StoreGpuPageValidity(word, pattern);
    Require(LoadGpuPageValidity(word) == pattern, "Page validity lost bits");
    ++cases;
  }
  StoreGpuPageValidity(word, 0);
  constexpr uint32_t rounds = 20000;
  uint64_t payload = 0;
  std::atomic<bool> failed = false;
  std::thread writer([&] {
    for (uint64_t round = 1; round <= rounds; ++round) {
      while (LoadGpuPageValidity(word) != 0)
        std::this_thread::yield();
      payload = (round * 0xD6E8FEB86659FD93ull) ^ (round << 33);
      StoreGpuPageValidity(word, 1);
    }
  });
  for (uint64_t round = 1; round <= rounds; ++round) {
    while (LoadGpuPageValidity(word) != 1)
      std::this_thread::yield();
    if (payload != ((round * 0xD6E8FEB86659FD93ull) ^ (round << 33)))
      failed.store(true, std::memory_order_relaxed);
    StoreGpuPageValidity(word, 0);
    ++cases;
  }
  writer.join();
  Require(!failed.load(), "A valid page was published before its data");
  return cases;
}
}  // namespace

int main(int argc, char** argv) {
  try {
    Require(argc == 1 || (argc == 3 && std::strcmp(argv[1], "--output") == 0),
            "usage: graphics_resource_check [--output report.json]");
    uint64_t backends = CheckBackends(), pages = CheckPages(),
             validity = CheckValidityPublication();
    auto [single, batch] = BenchmarkPages();
    char report[512];
    std::snprintf(report, sizeof(report),
                  "{\"passed\":true,\"backend_cases\":%llu,\"page_cases\":%llu,"
                  "\"validity_cases\":%llu,"
                  "\"single_page_ms\":%.3f,\"batch_ms\":%.3f,\"scope\":\"CPU ownership "
                  "microbenchmark, not game FPS\"}\n",
                  static_cast<unsigned long long>(backends), static_cast<unsigned long long>(pages),
                  static_cast<unsigned long long>(validity), single, batch);
    std::fputs(report, stdout);
    if (argc == 3) {
      std::ofstream output(argv[2]);
      output << report;
      Require(bool(output), "Could not write graphics resource report");
    }
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
  }
}
