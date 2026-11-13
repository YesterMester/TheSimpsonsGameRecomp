#include "scenes.h"
#include "entities.h"

#include <fstream>
#include <iostream>

using namespace simpsons::modkit;
namespace {
unsigned checks = 0;
void Require(bool ok, const char* text) { ++checks; if (!ok) throw std::runtime_error(text); }
template<class F> void Reject(F fn, const char* text) {
  bool rejected = false; try { fn(); } catch (const std::exception&) { rejected = true; }
  Require(rejected, text);
}
Json Pose() {
  return {{"position", {{"x", 1}, {"y", 2}, {"z", 3}}}, {"right", {{"x", 1}, {"y", 0}, {"z", 0}}},
      {"up", {{"x", 0}, {"y", 1}, {"z", 0}}}, {"forward", {{"x", 0}, {"y", 0}, {"z", 1}}}};
}
Json Object(std::string id = "scene-1") {
  return {{"id", id}, {"name", "Original prop"}, {"source", {{"guid", "0123456789abcdef0123456789abcdef"},
      {"class", "ScriptableProp"}, {"stream", "BRT/BRT/zone01.str"}}}, {"transform", Pose()}, {"radius", 1}};
}
Json Scene() { auto v = EmptyScene(); v["level_root"] = "GAMEHUB/GAMEHUB.str"; v["objects"].push_back(Object()); return v; }
}
int main(int argc, char** argv) {
  try {
    Require(ValidateScene(EmptyScene()) == EmptyScene(), "Empty authoring documents are canonical");
    auto lua = EmptyScene(); lua["objects"] = Json::object();
    Require(ValidateScene(lua) == EmptyScene(), "Empty Lua tables become empty object lists");
    const auto valid = ValidateScene(Scene());
    Require(valid["level_root"] == "gamehub/gamehub.str" && valid["objects"][0]["source"]["guid"] == "0123456789ABCDEF0123456789ABCDEF",
            "Episode/source identities canonicalize independently of temporary handles");
    Require(ValidateScene(valid) == valid, "Scene validation round-trips without drift");
    for (const Json value : {Json(true), Json(1.0), Json(4294967297ULL), Json(2)}) {
      auto v = Scene(); v["schema"] = value;
      Reject([&]{ ValidateScene(v); }, "Invalid or overflowing scene versions reject");
    }
    for (const auto* path : {"../gamehub.str", "gamehub/gamehub/zone01.str", "gamehub/another.str", "gamehub\\gamehub.str", ""}) {
      auto v = Scene(); v["level_root"] = path;
      Reject([&]{ ValidateScene(v); }, "A nonempty scene needs its exact episode root");
    }
    auto v = Scene(); v["objects"].push_back(Object());
    Reject([&]{ ValidateScene(v); }, "Duplicate scene tags cannot alias one preview object");
    v = Scene(); v["objects"][0]["source"]["guid"] = "template:1";
    Reject([&]{ ValidateScene(v); }, "Temporary template handles never become scene identities");
    v = Scene(); v["objects"][0]["source"]["address"] = 0x82000000u;
    Reject([&]{ ValidateScene(v); }, "Scene sources accept no guest address fields");
    v = Scene(); v["objects"][0]["transform"]["right"] = v["objects"][0]["transform"]["up"];
    Reject([&]{ ValidateScene(v); }, "Singular pose matrices reject before mutation");
    v = Scene(); v["objects"][0]["transform"]["position"]["x"] = 1000001;
    Reject([&]{ ValidateScene(v); }, "Scene coordinate bounds match the editor's native ranges");
    v = Scene(); v["objects"][0]["radius"] = 1e-100;
    Reject([&]{ ValidateScene(v); }, "A radius cannot underflow to zero in native float32");
    v = Scene(); v["objects"] = Json::array();
    for (unsigned i = 0; i < 64; ++i) v["objects"].push_back(Object("scene-"+std::to_string(i)));
    Require(ValidateScene(v)["objects"].size() == 64, "A complete 64-object scene fits the owned-object limit");
    v["objects"].push_back(Object("scene-65"));
    Reject([&]{ ValidateScene(v); }, "A scene cannot exceed its native ownership budget");
    OwnedEntities owned;
    HostOwner a{1,"creator.scene"}, b{1,"creator.other"};
    Json desc = {{"class","ScriptableProp"},{"vtable",0x82100000u},{"guid",{0x534D4F44u,1u,2u,1u}},{"tag","scene-1"}};
    owned.CheckTag(a,"scene-1"); owned.Add(a,1,desc);
    Reject([&]{ owned.CheckTag(a,"scene-1"); }, "A live tag cannot be reused within its owner");
    owned.CheckTag(b,"scene-1"); owned.Destroy(a,1);
    Reject([&]{ owned.CheckTag(a,"scene-1"); }, "A pending destructor continues to reserve its tag");
    owned.Forget(1); owned.CheckTag(a,"scene-1"); ++checks;
    owned.Add(a,2,desc); auto saved=owned.Save(a);
    OwnedEntities::Validate({saved});
    Require(saved[0]["tag"]=="scene-1", "Object tags persist in exact-save ownership metadata");
    auto duplicate = saved; auto another=desc; another["guid"][3]=2u; duplicate.push_back(another);
    Reject([&]{ OwnedEntities::Validate({duplicate}); }, "One saved owner cannot restore duplicate tags");
    for (int i=1;i<argc;++i) {
      std::ifstream file(argv[i]); Require(bool(file), "Captured portable scene must exist");
      const auto document=Json::parse(file); ValidateScene(document); ++checks;
    }
    std::cout << "{\"ok\":true,\"checks\":" << checks << "}\n";
  } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
