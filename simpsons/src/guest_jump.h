#pragma once

#include <csetjmp>
#include <cstdint>
#include <cstdlib>
#include <unordered_map>

// Windows setjmp saves XMM registers with aligned stores. Some toolchains
// give jmp_buf only 8-byte type alignment, so a map value needs an explicit
// 16-byte boundary regardless of the guest key that precedes it.
struct alignas(16) PPCJumpBuffer {
  jmp_buf value;
};

inline std::unordered_map<uint32_t, PPCJumpBuffer>& get_jmp_buf_map() {
  static thread_local std::unordered_map<uint32_t, PPCJumpBuffer> map;
  return map;
}

#define ppc_setjmp(guest_buf_addr) (setjmp(::get_jmp_buf_map()[(guest_buf_addr)].value))

[[noreturn]] inline void ppc_longjmp(uint32_t guest_buf_addr, int val) {
  auto& map = get_jmp_buf_map();
  auto it = map.find(guest_buf_addr);
  if (it != map.end()) {
    longjmp(it->second.value, val);
  }
  std::abort();
}
