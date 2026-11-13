// The production adapter executes unchanged native health/message routines.
// World events, class callbacks and automatic-update scheduling are explicit
// fixture boundaries; their full scene behavior is checked in a private game.
#include "../../simpsons/src/entity_health.h"
#include "../../simpsons/src/game_thread.h"
#include "../../simpsons/src/guest_memory.h"
#include <simpsons/modkit.hpp>
#include <rex/ppc/context.h>

#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <sys/mman.h>
#endif

REX_EXTERN(sub_82937200); REX_EXTERN(sub_82957208); REX_EXTERN(sub_82937118);
REX_EXTERN(sub_82956900); REX_EXTERN(sub_82955D00); REX_EXTERN(sub_8269ADC0);
REX_EXTERN(sub_827455E0); REX_EXTERN(sub_829573C8); REX_EXTERN(sub_82956530);
REX_EXTERN(sub_82937650); REX_EXTERN(sub_82881400); REX_EXTERN(sub_826D90C8);
REX_EXTERN(sub_82956768); REX_EXTERN(sub_8268F470); REX_EXTERN(sub_82937378);
REX_EXTERN(sub_823941C0); REX_EXTERN(sub_82937020); REX_EXTERN(sub_82956C00);
REX_EXTERN(sub_82A22928); REX_EXTERN(sub_8268FAA8); REX_EXTERN(sub_82690168);
REX_EXTERN(sub_829563B8); REX_EXTERN(sub_82880798); REX_EXTERN(sub_82955B08);
REX_EXTERN(sub_82393DD0); REX_EXTERN(sub_826A4518); REX_EXTERN(sub_82983CC8);
REX_EXTERN(sub_826900B0); REX_EXTERN(__savegprlr_29); REX_EXTERN(__restgprlr_29);
REX_EXTERN(__savegprlr_27); REX_EXTERN(__restgprlr_27);
REX_EXTERN(sub_82892340); REX_EXTERN(sub_829F75B0); REX_EXTERN(sub_8287DDA0);
REX_EXTERN(sub_8268F998); REX_EXTERN(sub_8268F918);

namespace {
constexpr uint32_t entity = 0x2000, bank = 0x3000, items = 0x4000;
constexpr uint32_t health = 0x5000, callback = 0x6000, callback_table = 0x6100;
constexpr uint32_t table = 0x8218AB98, messages = 0x82E00000;
constexpr size_t kGuestBytes = 0x100000000ull;
uint8_t* memory = nullptr;
int checks = 0, hits = 0, deaths = 0, heals = 0, events = 0, schedules = 0;
int infos = 0, releases = 0, handlers = 0;
uint32_t denied = 0;
uint64_t lifetime = 1;
bool protected_target = false, remove_on_hit = false;
void Require(bool ok, const char* text) {
  ++checks;
  if (!ok) throw std::runtime_error(text);
}
void Put(uint32_t at, uint32_t value) { simpsons::StoreGuestU32(memory, at, value); }
uint32_t Word(uint32_t at) { return simpsons::LoadGuestU32(memory, at); }
void Float(uint32_t at, float value) { simpsons::StoreGuestFloat(memory, at, value); }
float Float(uint32_t at) { return simpsons::LoadGuestFloat(memory, at); }
void Indirect(PPCContext& ctx, uint8_t*, uint32_t address);
void Commit(uint32_t at) {
#ifdef _WIN32
  if (!VirtualAlloc(memory + at, 65536, MEM_COMMIT, PAGE_READWRITE))
    throw std::runtime_error("Cannot commit a fixture memory page");
#endif
}
void Allocate() {
#ifdef _WIN32
  memory = static_cast<uint8_t*>(VirtualAlloc(nullptr, kGuestBytes, MEM_RESERVE, PAGE_NOACCESS));
#else
  auto* allocation = mmap(nullptr, kGuestBytes, PROT_READ | PROT_WRITE,
      MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  memory = allocation == MAP_FAILED ? nullptr : static_cast<uint8_t*>(allocation);
#endif
  if (!memory) throw std::runtime_error("Cannot reserve sparse guest fixture memory");
  // Reserve address space, not four GiB of RAM. Only these small regions are
  // committed/touched; production guest helpers read the real global addresses.
  for (auto at : {0u, 0x82180000u, 0x821D0000u, 0x82930000u, 0x82CF0000u,
                  0x82D50000u, messages}) Commit(at);
}
uint64_t Read64(const uint8_t* base, uint32_t at) {
  uint64_t value; std::memcpy(&value, base + at, 8); return std::byteswap(value);
}
void Write64(uint8_t* base, uint32_t at, uint64_t value) {
  value = std::byteswap(value); std::memcpy(base + at, &value, 8);
}
PPCContext Context() { PPCContext ctx{}; ctx.r1.u64 = 0xF000; return ctx; }
}  // namespace

#undef REX_LOAD_U32
#define REX_LOAD_U32(address) simpsons::LoadGuestU32(base, uint32_t(address))
#define REX_LOAD_U16(address) simpsons::LoadGuestU16(base, uint32_t(address))
#define REX_LOAD_U8(address) base[uint32_t(address)]
#define REX_STORE_U32(address, value) simpsons::StoreGuestU32(base, uint32_t(address), uint32_t(value))
#define REX_STORE_U8(address, value) (base[uint32_t(address)] = uint8_t(value))
#define REX_STORE_U16(address, value) do { const auto at = uint32_t(address); \
  base[at] = uint8_t(uint16_t(value) >> 8); base[at + 1] = uint8_t(value); } while (0)
