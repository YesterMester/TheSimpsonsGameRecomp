# Widescreen rendering

Full widescreen rendering is not yet enabled. `present_letterbox = false`
stretches the existing guest image; it does not create a wider camera view or
make the HUD use a different aspect ratio.

## Camera and culling

The camera manager singleton is at `0x82D08B10`. Its viewport count is at
`manager + 300`, and `manager + 20 + 4 * viewport` contains an `RwCamera*`.
The camera's `+4` field is its parent `RwFrame*`, not another camera. The color
raster is at `camera + 96`; the raster width and height are at `+12` and `+16`.

`sub_8269D130` writes a director's pose and field of view. It transforms the
camera's parent frame, computes `tan(fov / 2)`, and calls `sub_82714698`.
The latter calls `sub_82714588` to calculate the two-component view window,
then `sub_823F1C98` (`CameraSetViewWindow`). The setter writes the window at
`camera + 104/+108`, its reciprocals at `+112/+116`, and marks the parent
frame for an update through `sub_823F2460`. This keeps projection and the
game's frustum culling together. Changing only a host vertex shader would
leave objects culled against the old view.

`sub_82714588` takes the camera in `r4`, the widescreen flag in `r5`, the
vertical-FOV flag in `r6`, the output vector in `r7`, and `tan(fov / 2)` in
`f1`. For a horizontal FOV it writes `(tan, tan * height / width)`. For a
vertical FOV it writes `(aspect * factor * tan, factor * tan)`, with the
factor read from `0x8214E7B8` for widescreen or `0x821DD368` for 4:3.
Interlaced modes double the height before this calculation.
The loaded image's constants are exactly `0.5625` and `0.75`, respectively.

`src/widescreen.cpp` adds `widescreen_camera`, a development option that
defaults off. It changes this calculation only for the manager's single
gameplay camera with the director's widescreen flag set, using the effective
raster aspect calculated by the original helper (including interlacing).
The 4:3 director convention is left unchanged until its framing on a wider
display is qualified. Wider rasters retain the
reference vertical view and expose more horizontally; taller rasters retain
the horizontal view and expose more vertically. Stock 16:9/4:3 calculations
are unchanged regardless of the director's FOV flags. Split-screen and
auxiliary cameras are unchanged. The helper's
other caller, the camera-fitting code at `sub_82961FC0`, receives the same
corrected view window for the gameplay camera.

This option alone is not full widescreen support. With an unchanged 1280×720
raster, setting it cannot turn stretched presentation into a wider view.

## Render mode and allocation

Two independent layers pin the current rendering to the original modes:

- The front-end graphics setup around `0x82862004` writes `1280` and `720`
  into its open descriptor and sets the widescreen flags. Its other branch
  selects `1024×768`.
- `sub_823EE440` builds RenderWare's video-mode table. When either incoming
  width/height is zero, it ignores most of `XGetVideoMode`'s reported size and
  chooses `1280×720` or `640×480`. The selected dimensions are repeated across
  21 mode descriptors. `sub_823EFFD8` calls it at `0x823F0044`.

As a result, setting the SDK's `video_mode_width` / `video_mode_height` alone
is insufficient. A complete change must update the selected mode and the
resources derived from it together. In particular, larger color/depth targets
must preserve the EDRAM tile ownership and aliasing assumptions; increasing
the dimensions without that proof risks the effect flashes already fixed.

## HUD and movies

The UI scene callback is `sub_826D5CB0` (already wrapped by the photo mode).
It calls `sub_826D5050`. That function selects its own UI camera, sets a
constant view window from `0x8214F9EC`, and uses `sub_82A78188` to initialize
the 2D renderer. That initialization reads the camera raster's dimensions
and view window into a separate 2D context. The UI draw also derives its
uniform scale from the raster height and a per-viewport design height.
The UI must be changed at this layer, with consistent input and clipping,
rather than stretching the final frame or modifying every camera setter.

The widescreen camera hook deliberately leaves this path and movie cameras
alone. Menu/HUD centering, split-screen, post-effect dimensions and original
movie aspect still need a coherent implementation and rendering checks.

## Camera checks

The module compiles for Linux x86_64 and Android ARM64. A focused fixture
executes the original recompiled projection helper, camera setter and frame
dirtying function together with the hook. It checks 16 stock cases bit for
bit (16:9/4:3, both FOV conventions, both director flags and interlaced
fields), 10 changed-aspect cases (16:10, two ultrawide shapes, Pixel landscape
and 32:9), and six bypass cases (no viewport, split-screen, another camera,
the unqualified 4:3 director convention and the disabled option). The original
setter writes the new reciprocals and dirties the frame as expected.

These checks establish the camera hook's behavior and calling convention.
They are not a live game/render-target/HUD or phone qualification.

## Qualification still required

Before enabling widescreen or adding it to the launcher, compare a fixed
camera at 16:9, 16:10, 21:9, the Pixel landscape aspect and 32:9. Check added
scene coverage, edge culling, circles and characters, HUD/subtitles and menu
hit boxes, bloom/outlines/special attacks, loading and pause transitions,
in-game cutscenes and videos. Use the same checks on each backend's actual
output. The camera calculation alone is not evidence that these paths work.
