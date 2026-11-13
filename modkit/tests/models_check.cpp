#include "models.h"
#include "streams.h"
#include "source.h"
#include "../../simpsons/src/guest_memory.h"
#include <rex/ppc/context.h>

#include <bit>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>

using namespace simpsons::modkit;
REX_EXTERN(sub_82719770);
REX_EXTERN(__imp__sub_82719770);
namespace {
int checks = 0;
std::vector<uint8_t> memory;
constexpr uint32_t base_address = 0x1000;
constexpr uint32_t refs = 88, state = 184, variable = 216, props = 228;
constexpr uint32_t conditions = 348, part = 360, strings = 500;
uint32_t payload_size = 0;
const ModelVariants* hook_variants = nullptr;
bool hook_throw = false, original_throw = false;
int hook_calls = 0, original_calls = 0;
void Require(bool value, const char* message) {
  ++checks;
  if (!value)
    throw std::runtime_error(message);
}
template <class Function>
void Reject(Function function, const char* message) {
  bool rejected = false;
  try {
    function();
  } catch (const Failure&) {
    rejected = true;
  }
  Require(rejected, message);
}
uint32_t Read(uint32_t at, uint32_t width = 4) {
  Require(at >= base_address && uint64_t(at) + width <= uint64_t(base_address) + payload_size,
          "The original model reader stays within the validated payload");
  return width == 2 ? uint16_t(uint16_t(memory.at(at)) << 8 | memory.at(at + 1))
                    : simpsons::LoadGuestU32(memory.data(), at);
}
void Write(uint32_t at, uint32_t value) {
  Read(at);
  simpsons::StoreGuestU32(memory.data(), at, value);
}
}  // namespace
#undef REX_LOAD_U32
#define REX_LOAD_U32(address) Read(uint32_t(address))
#define REX_LOAD_U16(address) Read(uint32_t(address), 2)
#define REX_STORE_U32(address, value) Write(uint32_t(address), uint32_t(value))
#define REX_FUNC_PROLOGUE() ((void)0)
#include "native_model.h"

// Only the host-memory boundary is a fixture seam. The production hook and
// unchanged native parser run below, with complete memory/register comparisons.
namespace modkit_models {
void Apply(uint8_t* base, uint32_t address, uint32_t size) {
  ++hook_calls;
  if (hook_throw)
    throw Failure("fixture", "Model preparation failure");
  if (hook_variants) {
    Require(uint64_t(address) + size <= memory.size(), "Fixture model buffer is bounded");
    if (const auto edit = hook_variants->Apply({base + address, size}))
      std::copy(edit->bytes.begin(), edit->bytes.end(), base + address);
  }
}
}  // namespace modkit_models
REX_FUNC(__imp__sub_82719770) {
  ++original_calls;
  if (original_throw) {
    ctx.r8.u64 = 0x12345678;
    throw std::runtime_error("original parser failure");
  }
  GameModelRelocate(ctx, base);
}

