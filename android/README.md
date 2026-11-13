# Android development build

The ARM64 app uses the same launcher as Windows and Linux, with Android's game-folder picker and a multitouch gamepad. The first phone target is the Pixel 9 Pro. This is development work for 0.0.7.0. Initial phone testing has reported stutters and audio skips; performance fixes still need to be measured on the device, and rendering and suspend/resume need further validation.

The native game code is compiled for ARM64. Rendering uses the Vulkan backend and its native resource paths; remaining compatibility fallbacks are still present. Some native GPU features require Vulkan 1.3 or corresponding extensions. The app requires Android 10 or newer, a Vulkan 1.1 device and a 4 KB memory-page kernel. Start at 1× render scale.

Vertex memory exports can run through a compute prepass on devices without vertex-stage storage writes, including the reported Pixel 9 Pro Vulkan configuration. The path is checked on a desktop GPU with that feature disabled. Overlapping export inputs, tessellated exports, user clip planes and non-solid fill still need additional implementations on devices that lack the corresponding features. A successful ARM64 build does not establish that every game draw works on the phone.

The shared shader layout and SPIR-V 1.3 output pass strict Vulkan 1.1 validation. The Android package supplies native shader replacements and compact warmup recipes. The phone builds and caches translations for its translator and device configuration.

The shared shader set contains 6,660 validated translated variants. Compact native
vertex inputs also cover large indices without allocating unused buffer prefixes.
CPU target-bound estimation avoids stale copies of GPU-owned vertices and indices.
These shared Vulkan changes are checked on the Deck GPU; Pixel driver behavior
still needs device qualification.

Before the first game frame, Android prepares known shader variants and supported recorded pipeline states for the phone's graphics capabilities. A preparation screen shows completed work and the current stage. Shader programs are read from your own game executable; the package supplies shader identities and pipeline recipes, rather than another GPU's driver binaries. Compatible translations are saved separately for each translator configuration and render scale, and driver caches are saved during play as well as on a clean exit. Later launches reuse these results. New draw states can still compile during play; the supplied recipes do not cover every campaign scene.

Android's 16 KB memory-page mode is not supported yet. The launcher explains this before Play, and the native entry point also rejects it before setting up guest memory. The APK's ELF alignment supports installation on 16 KB devices, but the runtime still needs independent protection of the game's 4 KB allocations and consistent physical-memory aliases. Larger host pages need that work before they can be enabled safely.

## Build

Install Android SDK platform 36, NDK r29 (29.0.14206865), Java 17 or newer, CMake 3.25 or newer, Ninja, Python 3.12 or newer and `glslangValidator` (the `glslang-tools` package on Debian/Ubuntu). Set `ANDROID_HOME`, `ANDROID_NDK_HOME` and `JAVA_HOME` to those installations, then run from the repository root:

```sh
tools/android/build.sh
```

The script builds the actual game, runtime and SDL libraries, verifies ARM64 ELF dependencies and 16 KB segment alignment, and packages the launcher and shader set, including native GLSL replacements. It checks the translated set against the current sources before packaging. It reuses its native build cache on later runs. `ANDROID_BUILD_JOBS` controls compilation parallelism (default 2); `RECOMP_ANDROID_BUILD_DIR` selects a separate build directory.

The local debug APK is `android/app/build/outputs/apk/debug/app-debug.apk`. It uses Android's development signing key. Release signing must be supplied separately by the project owner.

The build script uses `ANDROID_USER_HOME` for the development key, defaulting to `$HOME/.android`. Keep the same key for later test builds so Android can update the app while retaining its data.

Android Studio can build the project in this directory directly. Its CMake default is 3.31.6; use `-PcmakeVersion=<version>` to select another installed version. The wrapper uses Gradle 8.14.3.

## Signed preview packaging

For an owner-signed preview, set `RECOMP_ANDROID_SIGNING_STORE_FILE`,
`RECOMP_ANDROID_SIGNING_STORE_PASSWORD`, `RECOMP_ANDROID_SIGNING_KEY_ALIAS` and
`RECOMP_ANDROID_SIGNING_KEY_PASSWORD` in your local environment, then run
`tools/android/build.sh release`. The output is
`android/app/build/outputs/apk/release/app-release.apk`. Release builds reject
missing credentials and the normal development keystore. Keep passwords and
keystores outside the checkout; Gradle reads the environment without requiring
passwords in command arguments. Android's [signing guide](https://developer.android.com/studio/publish/app-signing)
explains key ownership and update requirements.

The manual **Android preview** workflow builds and checks an APK artifact; it
does not publish a release or upload game data. Configure these repository
secrets before running it:

- `ANDROID_SIGNING_STORE_BASE64`: the owner's keystore encoded as one base64 line.
- `ANDROID_SIGNING_STORE_PASSWORD`, `ANDROID_SIGNING_KEY_ALIAS` and
  `ANDROID_SIGNING_KEY_PASSWORD`: the corresponding signing credentials.
- `ANDROID_SIGNING_CERT_SHA256`: the public SHA-256 certificate fingerprint,
  obtained independently from the owner's keystore.