#define REX_LOAD_U64(address) Read64(base, uint32_t(address))
#define REX_STORE_U64(address, value) Write64(base, uint32_t(address), uint64_t(value))
#undef REX_CALL_INDIRECT_FUNC
#define REX_CALL_INDIRECT_FUNC(address) Indirect(ctx, base, uint32_t(address))
#define REX_FUNC_PROLOGUE() ((void)0)
#include "native_health.h"

REX_FUNC(__savegprlr_29) { GameSave29(ctx, base); }
REX_FUNC(__restgprlr_29) { GameRestore29(ctx, base); }
REX_FUNC(__savegprlr_27) { GameSave27(ctx, base); }
REX_FUNC(__restgprlr_27) { GameRestore27(ctx, base); }
REX_FUNC(sub_82937200) { GameDamageCopy(ctx, base); }
REX_FUNC(sub_82937118) { GameDamageAssign(ctx, base); }
REX_FUNC(sub_82956900) { GameApplyDamage(ctx, base); }
REX_FUNC(sub_82955D00) { GameRegisterHealthTick(ctx, base); }
REX_FUNC(sub_827455E0) { GameReleaseEntityReference(ctx, base); }
REX_FUNC(sub_829573C8) { GameCheckHeal(ctx, base); }
REX_FUNC(sub_82956530) { GameApplyHeal(ctx, base); }
REX_FUNC(sub_82937650) { ++infos; GameDamageInit(ctx, base); }
REX_FUNC(sub_82881400) { ++releases; GameDamageDestroy(ctx, base); }
REX_FUNC(sub_82937378) { GameDamageReset(ctx, base); }
REX_FUNC(sub_823941C0) { GameAssignEntityReference(ctx, base); }
REX_FUNC(sub_82937020) { GameDamageMask(ctx, base); }
REX_FUNC(sub_829563B8) { GameOnDeath(ctx, base); }
REX_FUNC(sub_82880798) { GameDestroyEntityReference(ctx, base); }
REX_FUNC(sub_82957208) {
  if (protected_target) ctx.r3.u64 = 2;
  else GameCheckDamage(ctx, base);
}
REX_FUNC(sub_8268FAA8) {
  GameSendMessage(ctx, base);
}
REX_FUNC(sub_8268F998) { GameDeliverMessage(ctx, base); }
REX_FUNC(sub_8268F918) { throw std::runtime_error("Unexpected deferred subscriber cleanup in this fixture"); }
REX_FUNC(sub_826900B0) {
  Require(ctx.r3.u32 == health && ctx.r4.u32 == 0x82D573A0,
      "Native health recovery subscribes to the original update message");
  ++schedules;
}
REX_FUNC(sub_82393DD0) { ctx.r3.u64 = 0; }  // No authored death-effect GUID in this fixture.
REX_FUNC(sub_8269ADC0) { throw std::runtime_error("Unexpected component dependency"); }
REX_FUNC(sub_82956C00) { throw std::runtime_error("Scripted hits must not invent a faction source"); }
REX_FUNC(sub_82A22928) { throw std::runtime_error("Unexpected class vulnerability query"); }
REX_FUNC(sub_82690168) { throw std::runtime_error("Unexpected authored action in this fixture"); }
REX_FUNC(sub_826D90C8) { throw std::runtime_error("Unexpected post-spawn message"); }
REX_FUNC(sub_82956768) { throw std::runtime_error("Unexpected update message"); }
REX_FUNC(sub_8268F470) { throw std::runtime_error("Unexpected update unsubscription"); }
REX_FUNC(sub_82955B08) { throw std::runtime_error("Unexpected component property notification"); }
REX_FUNC(sub_826A4518) { throw std::runtime_error("Unexpected death-effect lookup"); }
REX_FUNC(sub_82983CC8) { throw std::runtime_error("Unexpected death-effect activation"); }
REX_FUNC(sub_82892340) { ctx.r3.u64 = 0; }  // Normal character damage state.
REX_FUNC(sub_829F75B0) { throw std::runtime_error("Unexpected class damage-state dependency"); }
REX_FUNC(sub_8287DDA0) { throw std::runtime_error("Source-free hits cannot impersonate a player attacker"); }

