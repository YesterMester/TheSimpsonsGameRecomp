#include "camera.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace simpsons::modkit {
namespace {
bool Keys(const Json& value, std::initializer_list<const char*> keys) {
  return value.is_object() && value.size() == keys.size() &&
         std::all_of(keys.begin(), keys.end(),
                     [&](const auto* key) { return value.contains(key); });
}
bool Valid(const CameraPose& pose) {
  return std::all_of(
             pose.position.begin(), pose.position.end(),
             [](float value) { return std::isfinite(value) && std::abs(value) <= 1000000; }) &&
         std::isfinite(pose.pitch) && std::abs(pose.pitch) <= 90 && std::isfinite(pose.yaw) &&
         std::abs(pose.yaw) <= 360000 && std::isfinite(pose.roll) &&
         std::abs(pose.roll) <= 360000 && std::isfinite(pose.fov_degrees) &&
         pose.fov_degrees >= 1 && pose.fov_degrees <= 120;
}
float Number(const Json& value, float minimum, float maximum) {
  if (!value.is_number())
    throw Failure("invalid_argument", "Camera coordinates, angles and lens values must be numbers");
  const auto number = value.get<double>();
  if (!std::isfinite(number) || number < minimum || number > maximum)
    throw Failure("invalid_argument", "Camera value is outside its supported range");
  return float(number);
}
CameraPose Canonical(CameraPose pose) {
  if (!Valid(pose))
    throw Failure("invalid_argument",
                  "Camera pose must contain finite coordinates and valid angles/lens");
  pose.yaw = std::remainder(pose.yaw, 360.0f);
  pose.roll = std::remainder(pose.roll, 360.0f);
  return pose;
}
}  // namespace

CameraPose ParseCameraPose(const Json& value) {
  if (!Keys(value, {"position", "rotation", "fov_degrees"}) ||
      !Keys(value.at("position"), {"x", "y", "z"}) ||
      !Keys(value.at("rotation"), {"pitch", "yaw", "roll"}))
    throw Failure("invalid_argument", "Camera pose needs position, rotation and fov_degrees");
  const auto& position = value.at("position");
  const auto& rotation = value.at("rotation");
  return Canonical(
      {{Number(position.at("x"), -1000000, 1000000), Number(position.at("y"), -1000000, 1000000),
        Number(position.at("z"), -1000000, 1000000)},
       Number(rotation.at("pitch"), -90, 90),
       Number(rotation.at("yaw"), -360000, 360000),
       Number(rotation.at("roll"), -360000, 360000),
       Number(value.at("fov_degrees"), 1, 120)});
}
Json SaveCameraPose(const CameraPose& value) {
  const auto pose = Canonical(value);
  return {{"position", {{"x", pose.position[0]}, {"y", pose.position[1]}, {"z", pose.position[2]}}},
          {"rotation", {{"pitch", pose.pitch}, {"yaw", pose.yaw}, {"roll", pose.roll}}},
          {"fov_degrees", pose.fov_degrees}};
}
CameraRequest ParseCameraRequest(const Json& value) {
  if ((!Keys(value, {"free", "photo"}) && !Keys(value, {"free", "photo", "pose"})) ||
      !value.at("free").is_boolean() || !value.at("photo").is_boolean())
    throw Failure("invalid_argument",
                  "Camera request needs boolean free/photo modes and an optional pose");
  CameraRequest result{value.at("free").get<bool>(), value.at("photo").get<bool>()};
  if (value.contains("pose"))
    result.pose = ParseCameraPose(value.at("pose"));
  if (!result.free && (result.photo || result.pose))
    throw Failure("invalid_argument", "Photo mode and authored poses need a free-camera request");
  return result;
}
Json SaveCameraRequest(const CameraRequest& request) {
  Json result{{"free", request.free}, {"photo", request.photo}};
  if (request.pose)
    result["pose"] = SaveCameraPose(*request.pose);
  return result;
}

std::optional<float> VerticalCameraFov(float view_window_y) {
  if (!std::isfinite(view_window_y) || view_window_y <= 0)
    return {};
  return float(2.0 * std::atan(double(view_window_y)) * 180.0 / std::acos(-1.0));
}
std::optional<float> NativeCameraFov(float native_radians, float view_window_y,
                                     float vertical_degrees) {
  const double pi = std::acos(-1.0);
  if (!std::isfinite(native_radians) || native_radians <= 0 || native_radians >= pi ||
      !std::isfinite(view_window_y) || view_window_y <= 0 || !std::isfinite(vertical_degrees) ||
      vertical_degrees < 1 || vertical_degrees > 120)
    return {};
  const double scale = double(view_window_y) / std::tan(double(native_radians) * 0.5);
  const double result = 2.0 * std::atan(std::tan(double(vertical_degrees) * pi / 360.0) / scale);
  if (!std::isfinite(result) || result <= 0 || float(result) >= pi)
    return {};
  return float(result);
}

CameraRequest CameraRequests::Read() const {
  std::lock_guard lock(mutex_);
  return request_;
}
CameraRequest CameraRequests::Baseline() const {
  std::lock_guard lock(mutex_);
  auto result = request_;
  if (result.free && !result.pose)
    result.pose = view_;
  return result;
}
uint64_t CameraRequests::Revision() const {
  std::lock_guard lock(mutex_);
  return revision_;
}
void CameraRequests::Apply(const CameraRequest& value) {
  auto request = value;
  if (request.pose)
    request.pose = Canonical(*request.pose);
  if (!request.free && (request.photo || request.pose))
    throw Failure("invalid_argument", "Photo mode and authored poses need a free-camera request");
  std::lock_guard lock(mutex_);
  if (request.pose != request_.pose)
    pending_ = request.pose;
  request_ = request;
  ++revision_;
}
void CameraRequests::Manual(bool free, bool photo) {
  std::lock_guard lock(mutex_);
  request_ = {free, free && photo};
  pending_.reset();
  ++revision_;
}
void CameraRequests::ManualMotion() {
  std::lock_guard lock(mutex_);
  request_.pose.reset();
  pending_.reset();
  ++revision_;
}
void CameraRequests::Reset() {
  std::lock_guard lock(mutex_);
  request_ = {};
  pending_.reset();
  view_.reset();
  ++revision_;
}
std::optional<CameraPose> CameraRequests::TakePose() {
  std::lock_guard lock(mutex_);
  return std::exchange(pending_, std::nullopt);
}
void CameraRequests::Publish(std::optional<CameraPose> pose) {
  if (pose && Valid(*pose))
    pose = Canonical(*pose);
  else
    pose.reset();
  std::lock_guard lock(mutex_);
  view_ = pose;
}
std::optional<CameraPose> CameraRequests::View() const {
  std::lock_guard lock(mutex_);
  return view_;
}

}  // namespace simpsons::modkit
