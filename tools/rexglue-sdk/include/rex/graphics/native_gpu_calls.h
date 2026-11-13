#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace rex::graphics::native_gpu_calls {

// Ordered transport for native API calls while unconverted game routines still
// share command buffers with them. Arguments live in the buffer itself, so a
// recorded/copied/replayed buffer never depends on a temporary side queue.
// This transport is an intermediate step, not a complete D3D replacement.
constexpr uint32_t kOpcode = 0x7D;
constexpr uint32_t kBareDrawOpcode = 0x7B;
constexpr uint32_t kVerticesDrawOpcode = 0x7A;
constexpr uint32_t kRangeDrawOpcode = 0x79;
constexpr uint32_t kDrawVersion = 0x44525701;  // DRW, layout version 1.
constexpr uint32_t kVerticesVersion = 0x56545801;  // VTX, layout version 1.
constexpr uint32_t kVerticesCapability = 1u << 0;
constexpr uint32_t kIndexedVerticesCapability = 1u << 1;
constexpr uint32_t kRangeDrawCapability = 1u << 2;
constexpr uint32_t kIndexedVerticesVersion = 0x56545802;
constexpr uint32_t kRangeVersion = 0x524E4701;

// BeginVertices' state is unconditional, but its draw obeys bin predication.
// Keep the original 16/18-word spans and all operands at their original offsets.
struct VerticesDrawCall {
  uint32_t first_vertex;
  std::array<uint32_t, 6> fetch;
  std::array<uint32_t, 3> constant_flush;
  uint32_t viz_query_condition;
  uint32_t initiator;
  uint32_t index_base = 0;
  uint32_t index_size = 0;
};

inline bool EncodeVertices(const VerticesDrawCall& call, std::span<uint32_t> words) {
  const bool indexed_record = words.size() == 18;
  const uint32_t source = (call.initiator >> 6) & 3;
  if ((!indexed_record && words.size() != 16) || (source != 0 && source != 2) ||
      (!indexed_record && source != 2))
    return false;
  words[0] = 0xC0000000u | (uint32_t(words.size() - 2) << 16) | (kVerticesDrawOpcode << 8);
  words[1] = call.first_vertex;
  words[2] = indexed_record ? kIndexedVerticesVersion : kVerticesVersion;
  for (std::size_t i = 0; i < call.fetch.size(); ++i)
    words[i + 3] = call.fetch[i];
  words[9] = 0;
  for (std::size_t i = 0; i < call.constant_flush.size(); ++i)
    words[i + 10] = call.constant_flush[i];
  words[13] = 0;
  words[14] = call.viz_query_condition;
  words[15] = call.initiator;
  if (indexed_record) {
    words[16] = call.index_base;
    words[17] = call.index_size;
  }
  return true;
}

inline bool DecodeVertices(std::span<const uint32_t> payload, VerticesDrawCall& call) {
  const bool indexed_record = payload.size() == 17;
  if ((!indexed_record && payload.size() != 15) ||
      payload[1] != (indexed_record ? kIndexedVerticesVersion : kVerticesVersion) ||
      payload[8] || payload[12])
    return false;
  const uint32_t source = (payload[14] >> 6) & 3;
  if ((source != 0 && source != 2) || (!indexed_record && source != 2))
    return false;
  call = {payload[0],
          {payload[2], payload[3], payload[4], payload[5], payload[6], payload[7]},
          {payload[9], payload[10], payload[11]}, payload[13], payload[14],
          indexed_record ? payload[15] : 0, indexed_record ? payload[16] : 0};
  return true;
}

// Prepared streams stay bound; only the starting vertex and conditional draw
// arguments change. This keeps the original five/seven-word operand layout.
struct RangeDrawCall {
  uint32_t first_vertex, viz_query_condition, initiator;
  uint32_t index_base = 0, index_size = 0;
};

