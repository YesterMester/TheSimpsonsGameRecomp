// The optimized copies must preserve the codec's forward-repeat semantics.
#include "refpack.h"
#include "streams.h"

#include <iostream>
#include <random>

using namespace simpsons;
using namespace simpsons::modkit;
namespace {
int checks = 0;
void Require(bool condition, const char* message) {
  ++checks;
  if (!condition)
    throw Failure("fixture", message);
}
void Packet(std::span<const uint8_t> packed, std::span<const uint8_t> expected, size_t offset) {
  std::vector<uint8_t> buffer(std::max(expected.size(), offset + packed.size()) + 64, 0xBC);
  std::copy(packed.begin(), packed.end(), buffer.begin() + offset);
  const auto before = buffer;
  const auto input = std::span<const uint8_t>(buffer).subspan(offset, packed.size());
  const auto output = std::span<uint8_t>(buffer).first(expected.size());
  const auto plan = refpack::Validate(input, output, 0x10000 + offset, 0x10000);
  Require(bool(plan) && plan.produced == expected.size(),
          "A valid packet has a complete bounded decode plan");
  Require(buffer == before, "Validation cannot write to either buffer");
  Require(bool(refpack::Decode(input.first(plan.consumed), output)),
          "An approved packet decodes in place");
  Require(std::equal(expected.begin(), expected.end(), buffer.begin()),
          "In-place output matches source bytes");
  Require(std::equal(buffer.end() - 64, buffer.end(), before.end() - 64),
          "The decode preserves allocation guards");
}
}  // namespace

int main() {
  try {
    for (const size_t distance : {1, 2, 3, 4, 7, 8, 15, 16, 31, 32, 63, 64, 1024, 131072})
      for (const size_t count : {3, 4, 5, 7, 10, 11, 64, 67, 128, 255, 511, 1028}) {
        std::vector<uint8_t> reference(distance + count);
        for (size_t i = 0; i < distance; ++i)
          reference[i] = uint8_t(i * 37 + 11);
        auto native = reference;
        for (size_t i = 0; i < count; ++i)
          reference[distance + i] = reference[i];
        refpack::Repeat(native.data() + distance, count, distance);
        Require(native == reference, "Bulk repeated copies match byte-forward copies");
      }
    std::mt19937 random(0x71A43);
    for (const size_t size : {0, 1, 3, 4, 127, 1028, 65536, 140000}) {
      std::vector<uint8_t> plain(size);
      for (auto& byte : plain)
        byte = uint8_t(random());
      for (const size_t stride : {1, 3, 8192}) {
        auto patterned = plain;
        for (size_t i = stride; i < patterned.size(); ++i)
          if (i % 4096 < 2048)
            patterned[i] = patterned[i - stride];
        uint32_t offset = 0;
        const auto packed = EncodeRefPack(patterned, &offset);
        Packet(packed, patterned, offset);
        std::vector<uint8_t> prefix(std::min<size_t>(17, patterned.size()));
        const auto decoded = refpack::Decode(packed, prefix, false);
        Require(bool(decoded) && std::equal(prefix.begin(), prefix.end(), patterned.begin()),
                "Prefix indexing decodes only its requested bytes");
      }
    }
    const std::vector<uint8_t> short_packet{0x10, 0xFB, 0,   0, 7, 0xE0, 'a',
                                            'b',  'c',  'd', 0, 0, 0xFC};
    std::vector<uint8_t> output(4096, 0xEF);
    for (size_t length = 0; length < short_packet.size(); ++length) {
      const auto before = output;
      Require(!refpack::Validate(std::span(short_packet).first(length), output, 0x20000, 0x10000),
              "Every truncated packet is rejected");
      Require(output == before, "A rejected plan cannot leave partial output");
    }
    Require(!refpack::Validate(short_packet, output, 0x10000, 0x20000),
            "Guest output above unread input retains the game's rejection rule");
    // Random bounded packets exercise malformed headers, commands and reference
    // distances under sanitizers. Successful plans must also execute safely.
    size_t accepted = 0;
    for (size_t test = 0; test < 20000; ++test) {
      std::vector<uint8_t> packed(5 + random() % 256);
      for (auto& byte : packed)
        byte = uint8_t(random());
      packed[0] = 0x10;
      packed[1] = 0xFB;
      packed[2] = 0;
      packed[3] = uint8_t(random() % 16);
      packed[4] = uint8_t(random());
      std::fill(output.begin(), output.end(), 0xEF);
      const auto checked = refpack::Validate(packed, output, 0x20000, 0x10000);
      if (checked) {
        Require(bool(refpack::Decode(packed, output)),
                "A successful random plan executes without failure");
        ++accepted;
      } else {
        Require(std::all_of(output.begin(), output.end(), [](auto c) { return c == 0xEF; }),
                "Malformed input preserves destination bytes");
      }
    }
    std::cout << Json{{"ok", true}, {"checks", checks}, {"random_plans_accepted", accepted}}.dump()
              << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
