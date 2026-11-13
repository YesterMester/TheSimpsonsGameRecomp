// Free camera and photo mode.
//
// The camera manager (a singleton at 0x82D08B10, built per level) applies the
// gameplay camera every frame: on the running tick, sub_8269D808 updates the
// camera directors and pushes viewport 0's pose through sub_8269D130, the one
// function that writes a pose and field of view into the viewport's RenderWare
// camera (and so into culling, LOD and the view and projection). After it, on
// the running and the paused tick alike, comes sub_8269D8F8: a developer fly
// camera that shipped but cannot be reached (enable flag at manager +264, pose
// at +268 and +280, field of view in degrees at +292, flown with controller
// 2). Overriding it gives one place, once per frame and after the gameplay
// camera, to apply our own pose. With the manager's enable flag set, the
// game's camera getters report our pose as well, as they did for the
// developers (freecam_game_follows).
//
// The photo mode pauses the level with the game's own pause (sub_826913A0,
// with a reason bit the game never uses): only the paused ticks run, so
// characters, AI, physics and the sequencer stop while the camera manager, the
// UI and rendering go on. The HUD and subtitles (the UI scene's draw callback)
// are left out meanwhile.
//
// While the free camera is on, the game reads a neutral, connected pad: its
// only controller read is the XInputGetState thunk sub_82B766A8. The
// keyboard's virtual pad is muted through the overlay-input lock, and the
// camera reads the keyboard from the window instead.
//
// With the mod menu's cheats on, B (F9) brings the family to the free camera
// and ends it, so the players go on from where the camera flew to (from
// frankyfife's fork). The free camera itself is not a cheat.
//
// Controller: L3+R3 free camera on/off, Y photo mode, B bring the family here;
// left stick move, right stick look, triggers down/up, LB/RB zoom out/in, R3
// resets the zoom, A faster, X slower. Keyboard: F6 free camera, F8 photo mode,
// F9 bring the family here; WASD move, arrows look, Q/E down/up, 1/3 zoom
// out/in, 2 resets the zoom, Shift faster, Ctrl slower.

#include "freecam.h"
#include "guest_memory.h"
#include "debug_menu.h"
#include "game_ui.h"
#include "modkit_input.h"
#include "players.h"
#include "native_ragdoll_preview.h"
#include "spawner.h"
#include "spawn_sources.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/ppc.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/overlay_input.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>

REXCVAR_DEFINE_DOUBLE(freecam_speed, 5.0, "Camera", "Free camera speed in metres per second")
    .range(0.5, 50.0);
REXCVAR_DEFINE_BOOL(freecam_hide_hud, true, "Camera",
                    "Hide the HUD and subtitles while the photo mode is on");
REXCVAR_DEFINE_BOOL(freecam_game_follows, true, "Camera",
                    "Let the game use the free camera as its camera (what it streams, culls and "
                    "hears from), as the developers' fly camera did");

// The camera manager's fly camera, which this file takes over.
REX_EXTERN(__imp__sub_8269D8F8);
REX_EXTERN(sub_8269D8F8);
// The pose -> RenderWare camera writer: r3 = manager, r4 = viewport, r5 =
// &position, r6 = &(pitch, yaw, roll), f1 = field of view in radians, r8/r9 =
// projection flags.
REX_EXTERN(sub_8269D130);
// The XInputGetState thunk (r3 = user, r4 = guest X_INPUT_STATE*), the game's
// only controller read.
REX_EXTERN(__imp__sub_82B766A8);
REX_EXTERN(sub_82B766A8);
// The simulation manager's tick, and its pause and unpause (r3 = reason bits).
REX_EXTERN(__imp__sub_82691540);
REX_EXTERN(sub_82691540);
REX_EXTERN(sub_826913A0);
REX_EXTERN(sub_82691368);
// SetSimEnabled (r3 = 0 when a level unloads or restarts).
REX_EXTERN(__imp__sub_82691530);
REX_EXTERN(sub_82691530);
// The UI scene's draw callback (HUD, subtitles); returns 1.
REX_EXTERN(__imp__sub_826D5CB0);
REX_EXTERN(sub_826D5CB0);
// Draws the HUD's movies in the HUD pass (sub_826D5050).
REX_EXTERN(__imp__sub_8272BA10);
REX_EXTERN(sub_8272BA10);

