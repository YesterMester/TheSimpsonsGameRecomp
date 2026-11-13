// The production adapter runs the unchanged native hide/show methods. Model
// notifications and the spatial container are explicit fixture boundaries;
// a private game separately qualifies rendered disappearance and reappearance.
#include "../../simpsons/src/entity_render.h"
#include "../../simpsons/src/game_thread.h"
#include "../../simpsons/src/guest_memory.h"
#include <simpsons/modkit.hpp>
#include <rex/ppc/context.h>

#include <fstream>
#include <iostream>
#include <random>
#include <stdexcept>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <sys/mman.h>
#endif

REX_EXTERN(sub_826AF408);
REX_EXTERN(sub_826AF4B0);
REX_EXTERN(sub_82394368);
REX_EXTERN(sub_826ADD28);
REX_EXTERN(sub_826ACD38);
REX_EXTERN(sub_826ACDF0);
REX_EXTERN(__savegprlr_29);
REX_EXTERN(__restgprlr_29);

namespace {
constexpr uint32_t entity = 0x2000, table = 0x3000, model = 0x4000, model_table = 0x5000;
constexpr uint32_t node = 0x6000, cull = 0x6200, scene = 0x7000, containers = 0x7100,
                   node_link = 0x7200;
constexpr uint32_t kScene = 0x82D08BF0, kZero = 0x821DD0D8;
constexpr size_t kGuestBytes = 0x100000000ull;
uint8_t* memory = nullptr;
uint32_t denied = 0;
int checks = 0, shows = 0, hides = 0, attaches = 0, detaches = 0;
bool reject_attachment = false;
void Require(bool value, const char* text) {
  ++checks;
  if (!value)
    throw std::runtime_error(text);
}
void Put(uint32_t at, uint32_t value) {
  simpsons::StoreGuestU32(memory, at, value);
}
uint32_t Word(uint32_t at) {
  return simpsons::LoadGuestU32(memory, at);
}
void Byte(uint32_t at, uint8_t value) {
  simpsons::GuestToHost(memory, at)[0] = value;
}
void Indirect(PPCContext& ctx, uint8_t* base, uint32_t address);
uint64_t Read64(const uint8_t* base, uint32_t at) {
  uint64_t value;
  std::memcpy(&value, base + at, 8);
  return std::byteswap(value);
}
void Write64(uint8_t* base, uint32_t at, uint64_t value) {
  value = std::byteswap(value);
  std::memcpy(base + at, &value, 8);
}
void Allocate() {
#ifdef _WIN32
  memory = static_cast<uint8_t*>(VirtualAlloc(nullptr, kGuestBytes, MEM_RESERVE, PAGE_NOACCESS));
  if (memory)
    for (auto at : {0u, kScene & ~0xFFFFu, kZero & ~0xFFFFu})
      if (!VirtualAlloc(memory + at, 65536, MEM_COMMIT, PAGE_READWRITE))
        throw std::runtime_error("Cannot commit render fixture pages");
#else
  auto* allocation = mmap(nullptr, kGuestBytes, PROT_NONE,
                          MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  memory = allocation == MAP_FAILED ? nullptr : static_cast<uint8_t*>(allocation);
  if (memory)
    for (auto at : {0u, kScene & ~0xFFFFu, kZero & ~0xFFFFu})
      if (mprotect(memory + at, 65536, PROT_READ | PROT_WRITE))
        throw std::runtime_error("Cannot commit render fixture pages");
#endif
  if (!memory)
    throw std::runtime_error("Cannot reserve sparse render fixture memory");
}
}  // namespace

#undef REX_LOAD_U32
#define REX_LOAD_U32(address) simpsons::LoadGuestU32(base, uint32_t(address))
#define REX_LOAD_U8(address) simpsons::GuestToHost(base, uint32_t(address))[0]
#define REX_STORE_U32(address, value) \
  simpsons::StoreGuestU32(base, uint32_t(address), uint32_t(value))
#define REX_STORE_U8(address, value) \
  (simpsons::GuestToHost(base, uint32_t(address))[0] = uint8_t(value))
#define REX_LOAD_U64(address) Read64(base, uint32_t(address))
#define REX_STORE_U64(address, value) Write64(base, uint32_t(address), uint64_t(value))
#undef REX_CALL_INDIRECT_FUNC
#define REX_CALL_INDIRECT_FUNC(address) Indirect(ctx, base, uint32_t(address))
#define REX_FUNC_PROLOGUE() ((void)0)
#include "native_render.h"

REX_FUNC(__savegprlr_29) {
  GameSave29(ctx, base);
}
REX_FUNC(__restgprlr_29) {
  GameRestore29(ctx, base);
}
REX_FUNC(sub_826AF408) {
  GameAttachNode(ctx, base);
}
REX_FUNC(sub_826AF4B0) {
  GameDetachNode(ctx, base);
}
REX_FUNC(sub_82394368) {
  Require(ctx.r3.u32 == cull && ctx.r4.u32 == node,
          "Native insertion refreshes this object's own culling state");
}
REX_FUNC(sub_826ADD28) {
  Require(ctx.r4.u32 == node, "Spatial insertion receives the original native node");
  if (!reject_attachment) {
    ++attaches;
    Put(node + 28, node_link);
    Put(node_link, 0x7400);
    Put(node_link + 4, 0);
  }
  ctx.r3.u64 = !reject_attachment;
}
REX_FUNC(sub_826ACD38) {
  sub_826ADD28(ctx, base);
}
REX_FUNC(sub_826ACDF0) {
  ++detaches;
}

namespace {
void Indirect(PPCContext& ctx, uint8_t* base, uint32_t address) {
  switch (address) {
    case 0x82697188:
      GameHide(ctx, base);
      break;
    case 0x826971C0:
      GameShow(ctx, base);
      break;
    case 0x82697200:
      GameForceShow(ctx, base);
      break;
    case 0x826993F0:
      GameAttachModel(ctx, base);
      break;
    case 0x826994E0:
      GameDetachModel(ctx, base);
      break;
    case 0x12340000:
      ++shows;
      Require(ctx.r3.u32 == model, "Only this native model is shown");
      break;
    case 0x12340004:
      ++hides;
      Require(ctx.r3.u32 == model, "Only this native model is hidden");
      break;
    case 0x12340008:
      Require(ctx.r3.u32 == entity,
              "Scene insertion obtains bounds from the entity's original class");
      for (uint32_t i = 0; i < 4; ++i)
        simpsons::StoreGuestFloat(base, ctx.r4.u32 + i * 4, i == 3 ? 1 : 0);
      break;
    default:
      throw std::runtime_error("Unreviewed render fixture virtual dispatch");
  }
}
void Reset(uint8_t count = 0) {
  std::memset(memory, 0, 65536);
  Put(entity, table);
  Put(entity + 136, model);
  Put(entity + 156, node);
  Put(entity + 160, cull);
  Put(table + 96, 0x82697188);
  Put(table + 100, 0x826971C0);
  Put(table + 104, 0x82697200);
  Put(table + 236, 0x12340008);
  Put(table + 256, 0x826993F0);
  Put(table + 260, 0x826994E0);
  Put(model, model_table);
  Put(model_table + 40, 0x12340000);
  Put(model_table + 44, 0x12340004);
  Put(kScene, scene);
  Put(scene + 12, 0x7300);
  Put(scene + 16, containers);
  Put(containers, 0x7400);
  Put(containers + 4, 0x7500);
  Put(kZero, 0);
  Byte(entity + 206, count);
  Byte(node + 26, count ? 0 : 4);
  if (!count) {
    Put(node + 28, node_link);
    Put(node_link, 0x7400);
  }
  denied = 0;
  shows = hides = attaches = detaches = 0;
  reject_attachment = false;
}
}  // namespace

namespace spawner {
bool Readable(uint32_t at, uint32_t bytes) {
  return at && !(at <= denied && uint64_t(at) + bytes > denied) && uint64_t(at) + bytes <= 0x10000;
}
}  // namespace spawner

bool CallGuest(PPCContext& ctx, uint8_t* base, uint32_t address,
               std::initializer_list<uint32_t> args, uint32_t* result, double*) {
  if (address != 0x82697188 && address != 0x826971C0)
    return false;
  auto saved = ctx;
  ctx.r1.u64 -= 512;
  size_t i = 0;
  for (auto value : args)
    (&ctx.r3)[i++].u64 = value;
  Indirect(ctx, base, address);
  if (result)
    *result = ctx.r3.u32;
  ctx = saved;
  return true;
}

int main(int argc, char** argv) {
  try {
    Allocate();
    PPCContext ctx{};
    ctx.r1.u64 = 0xF000;
    using namespace entity_render;
    Reset();
    auto state = Get(memory, entity);
    Require(state && !state->hide_count && state->has_model && state->scene_attached,
            "Native scene membership and hide count are independent readable properties");
    Require(Change(ctx, memory, entity, true) && Get(memory, entity)->hide_count == 1 &&
                !Get(memory, entity)->scene_attached && hides == 1 && detaches == 1,
            "First hide removes this object's native model and spatial node");
    Require(Change(ctx, memory, entity, true) && Get(memory, entity)->hide_count == 2 && hides == 1,
            "A second independent native hide request does not detach twice");
    Require(Change(ctx, memory, entity, false) && Get(memory, entity)->hide_count == 1 && !shows,
            "Releasing one native hide preserves the other request");
    Require(Change(ctx, memory, entity, false) && !Get(memory, entity)->hide_count &&
                Get(memory, entity)->scene_attached && shows == 1 && attaches == 1,
            "The last reveal restores the original model, bounds, culling and scene node");
    Require(Change(ctx, memory, entity, false) && shows == 1,
            "A reveal at zero does not insert the object twice");
    Reset(255);
    Require(!Change(ctx, memory, entity, true) && Get(memory, entity)->hide_count == 255 && !hides,
            "Overflow rejects before native code can wrap a hidden object to zero");
    for (auto offset : {96u, 100u, 104u, 256u, 260u}) {
      Reset();
      Put(table + offset, 1);
      Require(!Get(memory, entity) && !Change(ctx, memory, entity, true) && !hides,
              "An unreviewed class layout rejects before reading or changing its hide counter");
    }
    for (auto invalid : {0u, 1u, 0xFFFFFFFCu}) {
      Reset();
      Require(!Get(memory, invalid), "Null, unaligned and overflowing objects reject safely");
    }
    for (auto at :
         {entity + 207, table + 263, model + 3, model_table + 63, node + 31, cull + 163}) {
      Reset();
      denied = at;
      Require(!Get(memory, entity) && !Change(ctx, memory, entity, true),
              "Truncated entity, table, model, scene or culling storage rejects before native "
              "mutation");
    }
    Reset();
    Put(entity + 136, 0);
    Put(entity + 156, 0);
    Require(
        Get(memory, entity) && !Get(memory, entity)->has_model &&
            !Get(memory, entity)->scene_attached && Change(ctx, memory, entity, true),
        "An entity without a model reports logical visibility without inventing scene membership");
    Reset(1);
    reject_attachment = true;
    Require(
        Change(ctx, memory, entity, false) && !Get(memory, entity)->hide_count &&
            !Get(memory, entity)->scene_attached,
        "A rejected spatial insertion is observable instead of claiming the object is on screen");
    Reset(7);
    ctx.r3.u64 = entity;
    GameForceShow(ctx, memory);
    Require(!Get(memory, entity)->hide_count && shows == 1 && Get(memory, entity)->scene_attached,
            "The original force-show resets every hide and still invokes the original show method");
    std::mt19937 random(0x20071030);
    for (int i = 0; i < 500; ++i) {
      const auto initial = uint8_t(random() % 255);
      Reset(initial);
      Require(Change(ctx, memory, entity, true) && Get(memory, entity)->hide_count == initial + 1,
              "Adding one mod hide preserves varied existing authored requests");
      Require(Change(ctx, memory, entity, false) && Get(memory, entity)->hide_count == initial,
              "Releasing the mod hide restores the exact authored hide count");
    }
    using simpsons::modkit::Json;
    struct Reply {
      Json value;
      bool mutate;
    } reply;
    SimModHost host{sizeof(SimModHost), SIM_MOD_ABI_VERSION, &reply};
    host.call = [](void* context, const char* method, const char* arguments, SimModWriter writer,
                   void* user) -> SimModStatus {
      auto& reply = *static_cast<Reply*>(context);
      const auto args = Json::parse(arguments);
      Require(std::string_view(method) ==
                      (reply.mutate ? "entities.set_visible" : "entities.render_state") &&
                  args.at("entity") == "entity:test" &&
                  (!reply.mutate || args.at("visible") == false),
              "Native C++ visibility helpers preserve the named operation and owner handle");
      const auto text = Json{{"ok", true}, {"value", reply.value}}.dump();
      writer(user, text.data(), text.size());
      return SIM_MOD_OK;
    };
    simpsons::modkit::Game game(&host);
    reply.value = {
        {"hidden_by_mod", true}, {"hide_count", 2}, {"has_model", true}, {"scene_attached", false}};
    reply.mutate = false;
    auto sdk = game.RenderState({"entity:test"});
    Require(sdk && sdk->hidden_by_mod && sdk->hide_count == 2 && !sdk->scene_attached,
            "The C++ helper decodes all render state fields");
    reply.mutate = true;
    Require(game.SetVisible({"entity:test"}, false).has_value(),
            "The C++ helper can request a hidden object");
    reply.value = nullptr;
    Require(!game.SetVisible({"entity:test"}, false),
            "The C++ helper accepts removal by a native model callback");
    reply.mutate = false;
    Require(!game.RenderState({"entity:test"}),
            "The C++ helper accepts an unsupported visibility query");
    if (argc == 2) {
      std::ofstream output(argv[1]);
      output << Json{{"ok", true}, {"checks", checks}}.dump() << '\n';
      if (!output)
        throw std::runtime_error("Cannot write native render report");
    }
    std::cout << checks << " native entity render checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
