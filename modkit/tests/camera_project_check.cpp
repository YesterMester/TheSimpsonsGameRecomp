// Run the actual commented creator project. The native camera hook is an
// explicit host boundary here; its writer and presentation are checked live.
#include "camera.h"
#include "resources.h"
#include "source.h"

#include <iostream>

using namespace simpsons::modkit;
namespace {
int checks = 0;
void Require(bool value, const char* message) {
  ++checks;
  if (!value)
    throw std::runtime_error(message);
}
CameraPose Pose(float x) {
  return {{x, 2, 3}, 10, 20, 0, 60};
}
}  // namespace

int main(int argc, char** argv) {
  const auto root = fs::temp_directory_path() /
                    ("simpsons-camera-project-" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  try {
    Require(argc == 2 || argc == 3, "Pass the public Camera workshop project and optional report");
    fs::create_directories(root / "workshop");
    const auto source = fs::path(argv[1]);
    auto manifest = Json::parse(ReadText(source / "mod.json"));
    manifest["id"] = "workshop";
    WriteText(root / "workshop/mod.json", manifest.dump());
    WriteText(root / "workshop/main.lua", ReadText(source / "main.lua"));
    CameraRequests camera;
    Resources resources;
    resources.Bind("camera", {[&] { return SaveCameraRequest(camera.Read()); },
                              [&](const Json& value) { camera.Apply(ParseCameraRequest(value)); },
                              [&] { return camera.Revision(); },
                              [](const Json& value) {
                                if (SaveCameraRequest(ParseCameraRequest(value)) != value)
                                  throw Failure("invalid_argument",
                                                "Camera request is not canonical");
                              },
                              true, [&] { return SaveCameraRequest(camera.Baseline()); }});
    bool ready = false;
    uint64_t epoch = 1;
    size_t writes = 0;
    HostServices host;
    host.call = [&](const HostOwner& owner, std::string_view method, const Json& args) -> Json {
      if (method == "world.status")
        return {{"ready", ready}, {"epoch", epoch}};
      if (method == "camera.status") {
        const auto request = camera.Read();
        return {{"free", request.free}, {"photo", request.photo}};
      }
      if (method == "camera.pose") {
        const auto pose = ready ? camera.View() : std::nullopt;
        return pose ? SaveCameraPose(*pose) : Json();
      }
      if (method == "camera.release") {
        resources.Release(owner, "camera");
        return nullptr;
      }
      if (!ready)
        throw Failure("not_ready", "Load a level before changing a camera");
      auto request = camera.Read();
      if (method == "camera.set_pose") {
        if (!camera.View())
          throw Failure("not_ready", "No level camera is available in this view");
        request.pose = ParseCameraPose(args.at("pose"));
        request.free = true;
      } else if (method == "camera.free") {
        request.free = args.at("enabled").get<bool>();
        if (!request.free) {
          request.photo = false;
          request.pose.reset();
        }
      } else if (method == "camera.photo") {
        request.photo = args.at("enabled").get<bool>();
        if (request.photo)
          request.free = true;
      } else
        throw std::runtime_error("Unexpected camera project operation");
      ++writes;
      resources.Set(owner, "camera", SaveCameraRequest(request));
      return request.pose ? SaveCameraPose(*request.pose) : Json();
    };
    host.save = [&](const HostOwner& owner) { return resources.Save(owner); };
    host.validate = [&](const std::vector<Json>& state) { resources.Validate(state); };
    host.restore = [&](const HostOwner& owner, const Json& state, bool checkpoint) {
      resources.Restore(owner, state, checkpoint);
    };
    host.release = [&](const HostOwner& owner) { resources.Release(owner); };
    auto runtime = std::make_unique<Runtime>([](auto, const auto&) { return Json(); }, [](auto) {},
                                             "en", host);
    runtime->EnableInspection(true);
    runtime->Load(root, root / "storage", {"workshop"});
    runtime->Tick(false, .01);
    auto action = [&](const char* row, Json value = nullptr, uint32_t player = 0) {
      runtime->Activate("workshop:camera", row, value, player);
      Require(!runtime->Status()[0]["failed"].get<bool>(),
              "The public camera callback remains healthy");
    };
    auto state = [&] { return runtime->Inspect({{"action", "state"}, {"owner", "workshop"}}); };
    auto native_tick = [&] {
      if (const auto pose = camera.TakePose())
        camera.Publish(pose);
    };
    action("capture_a");
    Require(writes == 0 && state()["shots"].empty(),
            "A menu action before a level cannot edit a camera");
    ready = true;
    ++epoch;
    camera.Publish(Pose(1));
    runtime->Tick(true, .01);
    action("capture_a", nullptr, 1);
    camera.Publish(Pose(20));
    action("capture_b");
    Require(state()["shots"]["a"] == SaveCameraPose(Pose(1)) &&
                state()["shots"]["b"] == SaveCameraPose(Pose(20)),
            "Either player's native menu action captures independent value records");
    action("preview_a");
    native_tick();
    Require(camera.Read().free && !camera.Read().photo && camera.View() == Pose(1),
            "Preview selects the captured native view without freezing gameplay by default");
    action("close");
    native_tick();
    Require(camera.View()->fov_degrees == 35 && state()["shots"]["a"]["fov_degrees"] == 60,
            "Live lens editing does not silently mutate stored shots");
    action("capture_a");
    action("photo", true);
    action("preview_b");
    native_tick();
    Require(camera.Read().photo && camera.View() == Pose(20),
            "A frozen preview preserves the chosen shot and photo preference");
    const auto saved = runtime->Save();
    const auto before = camera.Read();
    auto invalid = saved;
    invalid["mods"][0]["state"]["shots"]["a"]["rotation"]["pitch"] = 100;
    bool rejected = false;
    try {
      runtime->PrepareRestore(invalid);
    } catch (const Failure&) {
      rejected = true;
    }
    Require(rejected && camera.Read() == before && !runtime->Status()[0]["failed"].get<bool>(),
            "Invalid stored shots reject before replacing the running code or camera");
    auto prepared =
        runtime->PrepareReload(PrepareSources(runtime->Sources(), root / "images", true));
    runtime.swap(prepared);
    prepared.reset();
    runtime->NotifyReloaded();
    native_tick();
    Require(camera.Read() == before && state()["shots"] == saved["mods"][0]["state"]["shots"],
            "Code reload preserves captured shots, photo state and the active pose");
    action("release");
    Require(!camera.Read().free && !camera.Read().photo,
            "Finishing the project returns to its original gameplay camera modes");
    camera.Publish(std::nullopt);
    action("capture_a");
    action("preview_b");
    Require(!camera.Read().free,
            "An unavailable native view is a recoverable error rather than a stale camera write");
    ready = false;
    runtime->Tick(false, .01);
    camera.Reset();
    ready = true;
    ++epoch;
    camera.Publish(Pose(50));
    runtime->Tick(true, .01);
    action("preview_a");
    Require(state()["shots"].empty() && !camera.Read().free,
            "A new world cannot preview captures from a different scene instance");
    action("free", true);
    action("normal");
    native_tick();
    Require(camera.Read().free && camera.View()->fov_degrees == 60,
            "The native page can start a fresh camera after a level change");
    runtime.reset();
    Require(!camera.Read().free && !camera.Read().photo,
            "Retiring the actual project releases its camera");
    const Json report{{"ok", true}, {"checks", checks}};
    if (argc == 3)
      WriteText(fs::path(argv[2]), report.dump());
    std::cout << report.dump() << '\n';
    fs::remove_all(root);
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    fs::remove_all(root);
    return 1;
  }
}
