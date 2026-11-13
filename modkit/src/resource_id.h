#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace simpsons::modkit {
// Native resource GUIDs are sixteen big-endian bytes, not guest addresses or
// the launcher's transient asset handles. Zero means no resource identity.
inline std::string ResourceId(const std::array<uint32_t, 4>& guid) {
  constexpr char hex[] = "0123456789ABCDEF";
  std::string result;
  result.reserve(32);
  bool nonzero = false;
  for (const auto word : guid) {
    nonzero |= word != 0;
    for (int shift = 28; shift >= 0; shift -= 4)
      result += hex[(word >> shift) & 15];
  }
  return nonzero ? result : std::string();
}
}  // namespace simpsons::modkit