namespace {
void Indirect(PPCContext& ctx, uint8_t* base, uint32_t address) {
  switch (address) {
    case 0x82956FF0: GameHealthCurrent(ctx, base); break;
    case 0x82956FF8: GameHealthMaximum(ctx, base); break;
    case 0x82957018: GameIsDead(ctx, base); break;
    case 0x82955FC0: GameSetHealth(ctx, base); break;
    case 0x82955C28: GameLastDamage(ctx, base); break;
    case 0x82882C48: GameDamageMemberThunk(ctx, base); break;
    case 0x828836F8: GamePlayerDamageFilter(ctx, base); break;
    case 0x1200: {
      Require(ctx.r3.u32 == entity + 56, "Messages target the original entity subscriber");
      const auto id = Word(ctx.r4.u32);
      if (id == 0x9000 || id == 0x9100) {
        Require(Word(ctx.r4.u32 + 8) == 0, "Synchronous delivery borrows its original message payload");
        ++handlers; ctx.r3.u64 = health; GameHealthMessage(ctx, base);
      } else if (id == 0x9200) {
        Require(Word(Word(ctx.r4.u32 + 4) + 52) == 13,
            "Native hit notification preserves the scripted damage record");
        ++events;
        if (remove_on_hit) { ++lifetime; denied = entity; }
      } else throw std::runtime_error("Unexpected entity message");
      break;
    }
    case 0x1100: ++hits; break;
    case 0x1104: ++deaths; break;
    case 0x1108: ++heals; break;
    default: throw std::runtime_error("Unexpected health virtual method");
  }
}
void Reset(float current = 100, float maximum = 100) {
  std::memset(memory, 0, 65536);
  denied = 0; lifetime = 1; protected_target = remove_on_hit = false;
  hits = deaths = heals = events = schedules = infos = releases = handlers = 0;
  Put(entity + 48, bank); Put(entity + 52, items);
  Put(entity + 56, 0x9300); Put(entity + 60, 1); Put(0x9304, 0x1200);
  Put(entity + 140, 0x8000); Put(0x80A0, 0x8000);
  Float(0x8010, 1); Float(0x8024, 1); Float(0x8038, 1);
  Float(0x8040, 47); Float(0x8044, -3); Float(0x8048, 11);
  Put(bank + 12, 3u << 16); Put(items + 4, 0x7000); Put(items + 8, health); Put(items + 12, 0x7200);
  Put(health, table); Put(health + 4, 5); Put(health + 8, entity); Put(health + 40, entity);
  Float(health + 16, current); Float(health + 20, maximum); Float(health + 24, 999);
  Put(health + 44, callback); Put(callback, callback_table);
  Put(callback_table, 0x1100); Put(callback_table + 4, 0x1104); Put(callback_table + 8, 0x1108);
  Put(health + 76, 15871); Put(health + 80, 1);
  Put(table + 32, 0x82956FF0); Put(table + 36, 0x82956FF8);
  Put(table + 52, 0x82957018); Put(table + 56, 0x82955FC0);
  Put(table + 48, 0x82955C28);
  Put(messages + 0x7CC4, 0x9000); Put(messages + 0x7CD4, 0x9100); Put(messages + 0x7CB4, 0x9200);
  Put(0x82D572BC, 1000);
  auto ctx = Context(); ctx.r3.u64 = health + 124; GameDamageInit(ctx, memory);
}
}  // namespace