namespace {
void Oracle(const ModelDefinition& model) {
  payload_size = uint32_t(model.bytes().size());
  memory.assign(base_address + payload_size + 128, 0xD3);
  std::copy(model.bytes().begin(), model.bytes().end(), memory.begin() + base_address);
  PPCContext context{};
  context.r3.u64 = base_address;
  context.r4.u64 = payload_size;
  GameModelRelocate(context, memory.data());
  Require(context.r3.u32 == base_address, "The original parser accepts the validated model");
  const auto relocated = model.Relocated(base_address);
  Require(std::equal(relocated.begin(), relocated.end(), memory.begin() + base_address),
          "The host schema identifies exactly the original native relocation fields");
  context.r3.u64 = base_address;
  GameModelUnrelocate(context, memory.data());
  Require(std::equal(model.bytes().begin(), model.bytes().end(), memory.begin() + base_address),
          "The unchanged original inverse restores every source byte");
  Require(std::all_of(memory.begin(), memory.begin() + base_address,
                      [](auto c) { return c == 0xD3; }) &&
              std::all_of(memory.end() - 128, memory.end(), [](auto c) { return c == 0xD3; }),
          "Original operations preserve the surrounding guards");
}
std::vector<uint8_t> Fixture() {
  std::vector<uint8_t> bytes(768);
  auto word = [&](uint32_t at, uint32_t value) {
    simpsons::StoreGuestU32(bytes.data(), at, value);
  };
  auto half = [&](uint32_t at, uint16_t value) {
    bytes[at] = uint8_t(value >> 8);
    bytes[at + 1] = uint8_t(value);
  };
  word(0, 0x4D4D646C);
  word(4, 10);
  word(8, uint32_t(bytes.size()));
  for (int i = 0; i < 4; ++i)
    word(16 + i * 4, uint32_t(i + 1));
  word(44, refs);
  half(76, 3);
  word(48, state);
  half(78, 1);
  word(52, variable);
  half(80, 1);
  word(56, props);
  half(82, 5);
  word(60, conditions);
  half(84, 1);
  word(64, part);
  half(86, 1);
  for (uint32_t i = 0; i < 3; ++i) {
    word(refs + i * 32 + 8, i == 2 ? 0x12345678 : 0x9E3791CF);
    for (uint32_t j = 0; j < 4; ++j)
      word(refs + i * 32 + 12 + j * 4, i * 4 + j + 1);
  }
  std::copy_n("ModelPart\0Model\0distance\0lod1\0LodState\0transform\0offset\0layer\0flag", 67,
              bytes.begin() + strings);
  word(state + 4, strings + 30);
  word(state + 8, strings + 25);
  word(state + 20, 400);
  half(state + 28, 1);
  word(400, part);
  word(state + 24, 404);
  half(state + 30, 1);
  word(404, conditions);
  word(variable, strings + 16);
  half(variable + 8, 4);
  word(conditions, std::bit_cast<uint32_t>(25.0f));
  word(conditions + 4, variable);
  half(conditions + 8, 2);
  word(part + 4, strings);
  word(part + 8, strings);
  word(part + 12, 408);
  half(part + 16, 5);
  const uint32_t types[]{16, 2, 32, 128, 1}, names[]{10, 56, 39, 49, 62};
  for (uint32_t i = 0; i < 5; ++i) {
    word(props + i * 24 + 4, strings + names[i]);
    word(props + i * 24 + 8, strings);
    word(props + i * 24 + 20, types[i]);
    word(408 + i * 4, props + i * 24);
  }
  word(props, refs);
  word(props + 24, 14);
  word(props + 48, 600);
  word(props + 72, 664);
  for (uint32_t i = 0; i < 16; ++i)
    word(600 + i * 4, std::bit_cast<uint32_t>(i % 5 == 0 ? 1.0f : 0.0f));
  for (uint32_t i = 0; i < 3; ++i)
    word(664 + i * 4, std::bit_cast<uint32_t>(float(i)));
  return bytes;
}
void Hook(const std::vector<uint8_t>& source, const ModelVariants& variants) {
  payload_size = uint32_t(source.size());
  const auto changed = variants.Apply(source)->bytes;
  auto run = [&](bool active, bool failure) {
    memory.assign(base_address + source.size() + 128, 0xD3);
    const auto& gold = active && !failure ? changed : source;
    std::copy(gold.begin(), gold.end(), memory.begin() + base_address);
    PPCContext expected{};
    expected.r3.u64 = base_address;
    expected.r4.u64 = source.size();
    GameModelRelocate(expected, memory.data());
    const auto expected_bytes = memory;
    std::copy(source.begin(), source.end(), memory.begin() + base_address);
    PPCContext actual{};
    actual.r3.u64 = base_address;
    actual.r4.u64 = source.size();
    hook_variants = active ? &variants : nullptr;
    hook_throw = failure;
    original_calls = hook_calls = 0;
    sub_82719770(actual, memory.data());
    Require(original_calls == 1 && hook_calls == 1 && memory == expected_bytes &&
                !std::memcmp(&actual, &expected, sizeof(actual)),
            "Production model hook preserves the original parser's exact bytes, registers and "
            "single-call behavior");
  };
  run(false, false);
  run(true, false);
  run(true, true);
  hook_variants = nullptr;
  hook_throw = false;
  original_throw = true;
  PPCContext actual{};
  actual.r3.u64 = base_address;
  actual.r4.u64 = source.size();
  original_calls = hook_calls = 0;
  bool caught = false;
  try {
    sub_82719770(actual, memory.data());
  } catch (const std::runtime_error&) {
    caught = true;
  }
  Require(caught && original_calls == 1 && hook_calls == 1 && actual.r8.u64 == 0x12345678,
          "An original parser exception propagates once with its register state intact");
  original_throw = false;
}
}  // namespace

