// Original projection arithmetic and view-window writes are unchanged. Video
// mode discovery and RenderWare's frame-dirty callback are explicit boundaries.
#include "camera.h"
#include "../../simpsons/src/guest_memory.h"
#include <rex/ppc/context.h>

#include <bit>
#include <cmath>
#include <iostream>
#include <limits>

using namespace simpsons::modkit;
REX_EXTERN(sub_823EC7C0);
REX_EXTERN(sub_823EC768);
REX_EXTERN(sub_823F2460);
namespace {
std::array<uint8_t, 32768> bytes{};
constexpr uint32_t camera = 0x1000, raster = 0x2000, frame = 0x3000, output = 0x4000;
bool interlaced = false;
int checks = 0, dirty = 0;
void Require(bool value, const char* message) {
  ++checks;
  if (!value)
    throw std::runtime_error(message);
}
uint32_t Read(uint32_t at) {
  if (at == 0x8214E7B8)
    return std::bit_cast<uint32_t>(0.5625f);
  if (at == 0x821DD368)
    return std::bit_cast<uint32_t>(0.75f);
  if (at == 0x82000BB0)
    return std::bit_cast<uint32_t>(1.0f);
  Require(uint64_t(at) + 4 <= bytes.size(), "Original projection reads only reviewed fields");
  return simpsons::LoadGuestU32(bytes.data(), at);
}
uint64_t Read64(uint32_t at) {
  return uint64_t(Read(at)) << 32 | Read(at + 4);
}
void Put(uint32_t at, uint32_t value) {
  Require(uint64_t(at) + 4 <= bytes.size(),
          "Original projection writes within its fixture mapping");
  simpsons::StoreGuestU32(bytes.data(), at, value);
}
void Put64(uint32_t at, uint64_t value) {
  Put(at, uint32_t(value >> 32));
  Put(at + 4, uint32_t(value));
}
}  // namespace

#undef REX_LOAD_U32
#define REX_LOAD_U32(address) Read(uint32_t(address))
#define REX_STORE_U32(address, value) Put(uint32_t(address), uint32_t(value))
#define REX_LOAD_U64(address) Read64(uint32_t(address))
#define REX_STORE_U64(address, value) Put64(uint32_t(address), uint64_t(value))
#define REX_FUNC_PROLOGUE() ((void)0)
#include "native_camera.h"

REX_FUNC(sub_823EC7C0) {
  ctx.r3.u64 = 0x5000;
}
REX_FUNC(sub_823EC768) {
  Put(ctx.r3.u32 + 12, interlaced ? 4 : 0);
}
REX_FUNC(sub_823F2460) {
  Require(ctx.r3.u32 == frame, "The original camera writer dirties its native frame");
  ++dirty;
}

int main(int argc, char** argv) {
  try {
    const float pi = std::acos(-1.0f);
    auto project = [&](float native, bool wide, bool vertical) {
      PPCContext ctx{};
      ctx.r1.u64 = 0x7800;
      ctx.r4.u64 = camera;
      ctx.r5.u64 = wide;
      ctx.r6.u64 = vertical;
      ctx.r7.u64 = output;
      ctx.f1.f64 = float(std::tan(native * 0.5f));
      GameCameraProjection(ctx, bytes.data());
      ctx.r3.u64 = camera;
      ctx.r4.u64 = output;
      GameCameraViewWindow(ctx, bytes.data());
      Require(ctx.r1.u32 == 0x7800, "The original projection/writer preserve their stack");
      return simpsons::LoadGuestFloat(bytes.data(), camera + 108);
    };
    size_t cases = 0;
    for (const auto [width, height] : std::vector<std::pair<uint32_t, uint32_t>>{{640, 480},
                                                                                 {1280, 720},
                                                                                 {1280, 800},
                                                                                 {1920, 1080},
                                                                                 {2560, 1080},
                                                                                 {3440, 1440},
                                                                                 {1080, 2400},
                                                                                 {320, 900}})
      for (bool wide : {false, true})
        for (bool vertical : {false, true})
          for (bool fields : {false, true})
            for (float desired : {1.0f, 5.0f, 20.0f, 35.0f, 60.0f, 90.0f, 120.0f}) {
              bytes.fill(0);
              interlaced = fields;
              Put(camera + 96, raster);
              Put(camera + 4, frame);
              Put(raster + 12, width);
              Put(raster + 16, height);
              const float initial = 60 * pi / 180;
              const float y = project(initial, wide, vertical);
              const auto native = NativeCameraFov(initial, y, desired);
              Require(native.has_value(),
                      "Native projection flags and raster aspects accept an authored lens");
              const auto actual = VerticalCameraFov(project(*native, wide, vertical));
              Require(actual && std::abs(*actual - desired) < 0.0003f,
                      "The unchanged game projection produces the requested vertical angle");
              const auto unchanged = NativeCameraFov(
                  *native, simpsons::LoadGuestFloat(bytes.data(), camera + 108), desired);
              Require(unchanged && std::abs(*unchanged - *native) < 0.000001f,
                      "A stable shot needs no repeated lens correction");
              ++cases;
            }
    for (float bad : {0.0f, -1.0f, std::numeric_limits<float>::infinity(),
                      std::numeric_limits<float>::quiet_NaN()}) {
      Require(!VerticalCameraFov(bad), "Invalid native view windows have no readable lens");
      Require(!NativeCameraFov(1, bad, 60), "Invalid projections cannot create a lens request");
      Require(!NativeCameraFov(bad, 1, 60), "Invalid original angles cannot create a lens request");
    }
    Require(!NativeCameraFov(pi, 1, 60) && !NativeCameraFov(1, 1, 0) && !NativeCameraFov(1, 1, 121),
            "Degenerate angles and out-of-range requests reject");
    Require(dirty == int(cases * 2),
            "Every oracle projection uses the original view-window writer");
    const Json result{{"ok", true}, {"checks", checks}, {"cases", cases}};
    if (argc > 1)
      WriteText(fs::path(argv[1]), result.dump());
    std::cout << result.dump() << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
