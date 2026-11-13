// Work for the game's main thread, and the ScoreKeeper behind the Lua
// bindings in gamedata/simpsons_scores.lua.

#include "game_thread.h"
#include "achievements.h"
#include "guest_memory.h"
#include "guest_call_frame.h"
#include "debug_menu.h"
#include "game_ui.h"
#include "cutscene_skip.h"
#include "pause_menu.h"
#include "saves.h"
#include "snapshot.h"
#include "modkit_game.h"
#include "spawn_sources.h"

#include <deque>
#include <mutex>
#include <vector>

#include <rex/ppc.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>

// ScoreKeeper::GetScore(keeper, hash, context) -> f1, SetScore(keeper, hash,
// f1 = value, r6 = context) and IssueScoreEvent(keeper, hash, context), the
// natives under sk:GetScore, sk:SetScore and sk:IssueScoreEvent.
REX_EXTERN(sub_828A3100);
REX_EXTERN(sub_828A31A8);
REX_EXTERN(sub_828A3420);

namespace {
std::mutex g_mutex;
std::deque<GameTask> g_tasks;
rex::Runtime* g_runtime = nullptr;

// ScoreKeeper:GetScoreKeeper(); the number of scores is at +20.
constexpr uint32_t kScoreKeeper = 0x82D08BB8;
constexpr uint32_t kGlobalContext = 0;
}  // namespace

void InitGameThread(rex::Runtime* runtime) {
  g_runtime = runtime;
}

bool CallGuest(PPCContext& ctx, uint8_t* base, uint32_t address,
               std::initializer_list<uint32_t> args, uint32_t* result, double* float_result) {
  auto* function = g_runtime && address ? g_runtime->function_dispatcher()->GetFunction(address)
                                        : nullptr;
  if (!function)
    return false;
  return simpsons::InvokeGuestFrame(ctx, base, args,
      [&](PPCContext& call, uint8_t* memory) {
        call.ctr.u64 = address;
        function(call, memory);
      }, result, float_result);
}

void RunOnGameThread(GameTask task) {
  std::lock_guard lock(g_mutex);
  g_tasks.push_back(std::move(task));
}

void TickGameThread(PPCContext& ctx, uint8_t* base) {
  modkit_game::Context context(ctx, base);
  std::deque<GameTask> tasks;
  {
    std::lock_guard lock(g_mutex);
    tasks.swap(g_tasks);
  }
  for (auto& task : tasks) {
    simpsons::GuestContextScope restore(ctx);
    task(ctx, base);
  }
  spawn_sources::Tick(ctx, base);
  debug_menu::Tick(ctx, base);
  game_ui::Tick(ctx, base);
  TickCutsceneSkip(ctx, base);
  achievements::Tick(ctx, base);
  TickPauseMenu(ctx, base);
  snapshot::Tick(ctx, base);
  saves::Tick(ctx, base);
  modkit_game::Tick(ctx, base);
  // Last: loading a save gives the game the save's registers.
  game_ui::TickSaves(ctx, base);
}

namespace scores {

// The ScoreKeeper's name hash (sub_827451C0): ASCII lowercased, h * 65599 + c.
uint32_t Hash(std::string_view name) {
  uint32_t hash = 0;
  for (const char c : name) {
    uint32_t b = uint8_t(c);
    if (b - 'A' <= 'Z' - 'A')
      b += 32;
    hash = hash * 65599 + b;
  }
  return hash;
}

bool Ready(const uint8_t* base) {
  const uint32_t keeper = simpsons::LoadGuestU32(base, kScoreKeeper);
  return keeper && simpsons::LoadGuestU32(base, keeper + 20);
}

float Get(PPCContext& ctx, uint8_t* base, std::string_view name) {
  if (!Ready(base))
    return 0;
  const PPCContext saved = ctx;
  ctx.r3.u64 = simpsons::LoadGuestU32(base, kScoreKeeper);
  ctx.r4.u64 = Hash(name);
  ctx.r5.u64 = kGlobalContext;
  sub_828A3100(ctx, base);
  const float value = float(ctx.f1.f64);
  ctx = saved;
  return value;
}

void Set(PPCContext& ctx, uint8_t* base, std::string_view name, float value) {
  if (!Ready(base))
    return;
  const PPCContext saved = ctx;
  ctx.r3.u64 = simpsons::LoadGuestU32(base, kScoreKeeper);
  ctx.r4.u64 = Hash(name);
  ctx.f1.f64 = value;
  ctx.r6.u64 = kGlobalContext;
  sub_828A31A8(ctx, base);
  ctx = saved;
}

void Issue(PPCContext& ctx, uint8_t* base, std::string_view event) {
  if (!Ready(base))
    return;
  const PPCContext saved = ctx;
  ctx.r3.u64 = simpsons::LoadGuestU32(base, kScoreKeeper);
  ctx.r4.u64 = Hash(event);
  ctx.r5.u64 = kGlobalContext;
  sub_828A3420(ctx, base);
  ctx = saved;
}

}  // namespace scores