int main(int argc, char** argv) {
  try {
    const auto bytes = Fixture();
    const ModelDefinition model(bytes);
    Oracle(model);
    Require(model.Edit(Json::array()) == bytes, "A no-op edit preserves every source byte");
    auto changed = model.Edit(Json::array({{{"field", "property:0"}, {"value", "reference:1"}},
                                           {{"field", "property:1"}, {"value", 15}},
                                           {{"field", "condition:0"}, {"value", 45.0}}}));
    const ModelDefinition edited(changed);
    Oracle(edited);
    const auto description = edited.Describe();
    Require(description["fields"][0]["value"] == "reference:1" &&
                description["fields"][1]["value"] == 15 && description["fields"][5]["value"] == 45,
            "Typed model edits preserve references, integer properties and float conditions");
    Require(model.bytes() == bytes && edited.Id() == model.Id(),
            "Editing never mutates the source or native resource identity");
    auto document = model.Describe();
    document["changes"] = Json::array({{{"field", "property:0"}, {"value", "reference:1"}}});
    const auto variant = model.ApplyVariant(document);
    Oracle(ModelDefinition(variant));
    auto stale = document;
    stale["source_sha256"] = std::string(64, '0');
    Reject([&] { model.Apply(stale); }, "Source mismatches reject before model editing");
    auto dishonest = document;
    dishonest["fields"][0]["value"] = "reference:1";
    Reject([&] { model.Apply(dishonest); }, "Inspection metadata cannot hide an unintended edit");
    auto unreviewed = model.Describe();
    unreviewed["changes"] = Json::array({{{"field", "property:1"}, {"value", 0}}});
    Reject([&] { model.ApplyVariant(unreviewed); },
           "Live variants reject unqualified part-property semantics");
    const auto matrix = Json::array({2, 0, 0, 0, 0, 3, 0, 0, 0, 0, 4, 0, 5, 6, 7, 1});
    const auto transformed = ModelDefinition(
        model.Edit(Json::array({{{"field", "property:2"}, {"value", matrix}},
                                {{"field", "property:3"}, {"value", {-1.25, 2.5, 3.75}}}})));
    Oracle(transformed);
    Require(transformed.Describe()["fields"][2]["value"] == matrix &&
                transformed.Describe()["fields"][3]["value"] == Json::array({-1.25, 2.5, 3.75}),
            "Reviewed vector and matrix layouts round-trip typed offline edits");
    const auto catalog = Json{{"version", 1}, {"models", Json::array({document})}};
    Manifest low, high;
    low.id = "low";
    high.id = "high";
    low.model_edits = high.model_edits = std::make_shared<const Json>(catalog);
    const ModelVariants variants({low, high});
    Hook(bytes, variants);
    Require(variants.Apply(bytes)->owner == "high" && variants.Apply(bytes)->bytes == variant,
            "Whole-model priority follows resolved package order");
    auto foreign = bytes;
    simpsons::StoreGuestU32(foreign.data(), 16, 99);
    Require(!variants.Apply(foreign), "Unselected model identities need no parser or edits");
    auto drift = bytes;
    drift.back() = 1;
    Reject([&] { variants.Apply(drift); },
           "A selected identity still needs its exact source bytes");
    for (const auto& invalid :
         std::vector<Json>{Json(), Json{{"version", true}, {"models", Json::array()}},
                           Json{{"version", 1}, {"models", Json::array({document, document})}},
                           Json{{"version", 1}, {"models", Json::array({unreviewed})}}})
      Reject([&] { ValidateModelVariants(invalid); },
             "Invalid variant catalogs reject during preparation");

    const auto temporary =
        fs::temp_directory_path() /
        ("simpsons-model-check-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    struct Cleanup {
      fs::path folder;
      ~Cleanup() {
        std::error_code ignored;
        fs::remove_all(folder, ignored);
      }
    } cleanup{temporary};
    const auto project = temporary / "variant";
    WriteText(project / "mod.json", Json{{"id", "test.model"},
                                         {"name", "Model variant"},
                                         {"version", "1.0.0"},
                                         {"kind", "data"},
                                         {"model_edits", "assets/models.json"}}
                                        .dump());
    WriteText(project / "assets/models.json", "{\"version\":true,\"models\":[]}");
    Reject([&] { Manifest::Read(project); },
           "Shared native package validation rejects invalid catalogs before Android import");
    WriteText(project / "assets/models.json", catalog.dump());
    auto source = Manifest::Read(project);
    WriteText(project / "assets/models.json", catalog.dump(2));
    Reject([&] { PrepareCode(source, temporary / "cache"); },
           "Model source changes cannot bypass the captured fingerprint");
    WriteText(project / "assets/models.json", catalog.dump());
    source = Manifest::Read(project);
    PrepareCode(source, temporary / "cache");
    fs::rename(project / "assets", project / "unavailable");
    Require(ModelVariants({source}).Apply(bytes)->bytes == variant,
            "The live variant path uses immutable prepared bytes without reading source files");
    fs::rename(project / "unavailable", project / "assets");
    Runtime runtime({}, {});
    runtime.Load(temporary, temporary / "storage", {"test.model"});
    runtime.Tick(true, .01);
    auto next_document = document;
    next_document["changes"][0]["value"] = "reference:0";
    WriteText(project / "assets/models.json",
              Json{{"version", 1}, {"models", Json::array({next_document})}}.dump());
    Reject([&] { PrepareSources(runtime.Sources(), temporary / "cache", true); },
           "Model asset changes explicitly require restart rather than unsafe code reload");
    Require(runtime.ModelEdits()->Apply(bytes)->bytes == variant,
            "Rejected asset reload leaves the active prepared model selection intact");
    runtime.Stop();
    Require(!runtime.ModelEdits(), "Stopping a runtime releases its model documents");
    Reject([&] { model.Edit(Json::array({{{"field", "property:0"}, {"value", "reference:2"}}})); },
           "A render resource cannot be replaced with a different native class");
    Reject([&] { model.Edit(Json::array({{{"field", "property:4"}, {"value", true}}})); },
           "Unreviewed value kinds stay read-only");
    for (const auto& edit : std::vector<Json>{
             Json::array({{{"field", "missing"}, {"value", 0}}}),
             Json::array({{{"field", "property:1"}, {"value", -1}}}),
             Json::array({{{"field", "property:1"}, {"value", true}}}),
             Json::array(
                 {{{"field", "condition:0"}, {"value", std::numeric_limits<double>::infinity()}}}),
             Json::array({{{"field", "property:2"}, {"value", {1, 2, 3}}}}),
             Json::array({{{"field", "property:1"}, {"value", 2}},
                          {{"field", "property:1"}, {"value", 3}}})})
      Reject([&] { model.Edit(edit); }, "Invalid edits reject before modifying source data");
    for (const auto [offset, value] :
         std::vector<std::pair<uint32_t, uint32_t>>{{4, 9},
                                                    {8, uint32_t(bytes.size() + 1)},
                                                    {12, 4},
                                                    {72, 500},
                                                    {44, 0},
                                                    {48, 88},
                                                    {state + 20, 76},
                                                    {part + 12, 400},
                                                    {props + 4, UINT32_MAX},
                                                    {props, 92},
                                                    {conditions + 4, 220}}) {
      auto bad = bytes;
      simpsons::StoreGuestU32(bad.data(), offset, value);
      Reject([&] { ModelDefinition ignored(bad); },
             "Invalid layouts cannot reach the native model parser");
    }
    Json failures = Json::array(), types = Json::object();
    size_t stock = 0, skipped = 0, failed = 0, edited_stock = 0;
    if (argc >= 4 && std::string_view(argv[1]) == "--stock") {
      const auto root = fs::path(argv[2]);
      const auto index = Json::parse(ReadText(fs::path(argv[3]), 64 * 1024 * 1024));
      std::map<std::pair<std::string, size_t>, std::vector<Json>> entries;
      for (const auto& item : index.at("resources"))
        if (item.at("name").get<std::string>().ends_with(".meta.xml"))
          entries[{item.at("stream").get<std::string>(), item.at("entry").get<size_t>()}].push_back(
              item);
      for (const auto& [key, items] : entries) {
        const auto data = StreamFile(root / Utf8Path(key.first)).Read(key.second);
        for (const auto& item : items) {
          try {
            const auto offset = item.at("chunk_offset").get<size_t>();
            const auto length = item.at("chunk_size").get<size_t>();
            if (offset > data.size() || length > data.size() - offset || length < 16)
              throw Failure("invalid_model", "Inventory no longer matches this stream entry");
            const auto chunk = std::span(data).subspan(offset, length);
            const auto body = simpsons::LoadGuestU32(chunk.data(), 12) + 16;
            if (body + 4 > chunk.size() ||
                simpsons::LoadGuestU32(chunk.data(), body) != 0x4D4D646C) {
              ++skipped;
              continue;
            }
            const ModelResource resource(chunk);
            const auto definition = resource.Definition();
            Require(
                resource.Edit(Json::array()) == std::vector<uint8_t>(chunk.begin(), chunk.end()),
                "Named model no-op rebuild preserves the complete original wrapper and padding");
            Oracle(definition);
            ++stock;
            const auto description = definition.Describe();
            Json changes = Json::array();
            for (const auto& field : description.at("fields")) {
              const auto type = std::to_string(field.at("native_type").get<uint32_t>());
              types[type] = types.value(type, 0) + 1;
              if (!field.at("editable").get<bool>())
                continue;
              auto value = field.at("value");
              if (field.at("kind") == "uint32")
                value = value.get<uint32_t>() ^ 1;
              else if (field.at("kind") == "float")
                value = value.get<float>() == 1.0f ? 0.0f : 1.0f;
              else if (field.at("kind") == "vector3" || field.at("kind") == "matrix")
                value[0] = value[0].get<float>() == 1.0f ? 0.0f : 1.0f;
              else
                continue;
              changes.push_back({{"field", field.at("id")}, {"value", value}});
            }
            if (!changes.empty()) {
              Oracle(ModelDefinition(definition.Edit(changes)));
              ++edited_stock;
            }
          } catch (const std::exception& error) {
            ++failed;
            if (failures.size() < 32)
              failures.push_back({{"stream", key.first},
                                  {"entry", key.second},
                                  {"name", item.at("name")},
                                  {"error", error.what()}});
          }
        }
      }
    }
    const Json report{{"ok", failures.empty()},
                      {"checks", checks},
                      {"stock_models", stock},
                      {"non_model_metadata", skipped},
                      {"edited_stock_models", edited_stock},
                      {"field_types", types},
                      {"failure_count", failed},
                      {"failures", failures}};
    if (argc == 3 && std::string_view(argv[1]) == "--report")
      WriteText(fs::path(argv[2]), report.dump(2));
    else if (argc >= 5)
      WriteText(fs::path(argv[4]), report.dump(2));
    std::cout << report.dump() << '\n';
    return failures.empty() ? 0 : 1;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
