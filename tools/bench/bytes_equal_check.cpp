#include <rex/graphics/util/bytes_equal.h>

#include <array>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace {
void Require(bool value, const char* message) {
  if (!value)
    throw std::runtime_error(message);
}
bool Reference(const uint8_t* a, const uint8_t* b, size_t size) {
  for (size_t i = 0; i < size; ++i)
    if (a[i] != b[i])
      return false;
  return true;
}
class GuardedPage {
 public:
  GuardedPage() {
#if defined(_WIN32)
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    size = info.dwPageSize;
    allocation_ = VirtualAlloc(nullptr, size * 3, MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS);
    Require(allocation_ != nullptr, "Guard page allocation failed");
    data = static_cast<uint8_t*>(allocation_) + size;
    DWORD previous;
    Require(VirtualProtect(data, size, PAGE_READWRITE, &previous) != 0,
            "Guard page protection failed");
#else
    size = size_t(sysconf(_SC_PAGESIZE));
    allocation_ = mmap(nullptr, size * 3, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    Require(allocation_ != MAP_FAILED, "Guard page allocation failed");
    data = static_cast<uint8_t*>(allocation_) + size;
    Require(mprotect(data, size, PROT_READ | PROT_WRITE) == 0, "Guard page protection failed");
#endif
  }
  ~GuardedPage() {
#if defined(_WIN32)
    VirtualFree(allocation_, 0, MEM_RELEASE);
#else
    munmap(allocation_, size * 3);
#endif
  }
  GuardedPage(const GuardedPage&) = delete;
  GuardedPage& operator=(const GuardedPage&) = delete;
  uint8_t* data = nullptr;
  size_t size = 0;

 private:
  void* allocation_ = nullptr;
};
uint64_t Check() {
  using rex::graphics::draw_util::BytesEqual;
  Require(BytesEqual(nullptr, nullptr, 0), "Empty null ranges are unequal");
  std::vector<uint8_t> a(65537 + 64), b(a.size());
  uint64_t comparisons = 1;
  for (uint32_t alignment = 0; alignment < 32; ++alignment) {
    auto* x = a.data() + alignment;
    auto* y = b.data() + 31 - alignment;
    std::vector<size_t> lengths;
    for (size_t n = 0; n <= 192; ++n)
      lengths.push_back(n);
    for (size_t n : {255, 256, 257, 511, 512, 513, 1023, 1024, 1025, 4095, 4096, 4097, 65537})
      lengths.push_back(n);
    for (size_t n : lengths) {
      for (size_t i = 0; i < n; ++i)
        x[i] = y[i] = uint8_t(i * 37 + alignment * 11);
      Require(BytesEqual(x, y, n) == Reference(x, y, n), "Unaligned equal range mismatch");
      ++comparisons;
      // Every short-range position and a sample of large-range positions,
      // including first/last bytes and each SIMD block boundary, must reject.
      for (size_t i = 0; i < n; ++i) {
        if (n > 192 && i != 0 && i != n - 1 && (i & 31) != 0 && (i & 31) != 31)
          continue;
        y[i] ^= 0x80;
        Require(BytesEqual(x, y, n) == Reference(x, y, n), "Changed source byte was missed");
        ++comparisons;
        y[i] ^= 0x80;
      }
    }
  }
  GuardedPage a_page, b_page;
  Require(a_page.size == b_page.size, "Guard page sizes differ");
  for (size_t i = 0; i < a_page.size; ++i)
    a_page.data[i] = b_page.data[i] = uint8_t(i * 13);
  for (size_t n = 0; n <= a_page.size; ++n) {
    const auto* x = a_page.data + a_page.size - n;
    auto* y = b_page.data + b_page.size - n;
    Require(BytesEqual(x, y, n) == Reference(x, y, n), "Guarded equal range mismatch");
    ++comparisons;
    if (n) {
      y[n - 1] ^= 1;
      Require(!BytesEqual(x, y, n), "Guarded trailing mutation was missed");
      ++comparisons;
      y[n - 1] ^= 1;
    }
  }
  return comparisons;
}
}  // namespace

int main(int argc, char** argv) {
  std::string output;
  try {
    for (int i = 1; i < argc; ++i) {
      if (std::string(argv[i]) == "--output" && i + 1 < argc)
        output = argv[++i];
      else
        throw std::runtime_error("usage: bytes_equal_check [--output report.json]");
    }
    uint64_t comparisons = Check();
#if defined(__AVX2__)
    const char* path = "AVX2";
#else
    const char* path = "baseline";
#endif
    std::string report = "{\"passed\":true,\"path\":\"" + std::string(path) +
                         "\",\"comparisons\":" + std::to_string(comparisons) +
                         ",\"protected_boundaries\":true}\n";
    std::fputs(report.c_str(), stdout);
    if (!output.empty()) {
      std::ofstream file(output);
      file << report;
      Require(bool(file), "Writing byte comparison report failed");
    }
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
  }
}
