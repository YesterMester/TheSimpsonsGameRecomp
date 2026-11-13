#include "camera.h"
#include "resources.h"

#include <cmath>
#include <iostream>
#include <limits>

using namespace simpsons::modkit;
namespace {
int checks = 0;
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
CameraPose Pose(float x) {
  return {{x, 2, 3}, 10, 20, 0, 60};
}
}  // namespace

int main(int argc, char** argv) {
  try {
    CameraRequests camera;
    Resources resources;
    size_t captures = 0;
    resources.Bind("camera", {[&] { return SaveCameraRequest(camera.Read()); },
                              [&](const Json& value) { camera.Apply(ParseCameraRequest(value)); },
                              [&] { return camera.Revision(); },
                              [](const Json& value) {
                                if (SaveCameraRequest(ParseCameraRequest(value)) != value)
                                  throw Failure("invalid_argument",
                                                "Camera snapshot is not canonical");
                              },
                              true,
                              [&] {
                                ++captures;
                                return SaveCameraRequest(camera.Baseline());
                              }});
    bool protected_player = false;
    uint64_t protection_revision = 0;
    resources.Bind("protection", {[&] { return Json(protected_player); },
                                  [&](const Json& value) {
                                    protected_player = value.get<bool>();
                                    ++protection_revision;
                                  },
                                  [&] { return protection_revision; },
                                  [](const Json& value) {
                                    if (!value.is_boolean())
                                      throw Failure("invalid_argument", "Expected boolean");
                                  }});
    const HostOwner a{1, "first"}, b{1, "second"}, replacement{2, "first"};
    auto request = [](float x) { return SaveCameraRequest({true, true, Pose(x)}); };

    const auto round_trip = SaveCameraPose(ParseCameraPose(SaveCameraPose(Pose(12))));
    Require(round_trip == SaveCameraPose(Pose(12)),
            "Camera poses round-trip without borrowed state");
    auto normalized = round_trip;
    normalized["rotation"]["yaw"] = 740;
    normalized["rotation"]["roll"] = -725;
    const auto canonical = ParseCameraPose(normalized);
    Require(canonical.yaw == 20 && canonical.roll == -5, "Yaw and roll normalize by full turns");
    for (const auto& [group, field] :
         std::vector<std::pair<std::string, std::string>>{{"position", "x"},
                                                          {"position", "y"},
                                                          {"position", "z"},
                                                          {"rotation", "pitch"},
                                                          {"rotation", "yaw"},
                                                          {"rotation", "roll"}}) {
      for (const Json& invalid :
           std::vector<Json>{nullptr, true, "12", std::numeric_limits<double>::infinity(),
                             std::numeric_limits<double>::quiet_NaN(), 1e100}) {
        auto bad = round_trip;
        bad[group][field] = invalid;
        Reject([&] { ParseCameraPose(bad); }, "Invalid axes and angles fail before mutation");
      }
    }
    for (const Json& invalid : std::vector<Json>{0.999, 120.001, nullptr, true, "60"}) {
      auto bad = round_trip;
      bad["fov_degrees"] = invalid;
      Reject([&] { ParseCameraPose(bad); }, "The lens range and numeric type are checked");
    }
    for (const auto& invalid : std::vector<Json>{
             Json::array(), Json(), Json{{"free", false}}, Json{{"free", false}, {"photo", true}},
             Json{{"free", false}, {"photo", false}, {"pose", round_trip}},
             Json{{"free", true}, {"photo", false}, {"pose", nullptr}},
             Json{{"free", true}, {"photo", false}, {"unexpected", 1}}})
      Reject([&] { ParseCameraRequest(invalid); }, "Invalid request modes or fields reject");
    auto misspelled = round_trip;
    misspelled["rotation"]["heading"] = 30;
    Reject([&] { ParseCameraPose(misspelled); },
           "Unexpected pose fields cannot silently do nothing");

    camera.Publish(Pose(1));
    Require(!camera.Read().free && !camera.TakePose() && camera.View() == Pose(1),
            "Publishing a gameplay view does not acquire a camera request");
    camera.Manual(true, true);
    resources.Set(a, "camera", request(10));
    resources.Set(a, "protection", true);
    Require(camera.Read().pose == Pose(10) && camera.TakePose() == Pose(10) && !camera.TakePose(),
            "The native hook consumes an authored pose exactly once");
    camera.Publish(Pose(10));
    resources.Set(b, "camera", request(20));
    const auto saved_a = resources.Save(a), saved_b = resources.Save(b);
    Require(captures == 1 && camera.TakePose() == Pose(20),
            "A second owner preserves the manual baseline");
    resources.Release(b, "camera");
    Require(camera.TakePose() == Pose(10), "Targeted release restores the preceding explicit pose");
    resources.Release(a, "camera");
    Require(
        camera.Read().free && camera.Read().photo && camera.TakePose() == Pose(1) &&
            protected_player,
        "Last camera release restores the actual manual view without releasing damage protection");
    std::thread cleanup([&] { resources.Release(a); });
    cleanup.join();
    Require(!protected_player && camera.Read().pose == Pose(1) && captures == 1,
            "Shutdown-thread cleanup uses host state and never captures another baseline");
    resources.Release(a, "camera");
    Require(camera.Read().pose == Pose(1), "Repeated targeted release is harmless");
    Reject([&] { resources.Release(a, "unknown"); }, "Unknown resource releases reject");
    Reject([&] { resources.Release({}, "camera"); }, "Targeted release requires a real owner");

    camera.Reset();
    resources.Restore(b, saved_b, false);
    resources.Restore(replacement, saved_a, false);
    Require(camera.Read().pose == Pose(20), "Restored ownership order preserves the winning pose");
    resources.Release(a, "camera");
    Require(camera.Read().pose == Pose(20), "An old generation cannot release its replacement");
    resources.Release(b, "camera");
    Require(camera.TakePose() == Pose(10), "Reload can reveal the restored preceding generation");
    resources.Release(replacement);
    Require(camera.TakePose() == Pose(1), "Reload retains the captured manual baseline pose");
    camera.Reset();
    resources.Restore(replacement, saved_a, true);
    Require(!camera.Read().free && !camera.View() && !camera.TakePose() && protected_player,
            "A checkpoint discards old-world cameras while retaining global services");
    resources.Release(replacement);

    camera.Publish(Pose(5));
    resources.Set(a, "camera", request(10));
    camera.TakePose();
    camera.ManualMotion();
    camera.Publish(Pose(11));
    resources.Release(a);
    Require(camera.Read().free && !camera.Read().pose && camera.View() == Pose(11) &&
                !camera.TakePose(),
            "Manual camera movement supersedes the lease without snapping back at retirement");
    camera.Manual(false, false);
    resources.Set(a, "camera", request(10));
    const auto before = camera.Read();
    const auto revision = camera.Revision();
    auto bad = request(20);
    bad["pose"]["fov_degrees"] = 0;
    Reject([&] { resources.Set(a, "camera", bad); },
           "Bad live requests reject before touching a working lease");
    Require(camera.Read() == before && camera.Revision() == revision,
            "Rejected requests preserve the camera and pending pose");
    auto invalid_saved = saved_a;
    invalid_saved["camera"]["value"]["pose"]["rotation"]["yaw"] = 740;
    Reject([&] { resources.Validate({invalid_saved}); },
           "Noncanonical saved poses fail during preflight");
    invalid_saved = saved_a;
    invalid_saved["camera"]["baseline"]["pose"]["position"]["x"] = "not a coordinate";
    Reject([&] { resources.Validate({invalid_saved}); },
           "Invalid saved baseline views fail during preflight");
    resources.Release(a);

    const auto legacy = Json{{"free", true}, {"photo", false}};
    resources.Set(a, "camera", legacy);
    const auto legacy_saved = resources.Save(a);
    resources.Release(a);
    resources.Restore(replacement, legacy_saved, false);
    Require(SaveCameraRequest(camera.Read()) == legacy, "Mode-only snapshots remain compatible");
    resources.Release(replacement);
    camera.Publish(Pose(30));
    camera.Apply({true, true, Pose(40)});
    camera.Reset();
    Require(!camera.Read().free && !camera.Read().photo && !camera.TakePose() && !camera.View(),
            "World teardown clears modes, queued poses and the published view");
    auto invalid_pose = Pose(0);
    invalid_pose.pitch = std::numeric_limits<float>::quiet_NaN();
    camera.Publish(invalid_pose);
    Require(!camera.View(), "Unsupported native camera views publish nil");
    const Json result{{"ok", true}, {"checks", checks}};
    if (argc > 1)
      WriteText(fs::path(argv[1]), result.dump());
    std::cout << result.dump() << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
