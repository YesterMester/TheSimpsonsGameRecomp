#include "streams.h"

#include <algorithm>
#include <limits>

namespace simpsons::modkit {

std::vector<uint8_t> EncodeRefPack(std::span<const uint8_t> decoded, uint32_t* read_offset) {
  if (decoded.size() > UINT32_MAX)
    throw Failure("resource_limit", "RefPack input is too large");
  const bool wide = decoded.size() > 0xFFFFFF;
  std::vector<uint8_t> output{uint8_t(wide ? 0x90 : 0x10), 0xFB};
  output.reserve(decoded.size() + (decoded.size() / 4 + 27) / 28 + 8);
  for (int i = wide ? 3 : 2; i >= 0; --i)
    output.push_back(uint8_t(decoded.size() >> (i * 8)));

  // RefPack has a 128 KiB reference window. Hash chains keep compression work
  // bounded, including on large, poorly compressible creator resources.
  constexpr uint32_t missing = std::numeric_limits<uint32_t>::max();
  constexpr size_t window = 131072, buckets = 65536;
  std::vector<uint32_t> previous(window, missing), latest(buckets, missing);
  const auto hash = [&](size_t at) {
    const uint32_t value =
        uint32_t(decoded[at]) << 16 | uint32_t(decoded[at + 1]) << 8 | decoded[at + 2];
    return ((value * 0x1E35A7BDu) >> 16) & (buckets - 1);
  };
  const auto insert = [&](size_t at) {
    if (decoded.size() - at < 3)
      return;
    const auto key = hash(at);
    previous[at & (window - 1)] = latest[key];
    latest[key] = uint32_t(at);
  };
  size_t produced = 0, headroom = 0;
  const auto account = [&](size_t count) {
    produced += count;
    if (produced > output.size())
      headroom = std::max(headroom, produced - output.size());
  };
  size_t pending = 0, at = 0;
  const auto literals = [&]() {
    while (at - pending >= 4) {
      const auto count = std::min<size_t>((at - pending) & ~size_t(3), 112);
      output.push_back(uint8_t(0xE0 + count / 4 - 1));
      output.insert(output.end(), decoded.begin() + pending, decoded.begin() + pending + count);
      pending += count;
      account(count);
    }
  };
  while (at < decoded.size()) {
    size_t length = 0, distance = 0;
    const auto limit = std::min<size_t>(1028, decoded.size() - at);
    if (limit >= 3) {
      uint32_t candidate = latest[hash(at)];
      for (unsigned attempts = 0;
           attempts < 64 && candidate != missing && candidate < at && at - candidate <= window;
           ++attempts) {
        if (!length || decoded[candidate + length] == decoded[at + length]) {
          size_t count = 0;
          while (count < limit && decoded[candidate + count] == decoded[at + count])
            ++count;
          const auto gap = at - candidate;
          const size_t minimum = gap <= 1024 ? 3 : gap <= 16384 ? 4 : 5;
          if (count >= minimum && count > length) {
            length = count;
            distance = gap;
            if (length == limit)
              break;
          }
        }
        const auto next = previous[candidate & (window - 1)];
        if (next >= candidate)
          break;
        candidate = next;
      }
    }
    insert(at);
    if (!length) {
      ++at;
      continue;
    }
    literals();
    const auto plain = at - pending;
    const auto back = distance - 1;
    if (length <= 10 && distance <= 1024) {
      output.push_back(uint8_t(plain | ((length - 3) << 2) | ((back >> 8) << 5)));
      output.push_back(uint8_t(back));
    } else if (length <= 67 && distance <= 16384) {
      output.push_back(uint8_t(0x80 | (length - 4)));
      output.push_back(uint8_t((plain << 6) | (back >> 8)));
      output.push_back(uint8_t(back));
    } else {
      output.push_back(uint8_t(0xC0 | plain | (((length - 5) >> 8) << 2) | ((back >> 16) << 4)));
      output.push_back(uint8_t(back >> 8));
      output.push_back(uint8_t(back));
      output.push_back(uint8_t(length - 5));
    }
    output.insert(output.end(), decoded.begin() + pending, decoded.begin() + at);
    account(plain + length);
    for (size_t i = 1; i < length; ++i)
      insert(at + i);
    at += length;
    pending = at;
  }
  literals();
  output.push_back(uint8_t(0xFC + at - pending));
  output.insert(output.end(), decoded.begin() + pending, decoded.end());
  account(at - pending);
  // The decoder writes forward into the same allocation. Every command must
  // leave its unread input ahead of the output, including a small read margin.
  // Literal-only streams need 32 bytes; compressed streams need more as output
  // overtakes the consumed command bytes. Do not use a fixed offset for both.
  if (read_offset) {
    const auto offset = (headroom + 63) & ~size_t(31);
    if (offset > UINT32_MAX)
      throw Failure("resource_limit", "RefPack in-place allocation is too large");
    *read_offset = uint32_t(offset);
  }
  return output;
}

}  // namespace simpsons::modkit