namespace {

constexpr uint32_t kCameraManager = 0x82D08B10;
constexpr uint32_t kMgrDirectors = 16;  // pointer to the 0x1B8-byte directors
constexpr uint32_t kMgrFlyEnable = 264;
constexpr uint32_t kMgrFlyPosition = 268;  // x, y, z
constexpr uint32_t kMgrFlyAngles = 280;    // pitch, yaw, roll (radians)
constexpr uint32_t kMgrFlyFovDegrees = 292;
constexpr uint32_t kMgrViewportCount = 300;
constexpr uint32_t kMgrViewport0Director = 304;  // director index, -1 = none
constexpr uint32_t kDirectorSize = 440;
constexpr uint32_t kDirectorPosition = 0;
constexpr uint32_t kDirectorAngles = 24;
constexpr uint32_t kDirectorFov = 36;  // radians
constexpr uint32_t kDirectorFlags1 = 44;
constexpr uint32_t kDirectorFlags2 = 48;
constexpr uint32_t kSimEnabled = 0x82CED760;
constexpr uint32_t kPhotoPauseBit = 0x80000000;

constexpr uint16_t kPadUp = 0x0001;
constexpr uint16_t kPadDown = 0x0002;
constexpr uint16_t kPadLeft = 0x0004;
constexpr uint16_t kPadRight = 0x0008;
constexpr uint16_t kPadL3 = 0x0040;
constexpr uint16_t kPadR3 = 0x0080;
constexpr uint16_t kPadLB = 0x0100;
constexpr uint16_t kPadRB = 0x0200;
constexpr uint16_t kPadA = 0x1000;
constexpr uint16_t kPadB = 0x2000;
constexpr uint16_t kPadX = 0x4000;
constexpr uint16_t kPadY = 0x8000;

constexpr float kPi = 3.14159265f;
constexpr float kMinFov = 5.0f * kPi / 180.0f;
constexpr float kMaxFov = 120.0f * kPi / 180.0f;
constexpr float kZoomRate = 30.0f * kPi / 180.0f;  // per second
constexpr float kTurnRate = 1.5f;                  // radians per second
constexpr float kMaxPitch = 1.55f;

using simpsons::GuestToHost;
using simpsons::LoadGuestFloat;
using simpsons::LoadGuestU32;
using simpsons::StoreGuestFloat;
using simpsons::StoreGuestU32;

uint16_t LoadBE16(const uint8_t* p) {
  return uint16_t(p[0] << 8 | p[1]);
}
int16_t LoadBES16(const uint8_t* p) {
  return int16_t(LoadBE16(p));
}
void StoreBE16(uint8_t* p, uint16_t value) {
  p[0] = uint8_t(value >> 8);
  p[1] = uint8_t(value);
}

int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// Requests (any thread).
simpsons::modkit::CameraRequests g_requests;
std::atomic<bool> g_reset_fov{false};
std::atomic<bool> g_want_teleport{false};
// Game-thread state, published for the frame pacing, the HUD draw and the
// UI-thread toggles.
std::atomic<bool> g_applied{false};
std::atomic<bool> g_frozen{false};
std::atomic<int64_t> g_last_camera_tick_ns{0};

// Keyboard (UI thread -> game thread).
std::array<std::atomic<bool>, 256> g_keys{};
std::atomic<bool> g_key_repeat{false};

// Pad of user 0, as the game would have read it (game thread only).
struct Pad {
  uint16_t buttons = 0;
  uint8_t lt = 0, rt = 0;
  int16_t lx = 0, ly = 0, rx = 0, ry = 0;
};
Pad g_pad;
bool g_chord_down = false;          // L3+R3 held since it toggled
bool g_r3_tap = false;              // R3 pressed alone, a zoom reset on release
bool g_hold_until_neutral = false;  // after the free camera: until the pad rests

// The free camera (game thread only).
struct Camera {
  uint32_t manager = 0;
  float position[3] = {};
  float pitch = 0, yaw = 0, roll = 0;
  float fov = 1.0f, seed_fov = 1.0f;
  std::optional<float> vertical_fov;
  float seed_vertical_fov = 60;
  uint32_t flags1 = 1, flags2 = 2;
  int64_t last_ns = 0;
} g_cam;

bool CameraTicking() {
  const int64_t last = g_last_camera_tick_ns.load(std::memory_order_relaxed);
  return last && NowNs() - last < 500'000'000;
}

void ToggleFreecam() {
  if (g_requests.Read().free) {
    g_requests.Manual(false, false);
    return;
  }
  if (!CameraTicking()) {
    REXLOG_INFO("[freecam] no level camera running; the free camera works in levels only");
    return;
  }
  g_requests.Manual(true, false);
}

void TogglePhoto() {
  const auto request = g_requests.Read();
  if (request.photo) {
    g_requests.Manual(request.free, false);
    return;
  }
  if (!request.free) {
    if (!CameraTicking()) {
      REXLOG_INFO("[freecam] no level camera running; the photo mode works in levels only");
      return;
    }
  }
  g_requests.Manual(true, true);
}

// Bring the family to the free camera, with the cheats on.
void RequestTeleport() {
  if (!g_requests.Read().free)
    return;
  if (!debug_menu::CheatsEnabled()) {
    game_ui::Notify("Turn cheats on in the debug menu to bring the family here");
    return;
  }
  g_want_teleport = true;
}

float Axis(int16_t value, int dead_zone) {
  const float v = float(value);
  const float limit = 32767.0f - float(dead_zone);
  if (v > dead_zone)
    return std::min((v - dead_zone) / limit, 1.0f);
  if (v < -dead_zone)
    return std::max((v + dead_zone) / limit, -1.0f);
  return 0.0f;
}

bool Key(rex::ui::VirtualKey key) {
  return g_keys[size_t(key) & 0xFF].load(std::memory_order_relaxed);
}

bool ApplyPose(PPCContext& ctx, uint8_t* base, uint32_t manager, const float position[3],
               float pitch, float yaw, float roll, float fov, uint32_t flags1, uint32_t flags2) {
  StoreGuestFloat(base, manager + kMgrFlyPosition + 0, position[0]);
  StoreGuestFloat(base, manager + kMgrFlyPosition + 4, position[1]);
  StoreGuestFloat(base, manager + kMgrFlyPosition + 8, position[2]);
  StoreGuestFloat(base, manager + kMgrFlyAngles + 0, pitch);
  StoreGuestFloat(base, manager + kMgrFlyAngles + 4, yaw);
  StoreGuestFloat(base, manager + kMgrFlyAngles + 8, roll);
  StoreGuestFloat(base, manager + kMgrFlyFovDegrees, fov * 180.0f / kPi);
  ctx.r3.u64 = manager;
  ctx.r4.u64 = 0;
  ctx.r5.u64 = manager + kMgrFlyPosition;
  ctx.r6.u64 = manager + kMgrFlyAngles;
  ctx.r8.u64 = flags1;
  ctx.r9.u64 = flags2;
  ctx.f1.f64 = fov;
  sub_8269D130(ctx, base);
  return true;
}

// Viewport 0's camera director, or 0 when the manager has none.
uint32_t Director(const uint8_t* base, uint32_t manager) {
  if (!manager || LoadGuestU32(base, manager + kMgrViewportCount) == 0) {
    return 0;
  }
  const int32_t index = int32_t(LoadGuestU32(base, manager + kMgrViewport0Director));
  const uint32_t directors = LoadGuestU32(base, manager + kMgrDirectors);
  if (index < 0 || !directors) {
    return 0;
  }
  return directors + kDirectorSize * uint32_t(index);
}

std::optional<float> ViewWindowY(const uint8_t* base, uint32_t manager) {
  if (!spawner::Readable(manager, kMgrViewport0Director + 4) ||
      LoadGuestU32(base, manager + kMgrViewportCount) == 0)
    return {};
  const auto camera = LoadGuestU32(base, manager + 20);
  if (!spawner::Readable(camera, 112))
    return {};
  const float y = LoadGuestFloat(base, camera + 108);
  return std::isfinite(y) && y > 0 ? std::optional<float>(y) : std::nullopt;
}

std::optional<float> VerticalFov(const uint8_t* base, uint32_t manager) {
  const auto y = ViewWindowY(base, manager);
  return y ? simpsons::modkit::VerticalCameraFov(*y) : std::nullopt;
}

void Disable(PPCContext& ctx, uint8_t* base, uint32_t manager) {
  if (manager == g_cam.manager) {
    StoreGuestU32(base, manager + kMgrFlyEnable, 0);
    // In the game's paused mode the gameplay camera is not applied again
    // this frame: put its pose back here.
    if (const uint32_t d = Director(base, manager)) {
      float position[3] = {LoadGuestFloat(base, d + kDirectorPosition + 0),
                           LoadGuestFloat(base, d + kDirectorPosition + 4),
                           LoadGuestFloat(base, d + kDirectorPosition + 8)};
      ApplyPose(ctx, base, manager, position, LoadGuestFloat(base, d + kDirectorAngles + 0),
                LoadGuestFloat(base, d + kDirectorAngles + 4),
                LoadGuestFloat(base, d + kDirectorAngles + 8),
                LoadGuestFloat(base, d + kDirectorFov), LoadGuestU32(base, d + kDirectorFlags1),
                LoadGuestU32(base, d + kDirectorFlags2));
    }
  }
  rex::ui::ReleaseOverlayInput();
  if (g_requests.Read().free)
    g_requests.Manual(false, false);
  if (spawner::SpoonerEnabled())
    spawner::SetSpoonerEnabled(false);
  g_applied = false;
  g_hold_until_neutral = true;
  REXLOG_INFO("[freecam] off");
}

bool Enable(const uint8_t* base, uint8_t* mutable_base, uint32_t manager) {
  const uint32_t d = Director(base, manager);
  if (!d) {
    return false;
  }
  g_cam.manager = manager;
  g_cam.vertical_fov.reset();
  for (int i = 0; i < 3; ++i) {
    g_cam.position[i] = LoadGuestFloat(base, d + kDirectorPosition + 4 * i);
  }
  g_cam.pitch = LoadGuestFloat(base, d + kDirectorAngles + 0);
  g_cam.yaw = LoadGuestFloat(base, d + kDirectorAngles + 4);
  g_cam.roll = LoadGuestFloat(base, d + kDirectorAngles + 8);
  g_cam.fov = LoadGuestFloat(base, d + kDirectorFov);
  if (!(g_cam.fov > kMinFov && g_cam.fov < kMaxFov)) {
    g_cam.fov = 1.0472f;
  }
  g_cam.seed_fov = g_cam.fov;
  g_cam.flags1 = LoadGuestU32(base, d + kDirectorFlags1);
  g_cam.flags2 = LoadGuestU32(base, d + kDirectorFlags2);
  g_cam.last_ns = NowNs();
  rex::ui::AcquireOverlayInput();
  if (REXCVAR_GET(freecam_game_follows)) {
    StoreGuestU32(mutable_base, manager + kMgrFlyEnable, 1);
  }
  g_applied = true;
  REXLOG_INFO("[freecam] on at ({:.2f}, {:.2f}, {:.2f}), pitch {:.3f}, yaw {:.3f}, fov {:.1f} deg",
              g_cam.position[0], g_cam.position[1], g_cam.position[2], g_cam.pitch, g_cam.yaw,
              g_cam.fov * 180.0f / kPi);
  return true;
}

void Fly(float dt) {
  if (game_ui::Current() == game_ui::Screen::kAchievements) {
    // The achievements have the controller.
    return;
  }
  const Pad pad = g_pad;
  float move_x = Axis(pad.lx, 7849), move_z = Axis(pad.ly, 7849);
  float look_x = Axis(pad.rx, 8689), look_y = Axis(pad.ry, 8689);
  float rise = (pad.rt - pad.lt) / 255.0f;
  float zoom = (pad.buttons & kPadLB ? 1.0f : 0.0f) - (pad.buttons & kPadRB ? 1.0f : 0.0f);
  float speed = float(REXCVAR_GET(freecam_speed));
  if (!spawner::SpoonerEnabled()) {
    if (pad.buttons & kPadA)
      speed *= 5.0f;
    if (pad.buttons & kPadX)
      speed *= 0.2f;
  }

  using rex::ui::VirtualKey;
  if (Key(VirtualKey::kW))
    move_z += 1.0f;
  if (Key(VirtualKey::kS))
    move_z -= 1.0f;
  if (Key(VirtualKey::kD))
    move_x += 1.0f;
  if (Key(VirtualKey::kA))
    move_x -= 1.0f;
  if (Key(VirtualKey::kRight))
    look_x += 1.0f;
  if (Key(VirtualKey::kLeft))
    look_x -= 1.0f;
  if (Key(VirtualKey::kUp))
    look_y += 1.0f;
  if (Key(VirtualKey::kDown))
    look_y -= 1.0f;
  if (Key(VirtualKey::kE))
    rise += 1.0f;
  if (Key(VirtualKey::kQ))
    rise -= 1.0f;
  if (Key(VirtualKey::k1))
    zoom += 1.0f;
  if (Key(VirtualKey::k3))
    zoom -= 1.0f;
  if (Key(VirtualKey::k2))
    g_reset_fov = true;
  if (Key(VirtualKey::kShift) || Key(VirtualKey::kLShift))
    speed *= 5.0f;
  if (Key(VirtualKey::kControl) || Key(VirtualKey::kLControl))
    speed *= 0.2f;
  move_x = std::clamp(move_x, -1.0f, 1.0f);
  move_z = std::clamp(move_z, -1.0f, 1.0f);
  look_x = std::clamp(look_x, -1.0f, 1.0f);
  look_y = std::clamp(look_y, -1.0f, 1.0f);
  rise = std::clamp(rise, -1.0f, 1.0f);
  zoom = std::clamp(zoom, -1.0f, 1.0f);
  const bool reset_fov = g_reset_fov.exchange(false);
  if (move_x || move_z || look_x || look_y || rise || zoom || reset_fov) {
    g_requests.ManualMotion();
  }

  // D3DX YawPitchRoll as the game uses it: yaw about Y (0 looks along +Z,
  // positive turns left), pitch about X (positive looks down). Y is up.
  const float turn = kTurnRate * (g_cam.vertical_fov ? *g_cam.vertical_fov / g_cam.seed_vertical_fov
                                                     : g_cam.fov / g_cam.seed_fov);
  g_cam.yaw -= look_x * turn * dt;
  if (look_y)
    g_cam.pitch = std::clamp(g_cam.pitch - look_y * turn * dt, -kMaxPitch, kMaxPitch);
  const float sp = std::sin(g_cam.pitch), cp = std::cos(g_cam.pitch);
  const float sy = std::sin(g_cam.yaw), cy = std::cos(g_cam.yaw);
  const float at[3] = {cp * sy, -sp, cp * cy};
  const float right[3] = {-cy, 0.0f, sy};
  const float step = speed * dt;
  for (int i = 0; i < 3; ++i) {
    g_cam.position[i] += (at[i] * move_z + right[i] * move_x) * step;
  }
  g_cam.position[1] += rise * step;

  if (g_cam.vertical_fov) {
    if (reset_fov)
      g_cam.vertical_fov = g_cam.seed_vertical_fov;
    g_cam.vertical_fov = std::clamp(*g_cam.vertical_fov + zoom * 30.0f * dt, 1.0f, 120.0f);
  } else {
    if (reset_fov)
      g_cam.fov = g_cam.seed_fov;
    if (zoom || reset_fov)
      g_cam.fov = std::clamp(g_cam.fov + zoom * kZoomRate * dt, kMinFov, kMaxFov);
  }
}

// Reads the keyboard for the camera. Runs before the runtime's own listener,
// so the F6/F8 binds can tell a key's auto-repeat from a new press.
class FreecamKeys : public rex::ui::WindowInputListener, public rex::ui::WindowListener {
 public:
  void OnKeyDown(rex::ui::KeyEvent& e) override {
    auto& key = g_keys[size_t(e.virtual_key()) & 0xFF];
    g_key_repeat = key.load();
    key = true;
  }
  void OnKeyUp(rex::ui::KeyEvent& e) override { g_keys[size_t(e.virtual_key()) & 0xFF] = false; }
  void OnLostFocus(rex::ui::UISetupEvent&) override {
    for (auto& key : g_keys) {
      key = false;
    }
  }
};
FreecamKeys g_key_listener;

}  // namespace