The workflow verifies the signature against that fingerprint, rejects debug
signing/debuggable release packages, checks all five ARM64 libraries and their
dependencies, checks ELF and ZIP alignment, and hashes the indexed shader
assets. It removes the temporary keystore after the job. The same checker can
verify a local build:

```sh
python3 tools/android/check_apk.py android/app/build/outputs/apk/release/app-release.apk \
  --release --build-tools "$ANDROID_HOME/build-tools/36.0.0" \
  --readelf "$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-readelf" \
  --native-manifest android/.cxx/native-arm64/jniLibs/native-libraries.json \
  --expected-certificate-sha256 YOUR_PUBLIC_CERTIFICATE_FINGERPRINT
```

A package check does not qualify phone rendering, sustained performance or
suspend/resume. A release certificate also differs from the existing development
certificate; Android requires the same signer for an in-place update. Preserve
the installed app's saves before switching signing identities.

## Use

Install the APK, open the Install tab and select **Choose game ISO**. The app reads the Xbox disc filesystem and extracts the game directly on the phone. It supports trimmed XISO images and full disc images with the partition offsets used by the desktop extractor. Local files extract without a second ISO copy; a storage provider that only offers a stream needs temporary space for a local copy. If your ISO is inside a ZIP, unpack the ZIP first. The APK includes no game files.

For files you have already extracted, select **Import extracted folder** and choose the game folder or its parent. The importer searches subfolders for `default.xex` before copying. If it finds more than one game folder, choose the specific folder you want.

Imports run in a foreground service and continue when you switch apps or turn off the screen. Allow notifications to see progress and return to the Install tab. The launcher and notification show the current file, percentage and file count; the launcher also shows bytes remaining and an estimated time when the source reports file sizes. Cancel stops before the final directory swap. Failed or interrupted imports keep the previous game; saves and settings are stored separately. Android force-stop or a device restart interrupts an import; select the folder again to retry.

The app uses the launcher's donut icon, including Android themed icons. Startup shows preparation progress, and unchanged shader assets are reused across APK updates. The phone layout keeps all tabs visible, leaves room for button borders and shadows, and places the Play button below its status text.

Play uses a complete touch gamepad: movement and camera sticks, A/B/X/Y, the directional pad, shoulder buttons, triggers, stick clicks, Back and Pause. Multiple fingers can hold movement, camera and attacks together. In **Settings → Touch controls**, choose **Edit control layout** to drag, resize, change opacity or hide each control independently. The gear button in game opens the same landscape editor; saved layout changes apply when you return. Reset one control or the whole layout at any time. Settings also include stick deadzone, camera sensitivity and Y inversion, a floating movement stick, button haptics and automatic hiding when a physical controller connects. Controls respect screen cutouts and gesture edges. Global settings apply on the next game launch.

The **Patches** tab provides the same reversible logo skipping, cutscene-skip confirmation and 18 start-episode choices as the desktop launcher. Close the game before changing a patch. Original episode scripts are backed up and restored when the patch is turned off; edits made by another tool are checked before writing. Frame-rate choices remain in Settings.

The **Mods** tab imports and manages packaged mods and language packs through Android's document picker, with background installation, notifications, dependency and conflict checks, and isolated per-profile progress folders. Imported packages remain disabled until enabled and keep their creator or translator credits. Mod authoring, source editing, isolated Test sessions and package export live in the standalone Windows and Linux [Modkit](../modkit/README.md) application.

The audio mixer and XMA decoder now request Android's audio thread priority instead of falling back to normal scheduling. Automatic background pipeline compilation is limited to two workers on Android, and the launcher's WebView pauses while the game runs. The game also gives Android a display frame-rate hint for its configured cap. These changes address CPU contention and presentation cadence; they do not establish a measured FPS improvement or prove that every reported skip is fixed. Shutdown also lets the current native audio callback finish before stopping the decoder and freeing its resources. Closing pauses callbacks while their game threads are still available.

The frame limiter learns the scheduler's wake-up margin rather than yielding for a fixed 2 ms at the end of each frame. It still waits for the game's original deadline. Android performance-hint sessions discard work spanning a pause, remove retired threads and recover from temporary service failures. Main-thread registration and frame-work timing belong to each game thread, including later launches. Vulkan upload pools also select memory using the requirements of the buffer being allocated. These changes apply across Tensor and Snapdragon hardware; actual device FPS, sustained heat and driver behavior still need qualification. See [Android performance](../simpsons/re/android_performance.md) for the measurements and checks.

The shared launcher also provides graphics and audio settings, local save backups and support bundles. **Support → Create support bundle** includes recent game logs, audio queue diagnostics, settings, the control layout, and device memory and thermal information. Use it after a problematic session to distinguish mixer underruns from silent frames submitted by the game. Desktop-only Steam integration, keyboard bindings and artwork generation are hidden. The app downloads updates, checks their signing certificate and opens Android's installer for confirmation.

Android sends stereo audio to speakers and headphones, mixing the game's centre dialogue channel into both sides before handing it to AAudio. This avoids relying on surround routing on phones that accept six-channel streams despite having stereo speakers. The Dialogue setting adjusts that centre contribution. Device logs include the stream's output channel count and centre gain; a successful build alone does not confirm the result on the phone.