inline bool EncodeRange(const RangeDrawCall& call, std::span<uint32_t> words) {
  const bool indexed = words.size() == 7;
  const uint32_t source = (call.initiator >> 6) & 3;
  if ((!indexed && words.size() != 5) || (source != 0 && source != 2) ||
      (!indexed && source != 2))
    return false;
  words[0] = 0xC0000000u | (uint32_t(words.size() - 2) << 16) | (kRangeDrawOpcode << 8);
  words[1] = call.first_vertex;
  words[2] = kRangeVersion;
  words[3] = call.viz_query_condition;
  words[4] = call.initiator;
  if (indexed) { words[5] = call.index_base; words[6] = call.index_size; }
  return true;
}

inline bool DecodeRange(std::span<const uint32_t> payload, RangeDrawCall& call) {
  const bool indexed = payload.size() == 6;
  if ((!indexed && payload.size() != 4) || payload[1] != kRangeVersion)
    return false;
  const uint32_t source = (payload[3] >> 6) & 3;
  if ((source != 0 && source != 2) || (!indexed && source != 2))
    return false;
  call = {payload[0], payload[2], payload[3], indexed ? payload[4] : 0, indexed ? payload[5] : 0};
  return true;
}

struct DrawCall {
  // The native backends currently share these register-layout descriptors.
  // The call updates them directly; it does not generate or decode a Xenos draw.
  std::array<uint32_t, 6> fetch;
  uint32_t coherent_size;
  uint32_t coherent_base;
  uint32_t coherent_status;
  std::array<uint32_t, 3> constant_flush;
  uint32_t viz_query_condition;
  uint32_t initiator;
  uint32_t index_base;
  uint32_t index_size;
};

inline bool EncodeDraw(const DrawCall& call, bool indexed_record, std::span<uint32_t> words) {
  const uint32_t source = (call.initiator >> 6) & 3;
  if (words.size() != (indexed_record ? 27u : 24u) || (source != 0 && source != 2) ||
      (source == 0 && !indexed_record) ||
      (!indexed_record && (call.viz_query_condition || call.index_base || call.index_size)))
    return false;
  for (auto& word : words)
    word = 0;
  words[0] = 0xC0000000u | (uint32_t(words.size() - 2) << 16) | (kOpcode << 8);
  for (std::size_t i = 0; i < call.fetch.size(); ++i)
    words[i + 1] = call.fetch[i];
  words[7] = kDrawVersion;
  words[8] = call.coherent_status;
  words[10] = call.coherent_size;
  words[11] = call.coherent_base;
  words[13] = 3;
  words[14] = 0xA31;
  words[16] = 0x80000000;
  words[17] = 8;
  for (std::size_t i = 0; i < call.constant_flush.size(); ++i)
    words[i + 19] = call.constant_flush[i];
  words[indexed_record ? 24 : 23] = call.initiator;
  if (indexed_record) {
    words[23] = call.viz_query_condition;
    words[25] = call.index_base;
    words[26] = call.index_size;
  }
  return true;
}

inline bool DecodeDraw(std::span<const uint32_t> payload, DrawCall& call) {
  if ((payload.size() != 23 && payload.size() != 26) || payload[6] != kDrawVersion)
    return false;
  const bool indexed_packet = payload.size() == 26;
  const uint32_t initiator = payload[indexed_packet ? 23 : 22];
  const uint32_t source = (initiator >> 6) & 3;
  if ((source != 0 && source != 2) || (source == 0 && !indexed_packet))
    return false;
  if (payload[8] || payload[11] || payload[17] || payload[21] || payload[12] != 3 ||
      payload[13] != 0xA31 || payload[14] != 0 || payload[15] != 0x80000000 || payload[16] != 8)
    return false;
  // Keep operand locations compatible with the original command builder's
  // patch tables. Only its packet headers become native command metadata.
  call = {{payload[0], payload[1], payload[2], payload[3], payload[4], payload[5]},
          payload[9],
          payload[10],
          payload[7],
          {payload[18], payload[19], payload[20]},
          indexed_packet ? payload[22] : 0,
          initiator,
          indexed_packet ? payload[24] : 0,
          indexed_packet ? payload[25] : 0};
  return true;
}

// A capability handshake with the consuming runtime. A producer must keep its
// original path when there is no matching consumer (initialization/shutdown).
bool Enabled();
uint32_t Capabilities();
void SetEnabled(bool enabled);

}  // namespace rex::graphics::native_gpu_calls