bool FreecamWorldFrozen() {
  return g_frozen.load(std::memory_order_relaxed);
}

void BringFamilyToCamera() {
  RequestTeleport();
}

bool FreecamActive() {
  return g_requests.Read().free;
}

bool PhotoModeActive() {
  return g_requests.Read().photo;
}

void SetFreecam(bool on) {
  if (on && !CameraTicking())
    return;
  g_requests.Manual(on, on && PhotoModeActive());
}

void SetPhotoMode(bool on) {
  if (on && !CameraTicking())
    return;
  g_requests.Manual(on || FreecamActive(), on);
}

uint64_t FreecamRequestsRevision() {
  return g_requests.Revision();
}

void ApplyFreecamRequests(bool free, bool photo) {
  g_requests.Apply({free, free && photo});
}

void ApplyFreecamRequests(const simpsons::modkit::CameraRequest& request) {
  g_requests.Apply(request);
}

simpsons::modkit::CameraRequest ReadFreecamRequests() {
  return g_requests.Read();
}

simpsons::modkit::CameraRequest CaptureFreecamRequests() {
  return g_requests.Baseline();
}

std::optional<simpsons::modkit::CameraPose> ReadCameraPose() {
  return g_requests.View();
}

void InitFreecam(rex::ui::Window* window) {
  if (window) {
    window->AddInputListener(&g_key_listener, 32);
    window->AddListener(&g_key_listener);
  }
  rex::ui::RegisterBind("bind_freecam", "F6", "Free camera on/off", [] {
    if (!g_key_repeat.load()) {
      ToggleFreecam();
    }
  });
  rex::ui::RegisterBind("bind_spooner", "F7", "Spooner mode (freecam world editor) on/off", [] {
    if (!g_key_repeat.load()) {
      if (spawner::SpoonerEnabled()) {
        spawner::SetSpoonerEnabled(false);
        game_ui::Notify("Spooner mode off");
      } else {
        if (!CameraTicking()) {
          game_ui::Notify("Load a level before using Spooner mode");
          return;
        }
        g_requests.Manual(true, false);
        spawner::SetSpoonerEnabled(true);
        game_ui::Notify("Spooner mode on — aim at any object and press F / A to grab");
      }
    }
  });
  rex::ui::RegisterBind("bind_photo_mode", "F8",
                        "Photo mode (free camera, world paused, HUD hidden)", [] {
                          if (!g_key_repeat.load()) {
                            TogglePhoto();
                          }
                        });
  rex::ui::RegisterBind("bind_freecam_teleport", "F9",
                        "Bring the family to the free camera (cheats)", [] {
                          if (!g_key_repeat.load()) {
                            RequestTeleport();
                          }
                        });
}

