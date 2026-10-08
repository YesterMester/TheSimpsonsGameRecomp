// Clean eye shading.
//
// Characters are drawn with simpsons_skin (pixel shader 9C0E6CF3A21EB196),
// which writes the rim shadow and rim light as bits of the packed world
// buffer; the composite darkens rim-shadow pixels to RimShadowLightFactor
// (0.55 at run time). The artists switched that off for the eyes: every vertex
// of the eye whites and pupils has vertex colour red 255, and the shader
// drops the rim bits where floor(red) is 1 (instructions 5, 29, 36).
// Interpolated across a triangle, the red that is exactly 1.0 at the corners
// comes out a hair below 1.0 at scattered pixels, floor() gives 0 there and
// those pixels get the rim shadow: the grainy dark speckle on the shaded side
// of the eyes. The Xbox 360 shows it too.
//
// "clean" reads the flag with a tolerance: instruction 5 no longer floors the
// red (its scalar write mask cleared), and instruction 35's unused vector slot
// computes 0.9 >= red with the shader's own literal c254.w = 0.9, which
// instruction 36 then uses in place of instruction 29's 1 - floor(red). Red 0
// and red 1 give the same result as before; only the interpolation noise is
// ignored, so the eyes are drawn as authored and everything else is unchanged.
// The changed microcode no longer matches the precompiled shader, so
// simpsons_skin is then translated at run time. The game copies its shaders
// when it creates its effects, hence the patch right after the image is
// loaded and a restart to change it.

#include "image_patch.h"

#include <cstdint>
#include <iterator>
#include <cstring>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/xmemory.h>

REXCVAR_DEFINE_STRING(eye_shading, "clean", "GPU",
                      "The characters' eyes: clean (as the artists flagged them) or original (the "
                      "speckled rim shadow of the Xbox 360 game)")
    .allowed({"clean", "original"})
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace {

// simpsons_skin pixel shader microcode; instruction N is at + N * 12.
constexpr uint32_t kSkinMicrocode = 0x8200A2C8;
// Its literal c254.w, the 0.9 the tolerant test uses.
constexpr uint32_t kSkinLiteral254W = 0x8200A2B4;

using simpsons::ImageWordPatch;

constexpr uint32_t Instruction(uint32_t index, uint32_t dword) {
  return kSkinMicrocode + index * 12 + dword * 4;
}

constexpr ImageWordPatch kCleanEyesPatch[] = {
    // 5: sge r3.w ... + floors r3.x, r3.x -> the same without the scalar write
    {Instruction(5, 0), 0x34180303, 0x34080303},
    // 35: (no vector op) + mulsc r0.x -> sge r1.x, c254.wwww, r3.xxxx + mulsc r0.x
    {Instruction(35, 0), 0xA8100000, 0xA8110001},
    {Instruction(35, 1), 0x00000042, 0x001B6C42},
    {Instruction(35, 2), 0xC200002F, 0x46FE032F},
};

}  // namespace

void ApplyEyeShadingOptions(rex::memory::Memory* memory) {
  if (REXCVAR_GET(eye_shading) != "clean") {
    return;
  }
  // Guard the literal too, then apply every word as one transaction.
  std::vector<ImageWordPatch> patch(std::begin(kCleanEyesPatch), std::end(kCleanEyesPatch));
  patch.push_back({kSkinLiteral254W, 0x3F666666, 0x3F666666});
  if (!simpsons::ApplyImagePatch(memory, patch)) {
    REXLOG_WARN(
        "eye_shading: the game image did not match or could not be patched; "
        "eyes left as they are");
    return;
  }
  REXLOG_INFO("eye_shading: clean");
}
