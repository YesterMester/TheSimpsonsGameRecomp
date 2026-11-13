#include <jni.h>

#include <atomic>

#include <rex/graphics/shader_warmup.h>
#include <rex/input/touch/touch_input_driver.h>
#include <rex/platform/performance_hint.h>
#include <rex/ppc.h>
#include <rex/system/kernel_state.h>
#include "game_menu_pointer.h"
#include "game_ui.h"

// A game menu or a screen drawn in the game (the debug menu, the
// achievements) takes touches directly; the touch controls hide meanwhile.
extern "C" JNIEXPORT jboolean JNICALL
Java_io_github_yestermester_simpsons_GameActivity_nativeMenuActive(JNIEnv*, jclass) {
  return GameMenuPointerActive() || game_ui::IsOpen() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jintArray JNICALL
Java_io_github_yestermester_simpsons_GameActivity_nativeShaderPreparation(JNIEnv* env, jclass) {
  uint32_t done = 0, total = 0, phase = 0;
  rex_graphics_shader_warmup_progress(&done, &total, &phase);
  const jint progress[] = {jint(done), jint(total), jint(phase)};
  jintArray result = env->NewIntArray(3);
  if (result)
    env->SetIntArrayRegion(result, 0, 3, progress);
  return result;
}

extern "C" JNIEXPORT void JNICALL
Java_io_github_yestermester_simpsons_GameActivity_nativeTouchState(
    JNIEnv*, jclass, jint buttons, jint lt, jint rt, jint lx, jint ly, jint rx, jint ry) {
  rex::input::touch::UpdateState(uint16_t(buttons), uint8_t(lt), uint8_t(rt), int16_t(lx),
                                int16_t(ly), int16_t(rx), int16_t(ry));
}

// Story videos are decoded by the game's own VP6 code, whose motion
// compensation runs for the blocks of every video frame. The touch overlay
// polls this count and hides the pad while it grows.
namespace {
std::atomic<uint32_t> g_video_blocks{0};
}  // namespace
REX_EXTERN(__imp__sub_8237C878);
REX_EXTERN(sub_8237C878);
REX_EXTERN(__imp__sub_82382848);
REX_EXTERN(sub_82382848);
REX_FUNC(sub_8237C878) {
  g_video_blocks.fetch_add(1, std::memory_order_relaxed);
  __imp__sub_8237C878(ctx, base);
}
REX_FUNC(sub_82382848) {
  g_video_blocks.fetch_add(1, std::memory_order_relaxed);
  __imp__sub_82382848(ctx, base);
}

extern "C" JNIEXPORT jint JNICALL
Java_io_github_yestermester_simpsons_GameActivity_nativeVideoBlocks(JNIEnv*, jclass) {
  return jint(g_video_blocks.load(std::memory_order_relaxed));
}

// frame_pacing.cpp: a cap on the frame rate, 0 for none.
void SetFrameRateCap(int32_t fps);

// Android's game modes (Game Dashboard): battery caps the game at 30 FPS.
extern "C" JNIEXPORT void JNICALL
Java_io_github_yestermester_simpsons_GameActivity_nativeSetFrameRateCap(JNIEnv*, jclass,
                                                                       jint fps) {
  SetFrameRateCap(int32_t(fps));
}

// Leaving the app works like opening the Xbox Guide: the game is sent XN_SYS_UI
// and pauses itself, as single-player Xbox 360 games had to, and it gets
// XN_SYS_UI off when the app is back in front.
extern "C" JNIEXPORT void JNICALL
Java_io_github_yestermester_simpsons_GameActivity_nativeSetBackground(JNIEnv*, jclass,
                                                                     jboolean background) {
  rex::platform::SetPerformanceHintActive(background != JNI_TRUE);
  constexpr rex::system::XNotificationID kSystemUi = 0x00000009;
  static std::atomic<bool> system_ui_shown{false};
  auto* kernel_state = REX_KERNEL_STATE();
  if (!kernel_state || system_ui_shown.exchange(background == JNI_TRUE) == (background == JNI_TRUE)) {
    return;
  }
  kernel_state->BroadcastNotification(kSystemUi, background == JNI_TRUE ? 1 : 0);
}