void ShutdownFreecam(rex::ui::Window* window) {
  g_requests.Reset();
  rex::ui::UnregisterBind("bind_freecam");
  rex::ui::UnregisterBind("bind_spooner");
  rex::ui::UnregisterBind("bind_photo_mode");
  rex::ui::UnregisterBind("bind_freecam_teleport");
  if (window) {
    window->RemoveInputListener(&g_key_listener);
    window->RemoveListener(&g_key_listener);
  }
  if (g_applied.exchange(false)) {
    rex::ui::ReleaseOverlayInput();
  }
}

REX_FUNC(sub_8269D8F8) {
  const uint32_t manager = ctx.r3.u32;
  // The handler that calls this serves the camera manager only, but its
  // pointer is checked against the singleton all the same.
  if (manager != LoadGuestU32(base, kCameraManager)) {
    __imp__sub_8269D8F8(ctx, base);
    return;
  }
  const int64_t now = NowNs();
  g_last_camera_tick_ns.store(now, std::memory_order_relaxed);
  const bool want = g_requests.Read().free;
  if (g_applied && (!want || manager != g_cam.manager)) {
    Disable(ctx, base, manager);
  }
  if (!g_applied && want && !Enable(base, base, manager)) {
    REXLOG_INFO("[freecam] no camera to start from in this view");
    g_requests.Reset();
  }
  if (!g_applied) {
    ctx.r3.u64 = manager;
    __imp__sub_8269D8F8(ctx, base);
    if (const auto d = Director(base, manager)) {
      simpsons::modkit::CameraPose pose;
      for (size_t i = 0; i < pose.position.size(); ++i)
        pose.position[i] = LoadGuestFloat(base, d + kDirectorPosition + uint32_t(i) * 4);
      pose.pitch = LoadGuestFloat(base, d + kDirectorAngles) * 180.0f / kPi;
      pose.yaw = LoadGuestFloat(base, d + kDirectorAngles + 4) * 180.0f / kPi;
      pose.roll = LoadGuestFloat(base, d + kDirectorAngles + 8) * 180.0f / kPi;
      if (const auto fov = VerticalFov(base, manager)) {
        pose.fov_degrees = *fov;
        g_requests.Publish(pose);
      } else {
        g_requests.Publish(std::nullopt);
      }
    } else {
      g_requests.Publish(std::nullopt);
    }
    return;
  }
  if (const auto pose = g_requests.TakePose()) {
    std::copy(pose->position.begin(), pose->position.end(), g_cam.position);
    g_cam.pitch = pose->pitch * kPi / 180.0f;
    g_cam.yaw = pose->yaw * kPi / 180.0f;
    g_cam.roll = pose->roll * kPi / 180.0f;
    g_cam.vertical_fov = g_cam.seed_vertical_fov = pose->fov_degrees;
  }
  if (g_want_teleport.exchange(false)) {
    // Everyone to the camera, a metre apart, then back to the gameplay camera.
    const auto family = players::List(base);
    for (size_t i = 0; i < family.size(); ++i) {
      std::array<float, 3> position{g_cam.position[0] + float(i), g_cam.position[1],
                                    g_cam.position[2]};
      players::MoveTo(ctx, base, family[i], position);
    }
    if (!family.empty()) {
      g_requests.Manual(false, false);
    }
  }
  const float dt = std::clamp(float(now - g_cam.last_ns) * 1e-9f, 0.0f, 0.1f);
  g_cam.last_ns = now;
  Fly(dt);
  if (spawner::SpoonerEnabled() && !game_ui::IsOpen()) {
    using rex::ui::VirtualKey;
    static bool prev_f = false, prev_c = false, prev_del = false;
    static uint16_t prev_pad = 0;
    const bool cur_f = Key(VirtualKey::kF);
    const bool cur_c = Key(VirtualKey::kC);
    const bool cur_del = Key(VirtualKey::kDelete) || Key(VirtualKey::kBack);
    const uint16_t cur_pad = g_pad.buttons;
    const uint16_t pad_edge = cur_pad & ~prev_pad;

    if ((cur_f && !prev_f) || (pad_edge & kPadA)) {
      if (spawner::SpoonerToggleGrab(ctx, base)) {
        if (const auto sel = spawner::SpoonerSelectedInfo(ctx, base))
          game_ui::Notify("Grabbed " + sel->name);
      } else {
        game_ui::Notify("Dropped entity");
      }
    }
    if ((cur_c && !prev_c) || (pad_edge & kPadY)) {
      if (!spawner::SpoonerSelectedLifetime())
        spawner::SpoonerSelectHovered();
      if (const uint64_t cloned = spawner::SpoonerCloneSelected(ctx, base))
        game_ui::Notify("Cloned entity");
    }
    if ((cur_del && !prev_del) || (pad_edge & kPadX)) {
      if (!spawner::SpoonerSelectedLifetime())
        spawner::SpoonerSelectHovered();
      if (spawner::SpoonerDeleteSelected(ctx, base))
        game_ui::Notify("Deleted entity");
    }
    const float rot_dir =
        ((Key(VirtualKey::kX) || (cur_pad & kPadRight)) ? 1.0f : 0.0f) -
        ((Key(VirtualKey::kZ) || (cur_pad & kPadLeft)) ? 1.0f : 0.0f);
    if (rot_dir != 0.0f)
      spawner::SpoonerRotateSelected(ctx, base, rot_dir * 90.0f * dt);

    const float dist_dir =
        ((Key(VirtualKey::kT) || (cur_pad & kPadUp)) ? 1.0f : 0.0f) -
        ((Key(VirtualKey::kR) || (cur_pad & kPadDown)) ? 1.0f : 0.0f);
    if (dist_dir != 0.0f)
      spawner::AdjustSpoonerGrabDistance(dist_dir * 8.0f * dt);

    prev_f = cur_f;
    prev_c = cur_c;
    prev_del = cur_del;
    prev_pad = cur_pad;
  }
  ApplyPose(ctx, base, manager, g_cam.position, g_cam.pitch, g_cam.yaw, g_cam.roll, g_cam.fov,
            g_cam.flags1, g_cam.flags2);
  if (g_cam.vertical_fov) {
    const auto y = ViewWindowY(base, manager);
    const auto fov =
        y ? simpsons::modkit::NativeCameraFov(g_cam.fov, *y, *g_cam.vertical_fov) : std::nullopt;
    if (fov && std::abs(*fov - g_cam.fov) > 0.000001f) {
      // The shipped writer owns pose, projection flags and aspect handling.
      // Its tan-half-angle scaling is linear, so correct the lens through the
      // same writer when a new shot or raster aspect changes that scale.
      g_cam.fov = g_cam.seed_fov = *fov;
      ApplyPose(ctx, base, manager, g_cam.position, g_cam.pitch, g_cam.yaw, g_cam.roll, g_cam.fov,
                g_cam.flags1, g_cam.flags2);
    }
  }
  const auto fov = VerticalFov(base, manager);
  if (fov)
    g_requests.Publish(simpsons::modkit::CameraPose{
        {g_cam.position[0], g_cam.position[1], g_cam.position[2]},
        g_cam.pitch * 180.0f / kPi,
        g_cam.yaw * 180.0f / kPi,
        g_cam.roll * 180.0f / kPi,
        *fov >= 0.99999f && *fov <= 120.00001f ? std::clamp(*fov, 1.0f, 120.0f) : *fov});
  else
    g_requests.Publish(std::nullopt);
}

