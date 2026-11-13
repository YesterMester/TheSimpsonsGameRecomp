#pragma once

#include <cstdint>
#include "camera.h"

// The free camera and photo mode (freecam.cpp), for the debug menu. Both work
// in levels only; the requests are ignored elsewhere.
bool FreecamActive();
bool PhotoModeActive();
void SetFreecam(bool on);
void SetPhotoMode(bool on);
uint64_t FreecamRequestsRevision();
// Validated mod lifecycle state. The native camera manager applies or rejects
// the request on its next update; this never calls guest code during cleanup.
void ApplyFreecamRequests(bool free, bool photo);
void ApplyFreecamRequests(const simpsons::modkit::CameraRequest& request);
simpsons::modkit::CameraRequest ReadFreecamRequests();
simpsons::modkit::CameraRequest CaptureFreecamRequests();
std::optional<simpsons::modkit::CameraPose> ReadCameraPose();
bool FreecamWorldFrozen();
// With cheats on: the family goes where the free camera is, which ends it.
void BringFamilyToCamera();