namespace spawner {
bool Readable(uint32_t address, uint32_t size) {
  return address >= 0x100 && uint64_t(address) + size <= 65536 &&
      (!denied || denied < address || denied >= uint64_t(address) + size);
}
uint64_t Observe(uint32_t) { return lifetime; }
uint64_t Lifetime(uint32_t) { return lifetime; }
}  // namespace spawner

bool CallGuest(PPCContext& ctx, uint8_t* base, uint32_t address,
               std::initializer_list<uint32_t> args, uint32_t* result, double* floating) {
  const PPCContext saved = ctx;
  ctx.r1.u64 -= 512;
  PPCRegister* regs[] = {&ctx.r3, &ctx.r4, &ctx.r5, &ctx.r6, &ctx.r7, &ctx.r8, &ctx.r9, &ctx.r10};
  size_t i = 0; for (auto arg : args) regs[i++]->u64 = arg;
  switch (address) {
    case 0x82956FF0: GameHealthCurrent(ctx, base); break;
    case 0x82956FF8: GameHealthMaximum(ctx, base); break;
    case 0x82956FC0: GameHealthEnabled(ctx, base); break;
    case 0x82957018: GameIsDead(ctx, base); break;
    case 0x82937650: sub_82937650(ctx, base); break;
    case 0x82881400: sub_82881400(ctx, base); break;
    case 0x82957478: ++handlers; GameHealthMessage(ctx, base); break;
    case 0x8268FAA8: GameSendMessage(ctx, base); break;
    case 0x823F2540: ctx.r3.u64 = ctx.r3.u32 + 16; break;  // Frame's world matrix backend.
    default: ctx = saved; return false;
  }
  if (result) *result = ctx.r3.u32;
  if (floating) *floating = ctx.f1.f64;
  ctx = saved;
  return true;
}