REX_FUNC(sub_82B766A8) {
  modkit_input::Observation observation(ctx, base);
  const uint32_t user = ctx.r3.u32;
  const uint32_t state = ctx.r4.u32;
  __imp__sub_82B766A8(ctx, base);
  uint16_t injected = 0;
  if (ctx.r3.u32 != 0 && state && game_ui::InjectedButtons(user, &injected)) {
    // No controller there, but a press is being made for the game.
    static uint32_t packet = 0;
    simpsons::StoreGuestU32(base, state, ++packet);
    std::memset(GuestToHost(base, state + 4), 0, 12);
    StoreBE16(GuestToHost(base, state + 4), injected);
    ctx.r3.u64 = 0;
    return;
  }
  if (ctx.r3.u32 != 0 || !state) {
    if (user == 0) {
      // A disconnected controller must not leave the camera moving forever.
      g_pad = {};
      g_chord_down = g_r3_tap = false;
    }
    return;
  }
  uint8_t* gamepad = GuestToHost(base, state + 4);
  Pad pad;
  pad.buttons = LoadBE16(gamepad + 0);
  pad.lt = gamepad[2];
  pad.rt = gamepad[3];
  pad.lx = LoadBES16(gamepad + 4);
  pad.ly = LoadBES16(gamepad + 6);
  pad.rx = LoadBES16(gamepad + 8);
  pad.ry = LoadBES16(gamepad + 10);

  uint16_t game_buttons = 0;
  bool keep_sticks = false;
  if (game_ui::FilterController(user, pad.buttons, pad.lt, pad.rt, pad.lx, pad.ly, pad.rx,
                                pad.ry, &game_buttons, &keep_sticks)) {
    // A screen in the game has the controller, its buttons are still held, or
    // it leaves the pause menu for the player. The debug menu leaves the
    // sticks to the game.
    if (keep_sticks) {
      StoreBE16(gamepad, game_buttons);
      pad.buttons = game_buttons;
    } else {
      if (user == 0) {
        g_pad = {};
        g_chord_down = g_r3_tap = false;
      }
      std::memset(gamepad, 0, 12);
      StoreBE16(gamepad, game_buttons);
      return;
    }
  }

  if (user == 0) {
    const uint16_t pressed = pad.buttons & ~g_pad.buttons;
    const uint16_t released = g_pad.buttons & ~pad.buttons;
    const bool chord = (pad.buttons & (kPadL3 | kPadR3)) == (kPadL3 | kPadR3);
    if (chord && !g_chord_down) {
      g_chord_down = true;
      g_r3_tap = false;
      ToggleFreecam();
    } else if (!(pad.buttons & (kPadL3 | kPadR3))) {
      g_chord_down = false;
    }
    if ((pressed & kPadR3) && !(pad.buttons & kPadL3)) {
      g_r3_tap = true;
    }
    if (pad.buttons & kPadL3) {
      g_r3_tap = false;
    }
    if ((released & kPadR3) && g_r3_tap) {
      g_r3_tap = false;
      if (g_applied) {
        g_reset_fov = true;
      }
    }
    if ((pressed & kPadY) && g_applied && !spawner::SpoonerEnabled()) {
      TogglePhoto();
    }
    if ((pressed & kPadB) && g_applied) {
      RequestTeleport();
    }
    g_pad = pad;
    if (g_hold_until_neutral && !g_applied && pad.buttons == 0 && pad.lt < 30 && pad.rt < 30 &&
        Axis(pad.lx, 7849) == 0 && Axis(pad.ly, 7849) == 0 && Axis(pad.rx, 8689) == 0 &&
        Axis(pad.ry, 8689) == 0) {
      g_hold_until_neutral = false;
    }
  }

  if (g_applied || g_hold_until_neutral) {
    // A neutral, connected pad: buttons, triggers and sticks at rest.
    std::memset(gamepad, 0, 12);
  } else if (g_chord_down) {
    // The clicks that toggle the free camera are not the game's.
    StoreBE16(gamepad, pad.buttons & ~(kPadL3 | kPadR3));
  }
  native_ragdoll::FilterPlayerPreviewInput(base, user, gamepad);
}

