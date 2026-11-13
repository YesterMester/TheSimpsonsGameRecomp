#include "game_menu_apt.h"
#include "host_memory.h"
#include "guest_memory.h"

#include <array>
#include <bit>
#include <cstring>

#include <rex/ppc.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>

namespace simpsons {

GameMenuApt::GameMenuApt(rex::Runtime* runtime) : runtime_(runtime) {
  // Runtime owns the arena until guest threads have stopped. Menu shutdown
  // happens before that, so it must not release memory still used by a call.
  scratch_ = host_memory::Alloc(16384);
}

void GameMenuApt::Bind(PPCContext& ctx, uint8_t* base, std::string_view movie) {
  ctx_ = &ctx;
  base_ = base;
  offset_ = 0;
  movie_ = movie;
}

uint32_t GameMenuApt::Call(uint32_t function, std::span<const uint32_t> args) {
  auto* fn = runtime_->function_dispatcher()->GetFunction(function);
  if (!fn || !ctx_ || !scratch_)
    return 0;
  const PPCContext saved = *ctx_;
  ctx_->r1.u64 -= 512;
  StoreGuestU32(base_, ctx_->r1.u32, saved.r1.u32);
  std::array<PPCRegister*, 8> registers{&ctx_->r3, &ctx_->r4, &ctx_->r5, &ctx_->r6,
                                        &ctx_->r7, &ctx_->r8, &ctx_->r9, &ctx_->r10};
  for (size_t i = 0; i < args.size() && i < registers.size(); ++i)
    registers[i]->u64 = args[i];
  ctx_->ctr.u64 = function;
  fn(*ctx_, base_);
  const uint32_t result = ctx_->r3.u32;
  *ctx_ = saved;
  return result;
}

uint32_t GameMenuApt::Copy(std::string_view text) {
  const auto size = uint32_t(text.size() + 1);
  if (size > 4096)
    return 0;
  if (offset_ + size > 16384)
    offset_ = 0;
  const uint32_t address = scratch_ + offset_;
  std::memcpy(GuestToHost(base_, address), text.data(), text.size());
  GuestToHost(base_, address)[text.size()] = 0;
  offset_ = (offset_ + size + 3) & ~3u;
  return address;
}

std::string GameMenuApt::Path(std::string_view name) const {
  return name.starts_with("_level") ? std::string(name) : movie_ + "." + std::string(name);
}

uint32_t GameMenuApt::Get(std::string_view path) {
  const uint32_t reference = Copy("");
  StoreGuestU32(base_, reference, 0);
  const std::array<uint32_t, 2> construct{reference, Copy(Path(path))};
  Call(0x827C8180, construct);
  const std::array<uint32_t, 1> root_args{0};
  const std::array<uint32_t, 2> args{Call(0x827BF618, root_args), reference};
  const uint32_t result = Call(0x827BC0C8, args);
  const std::array<uint32_t, 1> release{reference};
  Call(0x827C7BB8, release);
  return result;
}

void GameMenuApt::Set(std::string_view path, uint32_t value) {
  if (!value)
    return;
  const uint32_t reference = Copy("");
  StoreGuestU32(base_, reference, 0);
  const std::array<uint32_t, 2> construct{reference, Copy(Path(path))};
  Call(0x827C8180, construct);
  const std::array<uint32_t, 1> root_args{0};
  const std::array<uint32_t, 3> args{Call(0x827BF618, root_args), reference, value};
  Call(0x827BC128, args);
  const std::array<uint32_t, 1> release{reference};
  Call(0x827C7BB8, release);
}

void GameMenuApt::SetText(std::string_view path, std::string_view text) {
  const std::array<uint32_t, 1> args{Copy(text)};
  Set(path, Call(0x827C6A60, args));
}

void GameMenuApt::SetNumber(std::string_view path, int32_t value) {
  const std::array<uint32_t, 1> args{uint32_t(value)};
  Set(path, Call(0x827C5CB8, args));
}

void GameMenuApt::SetReal(std::string_view path, float value) {
  const auto previous = ctx_->f1;
  ctx_->f1.f64 = value;
  const uint32_t number = Call(0x827C5EE0, {});
  ctx_->f1 = previous;
  Set(path, number);
}

float GameMenuApt::Real(std::string_view path) {
  const uint32_t value = Get(path);
  if (!value)
    return 0;
  if ((LoadGuestU32(base_, value + 4) & 0x7F) == 6)
    return LoadGuestFloat(base_, value + 8);
  const std::array<uint32_t, 1> args{value};
  return float(int32_t(Call(0x827C1A00, args)));
}

std::string GameMenuApt::Text(std::string_view path) {
  const uint32_t value = Get(path);
  if (!value || (LoadGuestU32(base_, value + 4) & 0x7F) != 1)
    return {};
  const auto string = LoadGuestU32(base_, value + 8);
  if (!string)
    return {};
  const uint16_t length =
      uint16_t(GuestToHost(base_, string)[2]) << 8 | GuestToHost(base_, string)[3];
  return {reinterpret_cast<const char*>(GuestToHost(base_, string + 8)), length};
}

int32_t GameMenuApt::Number(std::string_view path) {
  const uint32_t value = Get(path);
  const std::array<uint32_t, 1> args{value};
  return value ? int32_t(Call(0x827C1A00, args)) : 0;
}

void GameMenuApt::Invoke(std::string_view method, std::span<const std::string> strings) {
  if (strings.size() > 4)
    return;
  // Resolve the receiver separately. The engine entry point looks up the
  // function by path, but uses its movie argument as `this` for the call.
  // Passing a dotted method with the root movie would call Array.splice or
  // removeMovieClip on the root instead of the intended object.
  const size_t dot = method.rfind('.');
  const std::string receiver = dot == std::string_view::npos ? movie_ : Path(method.substr(0, dot));
  const auto name = dot == std::string_view::npos ? method : method.substr(dot + 1);
  std::array<uint32_t, 8> args{Copy(name), 0, Copy(receiver), uint32_t(strings.size())};
  for (size_t i = 0; i < strings.size(); ++i)
    args[4 + i] = Copy(strings[i]);
  Call(0x827BF8F8, args);
}

void GameMenuApt::SetArray(std::string_view path, std::span<const std::string> items) {
  // This interpreter exposes Array.length for reading but ignores writes.
  // Use its own splice method so removed values are released by the VM.
  const int32_t count = Number(std::string(path) + ".length");
  if (count > 0) {
    const std::array<std::string, 2> args{"0", std::to_string(count)};
    Invoke(std::string(path) + ".splice", args);
  }
  for (size_t i = 0; i < items.size(); ++i)
    SetText(std::string(path) + "." + std::to_string(i), items[i]);
}

}  // namespace simpsons
