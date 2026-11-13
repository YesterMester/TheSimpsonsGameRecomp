#include "project_documents.h"
#include "missions.h"

#include <iostream>
#include <limits>

using namespace simpsons::modkit;
namespace {
unsigned checks = 0;
void Require(bool value, const char* message) { ++checks; if (!value) throw std::runtime_error(message); }
template<class F> void Reject(F fn, const char* message) {
  bool failed = false; try { fn(); } catch (const Failure&) { failed = true; }
  Require(failed, message);
}
Json Point(std::string id = "point-1") {
  return {{"id", id}, {"title", "Reach the checkpoint"}, {"kind", "reach"},
      {"position", {{"x", 1}, {"y", 2}, {"z", 3}}}};
}
Json Document() {
  auto value = EmptyProjectDocument("mission");
  value["level_root"] = "GAMEHUB/GAMEHUB.str";
  value["objectives"].push_back(Point());
  return value;
}
}
int main() {
  try {
    Require(ValidateProjectDocument("mission", EmptyProjectDocument("mission")) == EmptyProjectDocument("mission"),
            "An empty authoring document round-trips");
    auto value = Document(); auto second = Point("point-2");
    second["kind"] = "stay"; second["seconds"] = .5; second["requires"] = {"point-1"};
    value["objectives"].push_back(second);
    auto canonical = ValidateProjectDocument("mission", value);
    Require(canonical["level_root"] == "gamehub/gamehub.str" && canonical["objectives"][0]["radius"] == 3,
            "Native coordinates, defaults and episode identity canonicalize");
    Require(ValidateProjectDocument("mission", canonical) == canonical, "Canonical mission documents do not drift");
    auto invalid = value; invalid["objectives"][0]["requires"] = {"point-2"};
    Reject([&]{ ValidateProjectDocument("mission", invalid); }, "The native graph oracle rejects cycles");
    invalid = value; invalid["objectives"][1]["requires"] = {"missing"};
    Reject([&]{ ValidateProjectDocument("mission", invalid); }, "Unknown prerequisite ids reject");
    invalid = value; invalid["objectives"][1]["id"] = "point-1";
    Reject([&]{ ValidateProjectDocument("mission", invalid); }, "Duplicate objective identities reject");
    invalid = value; for (auto& point : invalid["objectives"]) point["optional"] = true;
    Reject([&]{ ValidateProjectDocument("mission", invalid); }, "A nonempty route needs a required objective");
    for (const auto bad : {Json(true), Json(1.0), Json(4294967297ULL)}) {
      invalid = value; invalid["schema"] = bad;
      Reject([&]{ ValidateProjectDocument("mission", invalid); }, "Versions do not wrap or coerce booleans/floats");
      invalid = value; invalid["revision"] = bad;
      Reject([&]{ ValidateProjectDocument("mission", invalid); }, "Revisions do not wrap or coerce booleans/floats");
    }
    for (const char* root : {"", "../gamehub.str", "gamehub/wrong.str", "gamehub/gamehub/zone.str"}) {
      invalid = value; invalid["level_root"] = root;
      Reject([&]{ ValidateProjectDocument("mission", invalid); }, "Routes need an unambiguous episode catalog");
    }
    invalid = value; invalid["objectives"][0]["address"] = 0x82000000u;
    Reject([&]{ ValidateProjectDocument("mission", invalid); }, "Authored documents cannot retain guest addresses");
    invalid = value; invalid["objectives"][0]["position"]["x"] = 1000001;
    Reject([&]{ ValidateProjectDocument("mission", invalid); }, "Point bounds match native placement");
    invalid = value; invalid["objectives"][0]["radius"] = 1e-100;
    Reject([&]{ ValidateProjectDocument("mission", invalid); }, "Placement radius cannot underflow to zero");
    invalid = value; invalid["objectives"][1]["seconds"] = 0;
    Reject([&]{ ValidateProjectDocument("mission", invalid); }, "Timed goals need a positive duration");
    invalid = value; invalid["objectives"][1]["kind"] = "unknown";
    Reject([&]{ ValidateProjectDocument("mission", invalid); }, "Unsupported behaviors are not advertised as objectives");
    invalid = value; invalid["time_limit"] = std::numeric_limits<double>::infinity();
    Reject([&]{ ValidateProjectDocument("mission", invalid); }, "Infinite mission clocks reject");
    invalid = EmptyProjectDocument("mission"); invalid["level_root"] = "gamehub/gamehub.str";
    for (unsigned i = 0; i < 32; ++i) invalid["objectives"].push_back(Point("point-" + std::to_string(i)));
    Require(ValidateProjectDocument("mission", invalid)["objectives"].size() == 32, "A complete 32-goal route fits the native quota");
    invalid["objectives"].push_back(Point("point-33"));
    Reject([&]{ ValidateProjectDocument("mission", invalid); }, "The editor cannot exceed the native objective quota");

    Missions service;
    auto definition = MissionProjectDefinition(canonical);
    service.Call("owner", "missions.register", {{"definition", definition}});
    service.Call("owner", "missions.start", {{"id", "authored-mission"}});
    service.TakeEvent(service.Events()[0]);
    const auto before = service.Save("owner");
    const auto update = [&](Json updates) {
      return service.Call("owner", "missions.advance_batch", {{"id", "authored-mission"}, {"updates", updates}});
    };
    Reject([&]{ update({{{"objective", "point-1"}, {"amount", 1}}, {{"objective", "missing"}, {"amount", 1}}}); },
           "A late invalid batch member cannot commit earlier progress");
    Require(service.Save("owner") == before, "Rejected batch preserves progress and notifications");
    Reject([&]{ update({{{"objective", "point-1"}, {"amount", 1}}, {{"objective", "point-2"}, {"amount", .5}}}); },
           "A batch cannot unlock and complete its own next stage");
    Require(service.Save("owner") == before, "Prerequisite rejection is atomic");
    Reject([&]{ update({{{"objective", "point-1"}, {"amount", 1}}, {{"objective", "point-1"}, {"amount", 1}}}); },
           "One objective cannot appear twice in a batch");
    Reject([&]{ update({{{"objective", "point-1"}}}); }, "Missing nested amounts give recoverable failures");
    update(Json::array());
    Require(service.Save("owner") == before, "An empty batch does not manufacture events");
    auto progress = update({{{"objective", "point-1"}, {"amount", 20}}});
    Require(progress["objectives"][0]["progress"] == 1 && progress["objectives"][1]["state"] == "active",
            "Batches clamp progress and unlock the next stage");
    update({{{"objective", "point-2"}, {"amount", .5}}});
    Require(service.Call("owner", "missions.get", {{"id", "authored-mission"}})["status"] == "completed",
            "Completing every required objective finishes the native mission");
    Require(!service.Call("other", "missions.remove", {{"id", "authored-mission"}}).get<bool>(),
            "Other owners cannot remove this mission");
    Require(service.Call("owner", "missions.remove", {{"id", "authored-mission"}}).get<bool>(),
            "An editor can explicitly discard its preview service");
    Require(service.Save("owner") == Missions::Empty() && service.Events().empty(),
            "Removal releases progress, retry points and queued notifications");
    std::cout << "{\"ok\":true,\"checks\":" << checks << "}\n";
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