// hint_text.cpp: a hint held back until its text is loaded.
void HintTextTick(PPCContext& ctx, uint8_t* base);
// characters.cpp: a character change waiting for its checkpoint restart.
void CharactersTick(PPCContext& ctx, uint8_t* base);

REX_FUNC(sub_82691540) {
  CharactersTick(ctx, base);
  HintTextTick(ctx, base);
  const bool want = g_requests.Read().photo && g_applied.load() &&
                    LoadGuestU32(base, kCameraManager) != 0 && LoadGuestU32(base, kSimEnabled) != 0;
  if (want != g_frozen.load()) {
    const uint64_t r3 = ctx.r3.u64, r4 = ctx.r4.u64;
    ctx.r3.u64 = kPhotoPauseBit;
    if (want) {
      sub_826913A0(ctx, base);
    } else {
      sub_82691368(ctx, base);
    }
    ctx.r3.u64 = r3;
    ctx.r4.u64 = r4;
    g_frozen = want;
    REXLOG_INFO("[freecam] photo mode {}", want ? "on: level paused" : "off");
  }
  __imp__sub_82691540(ctx, base);
}

REX_FUNC(sub_82691530) {
  if (ctx.r3.u32 == 0) {
    // Restore/remove owned ragdolls before source lifetimes are invalidated.
    // Any locked-world cleanup remains owned by the native rig service.
    native_ragdoll::StopPlayerPreviews(ctx, base);
    // Entities built from this level's data go with it.
    spawn_sources::BeforeWorldReset(ctx, base);
    spawner::Reset();
    players::ResetSessionState();
    // The level unloads or restarts: never leave it paused or the camera free.
    g_requests.Reset();
    g_reset_fov = false;
    g_last_camera_tick_ns = 0;
    // The camera may never tick again after unloading. Release the input
    // lock here, without accessing the camera manager that is being freed.
    if (g_applied.exchange(false)) {
      rex::ui::ReleaseOverlayInput();
      g_hold_until_neutral = true;
      REXLOG_INFO("[freecam] off: level unloading");
    }
    if (g_frozen.load()) {
      const uint64_t r3 = ctx.r3.u64;
      ctx.r3.u64 = kPhotoPauseBit;
      sub_82691368(ctx, base);
      ctx.r3.u64 = r3;
      g_frozen = false;
      REXLOG_INFO("[freecam] photo mode off: level unloading");
    }
  }
  __imp__sub_82691530(ctx, base);
}

static bool HudHidden() {
  return (g_frozen.load(std::memory_order_relaxed) && REXCVAR_GET(freecam_hide_hud)) ||
         debug_menu::HideHud();
}

REX_FUNC(sub_826D5CB0) {
  // With a screen of game_ui.cpp open the HUD pass still runs, to draw it, and
  // only the HUD's own movies are left out (sub_8272BA10).
  if (HudHidden() && !game_ui::IsOpen()) {
    ctx.r3.u64 = 1;
    return;
  }
  __imp__sub_826D5CB0(ctx, base);
}

REX_FUNC(sub_8272BA10) {
  if (HudHidden())
    return;
  __imp__sub_8272BA10(ctx, base);
}