int main(int argc, char** argv) {
  try {
    Allocate(); auto ctx = Context();
    using namespace entity_health;
    Reset();
    Require(ComponentOwner(memory, health) == entity,
        "Accepted damage resolves an exact component-bank owner");
    Put(health + 40, entity + 4);
    Require(!ComponentOwner(memory, health), "Conflicting health owners are rejected");
    Reset(); Put(items + 8, 0);
    Require(!ComponentOwner(memory, health), "An orphaned health allocation is not a live actor");
    Reset(); Put(health, table + 4);
    Require(!ComponentOwner(memory, health), "An unrelated component cannot trigger a knockdown");
    Reset();
    auto h = Get(ctx, memory, entity);
    Require(h && h->current == 100 && h->maximum == 100 && !h->dead && h->enabled,
        "Read the real health component, using its healing limit rather than the low-health reference");
    Require(!DyingEntity(memory, health), "A living component cannot manufacture a death signal");
    const auto saved = ctx;
    const auto change = Apply(ctx, memory, entity, Action::kDamage, 25);
    Require(change && change->before.current == 100 && change->after->current == 75 &&
        hits == 1 && events == 1 && deaths == 0 && infos == releases && handlers == 1,
        "Damage follows native messages, hit callbacks and temporary reference destruction");
    Require(Word(health + 124 + 52) == 13 && Float(health + 124 + 44) == 25,
        "The native handler retains the actual last-damage record");
    Require(Word(health + 124 + 64) == 1000 && Float(health + 124 + 20) == 47 &&
        Float(health + 124 + 24) == -3 && Float(health + 124 + 28) == 11 &&
        Float(health + 124 + 32) == 47 && Float(health + 124 + 36) == -3 && Float(health + 124 + 40) == 11,
        "Authored hits carry the original simulation timestamp and target world impact coordinates");
    Require(ctx.r1.u64 == saved.r1.u64 && ctx.r3.u64 == saved.r3.u64 && ctx.f1.u64 == saved.f1.u64,
        "Host health operations preserve the caller's PPC registers");
    const auto healed = Apply(ctx, memory, entity, Action::kHeal, 1000);
    Require(healed && healed->after->current == 100 && heals == 1 && hits == 1,
        "Native healing clamps to the normal maximum and invokes recovery callbacks");
    Reset(); protected_target = true;
    Require(Apply(ctx, memory, entity, Action::kDamage, 1000)->after->current == 100 &&
        !hits && !deaths && !events && infos == releases, "The existing damage-protection hook is honored");
    Reset();
    Put(health + 52, entity); Put(health + 56, 0x828836F8); Put(health + 72, 0x82882C48);
    Put(entity + 344, health); Put(entity + 2604, 0x7800); Put(0x7800 + 180, 300);
    Require(Apply(ctx, memory, entity, Action::kDamage, 5)->after->current == 95,
        "A fresh authored timestamp passes the unchanged native player damage filter");
    Require(Apply(ctx, memory, entity, Action::kDamage, 5)->after->current == 95 && hits == 1,
        "The same-frame native hit cooldown remains active");
    Put(0x82D572BC, 1400);
    Require(Apply(ctx, memory, entity, Action::kDamage, 5)->after->current == 90 && hits == 2,
        "A later simulation timestamp accepts the next hit without a timebase speed hack");
    Reset(); Put(health + 28, 1);
    Require(!Get(ctx, memory, entity)->enabled &&
        Apply(ctx, memory, entity, Action::kDamage, 5)->after->current == 100 && !hits,
        "Disabled health components use the original damage filter");
    Reset(); Put(entity + 60, 0);
    Require(Apply(ctx, memory, entity, Action::kDamage, 5)->after->current == 100 &&
        handlers == 0 && hits == 0 && infos == releases,
        "The original subscriber activation check rejects inactive entities without forcing damage");
    Reset(); Put(health + 76, 0);
    Require(Apply(ctx, memory, entity, Action::kDamage, 5)->after->current == 100 && !hits,
        "The native accepted-damage mask remains effective");
    Reset(); Put(health + 28, 8); Float(health + 168, 17);
    Require(Apply(ctx, memory, entity, Action::kDamage, 5)->after->current == 100 && hits == 1 &&
        Float(health + 168) == 0, "Absorbed hits retain callbacks and reset their native recovery timer");
    Reset(); Put(health + 28, 0x200); Put(0x82D572BC, 42); Put(0x82CF8E88, 0);
    Require(Apply(ctx, memory, entity, Action::kDamage, 5)->after->current == 95 && schedules == 1,
        "Damage retains the original automatic-recovery scheduling");
    Reset(30); auto dead = Apply(ctx, memory, entity, Action::kDamage, 1000);
    Require(dead && dead->after && dead->after->current == 0 && dead->after->dead && deaths == 1 &&
        (Word(health + 28) & 0x10000), "A lethal native hit invokes death handling and marks the component");
    Require(DyingEntity(memory, health) == entity,
        "The native death hook identifies the same recognized component and owner");
    Require(Apply(ctx, memory, entity, Action::kHeal, 1000)->after->dead &&
        Float(health + 16) == 0 && !heals, "Healing cannot resurrect a native dead actor");
    Require(Apply(ctx, memory, entity, Action::kDamage, 5)->after->dead && deaths == 1,
        "Repeated hits do not dispatch death twice");
    Reset(0); Put(health + 28, 2);
    Require(!Get(ctx, memory, entity)->dead && Apply(ctx, memory, entity, Action::kHeal, 5)->after->current == 5,
        "The original immortal-at-zero policy is preserved");
    Float(health + 16, 0);
    Require(!DyingEntity(memory, health), "Immortal-at-zero actors do not emit a native death signal");
    Reset(); remove_on_hit = true;
    auto removed = Apply(ctx, memory, entity, Action::kDamage, 1);
    Require(removed && removed->removed && !removed->after && infos == releases,
        "Native target removal or address reuse never causes a post-callback allocation read");
    for (float amount : {-1.0f, 1000001.0f, std::numeric_limits<float>::infinity(),
                         std::numeric_limits<float>::quiet_NaN()}) {
      Reset(); Require(!Apply(ctx, memory, entity, Action::kDamage, amount) && handlers == 0,
          "Invalid health amounts do not reach native code");
    }
    Reset(); Require(Apply(ctx, memory, entity, Action::kDamage, 0)->after->current == 100 &&
        handlers == 0, "Zero damage is a side-effect-free no-op");
    for (int variant = 0; variant < 7; ++variant) {
      Reset();
      if (variant == 0) Put(entity + 48, 0);
      if (variant == 1) Put(entity + 52, 0);
      if (variant == 2) Put(bank + 12, 257u << 16);
      if (variant == 3) Put(items + 8, 0);
      if (variant == 4) Put(health + 8, entity + 16);
      if (variant == 5) Put(items + 12, health);
      if (variant == 6) Float(health + 16, std::numeric_limits<float>::quiet_NaN());
      Require(!Get(ctx, memory, entity) && !Apply(ctx, memory, entity, Action::kHeal, 5) && handlers == 0,
          "Missing, invalid, ambiguous or foreign health components fail before native mutation");
    }
    Reset();
    // A previous attacker reference is released by the game's real assignment
    // routine when our source-free hit replaces the persistent damage record.
    constexpr uint32_t source = 0x7500;
    Put(health + 128, source); Put(source + 4, health + 128);
    Apply(ctx, memory, entity, Action::kDamage, 5);
    Require(Word(source + 4) == 0 && Word(health + 128) == 0,
        "Replacing last damage detaches old intrusive source references correctly");
    std::mt19937 random(0x20071030);
    for (int i = 0; i < 500; ++i) {
      const float maximum = float(1 + random() % 10000), current = float(1 + random() % uint32_t(maximum));
      const float amount = float(random() % 20000); Reset(current, maximum);
      const auto action = i & 1 ? Action::kHeal : Action::kDamage;
      const auto result = Apply(ctx, memory, entity, action, amount);
      const float expected = action == Action::kHeal ? std::min(maximum, current + amount)
                                                    : std::max(0.0f, current - amount);
      Require(result && result->after && result->after->current == expected && infos == releases,
          "Native health arithmetic and temporary lifetime agree across varied hit/heal requests");
    }
    using simpsons::modkit::Json;
    Json snapshot = {{"current", 75}, {"maximum", 100}, {"dead", false}, {"enabled", true}};
    struct Replies { Json value; std::string expected; } reply;
    SimModHost host{sizeof(SimModHost), SIM_MOD_ABI_VERSION, &reply};
    host.call = [](void* context, const char* method, const char* arguments,
                   SimModWriter writer, void* user) -> SimModStatus {
      auto& reply = *static_cast<Replies*>(context); const auto args = Json::parse(arguments);
      Require(method == reply.expected, "C++ health helpers use their documented named operation");
      const auto key = reply.expected.starts_with("players.") ? "player" : "entity";
      Require(args.at(key) == "test:1" && (reply.expected.ends_with(".health") || args.at("amount") == 12),
          "C++ health helpers preserve the target and requested amount");
      const auto result = Json{{"ok", true}, {"value", reply.value}}.dump();
      writer(user, result.data(), result.size()); return SIM_MOD_OK;
    };
    simpsons::modkit::Game game(&host); const simpsons::modkit::Handle handle{"test:1"};
    reply = {snapshot, "players.health"};
    Require(game.PlayerHealth(handle)->current == 75, "The native SDK decodes a typed player health snapshot");
    reply.expected = "entities.health";
    Require(game.EntityHealth(handle)->maximum == 100, "The native SDK decodes owned health snapshots");
    reply.value = nullptr;
    Require(!game.EntityHealth(handle), "The native SDK preserves an absent health component");
    const Json change_reply = {{"before", snapshot}, {"after", snapshot}, {"removed", false}};
    reply = {change_reply, "players.damage"}; Require(!game.DamagePlayer(handle, 12).removed, "Typed player damage reply");
    reply.expected = "players.heal"; Require(game.HealPlayer(handle, 12).after->current == 75, "Typed player heal reply");
    reply.expected = "entities.damage"; Require(game.DamageEntity(handle, 12).before.maximum == 100, "Typed owned damage reply");
    reply.expected = "entities.heal"; Require(game.HealEntity(handle, 12).after->enabled, "Typed owned heal reply");
    reply.value["after"] = nullptr; reply.value["removed"] = true;
    Require(!game.HealEntity(handle, 12).after && game.HealEntity(handle, 12).removed,
        "The native SDK preserves target removal without fabricating an after snapshot");
    reply.value["before"] = nullptr;
    bool invalid_reply = false;
    try { game.HealEntity(handle, 12); }
    catch (const simpsons::modkit::Error& error) { invalid_reply = error.code() == "invalid_reply"; }
    Require(invalid_reply, "An invalid native reply rejects instead of dereferencing an empty snapshot");
    const auto report = "{\"ok\":true,\"checks\":" + std::to_string(checks) + "}\n";
    if (argc > 1) {
      std::ofstream output(argv[1]); output << report;
      if (!output) throw std::runtime_error("Cannot write health qualification report");
    }
    std::cout << report;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n'; return 1;
  }
}
