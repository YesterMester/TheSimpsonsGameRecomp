#include "input_state.h"

#include <bit>

namespace simpsons::modkit {
namespace {
uint16_t Big16(const uint8_t* bytes) {
  return uint16_t(bytes[0]) << 8 | bytes[1];
}
double Axis(int16_t value) {
  return double(value) / (value < 0 ? 32768.0 : 32767.0);
}
Json Buttons(uint32_t bits) {
  // Face names describe the game's logical buttons, regardless of the glyph
  // printed on a controller. Trigger buttons use the runtime's threshold of 30.
  static constexpr std::pair<const char*, uint32_t> buttons[] = {{"a", 0x1000},
                                                                 {"b", 0x2000},
                                                                 {"x", 0x4000},
                                                                 {"y", 0x8000},
                                                                 {"dpad_up", 1},
                                                                 {"dpad_down", 2},
                                                                 {"dpad_left", 4},
                                                                 {"dpad_right", 8},
                                                                 {"start", 0x10},
                                                                 {"back", 0x20},
                                                                 {"left_stick", 0x40},
                                                                 {"right_stick", 0x80},
                                                                 {"left_shoulder", 0x100},
                                                                 {"right_shoulder", 0x200},
                                                                 {"guide", 0x400},
                                                                 {"left_trigger", 0x10000},
                                                                 {"right_trigger", 0x20000}};
  Json result = Json::object();
  for (const auto& [name, mask] : buttons)
    result[name] = bool(bits & mask);
  return result;
}
}  // namespace

void InputSnapshots::Reset() {
  std::lock_guard lock(mutex_);
  // An original poll may still be completing on another thread. It cannot
  // populate the new world or a restored timeline with an old observation.
  epoch_.fetch_add(1, std::memory_order_relaxed);
  pending_ = {};
  frame_state_ = {};
}

void InputSnapshots::Observe(uint32_t player, uint64_t epoch, bool available,
                             std::span<const uint8_t, 16> bytes) {
  if (player >= pending_.size())
    return;
  Pad pad;
  if (available) {
    pad.buttons = Big16(bytes.data() + 4);
    pad.left_trigger = bytes[6];
    pad.right_trigger = bytes[7];
    pad.lx = std::bit_cast<int16_t>(Big16(bytes.data() + 8));
    pad.ly = std::bit_cast<int16_t>(Big16(bytes.data() + 10));
    pad.rx = std::bit_cast<int16_t>(Big16(bytes.data() + 12));
    pad.ry = std::bit_cast<int16_t>(Big16(bytes.data() + 14));
    if (pad.left_trigger > 30)
      pad.buttons |= 0x10000;
    if (pad.right_trigger > 30)
      pad.buttons |= 0x20000;
  }
  std::lock_guard lock(mutex_);
  if (epoch != Epoch())
    return;
  auto& state = pending_[player];
  // Starting, reconnecting or restoring with a held button is a baseline,
  // not a fresh press. Losing a device releases its previously held controls.
  if (state.sampled && state.available) {
    if (available)
      state.pressed |= pad.buttons & ~state.pad.buttons;
    state.released |= state.pad.buttons & ~pad.buttons;
  }
  state.pad = pad;
  state.sampled = true;
  state.available = available;
  state.updated = true;
}

void InputSnapshots::BeginFrame() {
  std::lock_guard lock(mutex_);
  ++frame_;
  frame_state_ = pending_;
  for (auto& state : pending_) {
    state.pressed = state.released = 0;
    state.updated = false;
  }
}

Json InputSnapshots::Read(uint32_t player) const {
  if (player >= pending_.size())
    throw Failure("invalid_argument", "Input player must be 0 or 1");
  State state;
  uint64_t frame;
  {
    std::lock_guard lock(mutex_);
    state = frame_state_[player];
    frame = frame_;
  }
  return {{"player", player},
          {"frame", frame},
          {"sampled", state.sampled},
          {"updated", state.updated},
          {"available", state.available},
          {"buttons", Buttons(state.pad.buttons)},
          {"pressed", Buttons(state.pressed)},
          {"released", Buttons(state.released)},
          {"left_stick", {{"x", Axis(state.pad.lx)}, {"y", Axis(state.pad.ly)}}},
          {"right_stick", {{"x", Axis(state.pad.rx)}, {"y", Axis(state.pad.ry)}}},
          {"left_trigger", state.pad.left_trigger / 255.0},
          {"right_trigger", state.pad.right_trigger / 255.0}};
}
}  // namespace simpsons::modkit
