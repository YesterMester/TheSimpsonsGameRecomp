# Native camera authoring

Choose **Camera workshop** in the desktop Modkit application, or run:

```sh
python3 tools/modkit/modkit.py new "My camera shots" --template camera-workshop --id yourname.camera --author "Your name"
```

Creation leaves the project disabled. Its commented source adds a page in
**Options > Extras > Mods**. Fly the camera, capture shots A and B, preview
either shot, freeze the scene, change the lens, then release the camera.
The page uses the original Options movie. Captures survive code reload in the
same world; a new scene instance clears them. The starter deliberately avoids
applying coordinates from a previous checkpoint or level.

## Pose and lens

`game.camera.pose()` returns the last applied viewport 0 pose, or `nil` when no
supported level view is available. Positions are world coordinates with Y up.
Rotation contains pitch, yaw and roll in degrees. Yaw zero looks toward +Z;
positive yaw turns toward +X. Positive pitch looks down.

```lua
game.on("ui.action", function(event)
    if event.page ~= "my-page" then return end
    local pose = game.camera.pose()
    if event.row == "closer-lens" and pose then
        pose.fov_degrees = 35
        game.camera.set_pose(pose)
    elseif event.row == "finish" then
        game.camera.release()
    end
end)
```

`game.camera.set_pose(pose)` accepts a complete pose and returns its canonical
requested value. Each position axis is finite and within ±1,000,000. Pitch is
−90 through 90 degrees. Yaw and roll accept ±360,000 degrees and normalize to
−180 through 180. `fov_degrees` is the **actual vertical angle**, from 1 through
120 degrees. Unknown fields and invalid values reject before the working
request changes. Use `system.try_call` for expected host failures.

The game applies a pose on its next native camera update. Reading immediately
after a request can still return the preceding applied view. The original
camera writer updates the RenderWare frame and projection. Its different
director/aspect conventions are accounted for using the actual view window;
a wider screen keeps the requested vertical lens angle.

## Ownership and manual controls

Setting a pose enables the free camera and keeps the current photo-mode value.
Use `camera.photo(true)` to freeze the scene or `camera.photo(false)` to let
simulation run. Photo mode pauses simulation timers as well as characters and
physics. It is useful for staging; it is not a real-time cinematic clock.

The latest active camera request wins. `camera.release()` releases only the
calling mod generation's camera. Another mod's preceding explicit pose can
resume; the last release restores the captured manual camera or gameplay
modes. Other owned services stay active. Repeated release is safe, and mod
failure or retirement releases the camera automatically.

Manual camera movement, looking, zooming, reset or mode controls supersede old
mod requests. Retiring an old request then leaves the player's manual view
alone. Capturing a shot reads values and does not acquire a camera lease.

Camera poses contain no guest pointers or entity handles. Resource snapshots
retain request order, poses and the prior manual baseline. Legacy mode-only
snapshots remain supported. Checkpoints discard old-world camera requests;
exact/reload preflight validates them before replacing the running instance.
Full-scene exact restore, cinematic tracks and visual track editing remain
part of the [full gate](PLAN.md).

This API authors viewport 0. Either logical player can operate the native
workshop page, but this is not separate camera-track control for both co-op
viewports. Native Windows and Pixel behavior require their own qualification.
