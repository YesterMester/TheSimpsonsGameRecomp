#include "../../simpsons/generated/default/simpsons_init.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <fstream>
#include <thread>

namespace {
bool CheckRoundTrip(uint32_t address) {
  auto& buffer = get_jmp_buf_map()[address];
  if (reinterpret_cast<uintptr_t>(&buffer.value) & 15)
    return false;
  switch (ppc_setjmp(address)) {
    case 0:
      ppc_longjmp(address, 17);
    case 17:
      return true;
    default:
      return false;
  }
}
}  // namespace

int main(int argc, char** argv) {
  std::string output;
  if (argc == 3 && std::string_view(argv[1]) == "--output")
    output = argv[2];
  else if (argc != 1)
    return 2;
  std::atomic<uint32_t> completed = 0;
  std::array<std::thread, 4> workers;
  for (auto& worker : workers) {
    worker = std::thread([&] {
      // Growing and rehashing the map must preserve live native buffers.
      for (uint32_t i = 0; i < 1024; ++i) {
        if (CheckRoundTrip(0x1000 + i * 4))
          ++completed;
      }
      for (uint32_t i = 0; i < 1024; ++i) {
        if (CheckRoundTrip(0x1000 + i * 4))
          ++completed;
      }
    });
  }
  for (auto& worker : workers)
    worker.join();
  bool passed = completed == 8192;
  std::string report = "{\"passed\":" + std::string(passed ? "true" : "false") +
                       ",\"scope\":\"native guest jump context round trips\",\"threads\":4," +
                       "\"round_trips\":" + std::to_string(completed.load()) + "}\n";
  std::fputs(report.c_str(), stdout);
  if (!output.empty()) {
    std::ofstream file(output);
    file << report;
    if (!file)
      return 2;
  }
  return passed ? 0 : 1;
}
