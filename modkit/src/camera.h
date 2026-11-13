#pragma once

#include "runtime.h"

#include <array>
#include <optional>

namespace simpsons::modkit {

// World coordinates, Y up. Angles and the vertical field of view are degrees;
// the original game camera owns the projection and RenderWare frame.
struct CameraPose {
  std::array<float, 3> position{};
  float pitch = 0, yaw = 0, roll = 0, fov_degrees = 60;
  bool operator==(const CameraPose&) const = default;
};
struct CameraRequest {
  bool free = false, photo = false;
  std::optional<CameraPose> pose;
  bool operator==(const CameraRequest&) const = default;
};

CameraPose ParseCameraPose(const Json& value);
Json SaveCameraPose(const CameraPose& pose);
// Old saves contain just free/photo. A pose extends that same owned resource;
// it cannot survive without its free-camera request.
CameraRequest ParseCameraRequest(const Json& value);
Json SaveCameraRequest(const CameraRequest& request);
// RenderWare stores tan(vertical FOV / 2) in its view window. The original
// projection helper scales the title's lens angle by its flags/aspect; measure
// that scale rather than treating every director's input as a vertical angle.
std::optional<float> VerticalCameraFov(float view_window_y);
std::optional<float> NativeCameraFov(float native_radians, float view_window_y,
                                     float vertical_degrees);

// Host state only. Worker-thread retirement queues requests; the game camera
// hook consumes poses and publishes its actual view on the game thread.
class CameraRequests {
 public:
  CameraRequest Read() const;
  CameraRequest Baseline() const;
  uint64_t Revision() const;
  void Apply(const CameraRequest& request);
  void Manual(bool free, bool photo);
  void ManualMotion();
  void Reset();
  std::optional<CameraPose> TakePose();
  void Publish(std::optional<CameraPose> pose);
  std::optional<CameraPose> View() const;

 private:
  mutable std::mutex mutex_;
  CameraRequest request_;
  std::optional<CameraPose> pending_, view_;
  uint64_t revision_ = 0;
};

}  // namespace simpsons::modkit
