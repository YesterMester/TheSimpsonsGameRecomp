#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

namespace simpsons::refpack {

enum class Error { none, signature, truncated, output_size, reference, overlap, terminator };
struct Result {
  Error error = Error::none;
  uint32_t size = 0;
  size_t consumed = 0, produced = 0;
  explicit operator bool() const { return error == Error::none; }
};

inline std::string_view Describe(Error error) {
  switch (error) {
    case Error::none:
      return "";
    case Error::signature:
      return "Not a RefPack stream";
    case Error::truncated:
      return "RefPack command is incomplete";
    case Error::output_size:
      return "RefPack copy exceeds its input or output";
    case Error::reference:
      return "RefPack reference precedes the output";
    case Error::overlap:
      return "RefPack output would overwrite unread input";
    case Error::terminator:
      return "RefPack stopped before the resource ended";
  }
  return "Invalid RefPack stream";
}

inline Result Header(std::span<const uint8_t> input) {
  Result result;
  if (input.size() < 2 || (input[0] & 0x3E) != 0x10 || input[1] != 0xFB) {
    result.error = Error::signature;
    return result;
  }
  const size_t width = input[0] & 0x80 ? 4 : 3;
  const size_t start = 2 + ((input[0] & 1) ? width : 0);
  if (start + width > input.size()) {
    result.error = Error::truncated;
    return result;
  }
  for (size_t i = start; i < start + width; ++i)
    result.size = (result.size << 8) | input[i];
  result.consumed = start + width;
  return result;
}

// Only used for disjoint ranges. Fixed-width copies avoid library dispatch for
// short references without reading or writing beyond the resource's bounds.
inline void Copy(uint8_t* output, const uint8_t* input, size_t count) {
  if (count > 32) {
    std::memcpy(output, input, count);
  } else if (count >= 16) {
    uint8_t first[16], last[16];
    std::memcpy(first, input, 16);
    std::memcpy(last, input + count - 16, 16);
    std::memcpy(output, first, 16);
    std::memcpy(output + count - 16, last, 16);
  } else if (count >= 8) {
    uint64_t first, last;
    std::memcpy(&first, input, 8);
    std::memcpy(&last, input + count - 8, 8);
    std::memcpy(output, &first, 8);
    std::memcpy(output + count - 8, &last, 8);
  } else if (count >= 4) {
    uint32_t first, last;
    std::memcpy(&first, input, 4);
    std::memcpy(&last, input + count - 4, 4);
    std::memcpy(output, &first, 4);
    std::memcpy(output + count - 4, &last, 4);
  } else if (count) {
    output[0] = input[0];
    if (count > 1)
      output[1] = input[1];
    if (count > 2)
      output[2] = input[2];
  }
}

// A back-reference repeats bytes it just wrote when count exceeds distance.
// Seed one non-overlapping part, then double it. memmove alone does not have
// those semantics. Single-byte runs use the host's vectorized fill.
inline void Repeat(uint8_t* output, size_t count, size_t distance) {
  if (distance == 1) {
    std::memset(output, output[-1], count);
    return;
  }
  size_t copied = std::min(count, distance);
  Copy(output, output - distance, copied);
  while (copied < count) {
    const auto next = std::min(copied, count - copied);
    Copy(output + copied, output, next);
    copied += next;
  }
}

struct Command {
  size_t plain = 0, count = 0, distance = 0;
  bool stop = false;
};

inline bool ReadCommand(std::span<const uint8_t> input, size_t& at, Command& copy) {
  if (at >= input.size())
    return false;
  const unsigned command = input[at++];
  if (command < 0x80) {
    if (input.size() - at < 1)
      return false;
    const unsigned b = input[at++];
    copy.plain = command & 3;
    copy.count = ((command >> 2) & 7) + 3;
    copy.distance = ((command & 0x60) << 3) + b + 1;
  } else if (command < 0xC0) {
    if (input.size() - at < 2)
      return false;
    const unsigned b = input[at++], c = input[at++];
    copy.plain = b >> 6;
    copy.count = (command & 0x3F) + 4;
    copy.distance = ((b & 0x3F) << 8) + c + 1;
  } else if (command < 0xE0) {
    if (input.size() - at < 3)
      return false;
    const unsigned b = input[at++], c = input[at++], d = input[at++];
    copy.plain = command & 3;
    copy.count = ((command & 12) << 6) + d + 5;
    copy.distance = ((command & 16) << 12) + (b << 8) + c + 1;
  } else if (command < 0xFC) {
    copy.plain = ((command & 31) + 1) * 4;
  } else {
    copy.plain = command & 3;
    copy.stop = true;
  }
  return true;
}

struct StepResult {
  Error error = Error::none;
  size_t consumed = 0, produced = 0;
  bool stopped = false;
  explicit operator bool() const { return error == Error::none; }
};

// The asynchronous codec stops between commands, so a step may exceed its
// requested byte budget by one command. The previous output remains readable
// as the reference window; it is not re-decoded on the next step.
template <bool write>
StepResult Step(std::span<const uint8_t> input, std::span<uint8_t> output, size_t history,
                size_t budget, uint64_t source_address, uint64_t destination_address) {
  size_t at = 0, written = 0;
  const auto fail = [&](Error error) { return StepResult{error, at, written, false}; };
  if (history > output.size())
    return fail(Error::output_size);
  while (written < budget) {
    Command copy;
    if (!ReadCommand(input, at, copy) || copy.plain > input.size() - at)
      return fail(Error::truncated);
    if (copy.plain + copy.count > output.size() - history - written)
      return fail(Error::output_size);
    if (copy.count && copy.distance > history + written + copy.plain)
      return fail(Error::reference);
    if ((copy.plain && destination_address + written >= source_address + at) ||
        (copy.count && destination_address + written + copy.count > source_address + at))
      return fail(Error::overlap);
    if constexpr (write) {
      if (copy.plain)
        std::memmove(output.data() + history + written, input.data() + at, copy.plain);
      if (copy.count)
        Repeat(output.data() + history + written + copy.plain, copy.count, copy.distance);
    }
    at += copy.plain;
    written += copy.plain + copy.count;
    if (copy.stop)
      return {Error::none, at, written, true};
  }
  return {Error::none, at, written, false};
}

// write=false proves a complete decode without touching either buffer.
// The game's decoder also requires its output cursor below the unread input
// cursor, even for disjoint guest allocations. Guest addresses express that
// rule independently of the host's physical-memory mapping.
template <bool write, bool full, bool check_overlap = false>
Result Walk(std::span<const uint8_t> input, std::span<uint8_t> output, uint64_t source_address = 0,
            uint64_t destination_address = 0) {
  auto result = Header(input);
  if (!result)
    return result;
  if (full && result.size > output.size()) {
    result.error = Error::output_size;
    return result;
  }
  const auto goal = std::min<size_t>(result.size, output.size());
  size_t at = result.consumed, written = 0;
  const auto fail = [&](Error error) { return Result{error, result.size, at, written}; };
  bool stopped = false;
  while (written < goal || (full && !stopped)) {
    Command copy;
    if (!ReadCommand(input, at, copy))
      return fail(Error::truncated);
    const auto [plain, count, distance, stop] = copy;
    stopped = stop;
    if (plain > input.size() - at)
      return fail(Error::truncated);
    if (plain + count > size_t(result.size) - written)
      return fail(Error::output_size);
    if (count && distance > written + plain)
      return fail(Error::reference);
    if (check_overlap) {
      if (plain && destination_address + written >= source_address + at)
        return fail(Error::overlap);
      if (count && destination_address + written + count > source_address + at)
        return fail(Error::overlap);
    }
    const auto take = full ? plain : std::min(plain, goal - written);
    if constexpr (write) {
      if (take <= 3) {
        // Ascending order also preserves literals when the in-place source is
        // only one byte ahead of the destination.
        for (size_t i = 0; i < take; ++i)
          output[written + i] = input[at + i];
      } else
        std::memmove(output.data() + written, input.data() + at, take);
    }
    written += take;
    at += plain;
    if (count && (full || written < goal)) {
      const auto copy = full ? count : std::min(count, goal - written);
      if constexpr (write)
        Repeat(output.data() + written, copy, distance);
      written += copy;
    }
    if (stopped) {
      if (written != goal)
        return fail(Error::terminator);
      break;
    }
  }
  return {Error::none, result.size, at, written};
}

inline Result Validate(std::span<const uint8_t> input, std::span<uint8_t> output,
                       uint64_t source_address, uint64_t destination_address) {
  return Walk<false, true, true>(input, output, source_address, destination_address);
}
inline Result Decode(std::span<const uint8_t> input, std::span<uint8_t> output, bool full = true) {
  return full ? Walk<true, true>(input, output) : Walk<true, false>(input, output);
}

}  // namespace simpsons::refpack
