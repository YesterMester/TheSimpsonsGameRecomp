#include <rex/graphics/util/buffer_content.h>
#include <rex/graphics/util/register_write.h>

#include <algorithm>
#include <atomic>
#include <barrier>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <thread>
#include <vector>

namespace {
void Require(bool condition) {
  if (!condition) {
    std::fputs("Register and buffer content check failed\n", stderr);
    std::abort();
  }
}
}  // namespace

int main(int argc, char** argv) {
  Require(argc == 1 || (argc == 3 && std::strcmp(argv[1], "--output") == 0));
  using namespace rex::graphics::draw_util;
  uint64_t cases = 0;
  // Exercise simultaneous first use, including CPUs built without AVX2.
  std::barrier start(12);
  std::atomic<uint64_t> hash_cases{0};
  std::vector<std::thread> workers;
  for (size_t worker = 0; worker < 12; ++worker) {
    workers.emplace_back([&, worker] {
      std::vector<uint8_t> bytes(8192 + worker * 17);
      for (size_t i = 0; i < bytes.size(); ++i)
        bytes[i] = uint8_t(i * 117 + worker);
      const uint64_t expected = XXH3_64bits(bytes.data(), bytes.size());
      start.arrive_and_wait();
      for (size_t i = 0; i < 1000; ++i) {
        Require(HashNativeBuffer(bytes.data(), bytes.size()) == expected);
        ++hash_cases;
      }
    });
  }
  for (auto& worker : workers)
    worker.join();
  cases += hash_cases;
  std::vector<uint8_t> bytes((2u << 20) + 64);
  for (size_t i = 0; i < bytes.size(); ++i)
    bytes[i] = uint8_t(i * 117 + i / 13);
  for (size_t offset : {0u, 1u, 3u, 7u, 15u, 31u, 63u}) {
    for (size_t size = 0; size <= 1024; ++size) {
      Require(HashNativeBuffer(bytes.data() + offset, size) ==
              XXH3_64bits(bytes.data() + offset, size));
      ++cases;
    }
    for (size_t size : {4095u, 4096u, 4097u, 65535u, 65536u, 65537u, 131072u, 2097152u}) {
      Require(HashNativeBuffer(bytes.data() + offset, size) ==
              XXH3_64bits(bytes.data() + offset, size));
      ++cases;
    }
  }
  Require(!CopyAndSwapRegisters(nullptr, nullptr, 0));
  for (size_t offset = 0; offset < 8; ++offset) {
    for (size_t count = 0; count <= 2048; ++count) {
      std::vector<uint32_t> source(count + 16), actual(count + 16, 0x8E09B54D);
      for (size_t i = 0; i < count; ++i)
        source[i + offset] = std::byteswap(uint32_t(i * 7919 + count * 65537));
      auto reference = actual;
      for (size_t i = 0; i < count; ++i)
        reference[i + offset] = std::byteswap(source[i + offset]);
      const bool changed = actual != reference;
      Require(CopyAndSwapRegisters(actual.data() + offset, source.data() + offset, count) ==
              changed);
      Require(actual == reference);
      Require(!CopyAndSwapRegisters(actual.data() + offset, source.data() + offset, count));
      for (size_t index : {size_t(0), count / 2, count ? count - 1 : 0}) {
        if (!count)
          continue;
        source[offset + index] ^= 0x80000001;
        Require(CopyAndSwapRegisters(actual.data() + offset, source.data() + offset, count));
        reference[offset + index] = std::byteswap(source[offset + index]);
        Require(actual == reference);
        ++cases;
      }
      ++cases;
    }
  }
  for (size_t size : {size_t(0), size_t(1), size_t(63), size_t(64), size_t(255), size_t(256),
                      size_t(257), size_t(512), size_t(4096), size_t(65537), size_t(131071),
                      size_t(131072), size_t(131073), size_t(2097152)}) {
    std::vector<uint8_t> snapshot(size), source(size);
    for (size_t i = 0; i < size; ++i)
      snapshot[i] = uint8_t(i * 117 + i / 13);
    source = snapshot;
    auto hash = HashBufferContent(snapshot.data(), size);
    Require(BufferContentMatches(source.data(), snapshot.data(), size, hash));
    for (size_t index : {size_t(0), size_t(63), size_t(64), size / 2, size ? size - 1 : 0}) {
      if (index >= size)
        continue;
      source[index] ^= 0x80;
      Require(!BufferContentMatches(source.data(), snapshot.data(), size, hash));
      source[index] ^= 0x80;
      Require(BufferContentMatches(source.data(), snapshot.data(), size, hash));
      ++cases;
    }
  }
  char report[128];
  std::snprintf(report, sizeof(report), "{\"passed\":true,\"cases\":%llu}\n",
                (unsigned long long)cases);
  if (argc == 3) {
    std::ofstream file(argv[2]);
    file << report;
    file.flush();
    Require(bool(file));
  }
  std::fputs(report, stdout);
}
