# Native camera authoring adapter

`sub_8269D130` applies a viewport pose through the native RenderWare frame and
projection writers. It accepts manager, viewport, position, pitch/yaw/roll in
radians, a lens angle and two projection-mode values. Manager +20 is viewport
0's camera when the viewport count at +300 is nonzero.

`sub_82714588` computes a view window from tan(lens angle / 2), the video mode,
camera raster dimensions and two byte flags. Its vertical-preserving branch
multiplies by the shipped 0.5625 widescreen or 0.75 standard factor; the other
branch derives vertical extent from the raster aspect. Interlaced fields
double the effective height. The director's input angle therefore is not
unconditionally an actual vertical field of view.

`sub_823F1C98` stores horizontal/vertical view-window extents at camera +104/+108,
their reciprocals at +112/+116, and marks the camera's frame dirty when present.

## Pose requests and measured lenses

The shared free-camera hook runs after the gameplay director. Gameplay pose
queries publish that director's world position/angles with the actual vertical
angle obtained from the native camera's +108 view-window extent. While the
free camera is active, queries publish the pose actually passed to the writer.
Unavailable or unsupported native views publish no pose.

Owned pose requests contain host values only. The game-thread hook consumes
the latest queued pose, keeps the original projection flags, and uses the
original writer. The actual view-window extent measures its tan-half-angle
scale. If a new requested vertical lens or changed raster aspect needs a
correction, the same writer receives the corrected input angle in that update.
A stable shot does not require repeated corrections. No GPU resources or
RenderWare projection fields are patched directly by this adapter.

The camera request binding captures an existing manual free-camera pose only
when the first owner acquires it. Queued pose updates, current requests and
published views have separate host state. Publishing a rendered view cannot
invalidate its owner's request; manual input explicitly changes the revision
and supersedes old leases. Targeted release preserves other owned services.
Retirement and restore preparation never call guest camera routines from a
worker. World reset discards old pose requests and published views.

## Qualification boundaries

The camera fixture covers pose schemas, queued consumption, manual baselines,
competing generations, targeted release, manual takeover, invalid-state
preflight and legacy mode-only snapshots. The project fixture executes the
actual commented Camera workshop, including capture, lens edits, preview,
reload and new-world invalidation; its native writer is a host boundary.

The projection fixture executes unchanged `sub_82714588` and `sub_823F1C98`
bodies with original register-save helpers. Video-mode discovery and frame
dirty notification are explicit boundaries. It checks 448 combinations of
raster shape, both projection flags, interlaced/progressive video and requested
vertical lenses from 1 through 120 degrees. Original native view writes agree
within 0.0003 degrees, and stable projections need no second correction.

Full-scene exact restore, independent co-op camera tracks, cinematic sequencing,
native track editing and Windows/Pixel device behavior remain separate gates.

The private Linux menu qualification captures all three Camera workshop pages
inside the original Options movie. Their compact status labels fit the native
rows, and Capture A produces a saved shot through the actual page action. The
same run verifies that an explicit false request remains an owned camera lease
until release, with unchanged world count and clean exit.
